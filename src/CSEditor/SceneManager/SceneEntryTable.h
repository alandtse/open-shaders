#pragma once

#include "SceneSettingsManager.h"
#include "Utils/UI.h"

namespace SceneEntryTable
{
	/** @brief Shared presentation for scene table and feature-setting action flyouts. */
	Util::FlyoutStyle GetActionFlyoutStyle();
	/** @brief Returns the shared scaled scene-action button height. */
	float GetActionButtonSize();
	/** @brief Returns the shared scaled scene-action toggle size. */
	ImVec2 GetActionToggleSize();
	/** @brief Draws the source and user entry tables for one scene, including its period columns. */
	void Draw(const SceneSettingsManager::SceneContextId& context);
	/** @brief Whether a standalone add or confirmation dialog owns its scene target. */
	bool HasActiveDialog();
}
