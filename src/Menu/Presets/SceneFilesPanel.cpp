#include "SceneFilesPanel.h"

#include <imgui_stdlib.h>

#include "BaselineManager.h"
#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "CSEditor/FormEditSources.h"
#include "CSEditor/SceneManager/SceneEntryTable.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "CSEditor/SceneManager/SceneSettingsUI.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Menu/PresetsPageRenderer.h"
#include "Presets/PresetContent.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "State.h"
#include "Utils/UI.h"

namespace
{
	using Manager = SceneSettingsManager;
	using FileFilter = PresetContent::FileFilter;
	constexpr std::array kFileFilters{ FileFilter::All, FileFilter::Baseline, FileFilter::Scene, FileFilter::Records };
	constexpr float kSettingListLines = 12.0f;
	constexpr float kValueEditorWidthEm = 36.0f;
	constexpr float kFilePickerVisibleRows = 8.0f;
	std::string packId;
	std::filesystem::path relative;
	FileFilter fileFilter = FileFilter::All;
	char search[128]{};
	std::vector<PresetContent::File> files;
	std::map<std::filesystem::path, PresetActivation::Footprint> fileValues;
	std::vector<Manager::PresetSceneSetting> scenes;
	std::set<std::pair<std::filesystem::path, std::string>> selected;
	std::uint64_t revision = 0;
	Util::ConfirmationPopup confirmation;
	Util::ConfirmationPopup copyConfirmation;
	std::string copyPack;
	PresetContent::Selection copyFiles;
	std::vector<std::string> copySettings;
	bool deleteFile = false;
	std::string pendingPack;
	std::filesystem::path pendingFile;
	PresetContent::Selection pendingSelection;
	struct ExportContents
	{
		std::vector<Manager::PresetSceneSetting> settings;
		std::map<std::string, std::vector<size_t>> files;
	};
	std::array<Manager::RevisionCache<ExportContents>, 2> exportSettings;

	const ExportContents& GetExportContents(bool fromPreset)
	{
		auto* manager = Manager::GetSingleton();
		return exportSettings[fromPreset ? 1 : 0].Get(manager->GetEntryPresentationRevision(), [manager, fromPreset] {
			ExportContents result;
			result.settings = manager->GetSceneExportSettings(fromPreset ? Manager::EntrySource::Overwrite : Manager::EntrySource::User);
			for (size_t index = 0; index < result.settings.size(); ++index) {
				const auto& setting = result.settings[index];
				Manager::SettingEntry identity;
				identity.featureShortName = setting.entry.featureShortName;
				result.files[Manager::GetSceneExportKey(setting.context, identity)].push_back(index);
			}
			return result;
		});
	}
	struct ValueEdit
	{
		std::string pack;
		std::filesystem::path file;
		std::string path;
		nlohmann::json original;
		nlohmann::json prototype;
		nlohmann::json value;
		std::vector<Util::Settings::ValueControlBinding> controls;
		std::map<ImGuiID, std::string> rawValues;
		PresetContent::SettingEditResult result = PresetContent::SettingEditResult::Unchanged;
	};
	ValueEdit valueEdit;
	Util::DeferredModal valueModal;

	const char* FileFilterLabel(FileFilter filter)
	{
		switch (filter) {
		case FileFilter::Baseline:
			return T("menu.presets.all_baseline_files", "All Baseline Files");
		case FileFilter::Scene:
			return T("menu.presets.all_scene_files", "All Scene Files");
		case FileFilter::Records:
			return T("menu.presets.all_record_files", "All Record Files");
		default:
			return T("menu.presets.all_files", "All Files");
		}
	}

	bool IsVisibleFile(const PresetContent::File& file)
	{
		return relative.empty() ? PresetContent::MatchesFileFilter(file.relative, fileFilter) : file.relative == relative;
	}

	void DrawValueCell(const PresetContent::File& file, const std::string& path, const nlohmann::json& value)
	{
		const auto origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		const float buttonSize = BrowserUI::IconButtonSize();
		const float textWidth = std::max(1.0f, width - buttonSize - ImGui::GetStyle().ItemSpacing.x);
		ImGui::AlignTextToFramePadding();
		BrowserUI::EllipsizedText(value.dump().c_str(), textWidth, ImGui::GetStyleColorVec4(ImGuiCol_Text));
		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(origin.x + std::max(0.0f, width - buttonSize), origin.y));
		const Util::DisableGuard blocked(Manager::GetSingleton()->HasPendingContextEdit());
		if (BrowserUI::IconButton("##EditValue", Icons::TI(ICON_TI_PENCIL), T("menu.presets.edit_value_tooltip", "Edit this preset value"))) {
			const auto prototype = PresetContent::EditableValue(file, path);
			valueEdit = { packId, file.relative, path, value, prototype, prototype };
			const auto feature = PresetContent::IsBaselineFile(file.relative) ? Util::PathToUtf8(file.relative.stem()) :
			                     PresetContent::IsSceneFile(file.relative)    ? PresetContent::SceneFeatureName(file.relative, *file.document) :
			                                                                    std::string{};
			valueEdit.controls = Util::Settings::GetValueControlBindings(feature, *file.document, path);
			valueModal.Request();
		}
	}

	void DrawEditableValue(const nlohmann::json& original, nlohmann::json& value, const char* label)
	{
		using json = nlohmann::json;
		ImGui::PushID(label);
		const SKSE::stl::scope_exit restoreId([] { ImGui::PopID(); });
		if (original.is_null() || (original.is_structured() && original.empty())) {
			auto& text = valueEdit.rawValues.try_emplace(ImGui::GetID("##Value"), original.dump()).first->second;
			ImGui::TextUnformatted(label);
			ImGui::InputTextMultiline("##Value", &text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * kSettingListLines));
			value = json::parse(text, nullptr, false);
			return;
		}
		if (value.is_structured()) {
			for (auto& [key, member] : value.items())
				DrawEditableValue(original.is_array() ? original.at(std::stoull(key)) : original.at(key), member, key.c_str());
			return;
		}
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (value.is_boolean()) {
			auto edited = value.get<bool>();
			if (ImGui::Checkbox("##Value", &edited))
				value = edited;
		} else if (value.is_number_float()) {
			auto edited = value.get<double>();
			if (ImGui::InputScalar("##Value", ImGuiDataType_Double, &edited, nullptr, nullptr, "%.17g"))
				value = edited;
		} else if (value.is_number_unsigned()) {
			auto edited = value.get<std::uint64_t>();
			if (ImGui::InputScalar("##Value", ImGuiDataType_U64, &edited))
				value = edited;
		} else if (value.is_number_integer()) {
			auto edited = value.get<std::int64_t>();
			if (ImGui::InputScalar("##Value", ImGuiDataType_S64, &edited))
				value = edited;
		} else if (value.is_string()) {
			auto& text = value.get_ref<json::string_t&>();
			if (text.contains('\n'))
				ImGui::InputTextMultiline("##Value", &text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * kSettingListLines));
			else
				ImGui::InputText("##Value", &text);
		} else {
			ImGui::TextUnformatted("null");
		}
	}

	void DrawValueEditor()
	{
		if (!valueModal.IsActive())
			return;
		using Result = PresetContent::SettingEditResult;
		const auto title = std::string(T("menu.presets.edit_value", "Edit Value")) + "###PresetValueEditor";
		valueModal.Draw(title.c_str(), ImGui::GetFontSize() * kValueEditorWidthEm, [] {
			BrowserUI::MetaText(Util::PathToUtf8(valueEdit.file).c_str());
			ImGui::TextWrapped("%s", valueEdit.path.substr(1).c_str());
			if (!SceneEntryTable::DrawValueControls(valueEdit.controls, valueEdit.value)) {
				if (valueEdit.prototype.is_structured()) {
					if (ImGui::BeginChild("##Components", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * kSettingListLines)))
						DrawEditableValue(valueEdit.prototype, valueEdit.value, T("ui.value", "Value"));
					ImGui::EndChild();
				} else {
					DrawEditableValue(valueEdit.prototype, valueEdit.value, T("ui.value", "Value"));
				}
			}
			auto check = nlohmann::json{ { "value", valueEdit.original } };
			const bool valid = !valueEdit.value.is_discarded() &&
			                   PresetContent::ReplaceSetting(check, "/value", valueEdit.original, valueEdit.value, &valueEdit.prototype) != Result::InvalidValue;
			const char* error = nullptr;
			if (!valid)
				error = T("menu.presets.edit_value_invalid", "Enter a valid value of the same type.");
			else if (valueEdit.result == Result::Stale || valueEdit.result == Result::Missing)
				error = T("menu.presets.edit_value_stale", "This value changed on disk. Close and reopen the editor to load it.");
			else if (valueEdit.result == Result::Conflict)
				error = T("menu.presets.edit_value_conflict", "This change conflicts with another enabled preset. Disable the conflicting preset before saving.");
			else if (valueEdit.result == Result::Blocked)
				error = SceneSettingsUI::PendingContextEditMessage();
			else if (valueEdit.result == Result::Failed || valueEdit.result == Result::InvalidValue)
				error = T("menu.presets.edit_value_failed", "Could not save this value. Your edit is kept; check the log and try again.");
			if (error)
				Util::Text::WrappedWarning("%s", error);
			ImGui::Separator();
			{
				const Util::DisableGuard disabled(!valid || valueEdit.value == valueEdit.original);
				if (Util::Button(T("ui.save", "Save"))) {
					valueEdit.result = UnifiedPresetCatalog::GetSingleton().EditPackSetting(valueEdit.pack, valueEdit.file,
						valueEdit.path, valueEdit.original, valueEdit.value);
					if (valueEdit.result == Result::Changed || valueEdit.result == Result::Unchanged) {
						revision = 0;
						ImGui::CloseCurrentPopup();
					}
				}
			}
			ImGui::SameLine();
			if (Util::Button(T("ui.cancel", "Cancel")))
				ImGui::CloseCurrentPopup();
		});
		if (!valueModal.IsActive()) {
			if (valueEdit.result == Result::Stale || valueEdit.result == Result::Missing) {
				UnifiedPresetCatalog::GetSingleton().Discover();
				revision = 0;
			}
			valueEdit = {};
		}
	}

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
		if (!std::ranges::any_of(files, [](const auto& file) { return PresetContent::MatchesFileFilter(file.relative, fileFilter); }))
			fileFilter = FileFilter::All;
		selected.clear();
		fileValues.clear();
		for (const auto& file : files)
			if (IsVisibleFile(file) && file.document) {
				auto& values = fileValues[file.relative];
				PresetContent::CollectValues(*file.document, "", values);
				for (const auto& [path, value] : values) selected.emplace(file.relative, path);
			}
		revision = UnifiedPresetCatalog::GetSingleton().GetRevision();
	}

	PresetContent::Selection SelectedFiles()
	{
		PresetContent::Selection selection;
		for (const auto& [file, path] : selected)
			selection[file].push_back(path);
		return selection;
	}

	void ReportCopy(const std::string& sourcePack, std::span<const std::string> keys, Manager::CopyConflictPolicy policy)
	{
		if (Manager::GetSingleton()->HasPendingContextEdit()) {
			SceneSettingsUI::WarnPendingContextEdit();
			return;
		}
		Manager::CopyResult result;
		PresetContent::Selection records;
		if (std::ranges::any_of(copyFiles, [](const auto& file) { return PresetContent::IsRecordFile(file.first); }) && !FormEditSources::CanChangePacks()) {
			EditorWindow::GetSingleton()->ShowNotification(T("menu.presets.copy_records_pending", "Save or discard unsaved record edits before copying record settings."), Util::Colors::GetWarning());
			return;
		}
		if (!copyFiles.empty()) {
			const auto contents = PresetContent::List(sourcePack);
			for (const auto& [relativeFile, paths] : copyFiles) {
				if (PresetContent::IsRecordFile(relativeFile)) {
					records[relativeFile] = paths;
					continue;
				}
				const auto file = std::ranges::find(contents, relativeFile, &PresetContent::File::relative);
				auto* feature = Feature::FindFeatureByShortName(BaselineManager::ParseBaselineFeatureName(relativeFile));
				if (file == contents.end() || !file->document || !feature || !feature->HasPersonalSettings()) {
					logger::warn("Could not copy Base Settings from '{}' for '{}'", sourcePack, Util::PathToUtf8(relativeFile));
					result.cancelled = true;
					break;
				}
				const auto settings = Util::Settings::SelectSettingPaths(*file->document, paths);
				const auto copy = Util::Settings::SelectSettingsToCopy(settings, globals::state->GetUserEdits(feature), policy == Manager::CopyConflictPolicy::OverwriteExisting);
				if (!copy.values.empty() && !globals::state->CopyFeatureSettingsToUser(feature, copy.values)) {
					result.cancelled = true;
					break;
				}
				result.copied += copy.copied;
				result.overwritten += copy.overwritten;
				result.skipped += copy.skipped;
			}
		}
		if (!result.cancelled && !keys.empty()) {
			const auto sceneResult = Manager::GetSingleton()->CopyPresetSceneSettings(sourcePack, keys, policy);
			result.copied += sceneResult.copied;
			result.overwritten += sceneResult.overwritten;
			result.skipped += sceneResult.skipped;
			result.cancelled = sceneResult.cancelled;
			result.blockedByPendingEdit = sceneResult.blockedByPendingEdit;
		}
		if (!result.cancelled && !records.empty()) {
			if (const auto copied = FormEditSources::CopyPresetSettings(sourcePack, records, policy == Manager::CopyConflictPolicy::OverwriteExisting)) {
				result.copied += copied->copied;
				result.overwritten += copied->overwritten;
				result.skipped += copied->skipped;
			} else {
				result.cancelled = true;
			}
		}
		if (result.blockedByPendingEdit) {
			SceneSettingsUI::WarnPendingContextEdit();
			return;
		}
		auto copied = result.copied, overwritten = result.overwritten, skipped = result.skipped;
		EditorWindow::GetSingleton()->ShowNotification(result.cancelled && (copyFiles.size() > 1 || (!copyFiles.empty() && !keys.empty())) ?
														   T("menu.presets.copy_incomplete", "Some settings could not be copied. Previously copied settings were kept; check the log for details.") :
													   result.cancelled ?
														   T("menu.presets.copy_failed", "Could not save the copied settings. Your previous User Settings were restored.") :
														   std::vformat(T("cs_editor.scene_page_copy_result", "{} copied, {} overwritten, {} skipped"), std::make_format_args(copied, overwritten, skipped)),
			result.cancelled ? Util::Colors::GetWarning() : Util::Colors::GetSuccess());
	}

	void RequestCopy(std::vector<std::string> keys, PresetContent::Selection selection = {})
	{
		copyPack = packId;
		copyFiles = std::move(selection);
		copySettings = std::move(keys);
		copyConfirmation.title = T("menu.presets.copy_to_user", "Copy to User Settings");
		copyConfirmation.message = T("menu.presets.copy_conflict_prompt", "Replace existing User Settings or skip them?");
		copyConfirmation.confirmLabel = T("menu.presets.copy_skip_existing", "Skip Existing");
		copyConfirmation.alternateLabel = T("menu.presets.copy_replace_existing", "Replace Existing");
		copyConfirmation.cancelLabel = T("ui.cancel", "Cancel");
		copyConfirmation.focus = Util::ConfirmationPopup::Focus::Cancel;
		copyConfirmation.Request();
	}

	void RequestFileCopy()
	{
		PresetContent::Selection documents;
		std::vector<std::string> keys;
		for (const auto& [filePath, paths] : SelectedFiles()) {
			if (PresetContent::IsBaselineFile(filePath) || PresetContent::IsRecordFile(filePath)) {
				documents[filePath] = paths;
				continue;
			}
			const auto file = std::ranges::find(files, filePath, &PresetContent::File::relative);
			if (file == files.end())
				continue;
			for (const auto& setting : scenes)
				if (setting.entry.sourcePath == file->source && std::ranges::find(paths, SettingPath(setting.entry)) != paths.end())
					keys.push_back(setting.key);
		}
		RequestCopy(std::move(keys), std::move(documents));
	}

	void RequestDelete(bool entireFile, const std::filesystem::path& file = {})
	{
		deleteFile = entireFile;
		pendingPack = packId;
		pendingFile = file.empty() ? relative : file;
		pendingSelection = entireFile ? PresetContent::Selection{} : SelectedFiles();
		confirmation.title = T("menu.presets.delete_settings", "Delete preset settings");
		confirmation.message = entireFile ?
		                           std::format("{}\n\n{}", Util::PathToUtf8(pendingFile), T("menu.presets.delete_file_confirm", "Delete this backing file from disk? A lower-priority copy may become visible. This cannot be undone.")) :
		                           T("menu.presets.delete_settings_confirm", "Delete the selected settings from this preset's files? Other settings will be kept. This cannot be undone.");
		confirmation.confirmLabel = T("ui.delete", "Delete");
		confirmation.cancelLabel = T("ui.cancel", "Cancel");
		confirmation.focus = Util::ConfirmationPopup::Focus::Cancel;
		confirmation.Request();
	}

	void DrawFilePicker()
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(T("menu.presets.selected_file", "Selected File:"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		Util::ComboPreviewAction action{ .label = relative.empty() ? nullptr : T("menu.presets.delete_file", "Delete File"),
			.enabled = !Manager::GetSingleton()->HasPendingContextEdit() };
		std::optional<std::filesystem::path> chosen;
		const auto preview = relative.empty() ? std::string(FileFilterLabel(fileFilter)) : Util::PathToUtf8(relative);
		float filterWidth = ImGui::GetStyle().ItemSpacing.x * (kFileFilters.size() - 1);
		for (auto filter : kFileFilters)
			filterWidth += BrowserUI::MeasureChip(FileFilterLabel(filter), false);
		const float popupWidth = std::min(filterWidth + ImGui::GetStyle().WindowPadding.x * 2.0f, ImGui::GetWindowViewport()->WorkSize.x);
		const float popupHeight = ImGui::GetFrameHeightWithSpacing() * (kFilePickerVisibleRows + 1.0f) +
		                          ImGui::GetStyle().WindowPadding.y * 2.0f + ImGui::GetStyle().ItemSpacing.y;
		ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f), ImVec2(FLT_MAX, popupHeight));
		if (Util::BeginComboWithAction("##PresetFile", preview.c_str(), action)) {
			const auto available = [](FileFilter filter) {
				return filter == FileFilter::All || std::ranges::any_of(files, [filter](const auto& file) { return PresetContent::MatchesFileFilter(file.relative, filter); });
			};
			for (const auto filter : kFileFilters) {
				const auto* label = FileFilterLabel(filter);
				if (filter != FileFilter::All)
					BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(label, false));
				ImGui::PushID(static_cast<int>(filter));
				const Util::DisableGuard unavailable(!available(filter));
				if (BrowserUI::FilterChip("##FileFilter", { label }, relative.empty() && fileFilter == filter)) {
					fileFilter = filter;
					chosen.emplace();
				}
				ImGui::PopID();
			}
			ImGui::Separator();
			for (auto group : { FileFilter::Baseline, FileFilter::Scene, FileFilter::Records })
				for (const auto& file : files) {
					if (!PresetContent::MatchesFileFilter(file.relative, group) || !PresetContent::MatchesFileFilter(file.relative, fileFilter))
						continue;
					const auto path = Util::PathToUtf8(file.relative);
					ImGui::PushID(path.c_str());
					const auto origin = ImGui::GetCursorScreenPos();
					const float rowHeight = ImGui::GetFrameHeight();
					const float right = origin.x + ImGui::GetContentRegionAvail().x;
					const float buttonWidth = ImGui::CalcTextSize(T("menu.presets.delete_file", "Delete File")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
					const float buttonX = std::max(origin.x, right - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
					const bool actionHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(buttonX, origin.y), ImVec2(right, origin.y + rowHeight));
					const bool picked = Util::RoundedSelectable("##File", file.relative == relative || actionHovered,
						ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(0, rowHeight));
					const ImVec2 textPos(origin.x + Util::GetSelectableTextInset(), Util::CenterTextTopY(origin.y, rowHeight));
					const ImVec4 clip(textPos.x, origin.y, buttonX - ImGui::GetStyle().ItemSpacing.x, origin.y + rowHeight);
					ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, ImGui::GetColorU32(ImGuiCol_Text), path.c_str(), nullptr, 0.0f, &clip);
					ImGui::SameLine();
					ImGui::SetCursorScreenPos(ImVec2(buttonX, origin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
					{
						const Util::DisableGuard blocked(!action.enabled);
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0));
						const SKSE::stl::scope_exit restorePadding([] { ImGui::PopStyleVar(); });
						if (ImGui::Button(T("menu.presets.delete_file", "Delete File"))) {
							RequestDelete(true, file.relative);
							ImGui::CloseCurrentPopup();
						}
					}
					if (picked && !actionHovered) {
						chosen = file.relative;
						ImGui::CloseCurrentPopup();
					}
					ImGui::PopID();
				}
			ImGui::EndCombo();
		}
		if (action.pressed)
			RequestDelete(true);
		if (chosen) {
			relative = std::move(*chosen);
			Refresh();
		}
	}
}

void SceneFilesPanel::OpenForPack(const std::string& id)
{
	packId = id;
	relative.clear();
	fileFilter = FileFilter::All;
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
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(T("menu.presets.selected_preset", "Selected Preset:"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##ContentsPreset", pack ? pack->name.c_str() : T("menu.presets.none", "No presets"))) {
		for (const auto index : catalog.Query(std::nullopt, "")) {
			const auto& candidate = catalog.GetPacks()[index];
			if (!candidate.hasCSPresets && !candidate.hasBaseline && !candidate.hasFormEdits)
				continue;
			ImGui::PushID(candidate.id.c_str());
			const auto origin = ImGui::GetCursorScreenPos();
			if (ImGui::Selectable("##Preset", candidate.id == packId, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ImGui::GetFrameHeight())))
				OpenForPack(candidate.id);
			const auto next = ImGui::GetCursorScreenPos();
			const float rowHeight = ImGui::GetFrameHeight();
			const float nameX = origin.x + Util::GetSelectableTextInset();
			ImGui::GetWindowDrawList()->AddText(ImVec2(nameX, Util::CenterTextTopY(origin.y, rowHeight)), ImGui::GetColorU32(ImGuiCol_Text), candidate.name.c_str());
			ImGui::SetCursorScreenPos(ImVec2(nameX + ImGui::CalcTextSize(candidate.name.c_str()).x + ImGui::GetStyle().ItemSpacing.x,
				origin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
			PresetsPageRenderer::DrawBackendBadges(candidate.IsE11(), candidate.IsCS(), candidate.IsBaseline(), true);
			ImGui::SetCursorScreenPos(next);
			ImGui::Dummy(ImVec2(0.0f, 0.0f));
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (!pack)
		return;
	if (revision != catalog.GetRevision())
		Refresh();
	DrawFilePicker();
	const auto file = std::ranges::find(files, relative, &PresetContent::File::relative);
	const bool aggregate = relative.empty();
	if (files.empty() || (!aggregate && file == files.end())) {
		Util::Text::WrappedDisabled("%s", T("menu.presets.no_files", "No settings files in this preset."));
		return;
	}
	if (!aggregate && !file->document) {
		Util::Text::WrappedError("%s", T("menu.presets.file_unreadable", "This file could not be read. Check the log before editing it."));
		return;
	}
	if (aggregate)
		for (const auto& source : files)
			if (IsVisibleFile(source) && !source.document)
				Util::Text::WrappedError("%s: %s", Util::PathToUtf8(source.relative).c_str(), T("menu.presets.file_unreadable", "This file could not be read. Check the log before editing it."));
	if (Util::Button(T("ui.select_all", "Select All")))
		for (const auto& [filePath, values] : fileValues)
			for (const auto& [path, value] : values) selected.emplace(filePath, path);
	ImGui::SameLine();
	if (Util::Button(T("ui.clear_selection", "Clear Selection")))
		selected.clear();
	BrowserUI::SearchField("##PresetSettingSearch", search, sizeof(search), T("ui.search", "Search..."), -FLT_MIN, true);
	const float actionsWidth = ImGui::CalcTextSize(T("menu.presets.copy_to_user", "Copy to User Settings")).x +
	                           ImGui::CalcTextSize(T("menu.presets.delete_selected", "Delete Selected Settings")).x +
	                           ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
	const float actionRows = actionsWidth <= ImGui::GetContentRegionAvail().x ? 1.0f : 2.0f;
	const float height = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * actionRows);
	if (ImGui::BeginTable("##PresetSettings", aggregate ? 3 : 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_ScrollY, ImVec2(0, height))) {
		if (aggregate)
			ImGui::TableSetupColumn(T("ui.file", "File"));
		ImGui::TableSetupColumn(T("ui.setting", "Setting"));
		ImGui::TableSetupColumn(T("ui.value", "Value"));
		ImGui::TableHeadersRow();
		const auto filter = Util::ToLowerAscii(search);
		for (const auto& [filePath, values] : fileValues) {
			const auto source = std::ranges::find(files, filePath, &PresetContent::File::relative);
			assert(source != files.end());
			const auto filename = Util::PathToUtf8(filePath);
			ImGui::PushID(filename.c_str());
			for (const auto& [path, value] : values) {
				if (!filter.empty() && !Util::ToLowerAscii(filename + path).contains(filter))
					continue;
				ImGui::PushID(path.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				if (aggregate) {
					ImGui::AlignTextToFramePadding();
					BrowserUI::EllipsizedText(filename.c_str(), ImGui::GetContentRegionAvail().x, ImGui::GetStyleColorVec4(ImGuiCol_Text));
					ImGui::TableSetColumnIndex(1);
				}
				bool ticked = selected.contains({ filePath, path });
				if (ImGui::Checkbox("##Selected", &ticked)) {
					if (ticked)
						selected.emplace(filePath, path);
					else
						selected.erase({ filePath, path });
				}
				ImGui::SameLine();
				BrowserUI::EllipsizedText(path.substr(1).c_str(), ImGui::GetContentRegionAvail().x, ImGui::GetStyleColorVec4(ImGuiCol_Text));
				ImGui::TableSetColumnIndex(aggregate ? 2 : 1);
				DrawValueCell(*source, path, value);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	const Util::DisableGuard blocked(Manager::GetSingleton()->HasPendingContextEdit());
	ImGui::BeginDisabled(selected.empty());
	{
		const bool pendingRecords = !FormEditSources::CanChangePacks() &&
		                            std::ranges::any_of(selected, [](const auto& setting) { return PresetContent::IsRecordFile(setting.first); });
		const Util::DisableGuard pending(pendingRecords);
		if (Util::Button(T("menu.presets.copy_to_user", "Copy to User Settings")))
			RequestFileCopy();
		if (pendingRecords)
			Util::AddTooltip(T("menu.presets.copy_records_pending", "Save or discard unsaved record edits before copying record settings."), Util::kTooltipWhenDisabled);
	}
	Util::SameLineIfFits(ImGui::CalcTextSize(T("menu.presets.delete_selected", "Delete Selected Settings")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
	if (Util::Button(T("menu.presets.delete_selected", "Delete Selected Settings")))
		RequestDelete(false);
	ImGui::EndDisabled();
}

bool SceneFilesPanel::HasExportSelection(const std::optional<std::set<std::string>>& selectedSettings, bool fromPreset)
{
	return std::ranges::any_of(GetExportContents(fromPreset).settings, [&](const auto& setting) {
		return !selectedSettings || selectedSettings->contains(setting.key);
	});
}

SceneFilesPanel::ExportFileCounts SceneFilesPanel::GetExportFileCounts(const std::optional<std::set<std::string>>& selectedSettings, bool fromPreset)
{
	const auto& contents = GetExportContents(fromPreset);
	const auto included = std::ranges::count_if(contents.files, [&](const auto& file) {
		return std::ranges::any_of(file.second, [&](size_t index) { return !selectedSettings || selectedSettings->contains(contents.settings[index].key); });
	});
	return { static_cast<size_t>(included), contents.files.size() };
}

void SceneFilesPanel::DrawExportSelection(std::optional<std::set<std::string>>& selectedSettings, bool fromPreset,
	std::string_view filter, std::optional<bool> selectAll, float visibleLines)
{
	const auto& contents = GetExportContents(fromPreset);
	const auto& settings = contents.settings;
	auto* manager = Manager::GetSingleton();
	if (selectAll) {
		if (*selectAll)
			selectedSettings.reset();
		else
			selectedSettings.emplace();
		if (!*selectAll)
			selectedSettings->clear();
	}
	ImGui::PushID(fromPreset ? 1 : 0);
	const float height = ImGui::GetFrameHeightWithSpacing() * visibleLines + ImGui::GetStyle().WindowPadding.y * 2.0f;
	if (ImGui::BeginChild("##ExportScenes", ImVec2(0, height), ImGuiChildFlags_Borders)) {
		const auto setSelected = [&](const auto& group, bool enabled) {
			if (!selectedSettings) {
				selectedSettings.emplace();
				for (const auto& setting : settings) selectedSettings->insert(setting.key);
			}
			for (const auto index : group) {
				if (enabled)
					selectedSettings->insert(settings[index].key);
				else
					selectedSettings->erase(settings[index].key);
			}
		};
		bool any = false;
		for (const auto& [key, group] : contents.files) {
			const auto& first = settings[group.front()];
			auto* feature = Feature::FindFeatureByShortName(first.entry.featureShortName);
			const auto label = manager->GetSceneContextDisplayName(first.context) + " / " +
			                   (feature ? feature->GetDisplayName() : first.entry.featureShortName);
			const bool fileMatch = Util::StringMatchesSearch(label, filter);
			const auto matches = [&](size_t index) { return fileMatch || Util::StringMatchesSearch(settings[index].entry.displayName, filter); };
			if (!std::ranges::any_of(group, matches))
				continue;
			any = true;
			ImGui::PushID(key.c_str());
			const auto includedCount = std::ranges::count_if(group, [&](const auto index) { return !selectedSettings || selectedSettings->contains(settings[index].key); });
			int ticked = includedCount == 0 ? 0 : static_cast<size_t>(includedCount) == group.size() ? 3 :
			                                                                                           1;
			if (ImGui::CheckboxFlags("##IncludeSceneFile", &ticked, 3))
				setSelected(group, ticked == 3);
			ImGui::SameLine();
			if (!filter.empty())
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			const auto title = std::format("{} ({}/{})###Settings", label, includedCount, group.size());
			if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
				for (const auto index : group) {
					if (!matches(index))
						continue;
					const auto& setting = settings[index];
					ImGui::PushID(setting.key.c_str());
					bool included = !selectedSettings || selectedSettings->contains(setting.key);
					if (ImGui::Checkbox(setting.entry.displayName.c_str(), &included))
						setSelected(std::array{ index }, included);
					if (auto tooltip = Util::HoverTooltipWrapper())
						ImGui::TextWrapped("%s", Util::Settings::FormatValue(setting.entry.value).c_str());
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (!any)
			ImGui::TextDisabled("%s", !filter.empty() ? T("ui.no_results", "No results") :
														T("menu.presets.export_scenes_none", "No saved scene settings are available from this source."));
	}
	ImGui::EndChild();
	ImGui::PopID();
}

void SceneFilesPanel::DrawConfirmation()
{
	DrawValueEditor();
	const auto choice = copyConfirmation.DrawChoice();
	if (choice == Util::ConfirmationPopup::Choice::Confirm || choice == Util::ConfirmationPopup::Choice::Alternate)
		ReportCopy(copyPack, copySettings, choice == Util::ConfirmationPopup::Choice::Alternate ? Manager::CopyConflictPolicy::OverwriteExisting : Manager::CopyConflictPolicy::SkipExisting);
	if (!copyConfirmation.IsOpen()) {
		copyPack.clear();
		copyFiles.clear();
		copySettings.clear();
	}
	if (confirmation.Draw()) {
		const bool deleted = deleteFile ? UnifiedPresetCatalog::GetSingleton().DeletePackSettings(pendingPack, pendingFile, {}, true) :
		                                  UnifiedPresetCatalog::GetSingleton().DeletePackSettings(pendingPack, pendingSelection);
		if (!deleted)
			EditorWindow::GetSingleton()->ShowNotification(T("menu.presets.delete_failed", "Could not delete this preset. Check the log for details."), Util::Colors::GetWarning());
		revision = 0;
	}
}

bool SceneFilesPanel::IsOpen() { return confirmation.IsOpen() || copyConfirmation.IsOpen() || valueModal.IsActive(); }

void SceneFilesPanel::Dismiss()
{
	confirmation = {};
	copyConfirmation = {};
	valueModal = {};
	valueEdit = {};
	pendingPack.clear();
	pendingFile.clear();
	pendingSelection.clear();
	copyPack.clear();
	copyFiles.clear();
	copySettings.clear();
}
