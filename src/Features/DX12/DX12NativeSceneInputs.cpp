#include "DX12NativeSceneInputs.h"

#include <unordered_set>
#include <utility>

namespace DX12Native
{
	std::shared_ptr<const SceneInputs> CollectSceneInputs(std::shared_ptr<const DX12::Grid> a_grid)
	{
		auto inputs = std::make_shared<SceneInputs>();
		inputs->residency = std::move(a_grid);
		if (!inputs->residency)
			return inputs;

		const auto& grid = *inputs->residency;
		inputs->geometryRevision = grid.geometryRevision;
		inputs->lightRevision = grid.lightRevision;

		// A placed shape may be discovered by both companion and runtime observation.
		// Keep the companion version; do not require a tile to certify full coverage.
		std::unordered_set<uint64_t> seenShapes;
		const auto appendShape = [&](DX12::SubcellKey subcell, const std::shared_ptr<const DX12::Shape>& shape, bool supplementary) {
			if (!shape)
				return;
			if (shape->identity && !seenShapes.insert(shape->identity).second)
				return;
			inputs->geometry.push_back({ subcell, shape, supplementary });
		};
		for (const auto& tile : grid.resident) {
			if (!tile)
				continue;
			for (const auto& shape : tile->shapes)
				appendShape(tile->key, shape, false);
		}
		for (const auto& observed : grid.runtimeShapes)
			appendShape(observed.tile, observed.shape, true);

		// Light enumeration is independent of camera visibility and geometry
		// residency. The predictive ring contributes lights, not acceleration geometry.
		std::unordered_set<uint32_t> seenLights;
		const auto appendLights = [&](const auto& tiles) {
			for (const auto& tile : tiles) {
				if (!tile)
					continue;
				for (const auto& light : tile->lights) {
					if (light.referenceID && !seenLights.insert(light.referenceID).second)
						continue;
					inputs->lights.push_back(light);
				}
			}
		};
		appendLights(grid.resident);
		appendLights(grid.predictiveResident);
		return inputs;
	}
}
