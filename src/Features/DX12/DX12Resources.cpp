#include "DX12Resources.h"

#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/B/BSTriShape.h"
#include "RE/T/TESObjectLIGH.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/T/TESWorldSpace.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <ranges>
#include <unordered_set>

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
		next->geometryRevision = current->geometryRevision + 1;
		next->lightRevision = current->lightRevision;
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
		std::erase_if(observed, [&](const auto& entry) {
			return std::ranges::find(next->requested, entry.second) == next->requested.end();
		});
		snapshot.store(std::move(next), std::memory_order_release);
	}

	std::optional<SubcellKey> ResourceGrid::NextRequest()
	{
		std::scoped_lock lock(mutex);
		if (pending)
			return std::nullopt;
		const auto current = snapshot.load(std::memory_order_acquire);
		for (size_t i = 0; i < kGridCapacity; ++i)
			if (current->requested[i].scope && (!current->resident[i] || !current->resident[i]->prepared)) {
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
		const size_t slot = std::distance(current->requested.begin(), index);
		const auto& existing = current->resident[slot];
		constexpr uint64_t kMaximumTileBytes = 64ull * 1024 * 1024;
		uint64_t bytes = existing ? existing->bytes : 0;
		std::unordered_set<uint64_t> identities;
		if (existing)
			for (const auto& shape : existing->shapes)
				identities.insert(shape->identity);
		for (const auto& shape : shapes) {
			if (shape.positions.empty() || shape.indices.empty() || shape.indices.size() % 3 ||
				!std::ranges::all_of(shape.indices, [&](uint32_t vertex) { return vertex < shape.positions.size(); }))
				return false;
			const uint64_t size = uint64_t(shape.positions.size()) * sizeof(float3) + uint64_t(shape.indices.size()) * sizeof(uint32_t);
			if (size > kMaximumTileBytes - bytes)
				return false;
			if (!shape.identity || !identities.contains(shape.identity))
				bytes += size;
			identities.insert(shape.identity);
		}
		auto tile = existing ? std::make_shared<Tile>(*existing) : std::make_shared<Tile>();
		tile->key = key;
		tile->bytes = bytes;
		tile->prepared = true;
		tile->shapes.reserve(tile->shapes.size() + shapes.size());
		for (auto& shape : shapes) {
			if (shape.identity && existing && std::ranges::any_of(existing->shapes, [&](const auto& old) { return old->identity == shape.identity; }))
				continue;
			tile->shapes.push_back(std::make_shared<const Shape>(std::move(shape)));
		}
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		++next->geometryRevision;
		next->resident[slot] = std::move(tile);
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	bool ResourceGrid::Append(const SubcellKey& key, Shape shape)
	{
		if (!shape.identity || shape.positions.empty() || shape.indices.empty() || shape.indices.size() % 3 ||
			!std::ranges::all_of(shape.indices, [&](uint32_t index) { return index < shape.positions.size(); }) ||
			!std::ranges::all_of(shape.positions, [](const float3& position) {
				return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
			}))
			return false;
		constexpr uint64_t kMaximumTileBytes = 64ull * 1024 * 1024;
		const uint64_t bytes = uint64_t(shape.positions.size()) * sizeof(float3) + uint64_t(shape.indices.size()) * sizeof(uint32_t);
		std::scoped_lock lock(mutex);
		if (observed.contains(shape.identity))
			return false;
		const auto current = snapshot.load(std::memory_order_acquire);
		const auto index = std::ranges::find(current->requested, key);
		if (index == current->requested.end())
			return false;
		const size_t slot = std::distance(current->requested.begin(), index);
		const auto& previous = current->resident[slot];
		if (bytes > kMaximumTileBytes || (previous && bytes > kMaximumTileBytes - previous->bytes) ||
			(previous && previous->shapes.size() >= 2048))
			return false;
		auto tile = previous ? std::make_shared<Tile>(*previous) : std::make_shared<Tile>();
		tile->key = key;
		tile->shapes.push_back(std::make_shared<const Shape>(std::move(shape)));
		tile->bytes += bytes;
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		++next->geometryRevision;
		next->resident[slot] = std::move(tile);
		observed.emplace(next->resident[slot]->shapes.back()->identity, key);
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	void ResourceGrid::PublishLights(std::span<const PreparedLight> lights)
	{
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		if (!current->requested.front().scope)
			return;
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		++next->lightRevision;
		for (size_t i = 0; i < kGridCapacity; ++i) {
			const auto& key = current->requested[i];
			auto tile = current->resident[i] ? std::make_shared<Tile>(*current->resident[i]) : std::make_shared<Tile>();
			tile->key = key;
			tile->lights.clear();
			for (const auto& light : lights) {
				const auto x = Coordinate(light.position.x, current->size);
				const auto y = Coordinate(light.position.y, current->size);
				if (x && y && *x == key.x && *y == key.y)
					tile->lights.push_back(light);
			}
			next->resident[i] = std::move(tile);
		}
		snapshot.store(std::move(next), std::memory_order_release);
	}

	void ResourceGrid::Clear()
	{
		std::scoped_lock lock(mutex);
		pending.reset();
		observed.clear();
		auto next = std::make_shared<Grid>();
		next->revision = snapshot.load(std::memory_order_acquire)->revision + 1;
		next->geometryRevision = next->revision;
		next->lightRevision = next->revision;
		snapshot.store(std::move(next), std::memory_order_release);
	}
}

bool DX12Resources::IsActive() const
{
	return loaded && settings.Enabled && globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

void DX12Resources::DrawSettings()
{
	if (ImGui::Checkbox("Enable geometry and light preparation", &settings.Enabled) && !settings.Enabled) {
		grid.Clear();
		lastLightGrid = {};
	}
	int index = int(std::ranges::find(DX12::kSubcellSizes, settings.SubcellSize) - DX12::kSubcellSizes.begin());
	if (index >= int(DX12::kSubcellSizes.size()))
		index = 0;
	constexpr const char* labels[] = { "4096", "6144", "8192", "10240", "12288", "14336", "16384" };
	if (ImGui::Combo("Subcell size (game units)", &index, labels, int(std::size(labels)))) {
		settings.SubcellSize = DX12::kSubcellSizes[index];
		grid.Clear();
		lastLightGrid = {};
	}
	ImGui::TextDisabled("The live 3x3 grid holds only geometry and minimal light records. No disk cache.");
}

void DX12Resources::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", true);
	const uint32_t size = input.value("SubcellSize", DX12::kSubcellSizes.front());
	settings.SubcellSize = std::ranges::contains(DX12::kSubcellSizes, size) ? size : DX12::kSubcellSizes.front();
	grid.Clear();
	lastLightGrid = {};
}

void DX12Resources::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "SubcellSize", settings.SubcellSize } };
}

void DX12Resources::RestoreDefaultSettings()
{
	settings = {};
	grid.Clear();
	lastLightGrid = {};
}

void DX12Resources::EarlyPrepass()
{
	CS_GPU_PASS("DX12Resources::Grid");
	if (!IsActive()) {
		if (grid.Snapshot()->requested.front().scope) {
			grid.Clear();
			lastLightGrid = {};
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
	RefreshPreparedLights(globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0);
}

void DX12Resources::OnSceneTransitionReset(bool opening)
{
	if (opening) {
		grid.Clear();
		lastLightGrid = {};
	}
}

void DX12Resources::OnRuntimeDisabled()
{
	grid.Clear();
	lastLightGrid = {};
}

void DX12Resources::RefreshPreparedLights(uint32_t frame)
{
	const auto current = grid.Snapshot();
	if (!current->requested.front().scope ||
		(current->requested == lastLightGrid && uint32_t(frame - lightScanFrame) < 300))
		return;
	lastLightGrid = current->requested;
	lightScanFrame = frame;
	std::vector<DX12::PreparedLight> lights;
	lights.reserve(64);
	const auto* player = globals::game::player;
	if (auto* tes = globals::game::tes; tes && player) {
		tes->ForEachReferenceInRange(player, float(current->size) * 3.0f, [&](RE::TESObjectREFR* reference) {
			if (!reference || reference->IsDisabled() || reference->IsDeleted() || reference->IsDynamicForm())
				return RE::BSContainer::ForEachResult::kContinue;
			const auto* cell = reference->GetParentCell();
			if (!cell || (!cell->IsExteriorCell() && cell->GetFormID() != current->requested.front().scope))
				return RE::BSContainer::ForEachResult::kContinue;
			const auto* base = reference->GetBaseObject();
			const auto* light = base ? base->As<RE::TESObjectLIGH>() : nullptr;
			if (!light || !light->data.flags.any(RE::TES_LIGHT_FLAGS::kHemiShadow,
							  RE::TES_LIGHT_FLAGS::kOmniShadow, RE::TES_LIGHT_FLAGS::kSpotShadow))
				return RE::BSContainer::ForEachResult::kContinue;
			const auto pos = reference->GetPosition();
			const auto x = DX12::Coordinate(pos.x, current->size);
			const auto y = DX12::Coordinate(pos.y, current->size);
			if (!x || !y || std::ranges::find(current->requested, DX12::SubcellKey{ current->requested.front().scope, *x, *y }) == current->requested.end())
				return RE::BSContainer::ForEachResult::kContinue;
			if (lights.size() >= 256)
				return RE::BSContainer::ForEachResult::kStop;
			DX12::PreparedLight entry{};
			entry.referenceID = reference->GetFormID();
			entry.baseID = light->GetFormID();
			entry.flags = light->data.flags.underlying();
			entry.position = { pos.x, pos.y, pos.z };
			entry.color = { light->data.color.red / 255.0f, light->data.color.green / 255.0f, light->data.color.blue / 255.0f };
			entry.radius = std::max(0.0f, float(light->data.radius));
			entry.fade = std::max(0.0f, light->fade);
			entry.emitterRadius = light->data.fov;
			entry.falloffExponent = light->data.fallofExponent;
			if (std::isfinite(entry.radius) && entry.radius > 0 && std::isfinite(entry.fade) &&
				std::isfinite(entry.emitterRadius) &&
				std::isfinite(entry.falloffExponent))
				lights.push_back(entry);
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}
	std::ranges::sort(lights, {}, &DX12::PreparedLight::referenceID);
	grid.PublishLights(lights);
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

void DX12Resources::ObserveStaticGeometry(RE::BSGeometry& geometry)
{
	if (!IsActive() || geometry.name.empty())
		return;
	const auto* reference = geometry.GetUserData();
	if (!reference || reference->IsDynamicForm())
		return;
	const auto& runtime = geometry.GetGeometryRuntimeData();
	const auto* renderer = runtime.rendererData;
	if (runtime.skinInstance || runtime.alphaProperty || !renderer || !renderer->rawVertexData ||
		!renderer->rawIndexData || !runtime.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	const auto* tri = geometry.AsTriShape();
	if (!tri || geometry.AsDynamicTriShape())
		return;
	const auto& counts = tri->GetTrishapeRuntimeData();
	const uint32_t stride = runtime.vertexDesc.GetSize();
	if (!counts.vertexCount || !counts.triangleCount || stride < 16 || stride > 256 ||
		counts.vertexCount > 65535 || counts.triangleCount > 65535)
		return;
	const uint64_t length = uint64_t(counts.vertexCount) * stride + uint64_t(counts.triangleCount) * 3 * sizeof(uint16_t);
	if (length > 256 * 1024)
		return;
	const uint32_t frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0;
	if (frame != copyFrame) {
		copyFrame = frame;
		copiedFrameBytes = 0;
	}
	if (length > 512 * 1024 - copiedFrameBytes)
		return;
	const auto current = grid.Snapshot();
	const auto x = DX12::Coordinate(geometry.worldBound.center.x, current->size);
	const auto y = DX12::Coordinate(geometry.worldBound.center.y, current->size);
	if (!x || !y || !current->requested[0].scope)
		return;
	const DX12::SubcellKey key{ current->requested[0].scope, *x, *y };
	if (std::ranges::find(current->requested, key) == current->requested.end())
		return;
	DX12::Shape shape;
	shape.positions.reserve(counts.vertexCount);
	shape.indices.reserve(size_t(counts.triangleCount) * 3);
	uint64_t identity = 14695981039346656037ull;
	for (uint32_t shift = 0; shift < 32; shift += 8)
		identity = (identity ^ ((reference->GetFormID() >> shift) & 0xffu)) * 1099511628211ull;
	for (const unsigned char c : std::string_view(geometry.name.c_str()))
		identity = (identity ^ c) * 1099511628211ull;
	for (const float component : { geometry.worldBound.center.x, geometry.worldBound.center.y, geometry.worldBound.center.z }) {
		const uint32_t bits = std::bit_cast<uint32_t>(component);
		for (uint32_t shift = 0; shift < 32; shift += 8)
			identity = (identity ^ ((bits >> shift) & 0xffu)) * 1099511628211ull;
	}
	shape.identity = identity ? identity : 1;
	for (uint32_t i = 0; i < counts.vertexCount; ++i) {
		float3 position{};
		std::memcpy(&position, renderer->rawVertexData + size_t(i) * stride, sizeof(position));
		shape.positions.push_back(position);
	}
	for (size_t i = 0; i < size_t(counts.triangleCount) * 3; ++i)
		shape.indices.push_back(renderer->rawIndexData[i]);
	for (uint32_t row = 0; row < 3; ++row) {
		for (uint32_t col = 0; col < 3; ++col)
			shape.transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	}
	shape.transform[3] = geometry.world.translate.x;
	shape.transform[7] = geometry.world.translate.y;
	shape.transform[11] = geometry.world.translate.z;
	if (grid.Append(key, std::move(shape)))
		copiedFrameBytes += uint32_t(length);
}
