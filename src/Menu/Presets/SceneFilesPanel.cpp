#include "SceneFilesPanel.h"

#include <imgui_stdlib.h>

#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "CSEditor/SceneManager/SceneSettingsUI.h"
#include "I18n/I18n.h"
#include "Presets/PresetContent.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "Utils/UI.h"

namespace
{
	using Manager = SceneSettingsManager;
	constexpr float kSettingListLines = 12.0f;
	std::string packId;
	std::filesystem::path relative;
	char search[128]{};
	std::vector<PresetContent::File> files;
	PresetActivation::Footprint fileValues;
	std::vector<Manager::PresetSceneSetting> scenes;
	std::set<std::string> selected;
	std::set<std::string> selectedScenes;
	std::uint64_t revision = 0;
	Util::ConfirmationPopup confirmation;
	bool deleteFile = false;
	std::string pendingPack;
	std::filesystem::path pendingFile;
	std::vector<std::string> pendingSettings;
	std::vector<Manager::PresetSceneSetting> exportSettings;
	std::uint64_t exportRevision = 0;

	std::string SettingPath(const Manager::SettingEntry& entry)
	{
		nlohmann::json::json_pointer path;
		for (const auto& part : entry.settingPath) path /= part;
		path /= entry.settingKey;
		return path.to_string();
	}

	void Refresh()
	{
		files = PresetContent::List(packId);
		scenes = Manager::GetSingleton()->GetPresetSceneSettings(packId);
		if (!relative.empty() && std::ranges::find(files, relative, &PresetContent::File::relative) == files.end())
			relative.clear();
		if (scenes.empty() && relative.empty() && !files.empty())
			relative = files.front().relative;
		selectedScenes.clear();
		for (const auto& setting : scenes)
			if (!setting.entry.deleted)
				selectedScenes.insert(setting.key);
		selected.clear();
		fileValues.clear();
		const auto file = std::ranges::find(files, relative, &PresetContent::File::relative);
		if (file != files.end() && file->document) {
			PresetContent::CollectValues(*file->document, "", fileValues);
			for (const auto& [path, value] : fileValues) selected.insert(path);
		}
		revision = UnifiedPresetCatalog::GetSingleton().GetRevision();
	}

	void ReportCopy(std::span<const std::string> keys, Manager::CopyConflictPolicy policy)
	{
		const auto result = Manager::GetSingleton()->CopyPresetSceneSettings(packId, keys, policy);
		if (result.blockedByPendingEdit) {
			SceneSettingsUI::WarnPendingContextEdit();
			return;
		}
		auto copied = result.copied, overwritten = result.overwritten, skipped = result.skipped;
		EditorWindow::GetSingleton()->ShowNotification(result.cancelled ?
														   T("menu.presets.copy_failed", "Could not save the copied settings. Your previous User Settings were restored.") :
														   std::vformat(T("cs_editor.scene_page_copy_result", "{} copied, {} overwritten, {} skipped"), std::make_format_args(copied, overwritten, skipped)),
			result.cancelled ? Util::Colors::GetWarning() : Util::Colors::GetSuccess());
	}

	void ReportFileCopy(Manager::CopyConflictPolicy policy)
	{
		const auto file = std::ranges::find(files, relative, &PresetContent::File::relative);
		if (file == files.end())
			return;
		std::vector<std::string> keys;
		for (const auto& setting : scenes)
			if (setting.entry.sourcePath == file->source && selected.contains(SettingPath(setting.entry)))
				keys.push_back(setting.key);
		ReportCopy(keys, policy);
	}

	void DrawSceneCopy()
	{
		if (Util::Button(T("ui.select_all", "Select All")))
			for (const auto& setting : scenes)
				if (!setting.entry.deleted)
					selectedScenes.insert(setting.key);
		ImGui::SameLine();
		if (Util::Button(T("ui.clear_selection", "Clear Selection")))
			selectedScenes.clear();
		const float height = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.0f);
		if (ImGui::BeginTable("##PresetSceneCopy", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY, ImVec2(0, height))) {
			ImGui::TableSetupColumn(T("menu.presets.scene_context", "Scene"));
			ImGui::TableSetupColumn(T("ui.setting", "Setting"));
			ImGui::TableSetupColumn(T("ui.value", "Value"));
			ImGui::TableHeadersRow();
			const auto filter = Util::ToLowerAscii(search);
			for (const auto& setting : scenes) {
				if (setting.entry.deleted)
					continue;
				const auto context = Manager::GetSingleton()->GetSceneContextDisplayName(setting.context);
				const auto label = setting.entry.featureShortName + " / " + setting.entry.displayName;
				if (!filter.empty() && !Util::ToLowerAscii(context + " " + label).contains(filter))
					continue;
				ImGui::PushID(setting.key.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				bool ticked = selectedScenes.contains(setting.key);
				if (ImGui::Checkbox("##Selected", &ticked)) {
					if (ticked)
						selectedScenes.insert(setting.key);
					else
						selectedScenes.erase(setting.key);
				}
				ImGui::SameLine();
				BrowserUI::EllipsizedText(context.c_str(), ImGui::GetContentRegionAvail().x, ImGui::GetStyleColorVec4(ImGuiCol_Text));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", label.c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextWrapped("%s", setting.entry.value.dump().c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		const Util::DisableGuard blocked(selectedScenes.empty() || Manager::GetSingleton()->HasPendingContextEdit());
		const auto copy = [&](Manager::CopyConflictPolicy policy) {
			const std::vector<std::string> keys(selectedScenes.begin(), selectedScenes.end());
			ReportCopy(keys, policy);
		};
		if (Util::Button(T("menu.presets.copy_to_user", "Copy to User Settings")))
			copy(Manager::CopyConflictPolicy::SkipExisting);
		BrowserUI::SameLineIfFits(ImGui::CalcTextSize(T("menu.presets.replace_user", "Replace User Settings")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (Util::Button(T("menu.presets.replace_user", "Replace User Settings")))
			copy(Manager::CopyConflictPolicy::OverwriteExisting);
		Util::Text::WrappedDisabled("%s", T("menu.presets.copy_to_user_help", "Copy keeps existing User Settings. Replace changes the selected User Settings too. Preset files are never changed by copying."));
	}

	void RequestDelete(bool entireFile)
	{
		deleteFile = entireFile;
		pendingPack = packId;
		pendingFile = relative;
		pendingSettings.assign(selected.begin(), selected.end());
		confirmation.title = T("menu.presets.delete_settings", "Delete preset settings");
		confirmation.message = entireFile ?
		                           T("menu.presets.delete_file_confirm", "Delete this backing file from disk? A lower-priority copy may become visible. This cannot be undone.") :
		                           T("menu.presets.delete_settings_confirm", "Delete the selected settings from this preset's backing file? Other settings will be kept. This cannot be undone.");
		confirmation.confirmLabel = T("ui.delete", "Delete");
		confirmation.cancelLabel = T("ui.cancel", "Cancel");
		confirmation.focus = Util::ConfirmationPopup::Focus::Cancel;
		confirmation.Request();
	}
}

void SceneFilesPanel::OpenForPack(const std::string& id)
{
	packId = id;
	relative.clear();
	revision = 0;
}

void SceneFilesPanel::Draw()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto* pack = catalog.FindPack(packId);
	if (!pack) {
		for (const auto& candidate : catalog.GetPacks())
			if (candidate.hasCSPresets || candidate.hasBaseline || candidate.hasFormEdits) {
				packId = candidate.id;
				break;
			}
		pack = catalog.FindPack(packId);
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##ContentsPreset", pack ? pack->name.c_str() : T("menu.presets.none", "No presets"))) {
		for (const auto& candidate : catalog.GetPacks())
			if ((candidate.hasCSPresets || candidate.hasBaseline || candidate.hasFormEdits) && ImGui::Selectable(candidate.name.c_str(), candidate.id == packId))
				OpenForPack(candidate.id);
		ImGui::EndCombo();
	}
	if (!pack)
		return;
	if (revision != catalog.GetRevision())
		Refresh();
	Util::Text::WrappedDisabled("%s", T("menu.presets.contents_help", "Select a file and its settings. Copy scene values into your User Settings to edit them; the original preset stays in place."));
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##PresetFile", relative.empty() ? T("menu.presets.all_scene_files", "All scene files") : Util::PathToUtf8(relative).c_str())) {
		if (ImGui::Selectable(T("menu.presets.all_scene_files", "All scene files"), relative.empty())) {
			relative.clear();
			Refresh();
		}
		for (const auto& file : files)
			if (ImGui::Selectable(Util::PathToUtf8(file.relative).c_str(), file.relative == relative)) {
				relative = file.relative;
				Refresh();
				break;
			}
		ImGui::EndCombo();
	}
	if (relative.empty()) {
		BrowserUI::SearchField("##PresetSceneSearch", search, sizeof(search), T("ui.search", "Search..."), -FLT_MIN, true);
		DrawSceneCopy();
		return;
	}
	const auto file = std::ranges::find(files, relative, &PresetContent::File::relative);
	if (file == files.end()) {
		Util::Text::WrappedDisabled("%s", T("menu.presets.no_files", "No settings files in this preset."));
		return;
	}
	if (!file->document) {
		Util::Text::WrappedError("%s", T("menu.presets.file_unreadable", "This file could not be read. Check the log before editing it."));
		const Util::DisableGuard blocked(Manager::GetSingleton()->HasPendingContextEdit());
		if (Util::Button(T("menu.presets.delete_file", "Delete File")))
			RequestDelete(true);
		return;
	}
	BrowserUI::SearchField("##PresetSettingSearch", search, sizeof(search), T("ui.search", "Search..."), -FLT_MIN, true);
	const auto& values = fileValues;
	if (Util::Button(T("ui.select_all", "Select All")))
		for (const auto& [path, value] : values) selected.insert(path);
	ImGui::SameLine();
	if (Util::Button(T("ui.clear_selection", "Clear Selection")))
		selected.clear();
	const float height = std::max(ImGui::GetFrameHeight(), std::min(ImGui::GetTextLineHeightWithSpacing() * kSettingListLines,
															   ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f));
	if (ImGui::BeginTable("##PresetSettings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY, ImVec2(0, height))) {
		ImGui::TableSetupColumn(T("ui.setting", "Setting"));
		ImGui::TableSetupColumn(T("ui.value", "Value"));
		ImGui::TableHeadersRow();
		const auto filter = Util::ToLowerAscii(search);
		for (const auto& [path, value] : values) {
			if (!filter.empty() && !Util::ToLowerAscii(path).contains(filter))
				continue;
			ImGui::PushID(path.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			bool ticked = selected.contains(path);
			if (ImGui::Checkbox("##Selected", &ticked)) {
				if (ticked)
					selected.insert(path);
				else
					selected.erase(path);
			}
			ImGui::SameLine();
			BrowserUI::EllipsizedText(path.substr(1).c_str(), ImGui::GetContentRegionAvail().x, ImGui::GetStyleColorVec4(ImGuiCol_Text));
			ImGui::TableSetColumnIndex(1);
			ImGui::TextWrapped("%s", value.dump().c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	const Util::DisableGuard blocked(Manager::GetSingleton()->HasPendingContextEdit());
	ImGui::BeginDisabled(selected.empty());
	if (PresetContent::IsSceneFile(relative)) {
		if (Util::Button(T("menu.presets.copy_to_user", "Copy to User Settings")))
			ReportFileCopy(Manager::CopyConflictPolicy::SkipExisting);
		BrowserUI::SameLineIfFits(ImGui::CalcTextSize(T("menu.presets.replace_user", "Replace User Settings")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (Util::Button(T("menu.presets.replace_user", "Replace User Settings")))
			ReportFileCopy(Manager::CopyConflictPolicy::OverwriteExisting);
		Util::Text::WrappedDisabled("%s", T("menu.presets.copy_to_user_help", "Copy keeps existing User Settings. Replace changes the selected User Settings too. Preset files are never changed by copying."));
	}
	if (Util::Button(T("menu.presets.delete_selected", "Delete Selected Settings")))
		RequestDelete(false);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (Util::Button(T("menu.presets.delete_file", "Delete File")))
		RequestDelete(true);
}

void SceneFilesPanel::DrawExportSelection(std::optional<std::set<std::string>>& selectedSettings)
{
	if (!ImGui::CollapsingHeader(T("menu.presets.scene_selection", "Scene settings to include")))
		return;
	auto* manager = Manager::GetSingleton();
	if (exportRevision != manager->GetEntryPresentationRevision() || exportSettings.empty()) {
		exportSettings = manager->GetSceneExportSettings();
		exportRevision = manager->GetEntryPresentationRevision();
	}
	bool all = !selectedSettings;
	if (ImGui::Checkbox(T("menu.presets.all_scene_settings", "Include all scene settings"), &all)) {
		if (all)
			selectedSettings.reset();
		else {
			selectedSettings.emplace();
			for (const auto& setting : exportSettings) selectedSettings->insert(setting.key);
		}
	}
	if (!selectedSettings)
		return;
	if (ImGui::BeginChild("##ExportScenes", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * kSettingListLines), ImGuiChildFlags_Borders))
		for (const auto& setting : exportSettings) {
			ImGui::PushID(setting.key.c_str());
			bool ticked = selectedSettings->contains(setting.key);
			const auto label = manager->GetSceneContextDisplayName(setting.context) + " / " + setting.entry.featureShortName + " / " + setting.entry.displayName;
			if (ImGui::Checkbox(label.c_str(), &ticked)) {
				if (ticked)
					selectedSettings->insert(setting.key);
				else
					selectedSettings->erase(setting.key);
			}
			ImGui::PopID();
		}
	ImGui::EndChild();
}

void SceneFilesPanel::DrawConfirmation()
{
	if (confirmation.Draw()) {
		if (!UnifiedPresetCatalog::GetSingleton().DeletePackSettings(pendingPack, pendingFile, pendingSettings, deleteFile))
			EditorWindow::GetSingleton()->ShowNotification(T("menu.presets.delete_failed", "Could not delete this preset. Check the log for details."), Util::Colors::GetWarning());
		revision = 0;
	}
}

bool SceneFilesPanel::IsOpen() { return confirmation.IsOpen(); }
