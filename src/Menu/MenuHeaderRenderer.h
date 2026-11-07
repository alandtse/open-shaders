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
	/** @brief Draws menu actions and navigation around the editor's browser mode controls. */
	static void RenderEditorHeader(Menu& menu, bool& sidebarVisible, std::string_view selectedPage,
		const std::function<void()>& drawModes);

private:
	static void RenderActions(Menu& menu, bool& sidebarVisible, bool showBackground);
	static void RenderNavigation(Menu& menu, std::string_view selectedPage);
	static void DrawRestoreDefaultsConfirmation();
};
