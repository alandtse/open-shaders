#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace NativePBR
{
	/** @brief Sizes a snow-detail cell grid from scheduled native ray samples, never world distance. */
	inline float SnowRayCellFrequency(uint32_t a_width, uint32_t a_height, uint32_t a_densityMode,
		float a_raysPerPatch)
	{
		constexpr std::array fractions{ 1.0f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f,
			0.25f, 1.0f / 6.0f, 1.0f / 9.0f, 4.0f / 9.0f };
		if (!a_width || !a_height)
			return 1.0f;
		const float raysPerPatch = std::isfinite(a_raysPerPatch) ?
			std::clamp(a_raysPerPatch, 1.0f, 256.0f) : 64.0f;
		const float fraction = a_densityMode < fractions.size() ? fractions[a_densityMode] : 1.0f;
		return std::clamp(std::sqrt(float(a_width) * float(a_height) * fraction) / raysPerPatch,
			1.0f, 512.0f);
	}
}
