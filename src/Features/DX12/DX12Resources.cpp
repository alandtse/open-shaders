#include "DX12Resources.h"

#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TES.h"
#include "RE/T/TESWorldSpace.h"
#include "ScenePrepCoverage.h"
#include "ScenePrepWorker.h"
#include "Utils/Game.h"
#include "Utils/PerfUtils.h"
#include "Utils/SettingsControl.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <magic_enum/magic_enum.hpp>
#include <ranges>
#include <unordered_set>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DX12Resources::Settings,
	SubcellSize)

namespace
{
	constexpr std::array<const char*, 7> kSubcellSizeLabels = { "4096", "6144", "8192", "10240", "12288", "14336", "16384" };
	constexpr std::array<uint32_t, 7> kSubcellSizeValues = { 4096, 6144, 8192, 10240, 12288, 14336, 16384 };
	static_assert(kSubcellSizeValues == DX12::kSubcellSizes);
}

namespace DX12
{
	constexpr uint64_t kPredictiveTileBudgetBytes = 12ull * 1024 * 1024;
	constexpr size_t kRetiredTileLimit = 32;
	constexpr unsigned kPreparationAttemptLimit = 3;
	constexpr uint32_t kFirstRetryDelayMs = 500;
	constexpr uint32_t kSecondRetryDelayMs = 2000;
	constexpr uint32_t kRetiredCacheFlushTransitions = 2;
	constexpr double kMinimumMovementSquared = 1.0;

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
			if (std::isfinite(displacement) && displacement > kMinimumMovementSquared && displacement < double(size) * size) {
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
		next->geometryRevision = current->geometryRevision + 1;
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
				retry->retryAfterQpc = 0;
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
				auto tile = lookup(next->predictive[i]);
				resetFailedAfterMove(tile);
				if (tile && (tile->state == Tile::State::Ready || tile->state == Tile::State::ReadyPartial) &&
					tile->bytes > kPredictiveTileBudgetBytes) {
					// Moving from active residency must respect the same 12 MiB per-tile cap
					// as fresh predictive publications. Keep prepared lights available.
					auto deferred = std::make_shared<Tile>(*tile);
					deferred->shapes.clear();
					deferred->bytes = 0;
					deferred->state = Tile::State::Deferred;
					tile = std::move(deferred);
				}
				next->predictiveResident[i] = std::move(tile);
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
		// A -> B -> A must not repeat subcell preparation.
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
			while (recentTiles.size() > kRetiredTileLimit) recentTiles.pop_front();
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

		// Sample QPC at most once, and only if a candidate is waiting for retry.
		std::optional<int64_t> nowQpc;
		const auto retryPending = [&](int64_t deadline) {
			if (!nowQpc)
				nowQpc = Util::GetQpcTicks();
			return Util::QpcDeadlinePending(*nowQpc, deadline);
		};
		const auto start = [&](SubcellKey key, bool speculative, size_t index) -> std::optional<SubcellKey> {
			const auto& existing = speculative ? current->predictiveResident[index] : current->resident[index];
			if (!key.scope || (existing && existing->state != Tile::State::Requested &&
								  (existing->state != Tile::State::RetryWaiting || retryPending(existing->retryAfterQpc))))
				return std::nullopt;
			auto tile = existing ? std::make_shared<Tile>(*existing) : std::make_shared<Tile>();
			tile->key = key;
			tile->state = Tile::State::Preparing;
			auto next = std::make_shared<Grid>(*current);
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
		std::stable_sort(order.begin(), order.begin() + kPredictiveSideCount, [&](size_t a, size_t b) {
			const auto& left = current->predictive[a];
			const auto& right = current->predictive[b];
			return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
			       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
		});
		std::stable_sort(order.begin() + kPredictiveSideCount, order.end(), [&](size_t a, size_t b) {
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

	bool ResourceGrid::Publish(TilePublication publication)
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
		tile->preparationAttempts = static_cast<uint8_t>(std::min<unsigned>(existing ? unsigned(existing->preparationAttempts) + 1 : 1u, kPreparationAttemptLimit));
		tile->hasUnattachedCells = publication.hasUnattachedCells;
		tile->failureReason = std::move(publication.failureReason);
		tile->rejectionReasons = std::move(publication.rejectionReasons);
		tile->nifSources = std::move(publication.nifSources);
		tile->missingShapes = std::move(publication.missingShapes);
		// The worker has already verified all positions, triangle indices, transforms, identities and budgets.
		// Render-thread publication only transfers ownership of immutable GPU-scene inputs.
		tile->shapes = std::move(publication.shapes);
		tile->bytes = publication.geometryBytes;
		tile->lights = std::move(publication.lights);
		const bool complete = IsScenePrepCoverageComplete(
			publication.validated, publication.enumerationComplete, publication.failedShapes,
			publication.rejectedLights, publication.eligibleShapes, tile->shapes.size());
		if (publication.validated && (complete || !tile->shapes.empty() || !tile->lights.empty())) {
			// Partial geometry is usable but does not certify complete static-shadow coverage.
			tile->state = complete ? Tile::State::Ready : Tile::State::ReadyPartial;
		} else {
			// Malformed or empty-incomplete publications must not claim native scene readiness.
			tile->shapes.clear();
			tile->lights.clear();
			tile->bytes = 0;
			if (tile->preparationAttempts < kPreparationAttemptLimit) {
				tile->state = Tile::State::RetryWaiting;
				tile->retryAfterQpc = Util::QpcDeadlineAfterMs(
					Util::GetQpcTicks(), Util::GetQpcFrequency(), tile->preparationAttempts == 1 ? kFirstRetryDelayMs : kSecondRetryDelayMs);
			} else {
				tile->state = Tile::State::Incomplete;
			}
		}
		auto next = std::make_shared<Grid>(*current);
		if (active != current->requested.end()) {
			++next->geometryRevision;
			next->resident[std::distance(current->requested.begin(), active)] = std::move(tile);
		} else {
			// Sixteen predictive tiles have a strict combined 192 MiB CPU geometry allowance
			// (12 MiB per tile), independent of active GPU scene residency.
			if (tile->bytes > kPredictiveTileBudgetBytes) {
				tile->shapes.clear();
				tile->bytes = 0;
				tile->state = Tile::State::Deferred;
			}
			// A ready predictive emitter can cast toward visible receivers even when
			// the emitter is off-screen. Update lights, never BLAS/TLAS for this tile.
			next->predictiveResident[std::distance(current->predictive.begin(), predictive)] = std::move(tile);
		}
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	bool ResourceGrid::PublishRuntimeShape(std::shared_ptr<const Shape> shape, float centerX, float centerY, uint32_t expectedScope)
	{
		// Larger interior furniture and room pieces often exceed 256 KiB.
		// Keep the whole runtime supplement transient and strictly budgeted.
		constexpr size_t kMaximumRuntimeShapes = 384;
		constexpr uint64_t kMaximumRuntimeBytes = 64ull * 1024 * 1024;
		constexpr uint64_t kMaximumSingleShapeBytes = 1024ull * 1024;
		if (!shape || !shape->identity || shape->positions.empty() || shape->indices.empty() ||
			shape->indices.size() % 3)
			return false;
		const uint64_t shapeBytes = uint64_t(shape->positions.size()) * sizeof(float3) +
		                            uint64_t(shape->indices.size()) * sizeof(uint32_t);
		if (!shapeBytes || shapeBytes > kMaximumSingleShapeBytes)
			return false;
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		if (!current->requested.front().scope || (expectedScope && current->requested.front().scope != expectedScope))
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
		if (shape->runtimeObjectKey) {
			// A new transform replaces the prior observation of this physical mesh.
			// Existing immutable GPU snapshots keep old geometry alive until retirement.
			std::erase_if(next->runtimeShapes, [&](const RuntimeShape& observed) {
				if (!observed.shape || observed.shape->runtimeObjectKey != shape->runtimeObjectKey)
					return false;
				next->runtimeBytes -= observed.bytes;
				return true;
			});
		}
		if (next->runtimeShapes.size() >= kMaximumRuntimeShapes ||
			shapeBytes > kMaximumRuntimeBytes - next->runtimeBytes)
			return false;
		next->runtimeShapes.push_back({ key, std::move(shape), shapeBytes });
		next->runtimeBytes += shapeBytes;
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
		next->geometryRevision = current->geometryRevision + 1;
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
		pending.reset();
		snapshot.store(std::move(next), std::memory_order_release);
	}

	void ResourceGrid::RefreshUnattachedTiles()
	{
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		auto next = std::make_shared<Grid>(*current);
		bool changed = false;
		const auto markRefresh = [&](auto& tiles) {
			for (auto& tile : tiles) {
				if (tile && tile->hasUnattachedCells && tile->state != Tile::State::Requested &&
					tile->state != Tile::State::Preparing && tile->state != Tile::State::Deferred) {
					auto refreshed = std::make_shared<Tile>(*tile);
					refreshed->state = Tile::State::Requested;
					refreshed->preparationAttempts = 0;
					refreshed->retryAfterQpc = 0;
					refreshed->failureReason.clear();
					tile = std::move(refreshed);
					changed = true;
				}
			}
		};
		markRefresh(next->resident);
		markRefresh(next->predictiveResident);
		if (changed)
			snapshot.store(std::move(next), std::memory_order_release);
	}

}

namespace
{
	struct SubcellMapStyle
	{
		static constexpr ImU32 waiting = IM_COL32(46, 54, 65, 255);
		static constexpr ImU32 activeReady = IM_COL32(243, 201, 74, 255);
		static constexpr ImU32 predictiveReady = IM_COL32(117, 197, 239, 255);
		static constexpr ImU32 activePartial = IM_COL32(200, 164, 71, 255);
		static constexpr ImU32 predictivePartial = IM_COL32(91, 156, 194, 255);
		static constexpr ImU32 preparing = IM_COL32(182, 140, 91, 255);
		static constexpr ImU32 retryWaiting = IM_COL32(211, 76, 76, 255);
		static constexpr ImU32 skipped = IM_COL32(123, 129, 139, 255);
		static constexpr ImU32 activeOutline = IM_COL32(255, 225, 150, 200);
		static constexpr ImU32 predictiveOutline = IM_COL32(138, 174, 207, 180);
		static constexpr ImU32 partialStripe = IM_COL32(32, 44, 56, 170);
		static constexpr ImU32 playerFill = IM_COL32(30, 37, 48, 255);
		static constexpr ImU32 playerOutline = IM_COL32(255, 255, 255, 240);
		static constexpr ImVec4 activeLegend{ 0.95f, 0.79f, 0.29f, 1.0f };
		static constexpr ImVec4 predictiveLegend{ 0.46f, 0.77f, 0.94f, 1.0f };
		static constexpr ImVec4 retryLegend{ 0.83f, 0.30f, 0.30f, 1.0f };
		static constexpr ImVec4 statsHeading{ 0.40f, 0.77f, 0.49f, 1.0f };
		static constexpr float minPitch = 28.0f;
		static constexpr float maxPitch = 54.0f;
		static constexpr float gap = 4.0f;
		static constexpr float rounding = 3.0f;
		static constexpr float activeOutlineWidth = 1.8f;
		static constexpr float predictiveOutlineWidth = 1.0f;
		static constexpr float stripeInset = 4.0f;
		static constexpr float stripeWidth = 2.0f;
		static constexpr float playerRadius = 5.0f;
		static constexpr float playerOutlineWidth = 1.5f;
		static constexpr float threeColumnMinWidth = 1050.0f;
		static constexpr float twoColumnMinWidth = 680.0f;
		static constexpr float mapColumnWidth = 290.0f;
		static constexpr float detailsColumnWidth = 375.0f;
	};

	const char* TileStatus(const std::shared_ptr<const DX12::Tile>& tile)
	{
		using State = DX12::Tile::State;
		using Label = const char* (*)();
		static constexpr auto labels = [] {
			std::array<Label, magic_enum::enum_count<State>()> result{};
			result[*magic_enum::enum_index(State::Requested)] = [] { return T("menu.dx12.resources.status.not_processed", "Not processed"); };
			result[*magic_enum::enum_index(State::Preparing)] = [] { return T("menu.dx12.resources.status.processing", "Processing"); };
			result[*magic_enum::enum_index(State::Ready)] = [] { return T("menu.dx12.resources.status.loaded", "Loaded"); };
			result[*magic_enum::enum_index(State::ReadyPartial)] = [] { return T("menu.dx12.resources.status.partially_loaded", "Partially loaded"); };
			result[*magic_enum::enum_index(State::RetryWaiting)] = [] { return T("menu.dx12.resources.status.retry_pending", "Failed (retry pending)"); };
			result[*magic_enum::enum_index(State::Incomplete)] = [] { return T("menu.dx12.resources.status.retry_exhausted", "Skipped (retry exhausted)"); };
			result[*magic_enum::enum_index(State::Deferred)] = [] { return T("menu.dx12.resources.status.predictive_budget", "Skipped (predictive budget)"); };
			return result;
		}();
		static_assert(std::ranges::all_of(labels, [](Label label) { return label != nullptr; }));
		const auto index = magic_enum::enum_index(tile ? tile->state : State::Requested);
		return index ? labels[*index]() : T("menu.dx12.resources.status.unknown", "Unknown");
	}

	ImU32 TileColor(const std::shared_ptr<const DX12::Tile>& tile, bool active)
	{
		if (!tile)
			return SubcellMapStyle::waiting;
		switch (tile->state) {
		case DX12::Tile::State::Ready:
			return active ? SubcellMapStyle::activeReady : SubcellMapStyle::predictiveReady;
		case DX12::Tile::State::ReadyPartial:
			return active ? SubcellMapStyle::activePartial : SubcellMapStyle::predictivePartial;
		case DX12::Tile::State::Preparing:
			return SubcellMapStyle::preparing;
		case DX12::Tile::State::RetryWaiting:
			return SubcellMapStyle::retryWaiting;
		case DX12::Tile::State::Incomplete:
			return SubcellMapStyle::skipped;
		case DX12::Tile::State::Deferred:
			return SubcellMapStyle::skipped;
		case DX12::Tile::State::Requested:
			return SubcellMapStyle::waiting;
		}
		return SubcellMapStyle::waiting;
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
		const float pitch = std::clamp((available - SubcellMapStyle::gap) / DX12::kPredictiveDiameter,
			SubcellMapStyle::minPitch, SubcellMapStyle::maxPitch);
		const float side = pitch - SubcellMapStyle::gap;
		const float total = pitch * DX12::kPredictiveDiameter;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float left = origin.x + std::max(0.0f, (available - total) * 0.5f);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (int row = 0; row < DX12::kPredictiveDiameter; ++row) {
			for (int col = 0; col < DX12::kPredictiveDiameter; ++col) {
				const int dx = col - DX12::kPredictiveRadius, dy = DX12::kPredictiveRadius - row;
				const bool active = std::max(std::abs(dx), std::abs(dy)) <= DX12::kGridRadius;
				const DX12::SubcellKey key{ center.scope, center.x + dx, center.y + dy };
				const auto tile = center.scope ? FindTile(grid, key, active) : nullptr;
				const ImVec2 p0{ left + col * pitch, origin.y + row * pitch };
				const ImVec2 p1{ p0.x + side, p0.y + side };
				draw->AddRectFilled(p0, p1, TileColor(tile, active), SubcellMapStyle::rounding);
				draw->AddRect(p0, p1, active ? SubcellMapStyle::activeOutline : SubcellMapStyle::predictiveOutline, SubcellMapStyle::rounding, 0,
					active ? SubcellMapStyle::activeOutlineWidth : SubcellMapStyle::predictiveOutlineWidth);
				if (tile && tile->state == DX12::Tile::State::ReadyPartial) {
					// A diagonal distinguishes partially processed cells without hiding the zone color.
					draw->AddLine(ImVec2(p0.x + SubcellMapStyle::stripeInset, p1.y - SubcellMapStyle::stripeInset),
						ImVec2(p1.x - SubcellMapStyle::stripeInset, p0.y + SubcellMapStyle::stripeInset), SubcellMapStyle::partialStripe, SubcellMapStyle::stripeWidth);
				}
				if (col == DX12::kPredictiveRadius && row == DX12::kPredictiveRadius) {
					const ImVec2 marker{ (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
					draw->AddCircleFilled(marker, SubcellMapStyle::playerRadius, SubcellMapStyle::playerFill);
					draw->AddCircle(marker, SubcellMapStyle::playerRadius, SubcellMapStyle::playerOutline, 0, SubcellMapStyle::playerOutlineWidth);
				}
			}
		}
		ImGui::Dummy(ImVec2(available, total));
		// Hover uses the immutable grid snapshot, so no worker or scene locks are taken.
		if (center.scope && ImGui::IsItemHovered()) {
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const int col = int((mouse.x - left) / pitch);
			const int row = int((mouse.y - origin.y) / pitch);
			if (mouse.x >= left && mouse.y >= origin.y && col >= 0 && col < DX12::kPredictiveDiameter && row >= 0 && row < DX12::kPredictiveDiameter) {
				const int dx = col - DX12::kPredictiveRadius, dy = DX12::kPredictiveRadius - row;
				const bool active = std::max(std::abs(dx), std::abs(dy)) <= DX12::kGridRadius;
				const DX12::SubcellKey key{ center.scope, center.x + dx, center.y + dy };
				const auto tile = FindTile(grid, key, active);
				ImGui::BeginTooltip();
				ImGui::Text(T("menu.dx12.resources.tooltip.subcell", "%s subcell (%lld, %lld): %s"), active ? T("menu.dx12.resources.active", "Active") : T("menu.dx12.resources.predictive", "Predictive"),
					static_cast<long long>(key.x), static_cast<long long>(key.y), TileStatus(tile));
				if (tile && tile->preparationAttempts)
					ImGui::Text(T("menu.dx12.resources.tooltip.attempts", "Attempts: %u / 3"), unsigned(tile->preparationAttempts));
				if (tile && (!tile->nifSources.empty() || !tile->missingShapes.empty()))
					ImGui::Text(T("menu.dx12.resources.tooltip.nif_sources", "NIF sources: %zu | Missing shape references: %zu"),
						tile->nifSources.size(), tile->missingShapes.size());
				if (tile && !tile->failureReason.empty())
					ImGui::TextWrapped(T("menu.dx12.resources.tooltip.last_reason", "Last reason: %s"), tile->failureReason.c_str());
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
DX12Resources::~DX12Resources()
{
	if (worker) {
		worker->Cancel();
		worker.reset();
	}
}

void DX12Resources::ResetPreparation()
{
	if (!preparationActive)
		return;
	if (worker) {
		worker->Cancel();
		worker->DrainRetiredObjects();
	}
	inFlight.reset();
	grid.Clear();
	lastVisitedCell = previousVisitedCell = distinctTransitions = 0;
	lastExteriorGridX = lastExteriorGridY = std::numeric_limits<int32_t>::min();
	interiorRefsLoaded = false;
	generationStats.store(std::make_shared<const DX12::PreparationStats>(), std::memory_order_release);
}

void DX12Resources::DrawSettings()
{
	const uint32_t previousSize = settings.SubcellSize;
	if (Util::Combo(T("menu.dx12.resources.subcell_size", "Subcell size (game units)"), &settings.SubcellSize,
			kSubcellSizeLabels.data(), kSubcellSizeValues, 0) &&
		!SettingsControl::IsReplicating() && settings.SubcellSize != previousSize) {
		ResetPreparation();
	}

	ImGui::SeparatorText(T("menu.dx12.resources.subcell_status", "Subcell status (5x5)"));
	const auto snapshot = GetGrid();
	const auto timing = GetPreparationStats();

	const auto drawSubcellDetails = [&]() {
		if (snapshot) {
			size_t activeLoaded = 0, predictiveLoaded = 0;
			const auto classify = [&](const auto& tile, bool active) {
				if (tile && (tile->state == DX12::Tile::State::Ready || tile->state == DX12::Tile::State::ReadyPartial))
					(active ? activeLoaded : predictiveLoaded)++;
			};
			for (const auto& tile : snapshot->resident)
				classify(tile, true);
			for (const auto& tile : snapshot->predictiveResident)
				classify(tile, false);
			ImGui::Text(T("menu.dx12.resources.loaded_counts", "Loaded: active %zu / 9, predictive %zu / 16"), activeLoaded, predictiveLoaded);
		}
		ImGui::TextColored(SubcellMapStyle::activeLegend, T("menu.dx12.resources.legend.yellow", "Yellow: loaded 3x3"));
		ImGui::TextColored(SubcellMapStyle::predictiveLegend, T("menu.dx12.resources.legend.light_blue", "Light blue: loaded predictive ring"));
		ImGui::TextColored(SubcellMapStyle::retryLegend, T("menu.dx12.resources.legend.red", "Red: failed (retry pending)"));
		ImGui::TextDisabled(T("menu.dx12.resources.legend.diagonal", "Diagonal: partially loaded  |  Center dot: player subcell"));
	};

	const auto drawGenerationStats = [&]() {
		ImGui::TextColored(SubcellMapStyle::statsHeading, T("menu.dx12.resources.stats.title", "Subcell generation stats"));
		ImGui::TextDisabled(T("menu.dx12.resources.stats.timing_head", "Last / Avg / Worst (milliseconds)"));
		const auto show = [](const char* label, const DX12::PreparationTiming& value) {
			if (value.completed)
				ImGui::Text("%s: %.1f / %.1f / %.1f ms", label, value.lastMs, value.AverageMs(), value.worstMs);
			else
				ImGui::TextDisabled("%s: -- / -- / --", label);
		};
		if (timing) {
			show(T("menu.dx12.resources.stats.main", "Main"), timing->active);
			show(T("menu.dx12.resources.predictive", "Predictive"), timing->predictive);
			show(T("menu.dx12.resources.stats.corners", "Corners"), timing->corners);
		}
	};

	const float available = ImGui::GetContentRegionAvail().x;
	if (available >= SubcellMapStyle::threeColumnMinWidth &&
		ImGui::BeginTable("##SubcellStatusLayout", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
		ImGui::TableSetupColumn("##SubcellMap", ImGuiTableColumnFlags_WidthFixed, SubcellMapStyle::mapColumnWidth);
		ImGui::TableSetupColumn("##SubcellDetails", ImGuiTableColumnFlags_WidthFixed, SubcellMapStyle::detailsColumnWidth);
		ImGui::TableSetupColumn("##SubcellGenerationStats", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		if (snapshot)
			DrawSubcellMap(*snapshot);
		ImGui::TableSetColumnIndex(1);
		drawSubcellDetails();
		ImGui::TableSetColumnIndex(2);
		drawGenerationStats();
		ImGui::EndTable();
	} else if (available >= SubcellMapStyle::twoColumnMinWidth &&
			   ImGui::BeginTable("##SubcellStatusLayout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
		ImGui::TableSetupColumn("##SubcellMap", ImGuiTableColumnFlags_WidthFixed, SubcellMapStyle::mapColumnWidth);
		ImGui::TableSetupColumn("##SubcellDetails", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		if (snapshot)
			DrawSubcellMap(*snapshot);
		ImGui::TableSetColumnIndex(1);
		drawSubcellDetails();
		ImGui::EndTable();
		drawGenerationStats();
	} else {
		if (snapshot)
			DrawSubcellMap(*snapshot);
		drawSubcellDetails();
		drawGenerationStats();
	}
}

void DX12Resources::LoadSettings(json& input)
{
	const uint32_t previousSize = settings.SubcellSize;
	const Settings loadedSettings = input;
	settings.SubcellSize = std::ranges::contains(DX12::kSubcellSizes, loadedSettings.SubcellSize) ?
	                           loadedSettings.SubcellSize :
	                           DX12::kSubcellSizes.front();
	if (settings.SubcellSize != previousSize)
		ResetPreparation();
}

void DX12Resources::SaveSettings(json& output)
{
	output = settings;
}

void DX12Resources::RestoreDefaultSettings()
{
	const uint32_t defaultSize = DX12::kSubcellSizes.front();
	if (settings.SubcellSize != defaultSize) {
		settings.SubcellSize = defaultSize;
		ResetPreparation();
	}
}

bool DX12Resources::WantsPreShadowMapsHook() const { return IsActive(); }

void DX12Resources::PreShadowMaps()
{
	if (!IsActive() || globals::game::quitGame) {
		OnRuntimeDisabled();
		return;
	}
	CS_GPU_PASS("DX12Resources::Grid");
	const auto location = Util::GetPlayerSpatialLocation();
	if (!location)
		return;
	const auto* cell = location->cell;
	const auto* world = location->worldspace;
	const uint32_t cellID = cell->GetFormID();
	if (lastVisitedCell && cellID && cellID != lastVisitedCell) {
		if (cellID == previousVisitedCell)
			distinctTransitions = 0;  // Returning to a visited cell.
		else if (++distinctTransitions >= DX12::kRetiredCacheFlushTransitions) {
			grid.FlushRetiredCache();
			distinctTransitions = 0;
		}
		previousVisitedCell = lastVisitedCell;
	}
	if (cellID)
		lastVisitedCell = cellID;
	const uint32_t scope = world ? world->GetFormID() : cell->GetFormID();
	const auto& position = location->position;
	preparationActive = true;
	grid.Advance(scope, settings.SubcellSize, position.x, position.y);
	if (world && globals::game::tes) {
		const int32_t gx = globals::game::tes->currentGridX;
		const int32_t gy = globals::game::tes->currentGridY;
		if (lastExteriorGridX != std::numeric_limits<int32_t>::min() &&
			(gx != lastExteriorGridX || gy != lastExteriorGridY))
			grid.RefreshUnattachedTiles();
		lastExteriorGridX = gx;
		lastExteriorGridY = gy;
		interiorRefsLoaded = false;
	} else {
		lastExteriorGridX = lastExteriorGridY = std::numeric_limits<int32_t>::min();
		const bool refsLoaded = DX12::IsCellFullyLoaded(cell);
		if (refsLoaded && !interiorRefsLoaded)
			grid.RefreshUnattachedTiles();
		interiorRefsLoaded = refsLoaded;
	}
	runtimePublicationEnabled.store(true, std::memory_order_release);
	if (!worker)
		worker = std::make_unique<DX12::ScenePrepWorker>();
	worker->DrainRetiredObjects();
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
			worker->Cancel();
			worker->DrainRetiredObjects();
			grid.CancelPending();
			inFlight.reset();
		}
	}
	if (auto result = worker->Take()) {
		inFlight.reset();
		const auto key = result->publication.key;
		const bool active = std::ranges::contains(neighborhood->requested, key);
		const bool predictive = std::ranges::contains(neighborhood->predictive, key);
		const bool activeMissing = std::ranges::any_of(neighborhood->resident, [](const auto& tile) {
			return !tile || tile->state == DX12::Tile::State::Requested;
		});
		if ((!active && !predictive) || (predictive && activeMissing)) {
			grid.CancelPending();
		} else {
			result->publication.failureReason = result->failure.substr(0, 320);
			const bool wasPredictiveRequest = result->predictive;
			const bool wasCornerRequest = result->corner;
			const double elapsedMs = result->preparationMs;
			if (grid.Publish(std::move(result->publication))) {
				auto next = std::make_shared<DX12::PreparationStats>(
					*generationStats.load(std::memory_order_acquire));
				auto& timing = !wasPredictiveRequest ? next->active :
				               wasCornerRequest      ? next->corners :
				                                       next->predictive;
				++timing.completed;
				timing.lastMs = elapsedMs;
				timing.totalMs += elapsedMs;
				timing.worstMs = std::max(timing.worstMs, elapsedMs);
				generationStats.store(std::move(next), std::memory_order_release);
			}
		}
	}
	if (worker->Idle()) {
		if (const auto request = grid.NextRequest()) {
			const auto requestedGrid = grid.Snapshot();
			const bool predictive = !std::ranges::contains(requestedGrid->requested, *request);
			const bool corner = predictive &&
			                    std::ranges::find(requestedGrid->predictive.begin() + DX12::kPredictiveSideCount,
									requestedGrid->predictive.end(), *request) != requestedGrid->predictive.end();
			if (auto task = DX12::CaptureScenePrepRequest(*request, settings.SubcellSize, cell, world, predictive, corner);
				task && worker->Enqueue(std::move(*task))) {
				inFlight = *request;
				return;
			}
			DX12::TilePublication incomplete;
			incomplete.key = *request;
			grid.Publish(std::move(incomplete));
		}
	}
}

void DX12Resources::OnRuntimeDisabled()
{
	if (!preparationActive)
		return;
	runtimePublicationEnabled.store(false, std::memory_order_release);
	ResetPreparation();
	// Joins the worker here so model roots it retained are released on this thread, not at exit.
	worker.reset();
	preparationActive = false;
}

void DX12Resources::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening)
		ResetPreparation();
}

std::vector<DX12::PreparedLight> DX12Resources::GetPreparedLights() const
{
	std::vector<DX12::PreparedLight> lights;
	lights.reserve(DX12::kMaxPreparedLights);
	const auto current = grid.Snapshot();
	std::unordered_set<uint32_t> seen;
	const auto append = [&](const auto& tiles) {
		for (const auto& tile : tiles) {
			if (!tile)
				continue;
			for (const auto& light : tile->lights) {
				// Active tiles come first; never publish more than the shadow consumer can trace.
				if (lights.size() >= DX12::kMaxPreparedLights)
					return;
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
