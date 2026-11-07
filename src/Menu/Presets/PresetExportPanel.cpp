#include "Menu/Presets/PresetExportPanel.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

#include <imgui.h>
#include <imgui_stdlib.h>

#include "BaselineManager.h"
#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "CSEditor/FormEditSources.h"
#include "Feature.h"
#include "I18n/I18n.h"
#include "Menu/Presets/BaseSettingsEditor.h"
#include "Menu/Presets/SceneFilesPanel.h"
#include "Menu/PresetsPageRenderer.h"
#include "Presets/PresetCompatibility.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using PresetExportInfo = SceneSettingsManager::PresetExportInfo;
	using PresetType = SceneSettingsManager::PresetType;

	/// Fixed ID behind the translated title, so the popup survives a language switch.
	constexpr const char* kPickerPopupId = "##ScenePresetExportPicker";

	constexpr float kDescriptionLines = 3.0f;
	constexpr float kPickerListHeight = 200.0f;
	/// Visible rows before an included-content list scrolls.
	constexpr float kFeatureLines = 6.0f;
	constexpr size_t kSearchBufferSize = 128;
	/// Files a destructive confirmation names before it stops listing and counts the rest.
	constexpr size_t kMaxListedFiles = 8;

	enum class ExportSource
	{
		User,
		Preset
	};
	ExportSource baselineSource = ExportSource::User;
	ExportSource sceneSource = ExportSource::User;
	ExportSource recordSource = ExportSource::User;
	struct BaselineSelection
	{
		std::map<std::string, json> values;
		std::map<std::string, std::vector<Util::Settings::ExportSetting>> settings;
		std::map<std::string, std::vector<std::string>> paths;
		std::vector<std::string> features;
		std::map<std::string, bool> disableAtBoot;
	};
	std::array<BaselineSelection, 2> baselineSelections;
	std::array<FormEditSources::FormKeySet, 2> recordFiles;
	std::array<FormEditSources::FormKeySet, 2> selectedRecords;
	std::array<std::optional<std::set<std::string>>, 2> selectedScenes;
	bool sourceReadFailed = false;
	bool selectTypeTab = true;
	std::array<char, kSearchBufferSize> baselineSearch{};
	std::array<char, kSearchBufferSize> sceneSearch{};
	std::array<char, kSearchBufferSize> recordSearch{};

	BaselineSelection& SelectedBaselines() { return baselineSelections[static_cast<size_t>(baselineSource)]; }
	SceneSettingsManager::EntrySource SelectedSceneSource()
	{
		return sceneSource == ExportSource::User ? SceneSettingsManager::EntrySource::User : SceneSettingsManager::EntrySource::Overwrite;
	}

	void DrawSourceSwitch(const char* id, ExportSource& source)
	{
		const char* labels[] = { T("menu.presets.export_source_user", "Saved User Edits"), T("menu.presets.export_source_preset", "Enabled Presets") };
		int index = static_cast<int>(source);
		if (Util::SegmentedControl(id, labels, 2, index))
			source = static_cast<ExportSource>(index);
	}

	/// The name is kept as typed; tags and plugins stay unparsed until export. Its artwork paths are the ones
	/// a prior export of this name wrote, shown when nothing new is picked.
	PresetExportInfo form;
	std::string presetTags;
	std::string requiredPlugins;
	std::vector<std::filesystem::path> collidingFiles;
	std::string pickerSearch;

	enum class RequestKind
	{
		NewPreset,
		BaseSettings
	};
	struct ExportRequest
	{
		RequestKind kind;
		std::string feature;
		std::optional<std::vector<std::string>> pageKeys = std::nullopt;
	};
	std::optional<ExportRequest> requestedExport;
	bool initialized = false;
	Util::ConfirmationPopup exportConfirmation;

	std::string DescribeCollision(std::string name, const std::vector<std::filesystem::path>& files)
	{
		std::string listed;
		for (size_t index = 0; index < files.size() && index < kMaxListedFiles; ++index)
			listed += std::format("\n  {}", files[index].filename().string());
		if (files.size() > kMaxListedFiles) {
			auto remaining = files.size() - kMaxListedFiles;
			listed += std::vformat(
				T(TKEY("scene_export_replace_more"), "\n  ... and {} more"), std::make_format_args(remaining));
		}

		auto count = files.size();
		return std::vformat(T(TKEY("scene_export_replace_message"),
								"'{}' already has {} file(s) on disk. Exporting deletes every one of "
								"them and writes this preset in their place. If another mod owns "
								"these files, they are gone.{}"),
			std::make_format_args(name, count, listed));
	}

	/** @brief Shows a success or failure notification for an export. */
	void ReportExportResult(std::string name, bool exported)
	{
		auto message = exported ?
		                   std::vformat(T(TKEY("scene_export_result_success"), "Preset '{}' exported."),
							   std::make_format_args(name)) :
		                   std::vformat(T(TKEY("scene_export_result_failure"),
											"Preset '{}' export failed. Check the log for details."),
							   std::make_format_args(name));
		EditorWindow::GetSingleton()->ShowNotification(
			message, exported ? Util::Colors::GetSuccess() : Util::Colors::GetError());
	}

	/** @brief Draws one preset type tab. */
	void DrawPresetTypeOption(const char* label, PresetType type, const char* tooltip)
	{
		if (Util::BeginPillTabItem(label, nullptr, selectTypeTab && form.type == type ? ImGuiTabItemFlags_SetSelected : 0, tooltip)) {
			if (!selectTypeTab || form.type == type)
				form.type = type;
			Util::EndPillTabItem();
		}
	}

	/** @brief Explains the three preset types. */
	void DrawGuide()
	{
		Util::HelpMarker(T(TKEY("scene_export_guide"),
			"CS Preset: Scene settings and OS Editor record files, plus optional Base Settings.\n\n"
			"E11 Preset: Effects 11 ENB files, plus any settings you include.\n\n"
			"Base Settings Preset: Feature values used across all scenes."));
	}

	bool IsFeatureRequired(const std::string& shortName)
	{
		return std::find(form.requiredFeatures.begin(), form.requiredFeatures.end(), shortName) !=
		       form.requiredFeatures.end();
	}

	void SetFeatureRequired(const std::string& shortName, bool required)
	{
		const auto it = std::find(form.requiredFeatures.begin(), form.requiredFeatures.end(), shortName);
		if (required && it == form.requiredFeatures.end())
			form.requiredFeatures.push_back(shortName);
		else if (!required && it != form.requiredFeatures.end())
			form.requiredFeatures.erase(it);
	}

	/// Core features ship with every Community Shaders install, so a pack never needs to require them.
	bool IsCoreFeature(const std::string& shortName)
	{
		const auto* feature = Feature::FindFeatureByShortName(shortName);
		return feature && feature->IsCore();
	}

	/// Features the required list offers: loaded, user-facing, and not core.
	bool IsRequirableFeature(const Feature* feature)
	{
		return feature && feature->loaded && feature->IsInMenu() && !feature->IsCore();
	}

	/// Whether the export would carry settings for a feature, from the user or any mod layer.
	bool HasExportedSettings(const std::string& shortName)
	{
		const auto* manager = SceneSettingsManager::GetSingleton();
		return manager && manager->HasAnySceneEntriesForFeature(shortName);
	}

	PresetProvider* GetDedicatedBaselineProvider()
	{
		for (auto* provider : UnifiedPresetCatalog::GetProviders())
			if (!provider->GetDedicatedBaselineFeature().empty())
				return provider;
		return nullptr;
	}

	std::string GetDedicatedBaselineShortName()
	{
		const auto* provider = GetDedicatedBaselineProvider();
		return provider ? std::string(provider->GetDedicatedBaselineFeature()) : std::string{};
	}

	/// Loaded features whose settings can go in a Baseline folder.
	bool IsBaselineCandidate(Feature* feature)
	{
		return feature && feature->loaded && feature->HasPersonalSettings() && BaseSettingsEditor::HasExportableSettings(feature);
	}

	bool HasAnyBaselineCandidate()
	{
		const auto& features = Feature::GetFeatureList();
		return std::ranges::any_of(features, IsBaselineCandidate);
	}

	bool IsBaselineSelected(const std::string& shortName)
	{
		return std::ranges::find(SelectedBaselines().features, shortName) != SelectedBaselines().features.end();
	}

	void SetBaselineSelected(const std::string& shortName, bool selected)
	{
		const auto it = std::ranges::find(SelectedBaselines().features, shortName);
		if (selected && it == SelectedBaselines().features.end())
			SelectedBaselines().features.push_back(shortName);
		else if (!selected && it != SelectedBaselines().features.end())
			SelectedBaselines().features.erase(it);
	}

	/** @brief Clears the form and everything loaded from an existing preset; the type follows the live pipeline. */
	void ResetFormFields()
	{
		form = {};
		const auto* payloadProvider = UnifiedPresetCatalog::GetPayloadProvider(PresetType::E11);
		const bool e11Ready = payloadProvider && payloadProvider->CanExport();
		form.type = (e11Ready && payloadProvider->IsPipelineActive()) ?
		                PresetType::E11 :
		                PresetType::CS;
		form.csVersion = PresetCompatibility::CurrentCsVersionString();
		baselineSource = sceneSource = recordSource = ExportSource::User;
		baselineSelections = {};
		recordFiles = {};
		selectedRecords = {};
		for (auto& selection : selectedScenes)
			selection.emplace();
		sourceReadFailed = false;
		selectTypeTab = true;
		baselineSearch = {};
		sceneSearch = {};
		recordSearch = {};
		presetTags.clear();
		requiredPlugins.clear();
		collidingFiles.clear();
	}

	std::string JoinList(const std::vector<std::string>& values)
	{
		return values | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
	}

	/** @brief Fills empty fields from an existing preset and adopts its artwork, dropping any pending picks. */
	void PrefillFromExisting(const SceneSettingsManager::PresetMetadata& meta)
	{
		if (form.author.empty())
			form.author = meta.author;
		if (form.description.empty())
			form.description = meta.description;
		if (presetTags.empty())
			presetTags = JoinList(meta.tags);
		if (requiredPlugins.empty())
			requiredPlugins = JoinList(meta.requiredPlugins);
		if (!meta.version.empty())
			form.version = meta.version;
		if (!meta.csVersion.empty())
			form.csVersion = meta.csVersion;
		// Loaded features follow detection, so ones whose settings were removed drop out. Features not
		// loaded this session cannot be detected, so the pack's list is the only record of them.
		for (const auto& shortName : meta.requiredFeatures) {
			if (const auto* feature = Feature::FindFeatureByShortName(shortName);
				!shortName.empty() && (!feature || !feature->loaded) && !IsFeatureRequired(shortName))
				form.requiredFeatures.push_back(shortName);
		}
		form.logo = meta.logo;
		form.cover = meta.cover;
		form.screenshots = meta.screenshots;
		form.clearLogo = false;
		form.clearCover = false;
		form.clearScreenshots = false;
		form.logoSource.clear();
		form.coverSource.clear();
		form.screenshotSources.clear();
	}

	/** @brief Prefills from the pack the typed name targets, when one exists on disk. */
	void PrefillFromName()
	{
		const auto packId = Util::FileHelpers::SanitizeFileName(form.name);
		// Re-exporting a pack keeps the base settings it already carries unless the user unticks them.
		if (const auto* pack = UnifiedPresetCatalog::GetSingleton().FindPack(packId)) {
			for (const auto& shortName : pack->baselineFeatures) {
				if (auto* feature = Feature::FindFeatureByShortName(shortName); IsBaselineCandidate(feature) && SelectedBaselines().values.contains(shortName)) {
					SetBaselineSelected(shortName, true);
					if (const auto selected = BaseSettingsEditor::GetIncludedSettings(feature, packId))
						SelectedBaselines().paths[shortName] = *selected;
				}
			}
		}
		if (const auto meta = SceneSettingsManager::ReadPresetMetadata(Util::PathHelpers::GetUnifiedPackPath(packId)))
			PrefillFromExisting(*meta);
	}

	/** @brief Orphan and legacy Effects 11 installs live outside Presets/, and CS and E11 packs never take the other's export. */
	bool IsExportTarget(const UnifiedPresetCatalog::PackInfo& pack)
	{
		return pack.source == UnifiedPresetCatalog::SourceKind::UnifiedPack && pack.AcceptsExport(form.type);
	}

	/** @brief One pack row: name and type badges over author and version. @return Whether it was clicked. */
	bool DrawPickerRow(const UnifiedPresetCatalog::PackInfo& pack, bool selected)
	{
		ImGui::PushID(pack.id.c_str());
		const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
		const float rowPadding = ImGui::GetStyle().FramePadding.y;
		const float rowHeight = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing() + rowPadding * 2.0f;
		const bool clicked = Util::RoundedSelectable("##row", selected,
			ImGuiSelectableFlags_AllowOverlap | static_cast<ImGuiSelectableFlags>(ImGuiSelectableFlags_NoPadWithHalfSpacing), ImVec2(0.0f, rowHeight));

		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + Util::GetSelectableTextInset(), rowOrigin.y + rowPadding));
		ImGui::BeginGroup();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(pack.name.c_str());
		ImGui::SameLine();
		PresetsPageRenderer::DrawBackendBadges(pack.IsE11(), pack.IsCS(), pack.IsBaseline(), true);
		if (!pack.author.empty() || !pack.version.empty()) {
			const auto meta = pack.author.empty()  ? std::format("v{}", pack.version) :
			                  pack.version.empty() ? pack.author :
			                                         std::format("{} · v{}", pack.author, pack.version);
			ImGui::TextDisabled("%s", meta.c_str());
		}
		ImGui::EndGroup();

		// Park the cursor past the row so the next highlight cannot overlap this one.
		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y + rowHeight + ImGui::GetStyle().ItemSpacing.y));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
		ImGui::PopID();
		return clicked;
	}

	/** @brief Dropdown arrow after the name field, with a searchable list of the installed packs; picking one targets it as the export name. */
	void DrawPresetPicker()
	{
		const auto& packs = UnifiedPresetCatalog::GetSingleton().GetPacks();
		Util::DropdownArrowButton(kPickerPopupId, T(TKEY("scene_export_existing_tooltip"), "Export into an existing preset."),
			std::ranges::none_of(packs, IsExportTarget));
		if (!ImGui::BeginPopup(kPickerPopupId))
			return;
		const float scale = Util::GetUIScale();

		if (ImGui::IsWindowAppearing()) {
			pickerSearch.clear();
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ScenePresetExportPickerSearch", T("menu.presets.search", "Search presets..."), &pickerSearch);

		auto& catalog = UnifiedPresetCatalog::GetSingleton();
		const auto currentId = Util::FileHelpers::SanitizeFileName(form.name);
		const UnifiedPresetCatalog::PackInfo* picked = nullptr;
		if (ImGui::BeginChild("##ScenePresetExportPickerList", ImVec2(0.0f, kPickerListHeight * scale), ImGuiChildFlags_None)) {
			bool anyShown = false;
			for (const size_t index : catalog.Query(std::nullopt, pickerSearch)) {
				const auto& pack = catalog.GetPacks()[index];
				if (!IsExportTarget(pack))
					continue;
				anyShown = true;
				if (DrawPickerRow(pack, pack.id == currentId))
					picked = &pack;
			}
			if (!anyShown)
				ImGui::TextDisabled("%s", T(TKEY("scene_export_picker_empty"), "No presets match."));
		}
		ImGui::EndChild();

		if (picked) {
			form.name = picked->id;
			PrefillFromName();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	/// Compact CS version + feature checklist, sized like the artwork column beside it.
	void DrawCompatibilityBox()
	{
		ImGui::BeginChild("##SceneExportCompat", ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
		ImGui::TextUnformatted(T(TKEY("scene_export_compat"), "Compatibility"));
		Util::AddTooltip(T(TKEY("scene_export_compat_tooltip"),
			"Records the Community Shaders compatibility version, required features and plugins. Missing requirements appear in the Presets browser."));

		ImGui::TextUnformatted(T(TKEY("scene_export_cs_version"), "Community Shaders compatibility version"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ScenePresetExportCsVersion",
			T(TKEY("scene_export_cs_version_hint"), "e.g. 0.8.0"), &form.csVersion);
		Util::AddTooltip(T(TKEY("scene_export_cs_version_tooltip"),
			"The CS version this preset targets. Defaults to this build's synced CS version, independently of the {brand} release number."));

		ImGui::TextUnformatted(T(TKEY("scene_export_features"), "Required features"));
		const float listHeight = ImGui::GetTextLineHeightWithSpacing() * 5.5f + ImGui::GetStyle().FramePadding.y * 2.0f;
		if (ImGui::BeginChild("##SceneExportFeatures", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
			auto features = Feature::GetFeatureList();
			std::sort(features.begin(), features.end(), [](Feature* a, Feature* b) {
				return a->GetDisplayName() < b->GetDisplayName();
			});
			const char* usedLabel = T(TKEY("scene_export_feature_used"), "has settings");
			for (auto* feature : features) {
				if (!IsRequirableFeature(feature))
					continue;
				const auto shortName = feature->GetShortName();
				bool required = IsFeatureRequired(shortName);
				ImGui::PushID(shortName.c_str());
				if (ImGui::Checkbox(feature->GetDisplayName().c_str(), &required))
					SetFeatureRequired(shortName, required);
				if (HasExportedSettings(shortName)) {
					ImGui::SameLine();
					ImGui::TextDisabled("%s", usedLabel);
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%s",
			I18n::GetSingleton()->Format("cs_editor.scene_export_features_count",
									{ { "count", std::to_string(form.requiredFeatures.size()) } },
									"{count} selected")
				.c_str());
		Util::AddTooltip(T(TKEY("scene_export_features_tooltip"),
			"Selected contents add their required features automatically. Tick any additional requirements here. Core features are not listed."));

		ImGui::TextUnformatted(T(TKEY("scene_export_plugins"), "Required plugins"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ScenePresetExportPlugins",
			T(TKEY("scene_export_plugins_hint"), "e.g. Cathedral Weathers.esp"), &requiredPlugins);
		Util::AddTooltip(T(TKEY("scene_export_plugins_tooltip"),
			"Comma-separated .esp, .esm or .esl files this pack was made with. The Presets browser warns when one is not in the load order, but still lets the pack apply."));
		ImGui::EndChild();
	}

	/** @brief Splits on ',' or ';', trimming whitespace and dropping empty tags. */
	std::vector<std::string> ParseList(const std::string& text)
	{
		std::vector<std::string> tags;
		std::string current;
		const auto flush = [&] {
			while (!current.empty() && std::isspace(static_cast<unsigned char>(current.front())))
				current.erase(current.begin());
			while (!current.empty() && std::isspace(static_cast<unsigned char>(current.back())))
				current.pop_back();
			if (!current.empty())
				tags.push_back(current);
			current.clear();
		};
		for (char ch : text) {
			if (ch == ',' || ch == ';')
				flush();
			else
				current.push_back(ch);
		}
		flush();
		return tags;
	}

	std::string DisplayPath(const std::filesystem::path& path)
	{
		return Util::PathHelpers::ToUtf8(path.filename());
	}

	/** @brief Opens the Windows image picker. @return Whether any file was chosen. */
	bool BrowseImageFiles(bool allowMultiple, std::vector<std::filesystem::path>& outPaths)
	{
		outPaths.clear();
		constexpr DWORD kBufferChars = 32768;
		std::wstring buffer(kBufferChars, L'\0');

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		std::wstring filter = winrt::to_hstring(T(TKEY("scene_export_image_filter"), "Images (*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.dds)")).c_str();
		filter.push_back(L'\0');
		filter.append(L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.dds");
		filter.push_back(L'\0');
		filter.append(winrt::to_hstring(T(TKEY("scene_export_all_files_filter"), "All Files (*.*)")).c_str());
		filter.push_back(L'\0');
		filter.append(L"*.*");
		filter.append(2, L'\0');
		ofn.lpstrFilter = filter.c_str();
		ofn.lpstrFile = buffer.data();
		ofn.nMaxFile = kBufferChars;
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
		if (allowMultiple)
			ofn.Flags |= OFN_ALLOWMULTISELECT;

		if (!GetOpenFileNameW(&ofn))
			return false;

		const wchar_t* ptr = buffer.c_str();
		std::filesystem::path directory = ptr;
		ptr += directory.native().size() + 1;
		if (!*ptr) {
			// Single selection: buffer is the full path.
			outPaths.push_back(directory);
			return true;
		}
		while (*ptr) {
			outPaths.push_back(directory / ptr);
			ptr += std::wcslen(ptr) + 1;
		}
		return !outPaths.empty();
	}

	/** @brief Draws an artwork slot's status with Browse and Clear buttons; `multi` edits the screenshot list. */
	void DrawArtworkRow(const char* label, bool multi, std::filesystem::path* singleSource, bool& cleared,
		const std::string& existingRel)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const auto* browseLabel = T(TKEY("scene_export_browse"), "Browse");
		const auto* clearLabel = T(TKEY("scene_export_clear"), "Clear");
		ImGui::TextUnformatted(label);
		Util::AddTooltip(T(TKEY("scene_export_artwork_tooltip"),
			"Shown for this preset on the Presets page, and copied into the pack on export. Clear drops the current image."));

		std::string status;
		if (multi) {
			if (!form.screenshotSources.empty())
				status = I18n::GetSingleton()->Format("cs_editor.scene_export_artwork_selected",
					{ { "count", std::to_string(form.screenshotSources.size()) } }, "{count} file(s) selected");
			else if (!cleared && !form.screenshots.empty())
				status = I18n::GetSingleton()->Format("cs_editor.scene_export_artwork_existing",
					{ { "count", std::to_string(form.screenshots.size()) } }, "{count} existing");
			else
				status = T(TKEY("scene_export_artwork_none"), "None");
		} else if (singleSource && !singleSource->empty()) {
			status = DisplayPath(*singleSource);
		} else if (!cleared && !existingRel.empty()) {
			status = existingRel;
		} else {
			status = T(TKEY("scene_export_artwork_none"), "None");
		}

		const float buttonsWidth = ImGui::CalcTextSize(browseLabel).x + ImGui::CalcTextSize(clearLabel).x +
		                           style.FramePadding.x * 4.0f + style.ItemInnerSpacing.x;
		const float available = ImGui::GetContentRegionAvail().x;
		const bool buttonsInline = available >= buttonsWidth + style.ItemSpacing.x + ImGui::GetFrameHeight();
		BrowserUI::EllipsizedText(status.c_str(), buttonsInline ? available - buttonsWidth - style.ItemSpacing.x : available,
			ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if (buttonsInline)
			ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (ImGui::SmallButton(std::format("{}##Browse{}", browseLabel, label).c_str())) {
			std::vector<std::filesystem::path> picked;
			if (BrowseImageFiles(multi, picked)) {
				cleared = false;
				if (multi) {
					form.screenshotSources = std::move(picked);
				} else if (singleSource && !picked.empty()) {
					*singleSource = std::move(picked.front());
				}
			}
		}
		if (buttonsInline)
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		else
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(clearLabel).x + style.FramePadding.x * 2.0f);
		if (ImGui::SmallButton(std::format("{}##Clear{}", clearLabel, label).c_str())) {
			if (multi)
				form.screenshotSources.clear();
			else if (singleSource)
				singleSource->clear();
			cleared = true;
		}
	}

	/// A provider decides whether its dedicated Baseline option is compatible with this export type.
	bool CanIncludeDedicatedBaseline()
	{
		const auto* provider = GetDedicatedBaselineProvider();
		auto* feature = Feature::FindFeatureByShortName(GetDedicatedBaselineShortName());
		return provider && feature && feature->loaded && provider->CanExportBaseline(UnifiedPresetCatalog::GetPresetTypeName(form.type));
	}

	std::optional<bool> DrawSelectionControls(const char* id, std::array<char, kSearchBufferSize>& search)
	{
		ImGui::PushID(id);
		const auto* selectLabel = T("ui.select_all", "Select All");
		const auto* clearLabel = T("ui.clear_selection", "Clear Selection");
		const auto& style = ImGui::GetStyle();
		const ImVec2 sourceMin = ImGui::GetItemRectMin();
		const ImVec2 sourceMax = ImGui::GetItemRectMax();
		BrowserUI::SameLineIfFits(ImGui::CalcTextSize(selectLabel).x + ImGui::CalcTextSize(clearLabel).x +
								  style.FramePadding.x * 4.0f + style.ItemSpacing.x);
		if (ImGui::GetCursorScreenPos().y < sourceMax.y)
			ImGui::SetCursorScreenPos({ ImGui::GetCursorScreenPos().x, sourceMin.y + (sourceMax.y - sourceMin.y - ImGui::GetTextLineHeight()) * 0.5f });
		ImGui::BeginGroup();
		std::optional<bool> action;
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
			const SKSE::stl::scope_exit restorePadding([] { ImGui::PopStyleVar(); });
			if (Util::Button(selectLabel))
				action = true;
			const ImVec2 selectMin = ImGui::GetItemRectMin();
			const ImVec2 selectMax = ImGui::GetItemRectMax();
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(clearLabel).x + style.FramePadding.x * 2.0f);
			if (ImGui::GetCursorScreenPos().y < selectMax.y)
				ImGui::SetCursorScreenPos({ ImGui::GetCursorScreenPos().x, selectMin.y });
			if (Util::Button(clearLabel))
				action = false;
		}
		ImGui::EndGroup();
		BrowserUI::SearchField("##Search", search.data(), search.size(), T("ui.search", "Search..."), -FLT_MIN, false);
		ImGui::PopID();
		return action;
	}

	bool CanSelectBaseline(const std::string& name)
	{
		return name != GetDedicatedBaselineShortName() || CanIncludeDedicatedBaseline();
	}

	bool BeginContentSection(const char* title, const char* id, const char* description, size_t selected, size_t total)
	{
		{
			const auto& style = ImGui::GetStyle();
			ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, std::max(style.ChildBorderSize, ThemeManager::Constants::CONTROL_OUTLINE_WIDTH * Util::GetUIScale()));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, std::max(style.ChildRounding, style.FrameRounding));
			const SKSE::stl::scope_exit restoreStyle([] { ImGui::PopStyleVar(2); });
			if (!ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
				return false;
		}
		MenuFonts::ImFontGuard heading(MenuFonts::GetSectionHeadingFont());
		const auto label = std::format("{} ({}/{})###{}", title, selected, total, id);
		const bool open = ImGui::TreeNodeEx(label.c_str(),
			ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
		Util::AddTooltip(description);
		return open;
	}

	bool IsBaselineSettingSelected(const std::string& feature, const std::string& path)
	{
		const auto& selection = SelectedBaselines();
		const auto found = selection.paths.find(feature);
		return IsBaselineSelected(feature) && (found == selection.paths.end() || std::ranges::find(found->second, path) != found->second.end());
	}

	void SelectBaselineSetting(const std::string& feature, const std::string& path, bool selected)
	{
		auto& selection = SelectedBaselines();
		if (!selection.paths.contains(feature)) {
			auto& paths = selection.paths[feature];
			if (IsBaselineSelected(feature))
				for (const auto& setting : selection.settings.at(feature)) paths.push_back(setting.path);
		}
		auto& paths = selection.paths.at(feature);
		std::erase(paths, path);
		if (selected)
			paths.push_back(path);
		SetBaselineSelected(feature, !paths.empty());
	}

	/** @brief Features and their individual values share one searchable checklist. */
	void DrawBaselineFeatureList(float visibleLines)
	{
		DrawSourceSwitch("##BaselineSource", baselineSource);
		if (const auto action = DrawSelectionControls("##BaselineControls", baselineSearch)) {
			SelectedBaselines().features.clear();
			SelectedBaselines().paths.clear();
			if (*action)
				for (const auto& [name, values] : SelectedBaselines().values)
					if (CanSelectBaseline(name))
						SetBaselineSelected(name, true);
		}
		const float listHeight = ImGui::GetFrameHeightWithSpacing() * visibleLines + ImGui::GetStyle().WindowPadding.y * 2.0f;
		if (ImGui::BeginChild("##SceneExportBaselineFeatures", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
			auto features = Feature::GetFeatureList();
			std::ranges::sort(features, {}, [](Feature* feature) { return feature->GetDisplayName(); });
			bool any = false;
			for (auto* feature : features) {
				const auto name = feature->GetShortName();
				const auto found = SelectedBaselines().settings.find(name);
				if (found == SelectedBaselines().settings.end())
					continue;
				const auto& settings = found->second;
				const auto title = feature->GetDisplayName();
				const bool featureMatch = Util::StringMatchesSearch(title, baselineSearch.data());
				const auto matches = [&](const auto& setting) {
					return featureMatch || Util::StringMatchesSearch(setting.label, baselineSearch.data());
				};
				if (!std::ranges::any_of(settings, matches))
					continue;
				any = true;
				ImGui::PushID(name.c_str());
				auto disabled = Util::DisableGuard(!CanSelectBaseline(name));
				const auto count = std::ranges::count_if(settings, [&](const auto& setting) { return IsBaselineSettingSelected(name, setting.path); });
				int ticked = count == 0 ? 0 : static_cast<size_t>(count) == settings.size() ? 3 :
				                                                                              1;
				if (ImGui::CheckboxFlags("##IncludeFeature", &ticked, 3)) {
					SelectedBaselines().paths.erase(name);
					SetBaselineSelected(name, ticked == 3);
				}
				ImGui::SameLine();
				if (baselineSearch.front())
					ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				const auto label = std::format("{} ({}/{})###Settings", title, count, settings.size());
				if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
					for (const auto& setting : settings) {
						if (!matches(setting))
							continue;
						bool included = IsBaselineSettingSelected(name, setting.path);
						ImGui::PushID(setting.path.c_str());
						if (ImGui::Checkbox(setting.label.c_str(), &included))
							SelectBaselineSetting(name, setting.path, included);
						if (auto tooltip = Util::HoverTooltipWrapper())
							ImGui::TextWrapped("%s", Util::Settings::FormatValue(SelectedBaselines().values.at(name).at(json::json_pointer(setting.path))).c_str());
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			if (!any)
				ImGui::TextDisabled("%s", baselineSearch.front() ? T("ui.no_results", "No results") :
																   T(TKEY("scene_export_baseline_none"), "No saved feature edits are available from this source."));
		}
		ImGui::EndChild();
	}

	/** @brief Whether the form carries any selected base settings. */
	bool HasBaselinePayload()
	{
		return std::ranges::any_of(SelectedBaselines().features, [](const std::string& name) {
			return name != GetDedicatedBaselineShortName() || CanIncludeDedicatedBaseline();
		});
	}

	/** @brief The form as export info, with the sanitized name and parsed tags and plugins. */
	PresetExportInfo BuildExportInfo(const std::string& sanitizedName)
	{
		auto info = form;
		info.name = sanitizedName;
		info.tags = ParseList(presetTags);
		info.requiredPlugins = ParseList(requiredPlugins);
		// A pack that sets a feature's base settings needs that feature to apply them.
		auto require = [&](const std::string& shortName) {
			if (std::ranges::find(info.requiredFeatures, shortName) == info.requiredFeatures.end())
				info.requiredFeatures.push_back(shortName);
		};
		for (const auto& shortName : SelectedBaselines().features)
			if (shortName != GetDedicatedBaselineShortName() || CanIncludeDedicatedBaseline())
				require(shortName);
		info.sceneSource = SelectedSceneSource();
		info.selectedSceneSettings = selectedScenes[static_cast<size_t>(sceneSource)];
		if (form.type != PresetType::Baseline)
			for (const auto& setting : SceneSettingsManager::GetSingleton()->GetSceneExportSettings(info.sceneSource))
				if (!info.selectedSceneSettings || info.selectedSceneSettings->contains(setting.key))
					require(setting.entry.featureShortName);
		std::erase_if(info.requiredFeatures, IsCoreFeature);
		return info;
	}

	bool CaptureBaselines(PresetExportInfo& info)
	{
		try {
			for (const auto& shortName : SelectedBaselines().features) {
				if (shortName == GetDedicatedBaselineShortName() && !CanIncludeDedicatedBaseline())
					continue;
				const auto& settings = SelectedBaselines().values.at(shortName);
				if (const auto selected = SelectedBaselines().paths.find(shortName); selected != SelectedBaselines().paths.end())
					info.baselines[shortName] = Util::Settings::SelectSettingPaths(settings, selected->second);
				else
					info.baselines[shortName] = settings;
				if (const auto boot = SelectedBaselines().disableAtBoot.find(shortName); boot != SelectedBaselines().disableAtBoot.end())
					info.disableAtBoot[shortName] = boot->second;
			}
			if (info.type != PresetType::Baseline) {
				info.recordFiles.emplace();
				for (const auto& key : selectedRecords[static_cast<size_t>(recordSource)]) {
					const auto path = recordSource == ExportSource::User ?
					                      FormEditSources::GetFilePath(Util::PathHelpers::GetCommunityShaderPath(), key) :
					                      FormEditSources::ResolvePackFile(key);
					json document;
					if (!FormEditSources::ReadDocument(path, document))
						return false;
					info.recordFiles->push_back({ key, std::move(document) });
				}
			}
			return true;
		} catch (const std::exception& e) {
			logger::error("[ScenePresetExport] Could not capture base settings: {}", e.what());
			return false;
		}
	}

	/** @brief Preset type choice: CS, E11 or Baseline. */
	void DrawTypeSection()
	{
		const auto* provider = UnifiedPresetCatalog::GetPayloadProvider(PresetType::E11);
		const bool e11Ready = provider && provider->CanExport();
		if (!e11Ready && form.type == PresetType::E11) {
			form.type = PresetType::CS;
			selectTypeTab = true;
		}
		ImGui::BeginGroup();
		if (Util::BeginPillTabBar("##PresetExportType")) {
			DrawPresetTypeOption(T(TKEY("scene_export_type_cs"), "CS Preset"), PresetType::CS,
				T(TKEY("scene_export_type_cs_tooltip"), "Includes the Base Settings, scene files and record files you select."));
			ImGui::BeginDisabled(!e11Ready);
			DrawPresetTypeOption(T(TKEY("scene_export_type_e11"), "E11 Preset"), PresetType::E11,
				e11Ready ? T(TKEY("scene_export_type_e11_tooltip"), "Includes the active Effects 11 ENB files and the settings you select.") :
						   T(TKEY("scene_export_type_e11_unavailable"), "Load Effects 11 with a valid ENB preset (enbseries.ini + enbseries/) to export as E11."));
			ImGui::EndDisabled();
			DrawPresetTypeOption(T(TKEY("scene_export_type_baseline"), "Base Settings Preset"), PresetType::Baseline,
				T(TKEY("scene_export_type_baseline_tooltip"), "Base settings only: the features you tick, without scene layers or Effects 11 files."));
			Util::EndPillTabBar();
			selectTypeTab = false;
		}
		ImGui::EndGroup();
		BrowserUI::SameLineIfFits(Util::HelpMarkerWidth());
		DrawGuide();
	}

	void DrawRecordSelection()
	{
		DrawSourceSwitch("##RecordSource", recordSource);
		const auto index = static_cast<size_t>(recordSource);
		if (const auto action = DrawSelectionControls("##RecordControls", recordSearch))
			selectedRecords[index] = *action ? recordFiles[index] : FormEditSources::FormKeySet{};
		if (ImGui::BeginChild("##ExportRecords", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * kFeatureLines + ImGui::GetStyle().WindowPadding.y * 2.0f), ImGuiChildFlags_Borders)) {
			bool any = false;
			for (const auto& key : recordFiles[index]) {
				const auto label = key.first + " / " + key.second + ".json";
				if (!Util::StringMatchesSearch(label, recordSearch.data()))
					continue;
				any = true;
				bool selected = selectedRecords[index].contains(key);
				ImGui::PushID(label.c_str());
				if (ImGui::Checkbox("##IncludeRecord", &selected)) {
					if (selected)
						selectedRecords[index].insert(key);
					else
						selectedRecords[index].erase(key);
				}
				ImGui::SameLine();
				BrowserUI::EllipsizedText(label.c_str(), ImGui::GetContentRegionAvail().x, ImGui::GetStyleColorVec4(ImGuiCol_Text));
				ImGui::PopID();
			}
			if (!any)
				ImGui::TextDisabled("%s", recordSearch.front() ? T("ui.no_results", "No results") :
																 T("menu.presets.export_records_none", "No saved record files are available from this source."));
		}
		ImGui::EndChild();
	}

	/** @brief Name, version, author, description and tags. */
	void DrawMetadataFields()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::TextUnformatted(T(TKEY("scene_export_name"), "Preset name"));
		ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - style.ItemInnerSpacing.x);
		ImGui::InputText("##ScenePresetExportName", &form.name);
		if (ImGui::IsItemDeactivatedAfterEdit())
			PrefillFromName();
		Util::AddTooltip(T(TKEY("scene_export_name_tooltip"),
			"Folder name under Presets/. Naming an installed pack fills in its details and exports into it; the "
			"confirmation lists any files that get replaced."));
		DrawPresetPicker();

		ImGui::TextUnformatted(T(TKEY("scene_export_version"), "Version"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportVersion", &form.version);
		Util::AddTooltip(T(TKEY("scene_export_version_tooltip"),
			"MAJOR.MINOR.PATCH. 0.0.x is tagged Alpha and 0.x.x Beta on the Presets page; 1.0.0 and up is a full release."));

		ImGui::TextUnformatted(T(TKEY("scene_export_author"), "Author"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportAuthor", &form.author);

		ImGui::TextUnformatted(T(TKEY("scene_export_description"), "Description"));
		ImGui::InputTextMultiline("##ScenePresetExportDescription", &form.description,
			ImVec2(-1.0f, ImGui::GetTextLineHeight() * kDescriptionLines + style.FramePadding.y * 2.0f));

		ImGui::TextUnformatted(T(TKEY("scene_export_tags"), "Tags (comma-separated)"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportTags", &presetTags);
		Util::AddTooltip(T(TKEY("scene_export_tags_tooltip"), "Example: interior, weather, cinematic"));
	}

	/** @brief Artwork slots beside the compatibility box. */
	void DrawArtworkAndCompatibility()
	{
		if (!ImGui::BeginTable("##SceneExportArtCompat", 2, ImGuiTableFlags_SizingStretchSame))
			return;
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		DrawArtworkRow(T(TKEY("scene_export_logo"), "Logo"), false, &form.logoSource, form.clearLogo, form.logo);
		DrawArtworkRow(T(TKEY("scene_export_cover"), "Cover (poster)"), false, &form.coverSource, form.clearCover,
			form.cover);
		DrawArtworkRow(T(TKEY("scene_export_screenshots"), "Screenshots"), true, nullptr, form.clearScreenshots, {});

		ImGui::TableNextColumn();
		DrawCompatibilityBox();
		ImGui::EndTable();
	}

	/** @brief The Export button with the reasons it can be disabled, and the confirmation it raises. */
	void DrawExportButton(SceneSettingsManager* manager)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		auto sanitizedName = Util::FileHelpers::SanitizeFileName(form.name);
		const bool reservedName = SceneSettingsManager::IsReservedPresetName(sanitizedName);
		const bool validVersion = SceneSettingsManager::IsValidPresetVersion(form.version);
		const auto* existingPack = UnifiedPresetCatalog::GetSingleton().FindPack(sanitizedName);
		const bool typeMismatch = existingPack && !existingPack->AcceptsExport(form.type);
		const bool legacyTarget = existingPack && existingPack->source != UnifiedPresetCatalog::SourceKind::UnifiedPack;
		// A CS export with no scene settings, base settings or form edits would write an empty pack.
		const auto& selected = selectedScenes[static_cast<size_t>(sceneSource)];
		const bool hasScenes = SceneFilesPanel::HasExportSelection(selected, sceneSource == ExportSource::Preset);
		const bool nothingToExport = form.type == PresetType::Baseline ? !HasBaselinePayload() :
		                                                                 form.type == PresetType::CS && !hasScenes && !HasBaselinePayload() && selectedRecords[static_cast<size_t>(recordSource)].empty();
		const bool pendingSceneEdit = form.type != PresetType::Baseline && manager->HasUnsavedChanges();
		ImGui::BeginDisabled(sanitizedName.empty() || reservedName || !validVersion || typeMismatch || legacyTarget || nothingToExport || pendingSceneEdit || sourceReadFailed);
		if (Util::Button(T(TKEY("scene_export_confirm"), "Export"))) {
			collidingFiles = SceneSettingsManager::FindPresetFiles(sanitizedName);
			if (form.type != PresetType::Baseline)
				std::ranges::copy(FormEditSources::ListPackFormFiles(Util::PathHelpers::GetUnifiedPackPath(sanitizedName)),
					std::back_inserter(collidingFiles));
			exportConfirmation.title = T(TKEY("scene_export_title"), "Export preset");
			exportConfirmation.message = collidingFiles.empty() ?
			                                 std::vformat(T(TKEY("scene_export_create_message"),
															  "Write your settings out as the preset '{}'?"),
												 std::make_format_args(sanitizedName)) :
			                                 DescribeCollision(sanitizedName, collidingFiles);
			exportConfirmation.confirmLabel = collidingFiles.empty() ?
			                                      T(TKEY("scene_export_confirm"), "Export") :
			                                      T(TKEY("scene_export_replace_confirm"), "Delete and replace");
			exportConfirmation.cancelLabel = T(TKEY("cancel"), "Cancel");
			exportConfirmation.Request();
		}
		ImGui::EndDisabled();
		if (reservedName)
			Util::AddTooltip(T(TKEY("scene_export_hidden_name"), "Names starting with '_' or '.' are hidden from the Presets browser."),
				Util::kTooltipWhenDisabled);
		else if (!validVersion)
			Util::AddTooltip(T(TKEY("scene_export_invalid_version"), "Use a MAJOR.MINOR.PATCH version, such as 1.0.0."),
				Util::kTooltipWhenDisabled);
		else if (legacyTarget)
			Util::AddTooltip(T("menu.presets.legacy_export_target", "Legacy imports cannot be export destinations. Choose a new preset name."), Util::kTooltipWhenDisabled);
		else if (typeMismatch)
			Util::AddTooltip(T(TKEY("scene_export_type_mismatch"),
								 "A preset with this name already exists for the other pipeline. Pick a different name or switch the preset type."),
				Util::kTooltipWhenDisabled);
		else if (nothingToExport)
			Util::AddTooltip(T(TKEY("scene_export_nothing"),
								 "Nothing to export yet: author scene settings, edit forms, or tick base settings to include."),
				Util::kTooltipWhenDisabled);
		else if (pendingSceneEdit)
			Util::AddTooltip(T("feature.scene_manager.edit.finish_pending", "Save or discard unsaved scene settings before copying them, changing presets, or reloading."),
				Util::kTooltipWhenDisabled);
		else if (sourceReadFailed)
			Util::AddTooltip(T("menu.presets.export_source_failed", "Could not read saved export sources. Check the log and reopen Create Preset."), Util::kTooltipWhenDisabled);

		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (Util::Button(T(TKEY("cancel"), "Cancel")))
			PresetsPageRenderer::ShowLibrary();
	}

	/** @brief Preset contents, descriptive fields and export action. */
	void DrawForm(SceneSettingsManager& manager)
	{
		DrawTypeSection();
		if (ImGui::CollapsingHeader(T(TKEY("scene_export_contents"), "Contents"), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent();
			const auto* baselineLabel = baselineSource == ExportSource::User ?
			                                T(TKEY("scene_export_baseline_features"), "Base Settings to include (your saved feature page edits):") :
			                                T("menu.presets.export_baseline_preset", "Base Settings to include (enabled preset values):");
			const auto availableBaselines = std::ranges::count_if(SelectedBaselines().values, [](const auto& entry) { return CanSelectBaseline(entry.first); });
			const auto selectedBaselines = std::ranges::count_if(SelectedBaselines().features, CanSelectBaseline);
			if (BeginContentSection(T("menu.presets.filter_baseline", "Base Settings"), "ExportBaselinePanel", baselineLabel,
					static_cast<size_t>(selectedBaselines), static_cast<size_t>(availableBaselines)))
				DrawBaselineFeatureList(kFeatureLines);
			ImGui::EndChild();
			if (form.type != PresetType::Baseline) {
				const auto* sceneLabel = sceneSource == ExportSource::User ?
				                             T("menu.presets.scene_selection", "Scene files to include (your saved Scene Manager edits):") :
				                             T("menu.presets.export_scene_preset", "Scene files to include (enabled preset values):");
				const auto sceneCounts = SceneFilesPanel::GetExportFileCounts(selectedScenes[static_cast<size_t>(sceneSource)], sceneSource == ExportSource::Preset);
				if (BeginContentSection(T("menu.presets.export_scene_title", "Scene Files"), "ExportScenePanel", sceneLabel,
						sceneCounts.selected, sceneCounts.total)) {
					DrawSourceSwitch("##SceneSource", sceneSource);
					const auto action = DrawSelectionControls("##SceneControls", sceneSearch);
					SceneFilesPanel::DrawExportSelection(selectedScenes[static_cast<size_t>(sceneSource)], sceneSource == ExportSource::Preset,
						sceneSearch.data(), action, kFeatureLines);
				}
				ImGui::EndChild();
				const auto* recordLabel = recordSource == ExportSource::User ?
				                              T("menu.presets.record_selection", "Record files to include (your saved OS Editor edits):") :
				                              T("menu.presets.export_record_preset", "Record files to include (enabled preset files):");
				const auto recordIndex = static_cast<size_t>(recordSource);
				if (BeginContentSection(T("menu.presets.export_record_title", "Record Files"), "ExportRecordPanel", recordLabel,
						selectedRecords[recordIndex].size(), recordFiles[recordIndex].size()))
					DrawRecordSelection();
				ImGui::EndChild();
			}
			ImGui::Unindent();
		}
		if (ImGui::CollapsingHeader(T(TKEY("scene_export_details"), "Preset details"), ImGuiTreeNodeFlags_DefaultOpen))
			DrawMetadataFields();
		if (ImGui::CollapsingHeader(T(TKEY("scene_export_artwork_compatibility"), "Artwork and compatibility")))
			DrawArtworkAndCompatibility();
		ImGui::Separator();
		DrawExportButton(&manager);
	}
}

bool PresetExportPanel::CanExport()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return false;
	if (manager->HasAnyUserEntries() || !manager->GetOverwriteModNames().empty())
		return true;
	// E11-only export: no scene layer yet, but the live Effects 11 preset can still be written out.
	// Post Processing alone is enough: its base settings can always go out as a preset.
	const auto* baselineProvider = GetDedicatedBaselineProvider();
	if ((baselineProvider && baselineProvider->CanExportBaseline(UnifiedPresetCatalog::GetPresetTypeName(PresetType::Baseline))) || HasAnyBaselineCandidate())
		return true;
	const auto* provider = UnifiedPresetCatalog::GetPayloadProvider(PresetType::E11);
	return (provider && provider->CanExport()) || FormEditSources::HasEffectiveFormEdits();
}

void PresetExportPanel::Open()
{
	requestedExport = ExportRequest{ RequestKind::NewPreset, {} };
	PresetsPageRenderer::OpenExport();
}

void PresetExportPanel::OpenBaseline(const std::string& featureShortName, std::optional<std::vector<std::string>> pageKeys)
{
	requestedExport = ExportRequest{ RequestKind::BaseSettings, featureShortName, std::move(pageKeys) };
	PresetsPageRenderer::OpenExport();
}

void PresetExportPanel::DrawPage()
{
	if (!initialized || requestedExport) {
		const auto request = std::exchange(requestedExport, std::nullopt).value_or(ExportRequest{ RequestKind::NewPreset, {} });
		ResetFormFields();
		UnifiedPresetCatalog::GetSingleton().Discover();
		try {
			for (auto* feature : Feature::GetFeatureList()) {
				if (!IsBaselineCandidate(feature))
					continue;
				const auto name = feature->GetShortName();
				const auto saved = globals::state->ReadSavedFeatureSettings(*feature);
				if (!saved.personalEdits.empty())
					baselineSelections[0].values[name] = saved.personalEdits;
				if (!feature->IsAlwaysEnabled()) {
					baselineSelections[0].disableAtBoot[name] = globals::state->IsFeatureDisabled(name);
					for (const auto& id : UnifiedPresetCatalog::GetSingleton().GetBaselinePackIds())
						if (const auto* pack = UnifiedPresetCatalog::GetSingleton().FindPack(id); pack && pack->disableAtBoot.contains(name))
							baselineSelections[1].disableAtBoot[name] = pack->disableAtBoot.at(name);
				}
				auto presets = BaselineManager::GetSingleton()->BuildFeatureBaseline(name, saved.values);
				if (!presets.empty())
					baselineSelections[1].values[name] = std::move(presets);
			}
			if (request.pageKeys) {
				for (auto& selection : baselineSelections) {
					selection.disableAtBoot.erase(request.feature);
					const auto found = selection.values.find(request.feature);
					if (found == selection.values.end())
						continue;
					auto& values = found->second;
					std::erase_if(values.get_ref<json::object_t&>(), [&](const auto& entry) {
						return std::ranges::find(*request.pageKeys, entry.first) == request.pageKeys->end();
					});
				}
			}
			for (auto& selection : baselineSelections)
				std::erase_if(selection.values, [&](const auto& entry) {
					auto settings = Util::Settings::GetExportSettings(entry.first, entry.second);
					if (settings.empty())
						return true;
					selection.settings[entry.first] = std::move(settings);
					return false;
				});
			recordFiles[0] = FormEditSources::ScanKeys(Util::PathHelpers::GetCommunityShaderPath());
			recordFiles[1] = FormEditSources::GetPackFormKeys();
		} catch (const std::exception& error) {
			sourceReadFailed = true;
			logger::error("[Presets] Could not read export sources: {}", error.what());
		}
		if (request.kind == RequestKind::BaseSettings) {
			form.type = PresetType::Baseline;
			form.requiredFeatures.clear();
			if (IsBaselineCandidate(Feature::FindFeatureByShortName(request.feature)) && SelectedBaselines().values.contains(request.feature))
				SetBaselineSelected(request.feature, true);
		}
		initialized = true;
	}
	if (auto* manager = SceneSettingsManager::GetSingleton())
		DrawForm(*manager);
}

bool PresetExportPanel::IsOpen()
{
	return PresetsPageRenderer::IsPopupOpen() || exportConfirmation.IsOpen() || BaseSettingsEditor::IsOpen() || SceneFilesPanel::IsOpen();
}

void PresetExportPanel::Dismiss()
{
	requestedExport.reset();
	exportConfirmation = {};
	BaseSettingsEditor::Dismiss();
}

void PresetExportPanel::Draw()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return;

	if (exportConfirmation.Draw()) {
		auto sanitizedName = Util::FileHelpers::SanitizeFileName(form.name);
		auto info = BuildExportInfo(sanitizedName);
		const bool exported = CaptureBaselines(info) && manager->ExportPreset(info);
		ReportExportResult(sanitizedName, exported);
		if (exported) {
			UnifiedPresetCatalog::GetSingleton().Discover();
			BaselineManager::GetSingleton()->Refresh();
			globals::state->ReapplySettingsLayers();
			auto& catalog = UnifiedPresetCatalog::GetSingleton();
			if (info.type != PresetType::Baseline) {
				catalog.ApplyPack(sanitizedName, true, true);
			} else {
				catalog.ReapplyIfInUse(sanitizedName);
			}
			PresetsPageRenderer::ShowLibrary();
		}
	}
	BaseSettingsEditor::DrawExport();
}

#undef I18N_KEY_PREFIX
