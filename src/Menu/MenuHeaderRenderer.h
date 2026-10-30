#pragma once

#include "Menu.h"

/** @brief Compact main-menu actions, build title, and Home navigation. */
class MenuHeaderRenderer
{
public:
	/** @brief Brand and release or test-build version without branch metadata. */
	static std::string GetTitle();
	/** @brief Draws the same responsive action row in floating and docked menus. */
	static void RenderHeader(Menu& menu, bool& sidebarVisible, float uiScale);
};
