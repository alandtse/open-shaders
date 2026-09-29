#include "Utils/RegionOverlay.h"

#include <algorithm>

namespace Util::RegionOverlay
{
	ScreenRect MapToScreen(const Subrect::PixelRegion& a_rect, uint32_t a_sourceWidth, uint32_t a_sourceHeight,
		const ImVec2& a_imageMin, const ImVec2& a_imageSize)
	{
		ScreenRect result;
		if (!a_rect.w || !a_rect.h || !a_sourceWidth || !a_sourceHeight ||
			a_imageSize.x <= 0.0f || a_imageSize.y <= 0.0f)
			return result;
		const float inverseWidth = 1.0f / static_cast<float>(a_sourceWidth);
		const float inverseHeight = 1.0f / static_cast<float>(a_sourceHeight);
		// Summed as floats: a uint32 x + w would wrap for a rect declared past the frame, which is
		// exactly the input the clamp below exists to absorb.
		const float left = std::clamp(static_cast<float>(a_rect.x) * inverseWidth, 0.0f, 1.0f);
		const float top = std::clamp(static_cast<float>(a_rect.y) * inverseHeight, 0.0f, 1.0f);
		const float right = std::clamp((static_cast<float>(a_rect.x) + static_cast<float>(a_rect.w)) * inverseWidth, 0.0f, 1.0f);
		const float bottom = std::clamp((static_cast<float>(a_rect.y) + static_cast<float>(a_rect.h)) * inverseHeight, 0.0f, 1.0f);
		if (right <= left || bottom <= top)
			return result;
		result.min = ImVec2(a_imageMin.x + left * a_imageSize.x, a_imageMin.y + top * a_imageSize.y);
		result.max = ImVec2(a_imageMin.x + right * a_imageSize.x, a_imageMin.y + bottom * a_imageSize.y);
		result.valid = true;
		return result;
	}

	void Draw(const ImVec2& a_imageMin, const ImVec2& a_imageSize, uint32_t a_sourceWidth, uint32_t a_sourceHeight,
		std::span<const Region> a_regions)
	{
		auto* drawList = ImGui::GetWindowDrawList();
		for (const auto& region : a_regions) {
			const auto screen = MapToScreen(region.rect, a_sourceWidth, a_sourceHeight, a_imageMin, a_imageSize);
			if (!screen.valid)
				continue;
			drawList->AddRect(screen.min, screen.max, region.color, 0.0f, 0, kOutlineThicknessPixels);
			if (region.label)
				drawList->AddText(screen.min, region.color, region.label);
		}
	}
}
