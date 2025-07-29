#pragma once

#include <imgui.h>

class ThemeManager
{
public:
	static void SetupImGuiStyle(const class Menu& menu);
	static void ReloadFont(const class Menu& menu, float& cachedFontSize);

	struct Constants
	{
		static constexpr float MIN_FONT_SIZE = 8.0f;
		static constexpr float MAX_FONT_SIZE = 32.0f;
		static constexpr int FCONF_OVERSAMPLE_H = 3;
		static constexpr int FCONF_OVERSAMPLE_V = 1;
		static constexpr bool FCONF_PIXELSNAP_H = true;
		static constexpr float FCONF_RASTERIZER_MULTIPLY = 1.1f;
	};
};