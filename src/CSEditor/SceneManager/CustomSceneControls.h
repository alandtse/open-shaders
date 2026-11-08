#pragma once

#include "SceneSettingsManager.h"

#include <filesystem>
#include <functional>
#include <string>

namespace CustomSceneControls
{
	/** @brief Draws validated path controls for an armed scene context. */
	bool DrawPathSceneControls(const SceneSettingsManager::SettingIdentity& setting, std::string& path, std::string& error,
		const std::function<std::string(const std::filesystem::path&)>& validate);
	/** @brief Records a custom control's edited base value. */
	void RecordBaselineEdit(const SceneSettingsManager::SettingIdentity& setting, const json& value);
}
