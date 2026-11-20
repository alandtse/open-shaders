#include "DX12Resources.h"

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
		if (current->size == size) {
			const auto lookup = [&](SubcellKey key) -> std::shared_ptr<const Tile> {
				for (const auto& old : current->resident)
					if (old && old->key == key)
						return old;
				for (const auto& old : current->predictiveResident)
					if (old && old->key == key)
						return old;
				return {};
			};
			for (size_t i = 0; i < kGridCapacity; ++i)
				next->resident[i] = lookup(next->requested[i]);
			for (size_t i = 0; i < kPredictiveCapacity; ++i)
				next->predictiveResident[i] = lookup(next->predictive[i]);
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
			if (!key.scope || (existing && existing->state != Tile::State::Requested))
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
		constexpr uint64_t kMaximumTileBytes = 64ull * 1024 * 1024;
		uint64_t bytes = 0;
		std::unordered_set<uint64_t> identities;
		auto tile = std::make_shared<Tile>();
		tile->key = publication.key;
		bool valid = true;
		for (auto& shape : publication.shapes) {
			const uint64_t length = uint64_t(shape.positions.size()) * sizeof(float3) +
			                        uint64_t(shape.indices.size()) * sizeof(uint32_t);
			if (!shape.identity || !identities.insert(shape.identity).second ||
				shape.positions.empty() || shape.indices.empty() || shape.indices.size() % 3 ||
				length > kMaximumTileBytes - bytes ||
				!std::ranges::all_of(shape.positions, [](const float3& vertex) {
					return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z);
				}) ||
				!std::ranges::all_of(shape.transform, [](float component) { return std::isfinite(component); }) || !std::ranges::all_of(shape.indices, [&](uint32_t index) { return index < shape.positions.size(); })) {
				valid = false;
				continue;
			}
			bytes += length;
			tile->shapes.push_back(std::make_shared<const Shape>(std::move(shape)));
		}
		tile->bytes = bytes;
		tile->lights = std::move(publication.lights);
		tile->state = publication.enumerationComplete && publication.failedShapes == 0 && valid &&
		                      publication.eligibleShapes == tile->shapes.size() ?
		                  Tile::State::Ready :
		                  Tile::State::Incomplete;
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		if (active != current->requested.end()) {
			++next->geometryRevision;
			++next->lightRevision;
			next->resident[std::distance(current->requested.begin(), active)] = std::move(tile);
		} else {
			// Predictive preparation must never force BLAS/TLAS work or change active light revisions.
			next->predictiveResident[std::distance(current->predictive.begin(), predictive)] = std::move(tile);
		}
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	void ResourceGrid::Clear()
	{
		std::scoped_lock lock(mutex);
		pending.reset();
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

bool DX12Resources::IsActive() const
{
	return loaded && settings.Enabled && globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

DX12Resources::~DX12Resources() = default;

void DX12Resources::DrawSettings()
{
	if (ImGui::Checkbox("Enable companion geometry and lights", &settings.Enabled) && !settings.Enabled) {
		if (companion)
			companion->Cancel();
		inFlight.reset();
		grid.Clear();
	}
	int index = int(std::ranges::find(DX12::kSubcellSizes, settings.SubcellSize) - DX12::kSubcellSizes.begin());
	if (index >= int(DX12::kSubcellSizes.size()))
		index = 0;
	constexpr const char* labels[] = { "4096", "6144", "8192", "10240", "12288", "14336", "16384" };
	if (ImGui::Combo("Subcell size (game units)", &index, labels, int(std::size(labels)))) {
		settings.SubcellSize = DX12::kSubcellSizes[index];
		if (companion)
			companion->Cancel();
		inFlight.reset();
		grid.Clear();
	}
	ImGui::TextDisabled("5x5 companion: active 3x3, then 12 predictive sides and 4 corners; no persistent cache.");
}

void DX12Resources::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", true);
	const uint32_t size = input.value("SubcellSize", DX12::kSubcellSizes.front());
	settings.SubcellSize = std::ranges::contains(DX12::kSubcellSizes, size) ? size : DX12::kSubcellSizes.front();
	if (companion)
		companion->Cancel();
	inFlight.reset();
	grid.Clear();
}

void DX12Resources::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "SubcellSize", settings.SubcellSize } };
}

void DX12Resources::RestoreDefaultSettings()
{
	settings = {};
	if (companion)
		companion->Cancel();
	inFlight.reset();
	grid.Clear();
}

void DX12Resources::EarlyPrepass()
{
	CS_GPU_PASS("DX12Resources::Grid");
	if (!IsActive()) {
		if (grid.Snapshot()->requested.front().scope) {
			if (companion)
				companion->Cancel();
			inFlight.reset();
			grid.Clear();
		}
		return;
	}
	const auto* player = globals::game::player;
	const auto* cell = player ? player->GetParentCell() : nullptr;
	if (!cell)
		return;
	const auto* world = cell->IsExteriorCell() ? player->GetWorldspace() : nullptr;
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
		auto* handler = RE::TESDataHandler::GetSingleton();
		for (auto& record : result->lights) {
			const auto* reference = handler ? handler->LookupForm(record.referenceLocal, record.referencePlugin) : nullptr;
			const auto* base = handler ? handler->LookupForm(record.baseLocal, record.basePlugin) : nullptr;
			if (!reference || !base) {
				++result->publication.failedShapes;
				continue;
			}
			record.value.referenceID = reference->GetFormID();
			record.value.baseID = base->GetFormID();
			result->publication.lights.push_back(record.value);
		}
		grid.Publish(std::move(result->publication));
	}
	if (companion->Idle()) {
		if (const auto request = grid.NextRequest()) {
			const auto* source = world ? static_cast<const RE::TESForm*>(world) : static_cast<const RE::TESForm*>(cell);
			const auto* file = source ? source->GetFile(0) : nullptr;
			if (file && file->GetFilename()) {
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

void DX12Resources::OnSceneTransitionReset(bool opening)
{
	if (opening) {
		if (companion)
			companion->Cancel();
		inFlight.reset();
		grid.Clear();
	}
}

void DX12Resources::OnRuntimeDisabled()
{
	if (companion)
		companion->Cancel();
	inFlight.reset();
	grid.Clear();
}

std::vector<DX12::PreparedLight> DX12Resources::GetPreparedLights() const
{
	std::vector<DX12::PreparedLight> lights;
	const auto current = grid.Snapshot();
	for (const auto& tile : current->resident)
		if (tile)
			lights.insert(lights.end(), tile->lights.begin(), tile->lights.end());
	return lights;
}
