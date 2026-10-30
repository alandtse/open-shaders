#include "FeatureOverwritesPanel.h"

#include "BaselineManager.h"
#include "CSEditor/Browser/BrowserWidgets.h"
#include "Feature.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneSettingsManager.h"
#include "SettingsOverrideManager.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <format>
#include <tuple>
#include <unordered_map>

#define I18N_KEY_PREFIX "feature.scene_manager.overwrites."

namespace
{
	constexpr int kColumnCount = 5;
	constexpr int kActionButtonCount = 2;

	/** @brief One applied file, copied so actions that rediscover mid-frame are safe. */
	struct OverwriteRow
	{
		std::string feature;
		std::string featureName;
		std::string packId;
		std::string filePath;
		bool legacyOverride = false;
		/// Newline-joined keys the feature does not have; while any exist the file is not applied.
		std::string unknownKeys;
		size_t settingCount = 0;
	};

	SceneSettingsManager::RevisionCache<std::vector<OverwriteRow>> rowCache;
	bool notAppliedOnly = false;
	Util::ConfirmationPopup deletePopup;
	std::string deletePath;
	bool actionFailed = false;

	std::vector<OverwriteRow> BuildRows()
	{
		auto* manager = BaselineManager::GetSingleton();
		std::vector<OverwriteRow> rows;
		for (const auto& info : manager->GetFiles()) {
			if (!manager->IsApplicable(info))
				continue;
			auto* feature = Feature::FindFeatureByShortName(info.featureName);
			OverwriteRow row{ .feature = feature ? feature->GetDisplayName() : info.featureName,
				.featureName = info.featureName,
				.packId = info.packId,
				.filePath = info.filePath,
				.settingCount = Util::Settings::GetExportSettings(info.featureName, info.values).size() };
			auto unknownKeys = info.unknownKeys;
			if (feature && unknownKeys.empty()) {
				// Rediscovery clears the verdict until the next load applies the file, so recheck against the live shape.
				json shape;
				feature->SaveSettings(shape);
				Util::Settings::CollectUnknownSettingKeys(info.values, shape, "", unknownKeys);
			}
			for (const auto& key : unknownKeys)
				row.unknownKeys += (row.unknownKeys.empty() ? "" : "\n") + key;
			rows.push_back(std::move(row));
		}
		auto* overrides = SettingsOverrideManager::GetSingleton();
		for (const auto& info : overrides->GetOverrides()) {
			if (!info.enabled || !overrides->IsApplicable(info))
				continue;
			auto* feature = Feature::FindFeatureByShortName(info.featureName);
			OverwriteRow row{ .feature = feature ? feature->GetDisplayName() : T("feature.feature_overwrites.global", "Global"),
				.featureName = info.featureName,
				.packId = info.modName,
				.filePath = info.filePath,
				.legacyOverride = true,
				.settingCount = Util::Settings::GetExportSettings(info.featureName, info.overrideData).size() };
			rows.push_back(std::move(row));
		}
		std::ranges::sort(rows, {}, [](const OverwriteRow& row) { return std::tie(row.feature, row.packId); });
		return rows;
	}

	/** @brief Export button, and a chip narrowing the table to files that are not applied. */
	void DrawToolbar(const std::vector<OverwriteRow>& rows)
	{
		if (Util::Button(T(TKEY("export.button"), "Export Settings")))
			FeatureOverwritesPanel::BeginExport();
		Util::AddTooltip(T(TKEY("export.toolbar_tooltip"),
			"Choose a feature override file or a Base Settings preset, then export selected settings without scene-specific values."));

		auto count = static_cast<size_t>(std::ranges::count_if(rows, [](const OverwriteRow& row) { return !row.unknownKeys.empty(); }));
		notAppliedOnly &= count != 0;
		if (count == 0)
			return;
		const auto label = std::vformat(T(TKEY("not_applied_count"), "{} not applied"), std::make_format_args(count));
		const ImVec4 warning = Util::Colors::GetWarning();
		BrowserUI::RightAlign(BrowserUI::MeasureChip(label.c_str(), true));
		if (BrowserUI::Chip("##notApplied", label.c_str(), Icons::TI(ICON_TI_X), &warning, notAppliedOnly ? &warning : nullptr))
			notAppliedOnly = !notAppliedOnly;
		Util::AddTooltip(notAppliedOnly ? T("cs_editor.scene_copy_filter_clear_tooltip", "Click to list every setting again.") :
										  T(TKEY("not_applied_tooltip"), "These name settings their feature does not have, so they are skipped. Click to list only them."));
	}

	void DrawSource(const OverwriteRow& row)
	{
		BrowserUI::EllipsizedText(row.packId.c_str(), 0.0f, ImGui::GetStyleColorVec4(ImGuiCol_Text));
		Util::AddTooltip(row.filePath.c_str());
	}

	void DrawStatus(const OverwriteRow& row)
	{
		if (!row.unknownKeys.empty()) {
			ImGui::TextColored(Util::Colors::GetWarning(), "%s", T(TKEY("status_not_applied"), "Not applied"));
			const auto tooltip = std::format("{}\n{}",
				T(TKEY("status_not_applied_tooltip"), "Names settings this feature does not have, so none of it is applied:"), row.unknownKeys);
			Util::AddTooltip(tooltip.c_str());
		} else {
			ImGui::TextColored(Util::Colors::GetSuccess(), "%s", T(TKEY("status_applied"), "Applied"));
		}
	}

	/** @brief Edit, then Delete for an override or Open folder for a Baseline pack. */
	void DrawActions(const OverwriteRow& row)
	{
		auto* feature = Feature::FindFeatureByShortName(row.featureName);
		const bool editable = feature && FeatureOverwritesPanel::HasExportableSettings(feature);
		{
			const Util::DisableGuard notEditable(!editable);
			if (BrowserUI::IconButton("##edit", Icons::TI(ICON_TI_PENCIL), editable ? T(TKEY("edit_tooltip"), "Edit: opens the export on this file with its settings ticked.") : T(TKEY("edit_global_tooltip"), "Global overrides span several features and cannot be edited here."))) {
				if (row.legacyOverride)
					FeatureOverwritesPanel::BeginOverrideExport(feature, row.packId);
				else
					FeatureOverwritesPanel::BeginExportInto(feature, row.packId);
			}
		}
		ImGui::SameLine();
		if (row.legacyOverride) {
			if (BrowserUI::IconButton("##delete", Icons::TI(ICON_TI_TRASH), T("feature.feature_overwrites.delete", "Delete"), false,
					ImGui::GetColorU32(Util::Colors::GetError()))) {
				deletePath = row.filePath;
				const auto filename = std::filesystem::path(row.filePath).filename().string();
				deletePopup.title = T("feature.feature_overwrites.delete_title", "Delete Feature Override?");
				deletePopup.message = std::vformat(T("feature.feature_overwrites.delete_message", "Delete '{0}' from disk and remove orphaned user override entries? Saved personal settings are kept."), std::make_format_args(filename));
				deletePopup.confirmLabel = T("feature.feature_overwrites.delete", "Delete");
				deletePopup.cancelLabel = T("feature.feature_overwrites.cancel", "Cancel");
				deletePopup.Request();
			}
		} else if (BrowserUI::IconButton("##folder", Icons::TI(ICON_TI_FOLDER_OPEN),
					   T(TKEY("open_pack_tooltip"), "Open the pack folder. Remove the pack from the Presets page.")))
			UnifiedPresetCatalog::GetSingleton().OpenPackFolder(row.packId);
	}

	void DrawRow(const OverwriteRow& row)
	{
		const float rowHeight = BrowserUI::RowHeight();
		ImGui::PushID(row.filePath.c_str());
		ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
		ImGui::TableNextColumn();
		const float stripeRight = BrowserUI::RowStripe(ImGui::GetColorU32(row.unknownKeys.empty() ? Util::Colors::GetSuccess() : Util::Colors::GetWarning()), rowHeight);
		ImGui::AlignTextToFramePadding();
		const ImVec2 labelCursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(std::max(labelCursor.x, stripeRight + ImGui::GetStyle().ItemInnerSpacing.x), labelCursor.y));
		BrowserUI::EllipsizedText(row.feature.c_str(), 0.0f, ImGui::GetStyleColorVec4(ImGuiCol_Text));

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		DrawSource(row);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%zu", row.settingCount);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		DrawStatus(row);

		ImGui::TableNextColumn();
		DrawActions(row);
		ImGui::PopID();
	}

	void DrawTable(const std::vector<OverwriteRow>& rows)
	{
		BrowserUI::RowShadeScope shade;
		if (!ImGui::BeginTable("##OverwriteFiles", kColumnCount, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit,
				ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
			return;
		const float actionsWidth = BrowserUI::IconButtonSize() * kActionButtonCount + ImGui::GetStyle().ItemSpacing.x * (kActionButtonCount - 1);
		ImGui::TableSetupColumn(T(TKEY("feature"), "Feature"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("source"), "Source"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("settings"), "Settings"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("status"), "Status"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int column = 0; column < kColumnCount; ++column) {
			if (!ImGui::TableSetColumnIndex(column))
				continue;
			ImGui::PushID(column);
			ImGui::TableHeader(ImGui::TableGetColumnName());
			ImGui::PopID();
		}
		for (const auto& row : rows)
			if (!notAppliedOnly || !row.unknownKeys.empty())
				DrawRow(row);
		ImGui::EndTable();
	}
}

bool FeatureOverwritesPanel::HasExportableSettings(Feature* feature)
{
	assert(feature);
	// The settings schema is fixed per feature, so one SaveSettings probe is enough.
	static std::unordered_map<Feature*, bool> cache;
	auto [it, inserted] = cache.try_emplace(feature, false);
	if (inserted && feature->UsesMainSettings()) {
		json settings;
		feature->SaveSettings(settings);
		it->second = settings.is_object() && !settings.empty();
	}
	return it->second;
}

void FeatureOverwritesPanel::Draw()
{
	auto* manager = BaselineManager::GetSingleton();
	static std::uint64_t overrideRevision = 0;
	const auto currentOverrideRevision = SettingsOverrideManager::GetSingleton()->GetRevision();
	const auto& rows = rowCache.Get(manager->GetRevision(), BuildRows, currentOverrideRevision != overrideRevision);
	overrideRevision = currentOverrideRevision;

	DrawToolbar(rows);
	if (actionFailed)
		Util::Text::WrappedError("%s", T("feature.feature_overwrites.action_failed", "Could not update the override. Check the log and try again."));

	if (rows.empty())
		BrowserUI::EmptyState(T(TKEY("empty_baseline"), "No feature override or Base Settings preset files in use."),
			T(TKEY("empty_baseline_hint"), "Export settings to create an override or a preset, or enable one on the Presets page."));
	else
		DrawTable(rows);

	if (deletePopup.Draw())
		actionFailed = !SettingsOverrideManager::GetSingleton()->DeleteFile(deletePath);
	DrawExport();
}

#undef I18N_KEY_PREFIX
