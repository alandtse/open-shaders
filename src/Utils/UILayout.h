#pragma once

#include "Menu/ThemeManager.h"

#include <imgui.h>

namespace Util
{
	// Baseline font size for UI layout scaling (1080p dynamic font: DEFAULT_SCREEN_HEIGHT * DEFAULT_FONT_RATIO).
	// Theme style values and pixel constants are designed for this size.
	constexpr float kBaselineFontSize = ThemeManager::Constants::DEFAULT_SCREEN_HEIGHT * ThemeManager::Constants::DEFAULT_FONT_RATIO;

	inline float GetUIScaleForBaseline(float baselineFontSize) { return ImGui::GetFontSize() / baselineFontSize; }

	/** @brief Returns a scale factor relative to the 1080p baseline font size. */
	inline float GetUIScale() { return GetUIScaleForBaseline(kBaselineFontSize); }

	/** @brief Leaves text clear of a pill button's rounded ends. */
	inline float GetPillPaddingX(float height) { return std::max(ImGui::GetStyle().FramePadding.x, height * 0.5f); }

	/** @brief Returns a scale factor for search controls authored against the 2K baseline. */
	inline float GetSearchUIScale() { return GetUIScaleForBaseline(ThemeManager::Constants::SEARCH_BASELINE_SCREEN_HEIGHT * ThemeManager::Constants::DEFAULT_FONT_RATIO); }

	/** @brief Keeps the next item on the current row when its measured width fits. */
	inline void SameLineIfFits(float width)
	{
		ImGui::SameLine();
		if (ImGui::GetContentRegionAvail().x < width)
			ImGui::NewLine();
	}
}
