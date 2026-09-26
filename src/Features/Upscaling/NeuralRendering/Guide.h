#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace NR
{
	/** @brief Active texel region of a guide resource supplied to Feature 18. */
	struct GuideRegion
	{
		uint32_t baseX = 0, baseY = 0, width = 0, height = 0;
	};

	/** @brief Independent guide regions and motion conversion to NR input pixels. */
	struct GuideParameters
	{
		GuideRegion depth, motion;
		float motionScaleX = 1.0f, motionScaleY = 1.0f;
	};

	/**
	 * @brief Clamps a region to a resource extent; nullopt when it covers no texel.
	 *        Callers must skip the frame: an empty region is not a valid Feature 18 subrect.
	 */
	[[nodiscard]] constexpr std::optional<GuideRegion> ClampGuideRegion(uint32_t a_extentWidth, uint32_t a_extentHeight, const GuideRegion& a_region)
	{
		GuideRegion region;
		region.baseX = std::min(a_region.baseX, a_extentWidth);
		region.baseY = std::min(a_region.baseY, a_extentHeight);
		region.width = std::min(a_region.width, a_extentWidth - region.baseX);
		region.height = std::min(a_region.height, a_extentHeight - region.baseY);
		if (!region.width || !region.height)
			return std::nullopt;
		return region;
	}
}
