#include "CustomSceneControls.h"

#include <imgui.h>

#include "../../I18n/I18n.h"
#include "Feature.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneSettingsContextRules.h"
#include "SceneSettingsManager.h"
#include "SceneWidgetInterceptor.h"
#include "Utils/UI.h"

namespace
{
	/** @brief Applies path controls to one scene context instead of its base settings. */
	void DrawPathContextControls(const SceneSettingsManager::SettingIdentity& setting, std::string& path, std::string& error,
		const std::function<std::string(const std::filesystem::path&)>& validate, const SceneSettingsManager::SceneContextId& context)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto rules = SceneSettingsContextRules::GetSceneContextRules(context);
		const bool allowed = SceneSettingsManager::IsSettingAllowedForType(rules.sceneType, setting.featureShortName,
			setting.settingPath, setting.settingKey, rules.requireNumeric);
		const auto findEntry = [&] { return manager->FindContextUserEntry(context, setting.featureShortName, setting.settingPath, setting.settingKey); };
		const auto isLive = [&](std::optional<size_t> index) {
			const auto entries = manager->GetContextEntries(context);
			return index && *index < entries.size() && !entries[*index].deleted;
		};

		auto entryIndex = findEntry();

		ImGui::BeginGroup();
		ImGui::BeginDisabled(!allowed);
		if (Util::Button(T("feature.scene_manager.path.load", "Load"))) {
			error = validate(UnifiedPresetCatalog::GetSingleton().ResolveActivePackPath(path));
			if (error.empty()) {
				// A tombstoned entry holds no value; clear it so Load's write lands on a live entry.
				if (entryIndex && !isLive(entryIndex)) {
					manager->ClearContextTombstone(context, setting.featureShortName, setting.settingPath, setting.settingKey);
					entryIndex = std::nullopt;
				}
				const auto index = entryIndex ? entryIndex : manager->AddContextSetting(context, setting.featureShortName, setting.settingPath, setting.settingKey, true);
				if (index) {
					const SceneSettingsManager::EntryValueUpdate update{ *index, path };
					manager->UpdateContextEntryValues(context, { &update, 1 });
				}
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!isLive(entryIndex));
		if (Util::Button(T("feature.scene_manager.path.clear", "Clear")))
			manager->RemoveContextSetting(context, *entryIndex);
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::EndGroup();
		if (!allowed)
			Util::AddTooltip(T("feature.scene_manager.path.scene_scope_tooltip",
								 "Paths cannot blend, so they can only be set for Interior Only and location sets without time of day."),
				Util::kTooltipWhenDisabled);
		if (!error.empty()) {
			ImGui::SameLine();
			Util::Text::Error("%s", error.c_str());
		}

		// Remove/Add above can shift indices, so the texture line re-resolves rather than reusing entryIndex.
		entryIndex = findEntry();
		const auto entries = manager->GetContextEntries(context);
		const auto* entry = entryIndex && *entryIndex < entries.size() ? &entries[*entryIndex] : nullptr;
		if (entry && !entry->deleted && entry->value.is_string())
			ImGui::Text(T("feature.scene_manager.path.scene_texture", "Scene Texture: %s"), entry->value.get_ref<const std::string&>().c_str());
		else
			ImGui::TextDisabled("%s", T("feature.scene_manager.path.scene_texture_inherited", "Scene Texture: inherited"));
	}
}

namespace CustomSceneControls
{
	bool DrawPathSceneControls(const SceneSettingsManager::SettingIdentity& setting, std::string& path, std::string& error,
		const std::function<std::string(const std::filesystem::path&)>& validate)
	{
		const auto* scene = SceneWidgetInterceptor::GetArmedContext();
		if (!scene)
			return false;
		if (scene->baseline) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsActiveSceneSetting(setting.featureShortName, setting.settingPath, setting.settingKey))
				return false;
			const auto context = manager->FindWinningContext(setting);
			if (!context)
				return false;
			const Util::DisableGuard readOnly(true);
			DrawPathContextControls(setting, path, error, validate, *context);
			return true;
		}
		DrawPathContextControls(setting, path, error, validate, scene->contextId);
		return true;
	}

	void RecordBaselineEdit(const SceneSettingsManager::SettingIdentity& setting, const json&)
	{
		SceneSettingsManager::GetSingleton()->CaptureExternalFeatureChanges(Feature::FindRegisteredFeatureByShortName(setting.featureShortName));
	}
}
