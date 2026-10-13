#pragma once

#include "Tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace NR
{
	/** @brief A model scale at or above this runs the model at the eye size. */
	inline constexpr float kFullModelScale = 0.999f;

	/** @brief The model size is a whole number of these pixels, matching the 8-pixel groups of the downsample and expand kernels. */
	inline constexpr uint32_t kModelExtentAlignment = 8;

	/** @brief Smallest side the model is created at. */
	inline constexpr uint32_t kMinModelExtent = 64;

	/**
	 * @brief Size the model runs at for a model scale.
	 * @return Zero in both fields when it runs at the eye size: a full or non-finite scale, or a size that would not
	 *         be smaller than the eye in both directions.
	 */
	inline std::pair<uint32_t, uint32_t> ModelExtent(float scale, uint32_t width, uint32_t height)
	{
		if (!std::isfinite(scale) || scale >= kFullModelScale)
			return { 0, 0 };
		const float bounded = std::max(scale, Tuning::kMinModelScale);
		const auto reduce = [bounded](uint32_t full) {
			const auto aligned = static_cast<uint32_t>(std::lround(full * bounded / kModelExtentAlignment)) * kModelExtentAlignment;
			return std::max(kMinModelExtent, aligned);
		};
		const uint32_t reducedWidth = reduce(width), reducedHeight = reduce(height);
		return reducedWidth < width && reducedHeight < height ? std::pair{ reducedWidth, reducedHeight } : std::pair<uint32_t, uint32_t>{ 0, 0 };
	}
}
