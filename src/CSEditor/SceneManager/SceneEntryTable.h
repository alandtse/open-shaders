#pragma once

#include "SceneSettingsManager.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

namespace SceneEntryTable
{
	/** @brief Draws existing catalogue controls against detached preset values. */
	bool DrawValueControls(std::span<const Util::Settings::ValueControlBinding> bindings, nlohmann::json& value);
	/** @brief Shared presentation for scene table and feature-setting action flyouts. */
	Util::FlyoutStyle GetActionFlyoutStyle();
	/** @brief Returns the shared scaled scene-action button height. */
	float GetActionButtonSize();
	/** @brief Returns the shared scaled scene-action toggle size. */
	ImVec2 GetActionToggleSize();
	/** @brief Names enabled presets supplying this scene value. */
	std::string GetPresetSourceHint(const SceneSettingsManager::SettingEntry* entry);
	/** @brief Starts a detached preset-edit frame for the feature toolbar. */
	void BeginFeaturePresetFrame(const SceneSettingsManager::SceneContextId& context, std::string_view feature, std::uint64_t scopeRevision);
	/** @brief Returns the current drag value for a preset entry, if any. */
	const nlohmann::json* GetFeaturePresetDraft(const SceneSettingsManager::SettingEntry& entry);
	/** @brief Stages an edit without modifying the live scene or User Settings. */
	void StageFeaturePresetEdit(const SceneSettingsManager::SettingEntry& entry, nlohmann::json value);
	/** @brief Keeps the User Settings components of a mixed edit detached until the preset commits. */
	void StageFeatureUserEdit(const SceneSettingsManager::SettingIdentity& setting, nlohmann::json value);
	const nlohmann::json* GetFeatureUserDraft(const SceneSettingsManager::SettingIdentity& setting);
	/** @brief Opens the shared preset deletion confirmation for toolbar settings. */
	void RequestFeaturePresetDeletion(std::span<const SceneSettingsManager::SettingEntry* const> entries);
	/** @brief Draws the table's transition field for the toolbar's selected User Settings entries. */
	void DrawFeatureTransition(const SceneSettingsManager::SceneContextId& context, std::span<const size_t> indices);
	/** @brief Applies completed edits after feature controls have finished drawing. */
	void FinishFeaturePresetFrame();
	/** @brief Draws the source and user entry tables for one scene, including its period columns. */
	void Draw(const SceneSettingsManager::SceneContextId& context);
	/** @brief Finishes an in-progress preset change and closes its dialogs. */
	void EndPresetEditing();
	/** @brief Finishes preset editing after the menu stops drawing the table. */
	void EndPresetEditingIfHidden();
	/** @brief Whether a standalone add or confirmation dialog owns its scene target. */
	bool HasActiveDialog();
}
