#include "PresetsPageRenderer.h"
#include "PCH.h"

#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "Feature.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/IconLoader.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/Presets/BaseSettingsEditor.h"
#include "Menu/Presets/PresetExportPanel.h"
#include "Menu/Presets/SceneFilesPanel.h"
#include "Menu/ThemeManager.h"
#include "Presets/PresetCompatibility.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "State.h"
#include "Util.h"
#include "Utils/Format.h"
#include "Utils/UI.h"
#include "Utils/UIAnim.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

std::optional<UnifiedPresetCatalog::PresetType> PresetsPageRenderer::typeFilter;
char PresetsPageRenderer::searchBuffer[128] = {};
std::string PresetsPageRenderer::selectedPackId;
bool PresetsPageRenderer::discovered = false;
float PresetsPageRenderer::stackedListRatio = ThemeManager::Constants::PRESET_STACKED_LIST_RATIO;
std::string PresetsPageRenderer::lightboxPackId;
int PresetsPageRenderer::lightboxImageIndex = -1;
bool PresetsPageRenderer::lightboxSuppressClose = false;

namespace
{
	enum class PresetPopup
	{
		None,
		Create,
		Manage
	};
	PresetPopup requestedPopup = PresetPopup::None;
	bool popupOpen = false;
	bool closePopup = false;
	constexpr const char* kCreatePopup = "###CreatePreset";
	constexpr const char* kManagePopup = "###ManagePresetSettings";
	bool g_showThemes = false;
	std::string g_selectedTheme;
	void DrawRoundedImage(ImDrawList* dl, ImTextureID texture, const ImVec2& p0, const ImVec2& p1, float rounding, ImU32 tint = IM_COL32_WHITE);

	ImVec4 ThemeAccent(const ThemeManager::ThemeInfo& info)
	{
		std::array<ImVec4, ImGuiCol_COUNT> palette{};
		Menu::PaletteFromJson(info.themeData.contains("Theme") ? info.themeData["Theme"] : info.themeData, palette);
		ImVec4 accent = palette[ImGuiCol_Header];
		accent.w = 1.0f;
		return accent;
	}

	/** @brief Draws the optional theme logo or a palette glyph in the theme accent. */
	void DrawThemeLogo(const ThemeManager::ThemeInfo& info, float size)
	{
		ImGui::Dummy(ImVec2(size, size));
		const auto& logo = Util::IconLoader::GetIcons().logoMonochrome;
		// The logo is stored large; the mip nearest the drawn size keeps small list icons from aliasing
		if (logo.texture)
			DrawRoundedImage(ImGui::GetWindowDrawList(), reinterpret_cast<ImTextureID>(Util::IconLoader::ViewForSize(logo.texture, ImVec2(size, size))),
				ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 0.0f, ImGui::GetColorU32(ThemeAccent(info)));
		else
			Icons::DrawCenteredGlyph(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImVec2(size, size),
				size, Icons::TI(ICON_TI_PALETTE), ImGui::GetColorU32(ThemeAccent(info)));
	}

	void OpenThemesFolder()
	{
		// The game's Data path only exists inside a mod manager's virtual file system, which Explorer cannot see;
		// open the folder on disk that holds the plugin instead
		std::filesystem::path themesPath = Util::PathHelpers::GetThemesRealPath();
		std::error_code ec;
		if (Util::PathHelpers::GetRootRealPath().empty() || !std::filesystem::is_directory(themesPath, ec))
			themesPath = ThemeManager::GetSingleton()->GetThemesDirectory();
		ShellExecuteW(nullptr, L"open", themesPath.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	const char* BackendLabelE11(bool compact)
	{
		return compact ? "E11" : "Effects 11";
	}

	const char* BackendLabelCS(bool compact)
	{
		return compact ? "CS" : "CS Presets";
	}

	const char* BackendLabelBaseline(bool)
	{
		return T("menu.presets.filter_baseline", "Base Settings");
	}

	float BadgeGap()
	{
		return ImGui::GetStyle().ItemInnerSpacing.x;
	}

	/** @brief Add a badge gap to width unless this is the first badge. */
	void AppendBadgeGap(float& width, bool& needGap)
	{
		if (needGap)
			width += BadgeGap();
		needGap = true;
	}

	/** @brief A preset type label with its informational text color. */
	struct BackendBadge
	{
		const char* label;
		ImVec4 color;
	};

	/** @brief Calls visit with a BackendBadge for each backend present, in display order. */
	template <typename Visitor>
	void ForEachBackendBadge(bool hasE11, bool hasCSPresets, bool hasBaseline, bool compact, Visitor&& visit)
	{
		const auto& palette = globals::menu->GetTheme().StatusPalette;
		if (hasE11)
			visit(BackendBadge{ BackendLabelE11(compact), palette.InfoColor });
		if (hasCSPresets)
			visit(BackendBadge{ BackendLabelCS(compact), palette.Warning });
		if (hasBaseline)
			visit(BackendBadge{ BackendLabelBaseline(compact), palette.SuccessColor });
	}

	bool CanApplyPack(const UnifiedPresetCatalog::PackInfo& pack)
	{
		return pack.valid && (pack.hasEffects11 || pack.hasCSPresets || pack.hasBaseline || pack.hasFormEdits);
	}

	void ApplyPack(const UnifiedPresetCatalog::PackInfo& pack)
	{
		if (UnifiedPresetCatalog::GetSingleton().ApplyPack(pack.id, true, true))
			logger::info("[Presets] Applied pack '{}'", pack.id);
	}

	Util::ConfirmationPopup applyConfirmation;
	Util::ConfirmationPopup deleteConfirmation;
	std::string pendingDeletePackId;
	bool deleteFailed = false;
	/// Pack awaiting applyConfirmation; looked up again on confirm since a rescan may replace it.
	std::string pendingApplyPackId;

	/** @brief Applies the pack, first asking for confirmation when its compatibility check found problems. */
	void RequestApplyPack(const UnifiedPresetCatalog::PackInfo& pack)
	{
		const auto conflicts = UnifiedPresetCatalog::GetSingleton().GetConflictingPacks(pack.id);
		if (!pack.compat.HasIssues() && conflicts.empty()) {
			ApplyPack(pack);
			return;
		}
		std::string message;
		for (const auto* line : { &pack.compat.versionMessage, &pack.compat.featuresMessage }) {
			if (!line->empty())
				message += *line + "\n";
		}
		if (!conflicts.empty()) {
			message += T("menu.presets.replace_conflicts", "Applying this preset will disable these conflicting presets:") + std::string("\n");
			for (const auto& id : conflicts) {
				const auto* other = UnifiedPresetCatalog::GetSingleton().FindPack(id);
				message += (other ? other->name : id) + "\n";
			}
		}
		message += T("menu.presets.compat_apply_anyway", "Apply it anyway?");

		pendingApplyPackId = pack.id;
		applyConfirmation.title = T("menu.presets.compat_apply_title", "Compatibility warning");
		applyConfirmation.message = std::move(message);
		applyConfirmation.confirmLabel = T("menu.presets.apply", "Enable Preset");
		applyConfirmation.cancelLabel = T("ui.cancel", "Cancel");
		applyConfirmation.Request();
	}

	void DrawRoundedImage(ImDrawList* dl, ImTextureID texture, const ImVec2& p0, const ImVec2& p1, float rounding, ImU32 tint)
	{
		dl->AddImageRounded(texture, p0, p1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint, rounding);
	}

	/** @brief Circular translucent arrow button centered on center; returns true when clicked. */
	bool DrawCarouselArrow(const char* id, Icons::GlyphRef icon, const ImVec2& center, float diameter)
	{
		ImGui::SetCursorScreenPos(ImVec2(center.x - diameter * 0.5f, center.y - diameter * 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, diameter * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, ThemeManager::Constants::PRESET_VIEWER_ARROW_COLOR);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeManager::Constants::PRESET_VIEWER_ARROW_HOVER_COLOR);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeManager::Constants::PRESET_VIEWER_ARROW_ACTIVE_COLOR);

		// Rounded to a circle by the pushed FrameRounding
		const bool clicked = Util::Button(id, ImVec2(diameter, diameter));
		Icons::DrawCenteredGlyph(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), icon, ImGui::GetColorU32(ImGuiCol_Text));

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(4);
		return clicked;
	}

	/** @brief Baseline layer note, the features the pack sets, and its disable-at-boot list. */
	void DrawBaselineDetails(const UnifiedPresetCatalog::PackInfo& pack)
	{
		ImGui::Spacing();
		Util::Text::WrappedDisabled("%s", T("menu.presets.baseline_layer_note",
											  "Base settings are feature defaults applied beneath the Scene Manager, independent of the active pack. "
											  "They win over saved settings until you change a value yourself."));
		if (!pack.baselineFeatures.empty()) {
			ImGui::SeparatorText(T("menu.presets.baseline_features", "Included features"));
			for (const auto& featureName : pack.baselineFeatures) {
				auto* feature = Feature::FindFeatureByShortName(featureName);
				if (feature) {
					ImGui::PushID(featureName.c_str());
					ImGui::BulletText("%s", feature->GetDisplayName().c_str());
					BrowserUI::SameLineIfFits(ImGui::CalcTextSize(T("menu.presets.edit_settings", "Edit Settings...")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
					ImGui::BeginDisabled(!feature->loaded || pack.source != UnifiedPresetCatalog::SourceKind::UnifiedPack);
					if (ImGui::Button(T("menu.presets.edit_settings", "Edit Settings...")))
						BaseSettingsEditor::BeginExportInto(feature, pack.id);
					ImGui::EndDisabled();
					ImGui::PopID();
				} else
					ImGui::BulletText("%s %s", featureName.c_str(), T("menu.presets.baseline_feature_unavailable", "(not available)"));
			}
		}
		if (!pack.disableAtBoot.empty()) {
			ImGui::Spacing();
			ImGui::SeparatorText(T("menu.presets.baseline_boot", "Disable at boot"));
			Util::Text::WrappedDisabled("%s", T("menu.presets.baseline_boot_note",
												  "Applied when this Base Settings pack is enabled. A game restart is still required for load/unload."));
			std::vector<std::pair<std::string, bool>> bootEntries(pack.disableAtBoot.begin(), pack.disableAtBoot.end());
			std::ranges::sort(bootEntries, {}, &std::pair<std::string, bool>::first);
			for (const auto& [featureName, disabled] : bootEntries) {
				std::string label = featureName;
				for (auto* feature : Feature::GetFeatureList()) {
					if (feature && feature->GetShortName() == featureName) {
						label = feature->GetDisplayName();
						break;
					}
				}
				ImGui::BulletText("%s: %s", label.c_str(),
					disabled ? T("menu.features.disabled", "Disabled") : T("menu.features.enabled", "Enabled"));
			}
		}
	}

	/// Floor for a list row's name width, in font sizes, so badges cannot squeeze it to nothing.
	constexpr float kMinRowNameFontSizes = 5.0f;
	/// Narrowest the detail title column may get, in font sizes, before the badges move beneath it.
	constexpr float kMinTitleColumnFontSizes = 14.0f;

	std::array<const char*, 5> PresetFilterLabels()
	{
		return { T("menu.presets.filter_all", "All"), T("menu.presets.filter_e11", "E11"), T("menu.presets.filter_cs", "CS"),
			T("menu.presets.filter_baseline", "Base Settings"), T("menu.presets.themes", "UI Themes") };
	}

	/** @brief Apply or disable the pack, open its folder, or request deletion. */
	void DrawPackActions(UnifiedPresetCatalog& catalog, const UnifiedPresetCatalog::PackInfo& pack)
	{
		const auto& theme = globals::menu->GetTheme();
		const bool packActive = catalog.IsScenePackEnabled(pack.id);
		const bool baselineEnabled = catalog.IsBaselineEnabled(pack.id);

		// Each button after the first wraps to a new row when the pane is too narrow to hold it.
		const bool enabled = packActive || baselineEnabled;
		ImGui::BeginDisabled(!enabled && !CanApplyPack(pack));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Util::GetPillPaddingX(ImGui::GetFrameHeight()),
															ImGui::GetStyle().FramePadding.y));
		{
			const auto actionStyle = enabled ? Util::WarningButtonStyle() : Util::SuccessButtonStyle();
			if (Util::ButtonWithFlash(enabled ? T("menu.presets.disable", "Disable Preset") : T("menu.presets.apply", "Enable Preset"))) {
				if (enabled)
					catalog.DisablePack(pack.id);
				else
					RequestApplyPack(pack);
			}
		}
		ImGui::PopStyleVar(2);
		ImGui::EndDisabled();

		if (pack.hasCSPresets || pack.hasBaseline || pack.hasFormEdits) {
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(T("menu.presets.manage_contents", "Manage Settings...")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
			if (Util::Button(T("menu.presets.manage_contents", "Manage Settings...")))
				PresetsPageRenderer::OpenContents(pack.id);
		}
		if (!enabled) {
			const auto conflicts = catalog.GetConflictingPacks(pack.id);
			if (!conflicts.empty()) {
				Util::Text::WrappedWarning("%s", T("menu.presets.conflict_disabled", "Not applied because an enabled preset controls conflicting settings."));
				for (const auto& id : conflicts)
					if (const auto* other = catalog.FindPack(id))
						ImGui::BulletText("%s", other->name.c_str());
			}
		}
		if (pack.source == UnifiedPresetCatalog::SourceKind::UnifiedPack) {
			BrowserUI::SameLineIfFits(ImGui::CalcTextSize(T("menu.presets.update", "Update Preset...")).x + ImGui::GetStyle().FramePadding.x * 2.0f);
			if (Util::Button(T("menu.presets.update", "Update Preset...")))
				PresetExportPanel::OpenForPack(pack.id);
			Util::AddTooltip(T("menu.presets.update_tooltip", "Export current settings into this preset, starting with its metadata and included features."));
		}

		const Icons::GlyphRef folderIcon = Icons::TI(ICON_TI_FOLDER_OPEN);
		BrowserUI::SameLineIfFits(Icons::CalcGlyphSize(folderIcon).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		{
			auto _style = Util::TransparentIconButtonStyle();
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			if (Icons::Button("##OpenPack", folderIcon))
				catalog.OpenPackFolder(pack.id);
			ImGui::PopStyleVar();
		}
		Util::AddTooltip(T("menu.presets.open_pack", "Open Preset Folder"));

		if (catalog.CanDeletePack(pack.id)) {
			BrowserUI::SameLineIfFits(BrowserUI::IconButtonSize());
			if (BrowserUI::IconButton("##DeletePreset", Icons::TI(ICON_TI_TRASH), T("menu.presets.delete", "Delete Preset"), false, BrowserUI::DestructiveIconColor())) {
				deleteFailed = false;
				pendingDeletePackId = pack.id;
				deleteConfirmation.title = T("menu.presets.delete", "Delete Preset");
				deleteConfirmation.message = std::vformat((pack.source == UnifiedPresetCatalog::SourceKind::LegacyBaseline || pack.source == UnifiedPresetCatalog::SourceKind::LegacyScene) ?
															  T("menu.presets.delete_legacy_confirm", "Disable and remove {} from Presets? Original legacy files will be kept.") :
															  T("menu.presets.delete_confirm", "Disable and delete {}? Its folder will be moved to the library's hidden deleted-presets folder."),
					std::make_format_args(pack.name));
				deleteConfirmation.confirmLabel = T("menu.presets.delete", "Delete Preset");
				deleteConfirmation.cancelLabel = T("ui.cancel", "Cancel");
				deleteConfirmation.focus = Util::ConfirmationPopup::Focus::Cancel;
				deleteConfirmation.Request();
			}
		}
		if (deleteFailed)
			Util::Text::WrappedError("%s", T("menu.presets.delete_failed", "Could not delete this preset. Check the log for details."));

		if (packActive || baselineEnabled) {
			MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
			ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.SuccessColor);
			ImGui::TextWrapped("%s", packActive ? T("menu.presets.active_pack", "This pack is currently active.") :
												  T("menu.presets.baseline_enabled", "This pack's base settings are enabled."));
			ImGui::PopStyleColor();
		}
	}

	std::optional<bool> DrawBaselinePriorityActions(const ImVec2& origin, size_t priority, size_t count)
	{
		std::optional<bool> raise;
		ImGui::SetCursorScreenPos(origin);
		ImGui::BeginDisabled(priority == count);
		if (ImGui::ArrowButton("##Raise", ImGuiDir_Up))
			raise = true;
		ImGui::EndDisabled();
		Util::AddTooltip(T("menu.presets.raise_priority", "Increase Base Settings priority"));
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.y));
		ImGui::BeginDisabled(priority == 1);
		if (ImGui::ArrowButton("##Lower", ImGuiDir_Down))
			raise = false;
		ImGui::EndDisabled();
		Util::AddTooltip(T("menu.presets.lower_priority", "Decrease Base Settings priority"));
		return raise;
	}
}

float PresetsPageRenderer::MeasureBackendBadgesWidth(bool hasE11, bool hasCSPresets, bool hasBaseline, bool compact)
{
	float width = 0.0f;
	bool needGap = false;
	ForEachBackendBadge(hasE11, hasCSPresets, hasBaseline, compact, [&](const BackendBadge& badge) {
		AppendBadgeGap(width, needGap);
		width += ImGui::CalcTextSize(badge.label).x;
	});
	return width;
}

void PresetsPageRenderer::DrawBackendBadges(bool hasE11, bool hasCSPresets, bool hasBaseline, bool compact, bool muted)
{
	const float gap = BadgeGap();

	ImGui::BeginGroup();
	bool needGap = false;
	ForEachBackendBadge(hasE11, hasCSPresets, hasBaseline, compact, [&](const BackendBadge& badge) {
		if (needGap)
			ImGui::SameLine(0.0f, gap);
		needGap = true;
		ImGui::TextColored(muted ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : badge.color, "%s", badge.label);
	});
	ImGui::EndGroup();
}

void PresetsPageRenderer::RenderToolbar()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto& style = ImGui::GetStyle();
	const char* createLabel = T("menu.presets.create", "Create Preset...");
	const float createWidth = ImGui::CalcTextSize(createLabel).x + style.FramePadding.x * 2.0f;
	const float width = ImGui::GetContentRegionAvail().x;
	const bool inlineCreate = !g_showThemes && width >= ThemeManager::Constants::PRESET_SEARCH_WIDTH * Util::GetUIScale() + createWidth + style.ItemSpacing.x;
	BrowserUI::SearchField("##PresetSearch", searchBuffer, IM_ARRAYSIZE(searchBuffer),
		T("menu.presets.search", "Search presets..."), inlineCreate ? width - createWidth - style.ItemSpacing.x : -FLT_MIN, true);
	if (!g_showThemes) {
		if (inlineCreate)
			ImGui::SameLine();
		else
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - createWidth));
		if (Util::Button(createLabel))
			PresetExportPanel::Open();
	}

	using PresetType = UnifiedPresetCatalog::PresetType;
	const auto typeChip = [](const char* label, std::optional<PresetType> type, bool afterFirst = true) {
		if (afterFirst)
			BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(label, false));
		ImGui::PushID(type ? static_cast<int>(*type) : -1);
		if (BrowserUI::FilterChip("##type", { label }, typeFilter == type && !g_showThemes)) {
			typeFilter = type;
			g_showThemes = false;
		}
		ImGui::PopID();
	};
	const auto labels = PresetFilterLabels();
	typeChip(labels[0], std::nullopt, false);
	typeChip(labels[1], PresetType::E11);
	typeChip(labels[2], PresetType::CS);
	typeChip(labels[3], PresetType::Baseline);
	BrowserUI::SameLineIfFits(BrowserUI::MeasureChip(labels[4], false));
	if (BrowserUI::FilterChip("##themes", { labels[4] }, g_showThemes))
		g_showThemes = true;

	const float actionSize = BrowserUI::IconButtonSize();
	const float actionWidth = actionSize * 2.0f + ImGui::GetStyle().ItemSpacing.x;
	BrowserUI::SameLineIfFits(actionWidth);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - actionWidth));
	{
		if (BrowserUI::IconButton("##PresetsRefresh", Icons::TI(ICON_TI_REFRESH),
				T("menu.presets.refresh_tooltip", "Rescan unified packs and Effects 11 library presets."))) {
			catalog.Refresh();
			ThemeManager::GetSingleton()->RefreshThemes();
			discovered = true;
		}

		ImGui::SameLine();
		if (BrowserUI::IconButton("##PresetsOpenFolder", Icons::TI(ICON_TI_FOLDER_OPEN),
				g_showThemes ? T("menu.presets.open_theme_folder", "Open Theme Folder") : T("menu.presets.open_folder_tooltip", "Open the unified Presets library folder in Explorer."))) {
			if (g_showThemes)
				OpenThemesFolder();
			else
				catalog.OpenPresetsFolder();
		}
	}
}

void PresetsPageRenderer::RenderBrowser(const ImVec2& size)
{
	if (ImGui::BeginChild("##PresetBrowser", size, true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		RenderToolbar();
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		if (g_showThemes)
			RenderThemeList(0.0f);
		else
			RenderList();
	}
	ImGui::EndChild();
}

void PresetsPageRenderer::RenderList()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto& theme = globals::menu->GetTheme();
	const float scale = Util::GetUIScale();

	auto indices = catalog.Query(typeFilter, searchBuffer);
	const auto& baselineOrder = catalog.GetBaselinePackOrder();
	std::unordered_map<std::string, size_t> priority;
	for (size_t index = 0; index < baselineOrder.size(); ++index)
		priority[baselineOrder[index]] = index + 1;
	std::ranges::stable_sort(indices, [&](size_t first, size_t second) {
		const auto& a = catalog.GetPacks()[first];
		const auto& b = catalog.GetPacks()[second];
		const auto group = [](const auto& pack) { return pack.hasBaseline ? 2 : pack.IsE11() ? 0 :
			                                                                                   1; };
		if (group(a) != group(b))
			return group(a) < group(b);
		const auto aPriority = priority.contains(a.id) ? priority.at(a.id) : 0;
		const auto bPriority = priority.contains(b.id) ? priority.at(b.id) : 0;
		return aPriority > bPriority;
	});

	ImGui::BeginChild("##PresetList", ImVec2(0, 0), false);

	if (indices.empty()) {
		ImGui::TextDisabled("%s", T("menu.presets.empty", "No presets found. Drop packs into the Presets folder, install Effects 11 presets, or export from Scene Manager."));
	}

	const float rowPad = ThemeManager::Constants::PRESET_ROW_PADDING * scale;
	const float logoSize = ThemeManager::Constants::PRESET_ROW_LOGO_SIZE * scale;
	const float rowGap = ImGui::GetStyle().ItemSpacing.y;
	const float rowInset = ThemeManager::Constants::PRESET_ROW_INSET * scale;
	const float statusGap = ThemeManager::Constants::PRESET_ROW_STATUS_GAP * scale;
	std::string moveId;
	bool moveRaise = false;

	for (size_t idx : indices) {
		const auto& packId = catalog.GetPacks()[idx].id;
		auto* packPtr = catalog.FindPack(packId);
		if (!packPtr)
			continue;
		auto& pack = *packPtr;
		const bool selected = selectedPackId == pack.id;
		const bool isActive = catalog.IsScenePackEnabled(pack.id) || catalog.IsBaselineEnabled(pack.id);
		const auto& compat = pack.compat;
		const bool reorderable = pack.hasBaseline && priority.contains(pack.id) && baselineOrder.size() > 1;
		const float buttonSize = ImGui::GetFrameHeight();
		const float controlsWidth = reorderable ? buttonSize + rowInset : 0.0f;
		const float rowWidth = ImGui::GetContentRegionAvail().x;
		const float textWidth = std::max(ImGui::GetFontSize(), rowWidth - logoSize - rowInset * 3.0f - controlsWidth);
		const char* activeLabel = T("menu.presets.active", "Active");
		const char* invalidLabel = T("menu.presets.invalid", "Invalid");
		float trailingWidth;
		bool badgesBesideName;
		bool activeBesideBadges = true;
		bool invalidBesideStatus = true;
		int statusLines = 0;
		{
			MenuFonts::FontRoleGuard body(Menu::FontRole::Body);
			const float badgesWidth = MeasureBackendBadgesWidth(pack.IsE11(), pack.IsCS(), pack.IsBaseline(), true);
			trailingWidth = rowInset + badgesWidth;
			if (isActive)
				trailingWidth += statusGap + ImGui::CalcTextSize(activeLabel).x;
			if (!pack.valid)
				trailingWidth += statusGap + ImGui::CalcTextSize(invalidLabel).x;
			badgesBesideName = textWidth - trailingWidth >= ImGui::GetFontSize() * kMinRowNameFontSizes;
			if (!badgesBesideName) {
				float usedWidth = badgesWidth;
				const auto fitStatus = [&](bool visible, const char* label, bool& beside) {
					if (!visible)
						return;
					const float width = ImGui::CalcTextSize(label).x;
					beside = usedWidth + statusGap + width <= textWidth;
					if (!beside)
						++statusLines;
					usedWidth = beside ? usedWidth + statusGap + width : width;
				};
				fitStatus(isActive, activeLabel, activeBesideBadges);
				fitStatus(!pack.valid, invalidLabel, invalidBesideStatus);
			}
		}
		const int warnLines = (compat.versionGap != PresetCompatibility::VersionGap::None ? 1 : 0) +
		                      (!compat.featuresMessage.empty() ? 1 : 0) +
		                      (!compat.pluginsMessage.empty() ? 1 : 0);
		const float textLines = (badgesBesideName ? 2.0f : 3.0f) + static_cast<float>(warnLines + statusLines);
		const float textBlockH = ImGui::GetTextLineHeightWithSpacing() * textLines;
		const float controlsHeight = reorderable ? buttonSize * 2.0f + ImGui::GetStyle().ItemInnerSpacing.y : 0.0f;
		const float rowHeight = std::max({ logoSize, textBlockH, controlsHeight }) + rowPad * 2.0f;

		ImGui::PushID(pack.id.c_str());

		const ImVec2 targetOrigin = ImGui::GetCursorScreenPos();
		const auto rowBounds = Util::Anim::SelectionGlide(ImGui::GetID("##RowPosition"), targetOrigin,
			ImVec2(targetOrigin.x + rowWidth, targetOrigin.y + rowHeight));
		const ImVec2 rowOrigin = rowBounds.min;
		ImGui::SetCursorScreenPos(rowOrigin);
		const bool clicked = Util::RoundedSelectable("##row", selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(rowWidth, rowHeight));
		if (clicked) {
			selectedPackId = pack.id;
			lightboxPackId.clear();
			lightboxImageIndex = -1;
			lightboxSuppressClose = false;
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && CanApplyPack(pack))
				RequestApplyPack(pack);
		}

		// Draw contents inside the selectable bounds, then park the cursor past the row so the
		// next selectable cannot overlap this highlight.
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(isActive ? ImGuiCol_Text : ImGuiCol_TextDisabled));
		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + rowInset, rowOrigin.y + rowPad));

		catalog.EnsureArtwork(pack);
		{
			ImGui::Dummy(ImVec2(logoSize, logoSize));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 p0 = ImGui::GetItemRectMin();
			const ImVec2 p1 = ImGui::GetItemRectMax();
			const float rounding = ImGui::GetStyle().FrameRounding;
			if (pack.logoSRV)
				DrawRoundedImage(dl, reinterpret_cast<ImTextureID>(pack.logoSRV.get()), p0, p1, rounding,
					ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, isActive ? 1.0f : ImGui::GetStyle().DisabledAlpha)));
			else
				dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ThemeManager::Constants::PRESET_LOGO_PLACEHOLDER_COLOR), rounding);
		}

		ImGui::SameLine(0.0f, rowInset);
		ImGui::BeginGroup();
		{
			MenuFonts::FontRoleGuard body(Menu::FontRole::Body);

			// The name gives way to the badges and status, ellipsised so they stay on screen.
			BrowserUI::EllipsizedText(pack.name.c_str(), badgesBesideName ? textWidth - trailingWidth : textWidth,
				ImGui::GetStyleColorVec4(ImGuiCol_Text));

			if (badgesBesideName)
				ImGui::SameLine(0.0f, rowInset);
			DrawBackendBadges(pack.IsE11(), pack.IsCS(), pack.IsBaseline(), true, !isActive);
			if (isActive) {
				if (activeBesideBadges)
					ImGui::SameLine(0.0f, statusGap);
				ImGui::TextColored(theme.StatusPalette.SuccessColor, "%s", activeLabel);
			}
			if (!pack.valid) {
				if (invalidBesideStatus)
					ImGui::SameLine(0.0f, statusGap);
				ImGui::TextColored(theme.StatusPalette.Error, "%s", invalidLabel);
			}
		}
		{
			MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
			std::string meta;
			if (!pack.author.empty() && !pack.version.empty())
				meta = std::format("{} · v{}", pack.author, pack.version);
			else if (!pack.author.empty())
				meta = pack.author;
			else if (!pack.version.empty())
				meta = std::format("v{}", pack.version);
			else if (pack.source == UnifiedPresetCatalog::SourceKind::Effects11Orphan)
				meta = T("menu.presets.source_e11", "Effects 11 library");
			else if (pack.IsE11())
				meta = T("menu.presets.source_enb", "ENB / Effects 11");
			else if (pack.IsCS())
				meta = T("menu.presets.source_cs", "CS Presets");
			else if (pack.IsBaseline())
				meta = T("menu.presets.source_baseline", "Base Settings");
			if (!meta.empty())
				BrowserUI::EllipsizedText(meta.c_str(), textWidth, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			PresetCompatibility::DrawWarnings(compat, true);
		}
		ImGui::EndGroup();
		ImGui::PopStyleColor();

		if (reorderable) {
			const float x = rowOrigin.x + rowWidth - rowInset - buttonSize;
			if (const auto raise = DrawBaselinePriorityActions(ImVec2(x, rowOrigin.y + (rowHeight - controlsHeight) * 0.5f), priority.at(pack.id), baselineOrder.size())) {
				moveId = pack.id;
				moveRaise = *raise;
			}
		}
		ImGui::SetCursorScreenPos(ImVec2(targetOrigin.x, targetOrigin.y + rowHeight + rowGap));
		ImGui::Dummy(ImVec2(0, 0));

		ImGui::PopID();
	}
	if (!moveId.empty())
		catalog.MoveBaseline(moveId, moveRaise);

	ImGui::EndChild();
}

void PresetsPageRenderer::RenderDetail(const ImVec2& size)
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto& theme = globals::menu->GetTheme();
	const ImGuiStyle& style = ImGui::GetStyle();

	Util::Anim::PageFade fade(ImGui::GetID(selectedPackId.c_str()));
	ImGui::BeginChild("##PresetDetail", size, true);

	auto* pack = catalog.FindPack(selectedPackId);
	if (!pack) {
		ImGui::TextDisabled("%s", T("menu.presets.select_prompt", "Select a preset from the list to view details."));
		ImGui::EndChild();
		return;
	}

	catalog.EnsureArtwork(*pack);

	if (lightboxPackId != pack->id) {
		lightboxPackId = pack->id;
		lightboxImageIndex = -1;
		lightboxSuppressClose = false;
	}
	if (lightboxImageIndex >= static_cast<int>(pack->screenshotSRVs.size())) {
		lightboxImageIndex = -1;
		lightboxSuppressClose = false;
	}

	const float badgesWidth = MeasureBackendBadgesWidth(pack->IsE11(), pack->IsCS(), pack->IsBaseline(), false);
	const float posterH = ImGui::GetFrameHeight() * 6.0f;
	const float posterW = posterH * (2.0f / 3.0f);  // movie-poster portrait

	// A narrow pane would leave the title no room beside the badges, so they drop beneath it instead.
	const bool badgesBesideTitle = ImGui::GetContentRegionAvail().x - posterW - badgesWidth - style.ItemSpacing.x * 2.0f >=
	                               ImGui::GetFontSize() * kMinTitleColumnFontSizes;

	// Poster | title+author | badges in one table so pills share a baseline and spacing is style-driven.
	if (ImGui::BeginTable("##PresetDetailHeader", badgesBesideTitle ? 3 : 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
		ImGui::TableSetupColumn("poster", ImGuiTableColumnFlags_WidthFixed, posterW);
		ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
		if (badgesBesideTitle)
			ImGui::TableSetupColumn("badges", ImGuiTableColumnFlags_WidthFixed, badgesWidth);

		ImGui::TableNextRow(ImGuiTableRowFlags_None, posterH);
		ImGui::TableSetColumnIndex(0);
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(posterW, posterH));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 p1(origin.x + posterW, origin.y + posterH);
			const float rounding = style.FrameRounding;
			// Deliberately the pack's own cover art only - never a screenshot, so this stays a
			// stable "poster" instead of jumping to whatever the filmstrip/hero viewer is showing.
			if (ID3D11ShaderResourceView* poster = pack->coverSRV.get()) {
				DrawRoundedImage(dl, reinterpret_cast<ImTextureID>(poster), origin, p1, rounding);
			} else {
				dl->AddRectFilled(origin, p1,
					ImGui::GetColorU32(ThemeManager::Constants::PRESET_POSTER_PLACEHOLDER_COLOR), rounding);
			}
		}

		// Top-aligned rather than centred in the poster's full height.
		const float titleTopPad = 2.0f * Util::GetUIScale();

		ImGui::TableSetColumnIndex(1);
		{
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + titleTopPad);
			ImGui::BeginGroup();
			{
				const auto stage = PresetCompatibility::ReleaseStageFromVersion(pack->version);
				const std::string stageTag = Feature::GetReleaseStageTag(stage);

				MenuFonts::FontRoleGuard title(Menu::FontRole::Heading);
				ImGui::SetWindowFontScale(1.35f);
				ImGui::TextWrapped("%s", pack->name.c_str());
				if (!stageTag.empty()) {
					MenuFonts::FontRoleGuard body(Menu::FontRole::Body);
					BrowserUI::SameLineIfFits(ImGui::CalcTextSize(stageTag.c_str()).x);
					ImGui::TextColored(Feature::GetReleaseStageColor(stage), "%s", stageTag.c_str());
				}
				ImGui::SetWindowFontScale(1.0f);
			}
			{
				MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
				std::string metaLine;
				if (!pack->author.empty())
					metaLine += pack->author;
				if (!pack->version.empty()) {
					if (!metaLine.empty())
						metaLine += "  ·  ";
					metaLine += "v" + pack->version;
				}
				if (!metaLine.empty())
					Util::Text::Secondary("%s", metaLine.c_str());
				if (!pack->nexusUrl.empty()) {
					ImGui::SameLine(0.0f, style.ItemSpacing.x);
					if (Icons::Button("##NexusLink", Icons::TI(ICON_TI_EXTERNAL_LINK)))
						Util::FileHelpers::OpenWithShell(pack->nexusUrl);
					Util::AddTooltip(T("menu.presets.nexus_link_tooltip", "Open this preset's Nexus Mods page."));
				}
				PresetCompatibility::DrawWarnings(pack->compat, false);
			}

			if (!badgesBesideTitle && badgesWidth > 0.0f) {
				ImGui::Spacing();
				DrawBackendBadges(pack->IsE11(), pack->IsCS(), pack->IsBaseline(), false);
			}

			ImGui::Spacing();
			DrawPackActions(catalog, *pack);

			ImGui::EndGroup();
		}

		if (badgesBesideTitle && badgesWidth > 0.0f) {
			ImGui::TableSetColumnIndex(2);
			// Aligned to the title line rather than centred in the whole poster height, now that
			// the title column reads top-down (title, author, actions) instead of floating in the middle.
			const float titleLineH = ImGui::GetTextLineHeight() * 1.35f;
			const float badgeH = ImGui::GetTextLineHeight();
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + titleTopPad + std::max(0.0f, (titleLineH - badgeH) * 0.5f));
			DrawBackendBadges(pack->IsE11(), pack->IsCS(), pack->IsBaseline(), false);
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();

	if (!pack->description.empty()) {
		MenuFonts::FontRoleGuard body(Menu::FontRole::Body);
		ImGui::TextWrapped("%s", pack->description.c_str());
	} else {
		ImGui::TextDisabled("%s", T("menu.presets.no_description", "No description provided."));
	}

	if (pack->hasCSPresets) {
		ImGui::Spacing();
		Util::Text::WrappedDisabled("%s", T("menu.presets.cs_layer_note",
											  "Applying makes this pack's CS Presets the active scene layer, replacing the previous pack's."));
	}

	if (pack->hasBaseline)
		DrawBaselineDetails(*pack);

	if (!pack->valid && !pack->invalidReason.empty()) {
		ImGui::Spacing();
		ImGui::TextColored(theme.StatusPalette.Error, "%s", pack->invalidReason.c_str());
	}

	if (!pack->screenshotSRVs.empty()) {
		ImGui::Spacing();
		ImGui::SeparatorText(T("menu.presets.screenshots", "Screenshots"));
		const float thumb = ImGui::GetFrameHeight() * 4.0f;
		const float thumbH = thumb * (9.0f / 16.0f);
		for (size_t i = 0; i < pack->screenshotSRVs.size(); ++i) {
			if (i > 0)
				ImGui::SameLine(0.0f, style.ItemSpacing.x);
			ImGui::PushID(static_cast<int>(i));
			const bool isOpen = static_cast<int>(i) == lightboxImageIndex;
			const bool clicked = ImGui::InvisibleButton("##shot", ImVec2(thumb, thumbH));
			const ImVec2 p0 = ImGui::GetItemRectMin();
			const ImVec2 p1 = ImGui::GetItemRectMax();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			if (pack->screenshotSRVs[i])
				DrawRoundedImage(dl, reinterpret_cast<ImTextureID>(pack->screenshotSRVs[i].get()), p0, p1, style.FrameRounding);
			if (isOpen || ImGui::IsItemHovered()) {
				const ImU32 border = ImGui::GetColorU32(isOpen ?
															theme.StatusPalette.InfoColor :
															ImGui::GetStyleColorVec4(ImGuiCol_Border));
				dl->AddRect(p0, p1, border, style.FrameRounding, 0, style.FrameBorderSize + (isOpen ? 1.0f : 0.0f));
			}
			if (clicked) {
				lightboxImageIndex = static_cast<int>(i);
				lightboxSuppressClose = true;
			}
			ImGui::PopID();
		}
	}

	ImGui::EndChild();
}

bool PresetsPageRenderer::CloseLightboxIfOpen()
{
	if (lightboxImageIndex < 0)
		return false;
	lightboxImageIndex = -1;
	lightboxSuppressClose = false;
	return true;
}

void PresetsPageRenderer::RenderScreenshotLightbox()
{
	if (lightboxImageIndex < 0)
		return;

	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	auto* pack = catalog.FindPack(selectedPackId);
	if (!pack || lightboxImageIndex >= static_cast<int>(pack->screenshotSRVs.size()) ||
		!pack->screenshotSRVs[static_cast<size_t>(lightboxImageIndex)]) {
		lightboxImageIndex = -1;
		return;
	}

	Util::Anim::PageFade fade(ImGui::GetID(pack->id.c_str()));
	catalog.EnsureArtwork(*pack);

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImGuiStyle& style = ImGui::GetStyle();
	const int shotCount = static_cast<int>(pack->screenshotSRVs.size());

	// Full-viewport dim layer. Semi-transparent WindowBg lets BackgroundBlur show through.
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::SetNextWindowFocus();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.04f, 0.72f));

	constexpr ImGuiWindowFlags kFlags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoCollapse;

	if (!ImGui::Begin("##PresetScreenshotLightbox", nullptr, kFlags)) {
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
		return;
	}

	const float maxW = viewport->Size.x * 0.8f;
	const float maxH = viewport->Size.y * 0.8f;
	float imgW = maxW;
	float imgH = imgW * (9.0f / 16.0f);
	if (imgH > maxH) {
		imgH = maxH;
		imgW = imgH * (16.0f / 9.0f);
	}

	const ImVec2 imgOrigin(
		viewport->Pos.x + (viewport->Size.x - imgW) * 0.5f,
		viewport->Pos.y + (viewport->Size.y - imgH) * 0.5f);
	const ImVec2 imgEnd(imgOrigin.x + imgW, imgOrigin.y + imgH);

	// Backdrop → image → arrows. AllowOverlap so later items win hover/click over earlier ones;
	// without it, arrow hits land on the image (or image hits on the backdrop) and dismiss.
	ImGui::SetNextItemAllowOverlap();
	ImGui::SetCursorScreenPos(viewport->Pos);
	const bool backdropClicked = ImGui::InvisibleButton("##LightboxBackdrop", viewport->Size);

	ImGui::SetNextItemAllowOverlap();
	ImGui::SetCursorScreenPos(imgOrigin);
	const bool imageClicked = ImGui::InvisibleButton("##LightboxImage", ImVec2(imgW, imgH));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	DrawRoundedImage(dl,
		reinterpret_cast<ImTextureID>(pack->screenshotSRVs[static_cast<size_t>(lightboxImageIndex)].get()),
		imgOrigin, imgEnd, style.FrameRounding);

	bool navigated = false;
	if (shotCount > 1) {
		const float arrowBtn = ImGui::GetFrameHeight() * 1.35f;
		const float midY = imgOrigin.y + imgH * 0.5f;
		const float inset = style.ItemSpacing.x * 2.0f;
		if (DrawCarouselArrow("##LightboxPrev", Icons::TI(ICON_TI_CHEVRON_LEFT),
				ImVec2(imgOrigin.x + inset + arrowBtn * 0.5f, midY), arrowBtn)) {
			lightboxImageIndex = (lightboxImageIndex - 1 + shotCount) % shotCount;
			navigated = true;
		}
		if (DrawCarouselArrow("##LightboxNext", Icons::TI(ICON_TI_CHEVRON_RIGHT),
				ImVec2(imgEnd.x - inset - arrowBtn * 0.5f, midY), arrowBtn)) {
			lightboxImageIndex = (lightboxImageIndex + 1) % shotCount;
			navigated = true;
		}
	}

	// Escape is primarily consumed in Menu::ProcessInputEventQueue via CloseLightboxIfOpen.
	// Suppress click-to-close until the mouse releases so the opening thumb click cannot dismiss it.
	if (lightboxSuppressClose && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
		lightboxSuppressClose = false;

	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		lightboxImageIndex = -1;
	else if (!lightboxSuppressClose && !navigated && (imageClicked || backdropClicked))
		lightboxImageIndex = -1;

	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

void PresetsPageRenderer::OpenExport()
{
	requestedPopup = PresetPopup::Create;
	auto* editor = EditorWindow::GetSingleton();
	if (editor->open)
		editor->ShowMenuBrowser();
	else
		globals::menu->IsEnabled = true;
	globals::menu->SelectFeatureMenu(std::string(MenuPageIds::kPresets));
}

void PresetsPageRenderer::OpenContents(const std::string& id)
{
	SceneFilesPanel::OpenForPack(id);
	requestedPopup = PresetPopup::Manage;
}

void PresetsPageRenderer::ShowLibrary()
{
	closePopup = true;
}

bool PresetsPageRenderer::IsPopupOpen()
{
	return popupOpen || requestedPopup != PresetPopup::None;
}

void PresetsPageRenderer::DrawPopups()
{
	const auto request = std::exchange(requestedPopup, PresetPopup::None);
	if (request != PresetPopup::None) {
		ImGui::OpenPopup(request == PresetPopup::Create ? kCreatePopup : kManagePopup);
		closePopup = false;
	}
	popupOpen = false;
	bool drewExportDialogs = false;
	for (const auto kind : { PresetPopup::Create, PresetPopup::Manage }) {
		const auto* id = kind == PresetPopup::Create ? kCreatePopup : kManagePopup;
		if (!ImGui::IsPopupOpen(id))
			continue;
		const auto* viewport = ImGui::GetMainViewport();
		const float scale = Util::GetUIScale();
		const ImVec2 maximum(viewport->WorkSize.x - ImGui::GetStyle().WindowPadding.x * 2.0f,
			viewport->WorkSize.y - ImGui::GetStyle().WindowPadding.y * 2.0f);
		ImGui::SetNextWindowSize(ImVec2(std::min(ThemeManager::Constants::PRESET_DIALOG_WIDTH * scale, maximum.x),
									 std::min(ThemeManager::Constants::PRESET_DIALOG_HEIGHT * scale, maximum.y)),
			ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), maximum);
		const auto title = std::string(kind == PresetPopup::Create ? T("menu.presets.create", "Create Preset...") :
																	 T("menu.presets.manage_contents", "Manage Settings...")) +
		                   id;
		bool open = true;
		if (auto popup = Util::CenteredPopupModal(title.c_str(), &open, ImGuiWindowFlags_NoResize)) {
			if (kind == PresetPopup::Create) {
				if (ImGui::BeginChild("##PresetAuthoring", ImVec2(0, 0)))
					PresetExportPanel::DrawPage();
				ImGui::EndChild();
				PresetExportPanel::Draw();
				drewExportDialogs = true;
			} else {
				SceneFilesPanel::Draw();
				SceneFilesPanel::DrawConfirmation();
			}
			if (closePopup) {
				ImGui::CloseCurrentPopup();
				open = false;
				closePopup = false;
			}
		}
		popupOpen |= open && ImGui::IsPopupOpen(id);
	}
	if (!drewExportDialogs)
		PresetExportPanel::Draw();
}

void PresetsPageRenderer::Render()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	if (!discovered) {
		catalog.Discover();
		discovered = true;
		if (selectedPackId.empty() && !catalog.GetActivePackId().empty())
			selectedPackId = catalog.GetActivePackId();
		else if (selectedPackId.empty() && !catalog.GetPacks().empty())
			selectedPackId = catalog.GetPacks().front().id;
	}

	ImGui::BeginChild("PresetsPage", ImVec2(0, 0), false);

	bool automatic = catalog.GetAutomaticPresets();
	if (ImGui::Checkbox(T("menu.presets.auto_apply", "Automatically Apply Presets at Startup"), &automatic))
		catalog.SetAutomaticPresets(automatic);
	{
		const float scale = Util::GetUIScale();
		const float width = ImGui::GetContentRegionAvail().x;
		const auto labels = PresetFilterLabels();
		float toolbarWidth = BrowserUI::IconButtonSize() * 2.0f + ImGui::GetStyle().ItemSpacing.x * (labels.size() + 1) +
		                     ImGui::GetStyle().WindowPadding.x * 2.0f + ImGui::GetStyle().CellPadding.x * 2.0f;
		for (const auto* label : labels)
			toolbarWidth += BrowserUI::MeasureChip(label, false);
		const float listWidth = std::clamp(std::max(width * ThemeManager::Constants::PRESET_LIST_WIDTH_RATIO, toolbarWidth),
			ThemeManager::Constants::PRESET_LIST_MIN_WIDTH * scale, std::max(ThemeManager::Constants::PRESET_LIST_MAX_WIDTH * scale, toolbarWidth));
		const float detailWidth = std::max(ImGui::GetFontSize(), width - listWidth - ImGui::GetStyle().ItemSpacing.x) / scale;
		BrowserUI::SplitPane({ "##PresetSplitProportional", ThemeManager::Constants::PRESET_BROWSER_MIN_WIDTH, detailWidth,
								 ThemeManager::Constants::PRESET_STACKED_LIST_RATIO, true },
			stackedListRatio, RenderBrowser, [](const ImVec2& size) {
				if (g_showThemes)
					RenderThemeDetail(size);
				else
					RenderDetail(size);
			});
	}

	ImGui::EndChild();

	RenderScreenshotLightbox();

	if (applyConfirmation.Draw()) {
		if (const auto* pack = catalog.FindPack(pendingApplyPackId))
			ApplyPack(*pack);
	}
	if (deleteConfirmation.Draw()) {
		deleteFailed = !catalog.DeletePack(pendingDeletePackId);
		if (!deleteFailed)
			selectedPackId.clear();
	}
}

void PresetsPageRenderer::RenderThemeList(float width)
{
	auto* themeManager = ThemeManager::GetSingleton();
	const auto& selectedPreset = globals::menu->GetSettings().SelectedThemePreset;
	const float scale = Util::GetUIScale();
	const float rowPad = ThemeManager::Constants::PRESET_ROW_PADDING * scale;
	const float rowInset = ThemeManager::Constants::PRESET_ROW_INSET * scale;
	const float logoSize = ThemeManager::Constants::PRESET_ROW_LOGO_SIZE * scale;
	const std::string query = Util::FoldForSearch(searchBuffer);

	ImGui::BeginChild("##ThemeList", ImVec2(width, 0), true);

	if (g_selectedTheme.empty())
		g_selectedTheme = selectedPreset;
	for (const auto& info : themeManager->GetThemes()) {
		if (!info.isValid || (!query.empty() && Util::FoldForSearch(info.displayName).find(query) == std::string::npos))
			continue;
		ImGui::PushID(info.name.c_str());
		const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
		const float textWidth = std::max(ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x - logoSize - rowInset * 2.0f);
		const bool active = info.name == selectedPreset;
		const char* activeLabel = T("menu.presets.active", "Active");
		const float statusGap = ThemeManager::Constants::PRESET_ROW_STATUS_GAP * scale;
		const bool inlineStatus = ImGui::CalcTextSize(info.displayName.c_str()).x + statusGap + ImGui::CalcTextSize(activeLabel).x <= textWidth;
		float textHeight = ImGui::CalcTextSize(info.displayName.c_str(), nullptr, false, textWidth).y;
		if (active && !inlineStatus)
			textHeight += ImGui::GetTextLineHeightWithSpacing();
		{
			MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
			textHeight += ImGui::GetStyle().ItemSpacing.y + ImGui::CalcTextSize(info.author.c_str(), nullptr, false, textWidth).y;
		}
		const float rowHeight = std::max(logoSize, textHeight) + rowPad * 2.0f;
		if (Util::Selectable("##row", g_selectedTheme == info.name, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight))) {
			g_selectedTheme = info.name;
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				globals::menu->LoadThemePreset(info.name);
		}
		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + rowInset, rowOrigin.y + rowPad));
		DrawThemeLogo(info, logoSize);
		ImGui::SameLine(0.0f, rowInset);
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
		ImGui::TextWrapped("%s", info.displayName.c_str());
		if (active) {
			if (inlineStatus)
				ImGui::SameLine(0.0f, statusGap);
			ImGui::TextColored(globals::menu->GetTheme().StatusPalette.SuccessColor, "%s", T("menu.presets.active", "Active"));
		}
		{
			MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
			Util::Text::WrappedDisabled("%s", info.author.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y + rowHeight + ImGui::GetStyle().ItemSpacing.y));
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::PopID();
	}
	ImGui::EndChild();
}

void PresetsPageRenderer::RenderThemeDetail(const ImVec2& size)
{
	auto* themeManager = ThemeManager::GetSingleton();
	ImGui::BeginChild("##ThemeDetail", size, true);
	const auto* info = themeManager->GetThemeInfo(g_selectedTheme);
	if (!info) {
		ImGui::TextDisabled("%s", T("menu.presets.select_theme_prompt", "Select a theme from the list to view details."));
		ImGui::EndChild();
		return;
	}

	const float posterSize = ImGui::GetFrameHeight() * ThemeManager::Constants::PRESET_POSTER_HEIGHT_FRAMES;
	DrawThemeLogo(*info, std::min(posterSize, ImGui::GetContentRegionAvail().x));
	BrowserUI::SameLineIfFits(ImGui::CalcTextSize(info->displayName.c_str()).x);
	ImGui::BeginGroup();
	{
		MenuFonts::FontRoleGuard title(Menu::FontRole::Heading);
		ImGui::TextWrapped("%s", info->displayName.c_str());
	}
	{
		MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
		const std::string meta = info->version.empty() ? info->author : std::format("{} \xC2\xB7 v{}", info->author, info->version);
		Util::Text::WrappedDisabled("%s", meta.c_str());
	}
	ImGui::EndGroup();

	ImGui::Spacing();
	if (!info->description.empty())
		ImGui::TextWrapped("%s", info->description.c_str());
	ImGui::Spacing();

	const bool active = globals::menu->GetSettings().SelectedThemePreset == info->name;
	{
		Util::DisableGuard disabled(active);
		if (BrowserUI::FilterChip("##ApplyTheme", { T("menu.presets.apply_theme", "Apply Theme") }, active))
			globals::menu->LoadThemePreset(info->name);
	}
	ImGui::SameLine();
	if (BrowserUI::IconButton("##OpenThemes", Icons::TI(ICON_TI_FOLDER_OPEN), T("menu.presets.open_theme_folder", "Open Theme Folder"))) {
		OpenThemesFolder();
	}
	if (active) {
		MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.SuccessColor, "%s", T("menu.presets.active_theme", "This is the current menu theme."));
	}
	ImGui::EndChild();
}
