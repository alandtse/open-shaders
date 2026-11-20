#include "DX12Resources.h"

#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"

#include <algorithm>
#include <ranges>

namespace DX12
{
	void ResourceGrid::Advance(uint32_t scope, uint32_t size, double x, double y)
	{
		const auto desired = Neighborhood(scope, size, x, y);
		if (!desired)
			return;
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		if (current->requested == *desired && current->size == size)
			return;
		auto next = std::make_shared<Grid>();
		next->revision = current->revision + 1;
		next->size = size;
		next->requested = *desired;
		if (current->size == size)
			for (size_t i = 0; i < kGridCapacity; ++i)
				for (const auto& old : current->resident)
					if (old && old->key == next->requested[i]) {
						next->resident[i] = old;
						break;
					}
		if (pending && std::ranges::find(next->requested, *pending) == next->requested.end())
			pending.reset();
		snapshot.store(std::move(next), std::memory_order_release);
	}

	std::optional<SubcellKey> ResourceGrid::NextRequest()
	{
		std::scoped_lock lock(mutex);
		if (pending)
			return std::nullopt;
		const auto current = snapshot.load(std::memory_order_acquire);
		for (size_t i = 0; i < kGridCapacity; ++i)
			if (current->requested[i].scope && !current->resident[i]) {
				pending = current->requested[i];
				return pending;
			}
		return std::nullopt;
	}

	bool ResourceGrid::Publish(const SubcellKey& key, std::vector<Shape> shapes)
	{
		std::scoped_lock lock(mutex);
		if (!pending || *pending != key)
			return false;
		pending.reset();
		const auto current = snapshot.load(std::memory_order_acquire);
		const auto index = std::ranges::find(current->requested, key);
		if (index == current->requested.end())
			return false;
		constexpr uint64_t kMaximumTileBytes = 64ull * 1024 * 1024;
		uint64_t bytes = 0;
		for (const auto& shape : shapes) {
			if (shape.positions.empty() || shape.indices.empty() || shape.indices.size() % 3 ||
				!std::ranges::all_of(shape.indices, [&](uint32_t vertex) { return vertex < shape.positions.size(); }))
				return false;
			const uint64_t size = uint64_t(shape.positions.size()) * sizeof(float3) + uint64_t(shape.indices.size()) * sizeof(uint32_t);
			if (size > kMaximumTileBytes - bytes)
				return false;
			bytes += size;
		}
		auto tile = std::make_shared<Tile>(key, std::move(shapes), bytes);
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		next->resident[std::distance(current->requested.begin(), index)] = std::move(tile);
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	void ResourceGrid::Clear()
	{
		std::scoped_lock lock(mutex);
		pending.reset();
		auto next = std::make_shared<Grid>();
		next->revision = snapshot.load(std::memory_order_acquire)->revision + 1;
		snapshot.store(std::move(next), std::memory_order_release);
	}
}

bool DX12Resources::IsActive() const
{
	return loaded && settings.Enabled && globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

void DX12Resources::DrawSettings()
{
	ImGui::Checkbox("Enable shape preparation", &settings.Enabled);
	int index = int(std::ranges::find(DX12::kSubcellSizes, settings.SubcellSize) - DX12::kSubcellSizes.begin());
	if (index >= int(DX12::kSubcellSizes.size()))
		index = 0;
	constexpr const char* labels[] = { "4096", "6144", "8192", "10240", "12288", "14336", "16384" };
	if (ImGui::Combo("Subcell size (game units)", &index, labels, int(std::size(labels)))) {
		settings.SubcellSize = DX12::kSubcellSizes[index];
		grid.Clear();
	}
	ImGui::TextDisabled("Only the current 3x3 grid is kept in memory. No geometry is written to disk.");
}

void DX12Resources::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", true);
	const uint32_t size = input.value("SubcellSize", DX12::kSubcellSizes.front());
	settings.SubcellSize = std::ranges::contains(DX12::kSubcellSizes, size) ? size : DX12::kSubcellSizes.front();
	grid.Clear();
}

void DX12Resources::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "SubcellSize", settings.SubcellSize } };
}

void DX12Resources::RestoreDefaultSettings()
{
	settings = {};
	grid.Clear();
}

void DX12Resources::EarlyPrepass()
{
	CS_GPU_PASS("DX12Resources::Grid");
	if (!IsActive()) {
		if (grid.Snapshot()->requested.front().scope)
			grid.Clear();
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
}

void DX12Resources::OnSceneTransitionReset(bool opening)
{
	if (opening)
		grid.Clear();
}

void DX12Resources::OnRuntimeDisabled()
{
	grid.Clear();
}
