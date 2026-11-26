#pragma once

#include <cstddef>
#include <cstdint>

namespace DX12
{
	/** @brief Determines whether a traversal exceeded its scene-graph depth limit. */
	[[nodiscard]] constexpr bool IsSceneGraphDepthExceeded(uint32_t a_depth, uint32_t a_maxDepth) noexcept
	{
		return a_depth > a_maxDepth;
	}

	/** @brief Reports complete static-scene coverage only when every eligible shape was captured. */
	[[nodiscard]] constexpr bool IsScenePrepCoverageComplete(
		bool a_validated, bool a_enumerationComplete, uint32_t a_failedShapes, uint32_t a_rejectedLights,
		size_t a_eligibleShapes, size_t a_capturedShapes) noexcept
	{
		return a_validated && a_enumerationComplete && a_failedShapes == 0 && a_rejectedLights == 0 &&
		       a_eligibleShapes == a_capturedShapes;
	}
}
