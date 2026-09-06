#include "SceneManager.h"

#include "SceneManagerUI.h"
#include "SceneSettingsManager.h"
#include "SceneSettingsUIHooks.h"

std::pair<std::string, std::vector<std::string>> SceneManager::GetFeatureSummary()
{
	return {
		T("feature.scene_manager.description",
			"Applies selected Open Shaders settings by interior, time of day, weather, and location."),
		{
			T("feature.scene_manager.key_feature_1", "Blends exterior settings across time of day and weather transitions"),
			T("feature.scene_manager.key_feature_2", "Applies interior settings separately from exterior settings"),
			T("feature.scene_manager.key_feature_3", "Supports region, location, and cell overrides with per-setting precedence"),
		},
	};
}

void SceneManager::DrawSettings()
{
	SceneManagerUI::Draw();
}

void SceneManager::SetupResources()
{
	LoadAll();
}

void SceneManager::PostPostLoad()
{
	SceneSettingsUIHooks::Install();
}

void SceneManager::DataLoaded()
{
	SceneSettingsManager::OnDataLoaded();
	MenuOpenCloseEventHandler::Register();
}

void SceneManager::Update()
{
	SceneSettingsManager::Update();
}
