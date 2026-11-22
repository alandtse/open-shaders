#pragma once

#include "DX12Resources.h"

#include <memory>
#include <vector>

namespace DX12Native
{
	/** @brief Immutable rigid geometry source, already placed in a runtime subcell. */
	struct GeometryInput
	{
		DX12::SubcellKey subcell{};
		std::shared_ptr<const DX12::Shape> shape;
		bool supplementary = false;
	};

	/** @brief Scene inputs retained independently of later BLAS/TLAS GPU lifetimes. */
	struct SceneInputs
	{
		std::shared_ptr<const DX12::Grid> residency;
		std::vector<GeometryInput> geometry;
		std::vector<DX12::PreparedLight> lights;
		uint64_t geometryRevision = 0;
		uint64_t lightRevision = 0;
	};

	/** @brief Collects the active 3x3's rigid shapes, missing runtime shapes and all prepared lights. */
	std::shared_ptr<const SceneInputs> CollectSceneInputs(std::shared_ptr<const DX12::Grid> a_grid);
}
