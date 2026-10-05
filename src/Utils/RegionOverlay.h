#pragma once

#include "Utils/Subrect.h"

#include <cstdint>
#include <imgui.h>
#include <span>

namespace Util::RegionOverlay
{
	/** @brief Outline width drawn over a preview image, in screen pixels. */
	inline constexpr float kOutlineThicknessPixels = 2.0f;

	/** @brief One read-only rectangle drawn over a preview image, in the previewed frame's pixels. */
	struct Region
	{
		Subrect::PixelRegion rect;
		ImU32 color = IM_COL32(0, 255, 0, 255);
		const char* label = nullptr;
	};

	/** @brief A source pixel rect mapped onto the drawn image; valid is false when it cannot be drawn. */
	struct ScreenRect
	{
		ImVec2 min{}, max{};
		bool valid = false;
	};

	/**
	 * @brief Maps a source-frame pixel rect onto the on-screen image rect.
	 *        The result is clamped to the image, so a rect the source frame only partly covers still
	 *        draws inside it rather than off the edge.
	 * @param a_sourceWidth,a_sourceHeight Size of the frame a_rect is measured in.
	 * @param a_imageMin,a_imageSize Position and size of the drawn preview in screen pixels.
	 * @return valid == false for an empty rect, a zero-sized source frame or a zero-sized image, and
	 *         for a rect that falls entirely outside the source frame.
	 */
	ScreenRect MapToScreen(const Subrect::PixelRegion& a_rect, uint32_t a_sourceWidth, uint32_t a_sourceHeight,
		const ImVec2& a_imageMin, const ImVec2& a_imageSize);

	/**
	 * @brief Draws read-only outlines, and optional labels, for regions over an already-drawn image.
	 * @param a_imageMin,a_imageSize Position and size of the drawn preview (ImGui::GetCursorScreenPos
	 *        captured before the image is drawn).
	 * @param a_sourceWidth,a_sourceHeight Size of the frame the regions' pixel rects are measured in.
	 */
	void Draw(const ImVec2& a_imageMin, const ImVec2& a_imageSize, uint32_t a_sourceWidth, uint32_t a_sourceHeight,
		std::span<const Region> a_regions);
}
