#pragma once

#include "DX12Resources.h"

#include <memory>
#include <vector>

namespace DX12Native
{
	/** @brief Scene inputs retained independently of later BLAS/TLAS GPU lifetimes. */
	struct SceneInputs
	{
		std::shared_ptr<const DX12::Grid> residency;
		std::vector<std::shared_ptr<const DX12::Shape>> geometry;
		uint64_t geometryRevision = 0;
	};

	/** @brief Collects geometry from the active 3x3 and runtime rigid supplements. */
	std::shared_ptr<const SceneInputs> CollectSceneInputs(std::shared_ptr<const DX12::Grid> a_grid);
}
