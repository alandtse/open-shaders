#include "DX12Resources.h"
#include "DX12NativeSceneInputs.h"

#include "CompanionWorker.h"
#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESWorldSpace.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <unordered_set>

namespace DX12
{

	void ResourceGrid::Advance(uint32_t scope, uint32_t size, double x, double y)
	{
		const auto desired = Neighborhood(scope, size, x, y);
		const auto predictive = PredictiveNeighborhood(scope, size, x, y);
		if (!desired || !predictive)
			return;
		std::scoped_lock lock(mutex);
		if (previousScope == scope && previousSize == size) {
			const double vx = x - previousX, vy = y - previousY;
			const double displacement = vx * vx + vy * vy;
			// Preserve the last non-jitter direction while stationary; disregard teleports.
			if (std::isfinite(displacement) && displacement > 1.0 && displacement < double(size) * size) {
				movementX = vx;
				movementY = vy;
			}
		} else {
			movementX = movementY = 0;
		}
		previousX = x;
		previousY = y;
		previousScope = scope;
		previousSize = size;
		const auto current = snapshot.load(std::memory_order_acquire);
		if (current->requested == *desired && current->size == size)
			return;
		auto next = std::make_shared<Grid>();
		next->revision = current->revision + 1;
		next->geometryRevision = current->geometryRevision + 1;
		next->lightRevision = current->lightRevision + 1;
		next->size = size;
		next->requested = *desired;
		next->predictive = *predictive;
		// A failed neighbor may be attempted at most three times while the player
		// remains in the same center subcell. A center transition starts a new
		// bounded retry window for those same neighboring keys.
		const bool playerChangedSubcell = current->requested.front() != next->requested.front();
		if (current->size == size) {
			const auto lookup = [&](SubcellKey key) -> std::shared_ptr<const Tile> {
				for (const auto& old : current->resident)
					if (old && old->key == key)
						return old;
				for (const auto& old : current->predictiveResident)
					if (old && old->key == key)
						return old;
				for (const auto& old : recentTiles)
					if (old && old->key == key)
						return old;
				return {};
			};
			const auto resetFailedAfterMove = [&](std::shared_ptr<const Tile>& tile) {
				if (!playerChangedSubcell || !tile ||
					(tile->state != Tile::State::Incomplete && tile->state != Tile::State::RetryWaiting))
					return;
				auto retry = std::make_shared<Tile>(*tile);
				retry->state = Tile::State::Requested;
				retry->preparationAttempts = 0;
				retry->retryAfter = {};
				retry->failureReason.clear();
				tile = std::move(retry);
			};
			for (size_t i = 0; i < kGridCapacity; ++i) {
				next->resident[i] = lookup(next->requested[i]);
				resetFailedAfterMove(next->resident[i]);
				if (next->resident[i] && next->resident[i]->state == Tile::State::Deferred) {
					// A CPU-budgeted speculative tile is re-requested at full quality on active promotion.
					auto promoted = std::make_shared<Tile>(*next->resident[i]);
					promoted->state = Tile::State::Requested;
					next->resident[i] = std::move(promoted);
				}
			}
			for (size_t i = 0; i < kPredictiveCapacity; ++i) {
				next->predictiveResident[i] = lookup(next->predictive[i]);
				resetFailedAfterMove(next->predictiveResident[i]);
			}
			// Runtime-only geometry follows the active neighborhood. Prune tiles
			// that leave 3x3 without a disk flush or stale GPU ownership.
			for (const auto& observed : current->runtimeShapes)
				if (std::ranges::contains(next->requested, observed.tile)) {
					next->runtimeShapes.push_back(observed);
					next->runtimeBytes += observed.bytes;
				}
		}
		// Keep recently retired immutable cell publications in bounded DRAM.
		// A -> B -> A must not repeat companion processing.
		if (current->size == size) {
			const auto retain = [&](const auto& tile) {
				if (!tile || (tile->state != Tile::State::Ready && tile->state != Tile::State::ReadyPartial))
					return;
				if (std::ranges::contains(next->requested, tile->key) || std::ranges::contains(next->predictive, tile->key))
					return;
				std::erase_if(recentTiles, [&](const auto& cached) { return cached && cached->key == tile->key; });
				recentTiles.push_back(tile);
			};
			for (const auto& tile : current->resident) retain(tile);
			for (const auto& tile : current->predictiveResident) retain(tile);
			while (recentTiles.size() > 32) recentTiles.pop_front();
		}
		if (pending && std::ranges::find(next->requested, *pending) == next->requested.end() &&
			std::ranges::find(next->predictive, *pending) == next->predictive.end())
			pending.reset();
		snapshot.store(std::move(next), std::memory_order_release);
	}

	std::optional<SubcellKey> ResourceGrid::NextRequest()
	{
		std::scoped_lock lock(mutex);
		if (pending)
			return std::nullopt;
		const auto current = snapshot.load(std::memory_order_acquire);
		const auto center = current->requested.front();
		if (!center.scope)
			return std::nullopt;

		const auto start = [&](SubcellKey key, bool speculative, size_t index) -> std::optional<SubcellKey> {
			const auto& existing = speculative ? current->predictiveResident[index] : current->resident[index];
			if (!key.scope || (existing && existing->state != Tile::State::Requested &&
								  (existing->state != Tile::State::RetryWaiting || std::chrono::steady_clock::now() < existing->retryAfter)))
				return std::nullopt;
			auto tile = existing ? std::make_shared<Tile>(*existing) : std::make_shared<Tile>();
			tile->key = key;
			tile->state = Tile::State::Preparing;
			auto next = std::make_shared<Grid>(*current);
			++next->revision;
			if (speculative)
				next->predictiveResident[index] = std::move(tile);
			else
				next->resident[index] = std::move(tile);
			snapshot.store(std::move(next), std::memory_order_release);
			pending = key;
			return pending;
		};
		const auto sorted = [&](auto keys, size_t begin, size_t end) {
			std::stable_sort(keys.begin() + begin, keys.begin() + end, [&](size_t a, size_t b) {
				const auto& left = current->requested[a];
				const auto& right = current->requested[b];
				return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
				       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
			});
			return keys;
		};
		std::array<size_t, kGridCapacity> activeOrder{};
		for (size_t i = 0; i < activeOrder.size(); ++i)
			activeOrder[i] = i;
		activeOrder = sorted(activeOrder, 1, activeOrder.size());
		for (size_t i : activeOrder) {
			const auto& key = current->requested[i];
			if (const auto request = start(key, false, i))
				return request;
		}
		std::array<size_t, kPredictiveCapacity> order{};
		for (size_t i = 0; i < order.size(); ++i)
			order[i] = i;
		std::stable_sort(order.begin(), order.begin() + 12, [&](size_t a, size_t b) {
			const auto& left = current->predictive[a];
			const auto& right = current->predictive[b];
			return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
			       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
		});
		std::stable_sort(order.begin() + 12, order.end(), [&](size_t a, size_t b) {
			const auto& left = current->predictive[a];
			const auto& right = current->predictive[b];
			return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
			       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
		});
		for (size_t i : order) {
			const auto& key = current->predictive[i];
			if (const auto request = start(key, true, i))
				return request;
		}
		return std::nullopt;
	}

	bool ResourceGrid::Publish(CompanionPublication publication)
	{
		std::scoped_lock lock(mutex);
		if (!pending || *pending != publication.key)
			return false;
		pending.reset();
		const auto current = snapshot.load(std::memory_order_acquire);
		const auto active = std::ranges::find(current->requested, publication.key);
		const auto predictive = std::ranges::find(current->predictive, publication.key);
		if (active == current->requested.end() && predictive == current->predictive.end())
			return false;
		auto tile = std::make_shared<Tile>();
		tile->key = publication.key;
		// Retain bounded retry state across failed publications of the same tile.
		const auto existing = active != current->requested.end() ?
		                          current->resident[std::distance(current->requested.begin(), active)] :
		                          current->predictiveResident[std::distance(current->predictive.begin(), predictive)];
		tile->preparationAttempts = static_cast<uint8_t>(std::min<unsigned>(existing ? unsigned(existing->preparationAttempts) + 1 : 1u, 3u));
		tile->failureReason = std::move(publication.failureReason);
		tile->rejectionReasons = std::move(publication.rejectionReasons);
		tile->nifSources = std::move(publication.nifSources);
		tile->missingShapes = std::move(publication.missingShapes);
		// The worker has already verified all positions, triangle indices, transforms, identities and budgets.
		// Render-thread publication only transfers ownership of immutable GPU-scene inputs.
		tile->shapes = std::move(publication.shapes);
		tile->bytes = publication.geometryBytes;
		tile->lights = std::move(publication.lights);
		const bool complete = publication.validated && publication.enumerationComplete &&
		                      publication.failedShapes == 0 && publication.eligibleShapes == tile->shapes.size();
		if (publication.validated && (complete || !tile->shapes.empty() || !tile->lights.empty())) {
			// Geometry validity and complete static-shadow coverage are different contracts.
			// Keep real partial geometry and placed lights available for native scene building.
			// Empty incomplete results must not be mislabeled as prepared subcells.
			// Only Ready (certified complete) subcells can offload raster shadows.
			tile->state = complete ? Tile::State::Ready : Tile::State::ReadyPartial;
		} else {
			// Malformed or empty-incomplete companion payloads must not claim native scene readiness.
			tile->shapes.clear();
			tile->lights.clear();
			tile->bytes = 0;
			if (tile->preparationAttempts < 3) {
				tile->state = Tile::State::RetryWaiting;
				tile->retryAfter = std::chrono::steady_clock::now() +
				                   std::chrono::milliseconds(tile->preparationAttempts == 1 ? 500 : 2000);
			} else {
				tile->state = Tile::State::Incomplete;
			}
		}
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		if (active != current->requested.end()) {
			++next->geometryRevision;
			++next->lightRevision;
			next->resident[std::distance(current->requested.begin(), active)] = std::move(tile);
		} else {
			// Sixteen predictive tiles have a strict combined 192 MiB CPU geometry allowance
			// (12 MiB per tile), independent of active GPU scene residency.
			constexpr uint64_t kPredictiveTileBudgetBytes = 12ull * 1024 * 1024;
			if (tile->bytes > kPredictiveTileBudgetBytes) {
				tile->shapes.clear();
				tile->bytes = 0;
				tile->state = Tile::State::Deferred;
			}
			// A ready predictive emitter can cast toward visible receivers even when
			// the emitter is off-screen. Update lights, never BLAS/TLAS for this tile.
			++next->lightRevision;
			next->predictiveResident[std::distance(current->predictive.begin(), predictive)] = std::move(tile);
		}
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	bool ResourceGrid::PublishRuntimeShape(std::shared_ptr<const Shape> shape, float centerX, float centerY)
	{
		constexpr size_t kMaximumRuntimeShapes = 128;
		constexpr uint64_t kMaximumRuntimeBytes = 24ull * 1024 * 1024;
		constexpr uint64_t kMaximumSingleShapeBytes = 256ull * 1024;
		if (!shape || !shape->identity || shape->positions.empty() || shape->indices.empty() ||
			shape->indices.size() % 3)
			return false;
		const uint64_t shapeBytes = uint64_t(shape->positions.size()) * sizeof(float3) +
		                            uint64_t(shape->indices.size()) * sizeof(uint32_t);
		if (!shapeBytes || shapeBytes > kMaximumSingleShapeBytes)
			return false;
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		if (!current->requested.front().scope || current->runtimeShapes.size() >= kMaximumRuntimeShapes ||
			shapeBytes > kMaximumRuntimeBytes - current->runtimeBytes)
			return false;
		const auto x = Coordinate(centerX, current->size);
		const auto y = Coordinate(centerY, current->size);
		if (!x || !y)
			return false;
		const SubcellKey key{ current->requested.front().scope, *x, *y };
		if (!std::ranges::contains(current->requested, key))
			return false;
		if (std::ranges::any_of(current->runtimeShapes, [&](const RuntimeShape& item) {
				return item.shape && item.shape->identity == shape->identity;
			}))
			return false;
		auto next = std::make_shared<Grid>(*current);
		next->runtimeShapes.push_back({ key, std::move(shape), shapeBytes });
		next->runtimeBytes += shapeBytes;
		++next->revision;
		++next->geometryRevision;
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	void ResourceGrid::FlushRetiredCache()
	{
		std::scoped_lock lock(mutex);
		recentTiles.clear();
	}

	void ResourceGrid::Clear()
	{
		std::scoped_lock lock(mutex);
		pending.reset();
		recentTiles.clear();
		previousScope = previousSize = 0;
		movementX = movementY = 0;
		const auto current = snapshot.load(std::memory_order_acquire);
		auto next = std::make_shared<Grid>();
		next->revision = current->revision + 1;
		next->geometryRevision = current->geometryRevision + 1;
		next->lightRevision = current->lightRevision + 1;
		snapshot.store(std::move(next), std::memory_order_release);
	}

	void ResourceGrid::CancelPending()
	{
		std::scoped_lock lock(mutex);
		if (!pending)
			return;
		const auto current = snapshot.load(std::memory_order_acquire);
		auto next = std::make_shared<Grid>(*current);
		const auto reset = [&](auto& keys, auto& resident) {
			const auto found = std::ranges::find(keys, *pending);
			if (found == keys.end())
				return;
			const auto i = size_t(std::distance(keys.begin(), found));
			if (resident[i] && resident[i]->state == Tile::State::Preparing) {
				auto tile = std::make_shared<Tile>(*resident[i]);
				tile->state = Tile::State::Requested;
				resident[i] = std::move(tile);
			}
		};
		reset(next->requested, next->resident);
		reset(next->predictive, next->predictiveResident);
		++next->revision;
		pending.reset();
		snapshot.store(std::move(next), std::memory_order_release);
	}

}

namespace
{
	const char* TileStatus(const std::shared_ptr<const DX12::Tile>& tile)
	{
		if (!tile)
			return "Not processed";
		switch (tile->state) {
		case DX12::Tile::State::Ready:
			return "Loaded";
		case DX12::Tile::State::ReadyPartial:
			return "Partially loaded";
		case DX12::Tile::State::Preparing:
			return "Processing";
		case DX12::Tile::State::RetryWaiting:
			return "Failed (retry pending)";
		case DX12::Tile::State::Incomplete:
			return "Skipped (retry exhausted)";
		case DX12::Tile::State::Deferred:
			return "Skipped (predictive budget)";
		case DX12::Tile::State::Requested:
			return "Not processed";
		}
		return "Unknown";
	}

	ImU32 TileColor(const std::shared_ptr<const DX12::Tile>& tile, bool active)
	{
		if (!tile)
			return IM_COL32(46, 54, 65, 255);  // Awaiting generation: dark slate.
		switch (tile->state) {
		case DX12::Tile::State::Ready:
			return active ? IM_COL32(243, 201, 74, 255) : IM_COL32(117, 197, 239, 255);
		case DX12::Tile::State::ReadyPartial:
			return active ? IM_COL32(200, 164, 71, 255) : IM_COL32(91, 156, 194, 255);
		case DX12::Tile::State::Preparing:
			return IM_COL32(182, 140, 91, 255);
		case DX12::Tile::State::RetryWaiting:
			return IM_COL32(211, 76, 76, 255);
		case DX12::Tile::State::Incomplete:
			return IM_COL32(123, 129, 139, 255);
		case DX12::Tile::State::Deferred:
			return IM_COL32(123, 129, 139, 255);
		case DX12::Tile::State::Requested:
			return IM_COL32(46, 54, 65, 255);
		}
		return IM_COL32(46, 54, 65, 255);
	}

	std::shared_ptr<const DX12::Tile> FindTile(
		const DX12::Grid& grid, DX12::SubcellKey key, bool active)
	{
		if (active) {
			for (size_t i = 0; i < DX12::kGridCapacity; ++i)
				if (grid.requested[i] == key)
					return grid.resident[i];
		} else {
			for (size_t i = 0; i < DX12::kPredictiveCapacity; ++i)
				if (grid.predictive[i] == key)
					return grid.predictiveResident[i];
		}
		return {};
	}

	void DrawSubcellMap(const DX12::Grid& grid)
	{
		const auto center = grid.requested.front();
		const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
		const float pitch = std::min(54.0f, std::max(28.0f, (available - 4.0f) / 5.0f));
		const float side = pitch - 4.0f;
		const float total = pitch * 5.0f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float left = origin.x + std::max(0.0f, (available - total) * 0.5f);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (int row = 0; row < 5; ++row) {
			for (int col = 0; col < 5; ++col) {
				const int dx = col - 2, dy = 2 - row;
				const bool active = std::max(std::abs(dx), std::abs(dy)) <= 1;
				const DX12::SubcellKey key{ center.scope, center.x + dx, center.y + dy };
				const auto tile = center.scope ? FindTile(grid, key, active) : nullptr;
				const ImVec2 p0{ left + col * pitch, origin.y + row * pitch };
				const ImVec2 p1{ p0.x + side, p0.y + side };
				draw->AddRectFilled(p0, p1, TileColor(tile, active), 3.0f);
				draw->AddRect(p0, p1, active ? IM_COL32(255, 225, 150, 200) : IM_COL32(138, 174, 207, 180), 3.0f, 0, active ? 1.8f : 1.0f);
				if (tile && tile->state == DX12::Tile::State::ReadyPartial) {
					// A diagonal distinguishes partially processed cells without hiding the zone color.
					draw->AddLine(ImVec2(p0.x + 4.0f, p1.y - 4.0f),
						ImVec2(p1.x - 4.0f, p0.y + 4.0f), IM_COL32(32, 44, 56, 170), 2.0f);
				}
				if (col == 2 && row == 2) {
					const ImVec2 marker{ (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
					draw->AddCircleFilled(marker, 5.0f, IM_COL32(30, 37, 48, 255));
					draw->AddCircle(marker, 5.0f, IM_COL32(255, 255, 255, 240), 0, 1.5f);
				}
			}
		}
		ImGui::Dummy(ImVec2(available, total));
		// Hover uses the immutable grid snapshot, so no worker or scene locks are taken.
		if (center.scope && ImGui::IsItemHovered()) {
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const int col = int((mouse.x - left) / pitch);
			const int row = int((mouse.y - origin.y) / pitch);
			if (mouse.x >= left && mouse.y >= origin.y && col >= 0 && col < 5 && row >= 0 && row < 5) {
				const int dx = col - 2, dy = 2 - row;
				const bool active = std::max(std::abs(dx), std::abs(dy)) <= 1;
				const DX12::SubcellKey key{ center.scope, center.x + dx, center.y + dy };
				const auto tile = FindTile(grid, key, active);
				ImGui::BeginTooltip();
				ImGui::Text("%s subcell (%lld, %lld): %s", active ? "Active" : "Predictive",
					static_cast<long long>(key.x), static_cast<long long>(key.y), TileStatus(tile));
				if (tile && tile->preparationAttempts)
					ImGui::Text("Attempts: %u / 3", unsigned(tile->preparationAttempts));
				if (tile && (!tile->nifSources.empty() || !tile->missingShapes.empty()))
					ImGui::Text("NIF sources: %zu | Missing shape references: %zu",
						tile->nifSources.size(), tile->missingShapes.size());
				if (tile && !tile->failureReason.empty())
					ImGui::TextWrapped("Last reason: %s", tile->failureReason.c_str());
				if (tile)
					for (const auto& [reason, count] : tile->rejectionReasons)
						ImGui::Text("%s: %u", reason.c_str(), count);
				ImGui::EndTooltip();
			}
		}
	}
}

bool DX12Resources::IsActive() const
{
	return loaded && globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

DX12Resources::DX12Resources() = default;
DX12Resources::~DX12Resources() = default;

void DX12Resources::ResetPreparation()
{
	// A new subcell size or world-scene epoch invalidates the grid and all
	// companion work. Keep this off the normal settings-load path when unchanged.
	if (companion)
		companion->Cancel();
	inFlight.reset();
	adopting.reset();
	lightAdoptionOffset = 0;
	grid.Clear();
	lastVisitedCell = previousVisitedCell = distinctTransitions = 0;
	flushGeneration.fetch_add(1, std::memory_order_release);
	preparationStatus.store(std::make_shared<const std::string>("Awaiting preparation"), std::memory_order_release);
	generationStats.store(std::make_shared<const DX12::PreparationStats>(), std::memory_order_release);
}

void DX12Resources::DrawSettings()
{
	int index = int(std::ranges::find(DX12::kSubcellSizes, settings.SubcellSize) - DX12::kSubcellSizes.begin());
	if (index >= int(DX12::kSubcellSizes.size()))
		index = 0;
	constexpr const char* labels[] = { "4096", "6144", "8192", "10240", "12288", "14336", "16384" };
	if (ImGui::Combo("Subcell size (game units)", &index, labels, int(std::size(labels)))) {
		const uint32_t selected = DX12::kSubcellSizes[index];
		if (settings.SubcellSize != selected) {
			settings.SubcellSize = selected;
			ResetPreparation();
		}
	}

	ImGui::SeparatorText("Subcell status (5x5)");
	const auto snapshot = GetGrid();

	// Keep the state counters and legend together beside the map on wide panels.
	// Narrow panels retain the original vertical layout without clipping text.
	const auto drawSubcellDetails = [&]() {
		if (snapshot) {
			size_t activeLoaded = 0, predictiveLoaded = 0, failed = 0, skipped = 0;
			const auto classify = [&](const auto& tile, bool active) {
				if (!tile)
					return;
				if (tile->state == DX12::Tile::State::Ready || tile->state == DX12::Tile::State::ReadyPartial)
					(active ? activeLoaded : predictiveLoaded)++;
				else if (tile->state == DX12::Tile::State::RetryWaiting)
					++failed;
				else if (tile->state == DX12::Tile::State::Incomplete || tile->state == DX12::Tile::State::Deferred)
					++skipped;
			};
			for (const auto& tile : snapshot->resident)
				classify(tile, true);
			for (const auto& tile : snapshot->predictiveResident)
				classify(tile, false);
			ImGui::Text("Loaded: active %zu / 9, predictive %zu / 16", activeLoaded, predictiveLoaded);
			ImGui::TextDisabled("Failed: %zu  |  Skipped: %zu", failed, skipped);
		}
		ImGui::TextColored(ImVec4(0.95f, 0.79f, 0.29f, 1.0f), "Yellow: loaded 3x3");
		ImGui::TextColored(ImVec4(0.46f, 0.77f, 0.94f, 1.0f), "Light blue: loaded predictive ring");
		ImGui::TextColored(ImVec4(0.83f, 0.30f, 0.30f, 1.0f), "Red: failed (retry pending)");
		ImGui::TextDisabled("Gray: skipped  |  Dark: not processed  |  Brown: processing");
		ImGui::TextDisabled("Diagonal: partially loaded  |  Center dot: player subcell");
		if (const auto status = GetPreparationStatus())
			ImGui::TextWrapped("Companion: %s", status->c_str());
	};

	if (ImGui::GetContentRegionAvail().x >= 680.0f &&
		ImGui::BeginTable("##SubcellStatusLayout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
		ImGui::TableSetupColumn("##SubcellMap", ImGuiTableColumnFlags_WidthFixed, 290.0f);
		ImGui::TableSetupColumn("##SubcellDetails", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		if (snapshot)
			DrawSubcellMap(*snapshot);
		ImGui::TableSetColumnIndex(1);
		drawSubcellDetails();
		ImGui::EndTable();
	} else {
		if (snapshot)
			DrawSubcellMap(*snapshot);
		drawSubcellDetails();
	}
}

void DX12Resources::DrawDiagnostics() const
{
	ImGui::SeparatorText("DX12 Resources / companion");
	if (const auto status = GetPreparationStatus())
		ImGui::TextWrapped("Companion: %s", status->c_str());
	const auto snapshot = GetGrid();
	if (snapshot) {
		ImGui::Text("Grid revision %llu, geometry revision %llu, light revision %llu",
			static_cast<unsigned long long>(snapshot->revision),
			static_cast<unsigned long long>(snapshot->geometryRevision),
			static_cast<unsigned long long>(snapshot->lightRevision));
		ImGui::Text("Scope: %08X; subcell size: %u; runtime shapes: %zu (%.1f MiB)",
			snapshot->requested.front().scope, snapshot->size, snapshot->runtimeShapes.size(),
			double(snapshot->runtimeBytes) / (1024.0 * 1024.0));
		std::array<size_t, 7> states{};
		size_t geometry = 0, lights = 0, nifSources = 0, missingShapes = 0;
		const auto accumulate = [&](const auto& tiles) {
			for (const auto& tile : tiles) {
				if (!tile)
					continue;
				++states[static_cast<size_t>(tile->state)];
				geometry += tile->shapes.size();
				lights += tile->lights.size();
				nifSources += tile->nifSources.size();
				missingShapes += tile->missingShapes.size();
			}
		};
		accumulate(snapshot->resident);
		accumulate(snapshot->predictiveResident);
		ImGui::Text("Tiles: requested %zu, preparing %zu, ready %zu, partial %zu, retry %zu, skipped %zu (exhausted %zu, predictive %zu)",
			states[0], states[1], states[2], states[3], states[4], states[5] + states[6], states[5], states[6]);
		ImGui::Text("Published shapes: %zu; placed lights: %zu (may include duplicates across tile boundaries)", geometry, lights);
		ImGui::Text("NIF source records: %zu; unresolved NIF shape records: %zu", nifSources, missingShapes);
		const auto supplemented = std::ranges::count_if(snapshot->runtimeShapes,
			[](const DX12::RuntimeShape& observed) { return observed.shape && !observed.shape->sourceNif.empty(); });
		ImGui::Text("NIF-matched runtime geometry additions: %zu", size_t(supplemented));
		const auto showRetries = [&](const auto& tiles, const char* region) {
			for (const auto& tile : tiles) {
				if (tile && (tile->state == DX12::Tile::State::Incomplete || tile->state == DX12::Tile::State::RetryWaiting)) {
					ImGui::Text("%s subcell (%lld, %lld): attempt %u / 3%s", region,
						static_cast<long long>(tile->key.x), static_cast<long long>(tile->key.y),
						unsigned(tile->preparationAttempts), tile->state == DX12::Tile::State::Incomplete ? " (skipped: retry exhausted)" : "");
					if (!tile->failureReason.empty())
						ImGui::TextWrapped("Last reason: %s", tile->failureReason.c_str());
				}
			}
		};
		showRetries(snapshot->resident, "Active");
		showRetries(snapshot->predictiveResident, "Predictive");
		const auto showRejections = [&](const auto& tiles, const char* region) {
			for (const auto& tile : tiles) {
				if (!tile || tile->rejectionReasons.empty())
					continue;
				ImGui::TextDisabled("%s subcell (%lld, %lld) geometry rejections:", region,
					static_cast<long long>(tile->key.x), static_cast<long long>(tile->key.y));
				for (const auto& [reason, count] : tile->rejectionReasons)
					ImGui::Text("%s: %u", reason.c_str(), count);
			}
		};
		showRejections(snapshot->resident, "Active");
		showRejections(snapshot->predictiveResident, "Predictive");
	}
	if (const auto stats = GetPreparationStats()) {
		const auto showTiming = [&](const char* label, const DX12::PreparationTiming& timing) {
			ImGui::Text("%s jobs: %llu completed, %llu failed, %llu partial; last %.1f ms, avg %.1f ms",
				label, static_cast<unsigned long long>(timing.completed),
				static_cast<unsigned long long>(timing.failed),
				static_cast<unsigned long long>(timing.partial), timing.lastMs, timing.AverageMs());
		};
		showTiming("Active", stats->active);
		showTiming("Predictive", stats->predictive);
	}
}

void DX12Resources::LoadSettings(json& input)
{
	const uint32_t requested = input.value("SubcellSize", DX12::kSubcellSizes.front());
	const uint32_t selected = std::ranges::contains(DX12::kSubcellSizes, requested) ?
	                              requested :
	                              DX12::kSubcellSizes.front();
	if (settings.SubcellSize != selected) {
		settings.SubcellSize = selected;
		ResetPreparation();
	}
}

void DX12Resources::SaveSettings(json& output)
{
	output = { { "SubcellSize", settings.SubcellSize } };
}

void DX12Resources::RestoreDefaultSettings()
{
	const uint32_t defaultSize = DX12::kSubcellSizes.front();
	if (settings.SubcellSize != defaultSize) {
		settings.SubcellSize = defaultSize;
		ResetPreparation();
	}
}

void DX12Resources::PreShadowMaps()
{
	CS_GPU_PASS("DX12Resources::Grid");
	if (!IsActive()) {
		if (grid.Snapshot()->requested.front().scope || inFlight || adopting)
			ResetPreparation();
		return;
	}
	const auto* player = globals::game::player;
	const auto* cell = player ? player->GetParentCell() : nullptr;
	if (!cell)
		return;
	const auto* world = cell->IsExteriorCell() ? player->GetWorldspace() : nullptr;
	const uint32_t cellID = cell->GetFormID();
	if (lastVisitedCell && cellID && cellID != lastVisitedCell) {
		if (cellID == previousVisitedCell)
			distinctTransitions = 0;  // Returning to a visited cell.
		else if (++distinctTransitions >= 2) {
			grid.FlushRetiredCache();
			flushGeneration.fetch_add(1, std::memory_order_release);
			distinctTransitions = 0;
		}
		previousVisitedCell = lastVisitedCell;
	}
	if (cellID)
		lastVisitedCell = cellID;
	const uint32_t scope = world ? world->GetFormID() : cell->GetFormID();
	const auto position = player->GetPosition();
	grid.Advance(scope, settings.SubcellSize, position.x, position.y);
	if (!companion)
		companion = std::make_unique<DX12::CompanionWorker>();
	const auto neighborhood = grid.Snapshot();
	if (inFlight) {
		const bool stillWanted = std::ranges::contains(neighborhood->requested, *inFlight) ||
		                         std::ranges::contains(neighborhood->predictive, *inFlight);
		// An in-flight predictive request must yield when an active tile becomes newly necessary.
		const bool speculative = !std::ranges::contains(neighborhood->requested, *inFlight);
		const bool activeMissing = std::ranges::any_of(neighborhood->resident, [](const auto& tile) {
			return !tile || tile->state == DX12::Tile::State::Requested;
		});
		if (!stillWanted || (speculative && activeMissing)) {
			companion->Cancel();
			grid.CancelPending();
			inFlight.reset();
		}
	}
	if (auto result = companion->Take()) {
		inFlight.reset();
		preparationStatus.store(std::make_shared<const std::string>(
									result->failure.empty() ? "Companion returned valid geometry and light data" : result->failure),
			std::memory_order_release);
		adopting = std::make_unique<DX12::CompanionResult>(std::move(*result));
		adopting->publication.failureReason = adopting->failure.substr(0, 320);
		adopting->publication.lights.reserve(adopting->lights.size());
		lightAdoptionOffset = 0;
	}
	if (adopting) {
		const auto key = adopting->publication.key;
		const bool active = std::ranges::contains(neighborhood->requested, key);
		const bool predictive = std::ranges::contains(neighborhood->predictive, key);
		const bool activeMissing = std::ranges::any_of(neighborhood->resident, [](const auto& tile) {
			return !tile || tile->state == DX12::Tile::State::Requested;
		});
		if ((!active && !predictive) || (predictive && activeMissing)) {
			// A superseded speculative publication must not block an active tile.
			adopting.reset();
			lightAdoptionOffset = 0;
			grid.CancelPending();
		} else {
			// Only engine FormID lookup must run on the game thread. Keep it bounded
			// while immutable geometry remains off-thread prepared.
			constexpr size_t kFormResolutionsPerFrame = 64;
			const size_t end = std::min(adopting->lights.size(), lightAdoptionOffset + kFormResolutionsPerFrame);
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (; lightAdoptionOffset < end; ++lightAdoptionOffset) {
				auto& record = adopting->lights[lightAdoptionOffset];
				const auto* reference = handler ? handler->LookupForm(record.referenceLocal, record.referencePlugin) : nullptr;
				const auto* base = handler ? handler->LookupForm(record.baseLocal, record.basePlugin) : nullptr;
				if (!reference || !base) {
					// This light cannot be matched; it must not invalidate unrelated
					// geometry or shadow coverage for other successfully resolved lights.
					continue;
				}
				record.value.referenceID = reference->GetFormID();
				record.value.baseID = base->GetFormID();
				adopting->publication.lights.push_back(record.value);
			}
			if (lightAdoptionOffset == adopting->lights.size()) {
				const auto& publication = adopting->publication;
				const bool valid = publication.validated;
				const bool prepared = valid && publication.enumerationComplete &&
				                      publication.failedShapes == 0 && publication.eligibleShapes == publication.shapes.size();
				const bool wasPredictiveRequest = adopting->predictive;
				const double elapsedMs = adopting->preparationMs;
				if (grid.Publish(std::move(adopting->publication))) {
					// This snapshot only counts accepted, finished worker requests; canceled
					// and superseded work is not misreported as generated geometry.
					auto next = std::make_shared<DX12::PreparationStats>(
						*generationStats.load(std::memory_order_acquire));
					auto& timing = wasPredictiveRequest ? next->predictive : next->active;
					++timing.completed;
					timing.failed += valid ? 0u : 1u;
					timing.partial += valid && !prepared ? 1u : 0u;
					timing.lastMs = elapsedMs;
					timing.totalMs += elapsedMs;
					generationStats.store(std::move(next), std::memory_order_release);
				}
				adopting.reset();
				lightAdoptionOffset = 0;
			}
		}
	}
	if (!adopting && companion->Idle()) {
		if (const auto request = grid.NextRequest()) {
			const auto* source = world ? static_cast<const RE::TESForm*>(world) : static_cast<const RE::TESForm*>(cell);
			const auto* file = source ? source->GetFile(0) : nullptr;
			if (file && !file->GetFilename().empty()) {
				DX12::CompanionRequest task;
				task.key = *request;
				task.predictive = !std::ranges::contains(grid.Snapshot()->requested, *request);
				task.size = settings.SubcellSize;
				task.localScopeID = source->GetLocalFormID();
				task.scopePlugin = file->GetFilename();
				task.worldspace = world != nullptr;
				if (companion->Enqueue(std::move(task))) {
					inFlight = *request;
					return;
				}
			}
			DX12::CompanionPublication incomplete;
			incomplete.key = *request;
			grid.Publish(std::move(incomplete));
		}
	}
}

void DX12Resources::OnSceneTransitionReset(bool /*opening*/)
{
	// Loading-menu transitions do not flush the DRAM cache.
	// The next observed cell ID determines whether this is a return trip.
}

void DX12Resources::OnRuntimeDisabled()
{
	ResetPreparation();
}

std::shared_ptr<const DX12Native::SceneInputs> DX12Resources::GetNativeSceneInputs() const
{
	return DX12Native::CollectSceneInputs(grid.Snapshot());
}

std::vector<DX12::PreparedLight> DX12Resources::GetPreparedLights() const
{
	std::vector<DX12::PreparedLight> lights;
	const auto current = grid.Snapshot();
	std::unordered_set<uint32_t> seen;
	const auto append = [&](const auto& tiles) {
		for (const auto& tile : tiles) {
			if (!tile)
				continue;
			for (const auto& light : tile->lights) {
				if (light.referenceID && !seen.insert(light.referenceID).second)
					continue;
				lights.push_back(light);
			}
		}
	};
	append(current->resident);
	// Screen-space receivers can be shadowed by emitters in the predictive ring.
	// Source visibility inside the camera frustum is not a light-admission test.
	append(current->predictiveResident);
	return lights;
}
