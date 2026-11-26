#pragma once

#include "SpatialGrid.h"

#include <cmath>
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

	/**
	 * @brief Reports whether every subcell touched by a light's XY radius is certified.
	 * @param a_certified Callable (int64_t tileX, int64_t tileY) -> bool for one subcell.
	 */
	template <class Certified>
	[[nodiscard]] bool IsLightRadiusCertified(
		double a_x, double a_y, double a_radius, uint32_t a_size, Certified&& a_certified)
	{
		if (!std::isfinite(a_radius) || a_radius < 0.0)
			return false;
		const auto minX = Coordinate(a_x - a_radius, a_size), maxX = Coordinate(a_x + a_radius, a_size);
		const auto minY = Coordinate(a_y - a_radius, a_size), maxY = Coordinate(a_y + a_radius, a_size);
		if (!minX || !maxX || !minY || !maxY || *maxX - *minX >= kGridDiameter || *maxY - *minY >= kGridDiameter)
			return false;
		for (int64_t y = *minY; y <= *maxY; ++y)
			for (int64_t x = *minX; x <= *maxX; ++x)
				if (!a_certified(x, y))
					return false;
		return true;
	}
}
