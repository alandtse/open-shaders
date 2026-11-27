#pragma once

#include "DX12Resources.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace DX12Native
{
	/** @brief Scene inputs retained independently of later BLAS/TLAS GPU lifetimes. */
	struct SceneInputs
	{
		std::shared_ptr<const DX12::Grid> residency;
		std::vector<std::shared_ptr<const DX12::Shape>> geometry;
		std::unordered_multimap<uintptr_t, size_t> casterSources;
		uint64_t geometryRevision = 0;
		size_t preparedShapes = 0, observedShapes = 0, duplicateShapes = 0;
		uint64_t triangles = 0;
	};

	/** @brief Collects geometry from the active 3x3 and runtime rigid supplements. */
	std::shared_ptr<const SceneInputs> CollectSceneInputs(std::shared_ptr<const DX12::Grid> a_grid);
}
