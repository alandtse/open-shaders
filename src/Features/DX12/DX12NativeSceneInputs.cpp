#include "DX12NativeSceneInputs.h"
#include "DX12ShapeEquivalence.h"

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

		// Native preparation takes precedence over equivalent observed rigid geometry.
		std::unordered_set<uint64_t> seenShapes;
		const auto appendShape = [&](const std::shared_ptr<const DX12::Shape>& shape) {
			if (!shape)
				return;
			if (shape->identity && !seenShapes.insert(shape->identity).second) {
				++inputs->duplicateShapes;
				return;
			}
			inputs->triangles += shape->indices.size() / 3;
			inputs->geometry.push_back(shape);
		};
		for (const auto& tile : grid.resident) {
			if (!tile)
				continue;
			for (const auto& shape : tile->shapes) {
				if (shape)
					++inputs->preparedShapes;
				appendShape(shape);
			}
		}
		const size_t preparedCount = inputs->geometry.size();
		for (const auto& observed : grid.runtimeShapes) {
			if (!observed.shape)
				continue;
			++inputs->observedShapes;
			const auto prepared = std::span(inputs->geometry).first(preparedCount);
			if (std::ranges::any_of(prepared, [&](const auto& shape) {
					return DX12::EquivalentGeometry(*shape, *observed.shape);
				})) {
				++inputs->duplicateShapes;
				continue;
			}
			appendShape(observed.shape);
		}

		return inputs;
	}
}
