#pragma once

#include "SceneSettingsManager.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

namespace SceneEntryTable
{
	inline constexpr std::array kRestoreActions{ SceneSettingsManager::RestoreSource::Defaults,
		SceneSettingsManager::RestoreSource::Preset };
	/** @brief Returns the restore actions supported by the selected source layer. */
	inline constexpr std::span<const SceneSettingsManager::RestoreSource> GetRestoreActions(SceneSettingsManager::EntrySource source)
	{
		const std::span<const SceneSettingsManager::RestoreSource> actions{ kRestoreActions };
		return source == SceneSettingsManager::EntrySource::Overwrite ? actions : actions.first(actions.size() - 1);
	}
	/** @brief Draws a theme-matched scene action icon. */
	bool DrawActionButton(const char* id, Icons::GlyphRef glyph, const char* tooltip, float size);
	/** @brief Draws the Pause or Resume icon for the current state. */
	bool DrawPauseAction(bool paused, float size, bool all = false);
	/// Draws one of the shared scene restore buttons.
	bool DrawRestoreAction(SceneSettingsManager::RestoreSource action, bool enabled, float size, bool all = false);
	/// Returns whether restoring the setting would change its current value.
	bool CanRestoreSetting(const SceneSettingsManager::SettingEntry& entry, SceneSettingsManager::RestoreSource action, const nlohmann::json* draft = nullptr);
	/** @brief Measures the pause, restore and delete icons for one layer. */
	float GetLayerActionsWidth(SceneSettingsManager::EntrySource source);
	/** @brief Draws pause, restore, and delete together for one layer. */
	void DrawLayerActions(const SceneSettingsManager::EntryLayerScope& scope, SceneSettingsManager::EntrySource source,
		bool allPaused, bool hasEntries, bool enabled, bool canDelete,
		const std::function<void()>& onPause, const std::function<void()>& onDelete, bool featureToolbar = false);
	/// Stages restores for one layer, feature or setting selection.
	void RestoreSettings(const SceneSettingsManager::EntryLayerScope& scope, SceneSettingsManager::EntrySource layer,
		SceneSettingsManager::RestoreSource action, bool featureToolbar = false);
	/// Applies completed preset edits and draws their confirmation in the active scene page or popup.
	void FinishPagePresetFrame();
	void RestorePresetSetting(const SceneSettingsManager::SettingEntry& entry, SceneSettingsManager::RestoreSource action, bool featureToolbar);
	/** @brief Draws existing catalogue controls against detached preset values. */
	bool DrawValueControls(std::span<const Util::Settings::ValueControlBinding> bindings, nlohmann::json& value);
	/** @brief Shared presentation for scene table and feature-setting action flyouts. */
	Util::FlyoutStyle GetActionFlyoutStyle();
	/** @brief Returns the shared scaled scene-action button height. */
	float GetActionButtonSize();
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
