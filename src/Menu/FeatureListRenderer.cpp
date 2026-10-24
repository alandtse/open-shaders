#include "FeatureListRenderer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "CSEditor/Browser/BrowserWidgets.h"
#include "CSEditor/EditorWindow.h"
#include "CSEditor/SceneManager/FeatureOverwritesPanel.h"
#include "CSEditor/SceneManager/ScenePresetExport.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "CSEditor/SceneManager/SceneSettingsUI.h"
#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "Feature.h"
#include "FeatureConstraints.h"
#include "FeatureIssues.h"
#include "Features/CSEditor.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Menu.h"
#include "Menu/HomePageRenderer.h"
#include "Menu/IconLoader.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/PerformanceRenderer.h"
#include "Menu/PresetsPageRenderer.h"
#include "Menu/ProfilingRenderer.h"
#include "Menu/ThemeManager.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

namespace
{
	void DrawFeatureActionHint(const char* label, const char* details = nullptr)
	{
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
			return;
		const auto itemMin = ImGui::GetItemRectMin();
		ImGui::PushID(ImGui::GetItemID());
		const SKSE::stl::scope_exit restoreID([]() noexcept { ImGui::PopID(); });
		auto* storage = ImGui::GetStateStorage();
		const ImGuiID progressId = ImGui::GetID("##ActionHintProgress");
		const ImGuiID frameId = ImGui::GetID("##ActionHintFrame");
		const int frame = ImGui::GetFrameCount();
		const float previous = storage->GetInt(frameId, -1) == frame - 1 ? storage->GetFloat(progressId) : 0.0f;
		const float progress = std::min(previous + ImGui::GetIO().DeltaTime / ThemeManager::Constants::SIDEBAR_ROW_FADE_DURATION, 1.0f);
		storage->SetFloat(progressId, progress);
		storage->SetInt(frameId, frame);
		const float easedProgress = Util::Smoothstep(0.0f, 1.0f, progress);
		const auto* viewport = ImGui::GetWindowViewport();
		const auto& style = ImGui::GetStyle();
		const auto padding = style.WindowPadding;
		const float right = itemMin.x - style.ItemSpacing.x;
		MenuFonts::FontRoleGuard font(Menu::FontRole::Subtext);
		const float wrapWidth = std::min(ImGui::GetFontSize() * ThemeManager::Constants::FEATURE_ACTION_HINT_WIDTH_EM,
			std::max(ImGui::GetFontSize(), right - viewport->WorkPos.x - padding.x * 2.0f));
		const auto labelSize = ImGui::CalcTextSize(label, nullptr, false, wrapWidth);
		const auto detailSize = details ? ImGui::CalcTextSize(details, nullptr, false, wrapWidth) : ImVec2();
		const ImVec2 size(std::max(labelSize.x, detailSize.x) + padding.x * 2.0f,
			labelSize.y + detailSize.y + (details ? style.ItemSpacing.y : 0.0f) + padding.y * 2.0f);
		const float left = std::max(viewport->WorkPos.x, right - size.x);
		const float top = std::clamp(itemMin.y, viewport->WorkPos.y,
			std::max(viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - size.y));
		const float slide = std::min(style.FramePadding.x, style.ItemSpacing.x) * (1.0f - easedProgress);
		const float animatedLeft = std::min(left + slide, std::max(viewport->WorkPos.x, viewport->WorkPos.x + viewport->WorkSize.x - size.x));
		ImGui::SetNextWindowPos(ImVec2(animatedLeft, top));
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * easedProgress);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, std::max(style.WindowRounding, style.FrameRounding));
		const SKSE::stl::scope_exit restoreStyle([]() noexcept { ImGui::PopStyleVar(2); });
		if (ImGui::BeginTooltip()) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
			ImGui::TextUnformatted(label);
			if (details)
				ImGui::TextUnformatted(details);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	// Core built-in menu names that always appear first in the menu list
	// These are canonical identifiers used for logic — NOT translated
	constexpr std::array<const char*, 8> CORE_MENU_NAMES = {
		"Home", "Presets", "General", "Performance", "Advanced", "Profiling", "CSEditor", "Display"
	};

	/** @brief Draws a full-width Selectable with a rounded accent pill that also carries its hover/selected state. @return True when clicked. */
	bool PillSelectable(const std::string& label, bool selected)
	{
		constexpr float idleAlpha = 0.18f, hoveredAlpha = 0.28f, selectedAlpha = 0.35f;
		auto* drawList = ImGui::GetWindowDrawList();
		ImDrawListSplitter channels;
		channels.Split(drawList, 2);
		channels.SetCurrentChannel(drawList, 1);

		for (auto col : { ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive })
			ImGui::PushStyleColor(col, ImVec4(0, 0, 0, 0));
		const bool clicked = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns);
		ImGui::PopStyleColor(3);

		const auto rowMin = ImGui::GetItemRectMin();
		const auto rowMax = ImGui::GetItemRectMax();
		ImVec4 pill = globals::menu->GetSettings().Theme.StatusPalette.InfoColor;
		pill.w = selected ? selectedAlpha : (ImGui::IsItemHovered() ? hoveredAlpha : idleAlpha);
		channels.SetCurrentChannel(drawList, 0);
		drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(pill), (rowMax.y - rowMin.y) * 0.5f);
		channels.Merge(drawList);
		return clicked;
	}

	/** @brief Glyph shown on the sort button for the given feature list sort mode. */
	Icons::GlyphRef GetSortModeIcon(Menu::FeatureSortMode mode)
	{
		switch (mode) {
		case Menu::FeatureSortMode::Alphabetical:
			return Icons::TI(ICON_TI_SORT_ASCENDING_LETTERS);
		case Menu::FeatureSortMode::Category:
		default:  // unknown values from a hand-edited config fall back to categories
			return Icons::TI(ICON_TI_CATEGORY);
		}
	}

	const char* GetCoreMenuDisplayName(const char* canonicalName)
	{
		if (std::strcmp(canonicalName, "Home") == 0)
			return T("menu.features.home", "Home");
		if (std::strcmp(canonicalName, "Presets") == 0)
			return T("menu.features.presets", "Presets");
		if (std::strcmp(canonicalName, "General") == 0)
			return T("menu.features.general", "General");
		if (std::strcmp(canonicalName, "Advanced") == 0)
			return T("menu.features.advanced", "Advanced");
		if (std::strcmp(canonicalName, "Profiling") == 0)
			return T("menu.features.profiling", "Profiling");
		if (std::strcmp(canonicalName, "CS Editor") == 0)
			return T("menu.features.cs_editor", "OS Editor");
		if (std::strcmp(canonicalName, "Display") == 0)
			return T("menu.features.display", "Display");
		return canonicalName;
	}

	bool IsCoreMenu(const std::string& menuName)
	{
		return std::find(CORE_MENU_NAMES.begin(), CORE_MENU_NAMES.end(), menuName) != CORE_MENU_NAMES.end();
	}

	/**
	 * @brief Determines if the left feature panel should be visible based on auto-hide settings and mouse position
	 * @return true if panel should be visible, false if it should be hidden
	 */
	bool ShouldShowLeftPanel()
	{
		bool autoHideEnabled = globals::menu->GetSettings().AutoHideFeatureList;
		static bool leftPanelVisible = true;
		static float hoverStartTime = 0.0f;
		static bool wasHovering = false;

		if (!autoHideEnabled) {
			leftPanelVisible = true;
			return true;
		}

		// Get mouse position and window bounds
		ImVec2 mousePos = ImGui::GetMousePos();
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();
		float currentTime = static_cast<float>(ImGui::GetTime());

		// Use constants for auto-hide behavior
		const float activationZoneWidth = ThemeManager::Constants::AUTOHIDE_ACTIVATION_ZONE_WIDTH;
		const float expandDelay = ThemeManager::Constants::AUTOHIDE_EXPAND_DELAY;
		const float panelWidth = windowSize.x * ThemeManager::Constants::AUTOHIDE_PANEL_WIDTH_RATIO;

		// Calculate relative X position
		const float relativeX = mousePos.x - windowPos.x;

		// For activation: only check if mouse is at left edge (allow any Y position for easier triggering)
		// Prevent negative X from triggering, but don't restrict Y-axis for activation
		bool mouseInActivationZone = relativeX >= 0.0f && relativeX < activationZoneWidth;

		// For staying visible: check both X and Y to ensure mouse is actually over the panel area
		const bool mouseOverPanelX = relativeX >= 0.0f && relativeX < panelWidth;
		const bool mouseOverPanelY = mousePos.y >= windowPos.y && mousePos.y <= (windowPos.y + windowSize.y);
		bool mouseOverPanel = leftPanelVisible && mouseOverPanelX && mouseOverPanelY;

		// Track hover start time
		if (mouseInActivationZone && !wasHovering) {
			hoverStartTime = currentTime;
			wasHovering = true;
		} else if (!mouseInActivationZone) {
			wasHovering = false;
		}

		// Expand only after delay has elapsed
		bool shouldExpand = mouseInActivationZone && (currentTime - hoverStartTime >= expandDelay);

		// Update visibility: expand with delay, or stay visible while mouse is over panel
		if (shouldExpand || mouseOverPanel) {
			leftPanelVisible = true;
		} else if (!mouseOverPanel && !mouseInActivationZone) {
			leftPanelVisible = false;
		}

		return leftPanelVisible;
	}

	void SeparatorTextWithFont(const char* text, Menu::FontRole role)
	{
		MenuFonts::FontRoleGuard guard(role);
		ImGui::SeparatorText(text);
	}

	void SeparatorTextWithFont(const std::string& text, Menu::FontRole role)
	{
		SeparatorTextWithFont(text.c_str(), role);
	}

	bool BeginTabItemWithFont(const char* label, Menu::FontRole role, ImGuiTabItemFlags flags = ImGuiTabItemFlags_None)
	{
		return MenuFonts::BeginTabItemWithFont(label, role, flags);
	}

	/**
	 * @brief Draws a feature header with the feature name in large text and version in smaller text
	 * @param featureName The display name of the feature
	 * @param version The version string (can be empty)
	 * @param description Short description shown below the title (single line, truncated if too long)
	 * @return The height of just the title line (for button alignment)
	 */
	float DrawFeatureHeader(const std::string& featureName, const std::string& version, const std::string& description = "", const std::string& stageTag = "", ImVec4 stageColor = {})
	{
		auto& themeSettings = globals::menu->GetTheme();
		auto& palette = themeSettings.Palette;
		auto& featureHeading = themeSettings.FeatureHeading;

		// Sanitize and clamp to UI slider range to prevent malformed theme JSON from destabilizing layout
		float titleScale = featureHeading.FeatureTitleScale;
		if (!std::isfinite(titleScale)) {
			titleScale = ThemeManager::Constants::DEFAULT_FEATURE_TITLE_SCALE;
		}
		titleScale = std::clamp(titleScale, 1.0f, 3.0f);

		ImVec2 startPos = ImGui::GetCursorScreenPos();

		// Calculate title size and draw feature name with Title font
		ImVec2 titleSize;
		ImVec2 titleMax;
		{
			MenuFonts::FontRoleGuard titleGuard(Menu::FontRole::Title);
			ImGui::SetWindowFontScale(titleScale);
			ImGui::TextUnformatted(featureName.c_str());
			titleSize = ImGui::GetItemRectSize();
			titleMax = ImGui::GetItemRectMax();
			ImGui::SetWindowFontScale(1.0f);
		}

		// Store the title-only height for return value
		float titleOnlyHeight = titleSize.y;

		// Running x for bottom-aligned annotations (stage tag, then version) to the right of the title
		float annotationX = titleMax.x + ImGui::GetStyle().ItemSpacing.x;

		// Draw stage marker ([ALPHA]/[BETA]) on same line, bottom-aligned
		if (!stageTag.empty()) {
			ImVec2 tagSize;
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				tagSize = ImGui::CalcTextSize(stageTag.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}

			ImGui::SetCursorScreenPos(ImVec2(annotationX, titleMax.y - tagSize.y));
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				ImGui::TextColored(stageColor, "%s", stageTag.c_str());
				annotationX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
				ImGui::SetWindowFontScale(1.0f);
			}

			ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + titleSize.y + ImGui::GetStyle().ItemSpacing.y * 0.25f));
		}

		// Draw version on same line with Body font, bottom-aligned if version exists
		if (!version.empty()) {
			// Format version: replace dashes with dots for consistency
			std::string formattedVersion = version;
			std::replace(formattedVersion.begin(), formattedVersion.end(), '-', '.');

			// Calculate version text size at scaled size
			ImVec2 versionSize;
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				versionSize = ImGui::CalcTextSize(("v" + formattedVersion).c_str());
				ImGui::SetWindowFontScale(1.0f);
			}

			// Position version text: right of the stage tag (or title), bottom-aligned
			float versionX = annotationX;
			float versionY = titleMax.y - versionSize.y;

			ImGui::SetCursorScreenPos(ImVec2(versionX, versionY));

			// Use dimmed text color for version
			ImVec4 versionColor = palette.Text;
			versionColor.w *= ThemeManager::Constants::VERSION_TEXT_OPACITY;

			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				ImGui::TextColored(versionColor, "v%s", formattedVersion.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}

			// Reset cursor to after the title block
			ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + titleSize.y + ImGui::GetStyle().ItemSpacing.y * 0.25f));
		}

		// Draw description if provided (wrapped to content width)
		if (!description.empty()) {
			MenuFonts::FontRoleGuard subtextGuard(Menu::FontRole::Subtext);
			ImVec4 descColor = palette.Text;
			descColor.w *= 0.7f;  // Slightly dimmed
			ImGui::PushStyleColor(ImGuiCol_Text, descColor);
			ImGui::TextWrapped("%s", description.c_str());
			ImGui::PopStyleColor();
		}

		// Draw plain separator below
		ImGui::Separator();

		return titleOnlyHeight;
	}

	// ---------------------------------------------------------------------------
	// Persistent state for the reactive constraint warning popup.
	// DrawMenuVisitor is reconstructed every frame (it's a temporary passed to
	// std::visit), so member state is lost immediately.  These file-scope
	// variables survive across frames so the popup can actually render.
	// ---------------------------------------------------------------------------

	// Set of constraint keys we have already "seen" (and therefore warned about
	// or suppressed).  Keyed as "featureShortName|settingPath".
	std::unordered_set<std::string> g_knownConstraintKeys;
	bool g_knownConstraintKeysInitialised = false;

	// Pending popup state: non-empty when we have new constraints to show.
	bool g_reactiveWarningShow = false;
	std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> g_reactiveWarningConstraints;

	// "Don't show again" checkbox state inside the modal (reset each time popup opens).
	bool g_dontShowAgainCheckbox = false;

	/**
	 * @brief Queues the warning popup for constraints that appeared since the last call; the first call only seeds.
	 * Diffing every frame also catches changes made outside DrawSettings (e.g. Upscaling's render-loop resolutionScale).
	 */
	void DetectReactiveConstraints()
	{
		if (g_reactiveWarningShow)  // don't overwrite a pending popup
			return;

		auto currentConstraints = FeatureConstraints::GetAllActiveConstraints();

		if (!g_knownConstraintKeysInitialised) {
			// First time: seed known set, no popup
			for (const auto& [settingId, result] : currentConstraints) {
				g_knownConstraintKeys.insert(settingId.featureShortName + "|" + settingId.settingPath);
			}
			g_knownConstraintKeysInitialised = true;
			return;
		}

		// Diff: find keys present now but not previously known
		std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> newConstraints;
		std::unordered_set<std::string> currentKeys;
		for (const auto& [settingId, result] : currentConstraints) {
			std::string key = settingId.featureShortName + "|" + settingId.settingPath;
			currentKeys.insert(key);
			if (g_knownConstraintKeys.find(key) == g_knownConstraintKeys.end()) {
				newConstraints.emplace_back(settingId, result);
			}
		}
		// Update known set to current (removes keys for constraints that went away)
		g_knownConstraintKeys = std::move(currentKeys);

		if (!newConstraints.empty() && !globals::menu->GetSettings().SkipConstraintWarning) {
			logger::info("Reactive constraint detection: {} new constraints", newConstraints.size());
			for (const auto& [settingId, result] : newConstraints) {
				logger::info("  - {}.{} forced to {} by {}", settingId.featureShortName, settingId.settingPath, FeatureConstraints::FormatConstraintValue(result.forcedValue), result.sources.empty() ? "?" : result.sources[0].featureName);
			}
			g_reactiveWarningShow = true;
			g_reactiveWarningConstraints = std::move(newConstraints);
			g_dontShowAgainCheckbox = false;
		}
	}
}

std::string FeatureListRenderer::TranslateCategory(std::string_view category)
{
	if (category == FeatureCategories::kCharacters)
		return T("feature.category.characters", "Characters");
	if (category == FeatureCategories::kDisplay)
		return T("feature.category.display", "Display");
	if (category == FeatureCategories::kFoliage)
		return T("feature.category.grass", "Foliage");
	if (category == FeatureCategories::kLandscapeAndTextures)
		return T("feature.category.landscape_and_textures", "Landscape & Textures");
	if (category == FeatureCategories::kLighting)
		return T("feature.category.lighting", "Lighting");
	if (category == FeatureCategories::kMaterials)
		return T("feature.category.materials", "Materials");
	if (category == "Post-Processing")
		return T("feature.category.post_processing", "Post-Processing");
	if (category == FeatureCategories::kOther)
		return T("feature.category.other", "Other");
	if (category == FeatureCategories::kSky)
		return T("feature.category.sky", "Sky");
	if (category == FeatureCategories::kUtility)
		return T("feature.category.utility", "Utilities");
	if (category == FeatureCategories::kWater)
		return T("feature.category.water", "Water");

	return std::string(category);
}

void FeatureListRenderer::RenderFeatureList(
	float footerHeight,
	size_t& selectedMenu,
	std::string& featureSearch,
	std::string& pendingFeatureSelection,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings, bool editorLayout, bool sidebarVisible, bool resetLayout)
{
	ImGui::BeginChild("Menus Table", ImVec2(0, -footerHeight));

	// A search filter or a collapsed category could hide the feature being navigated to
	if (!pendingFeatureSelection.empty()) {
		featureSearch.clear();
		if (const auto* feature = Feature::FindFeatureByShortName(pendingFeatureSelection))
			categoryExpansionStates[std::string(feature->GetCategory())] = true;
	}

	const auto buildVisibleMenus = [&] {
		auto menus = BuildMenuList(featureSearch, categoryExpansionStates, drawGeneralSettings, drawAdvancedSettings);
		if (editorLayout)
			std::erase_if(menus, [](const MenuFuncInfo& entry) {
				if (const auto* feature = std::get_if<Feature*>(&entry))
					return (*feature)->GetShortName() == "SceneManager";
				if (const auto* page = std::get_if<BuiltInMenu>(&entry))
					return page->canonicalId == "CSEditor";
				return false;
			});
		return menus;
	};
	auto menuList = buildVisibleMenus();

	HandlePendingFeatureSelection(pendingFeatureSelection, menuList, selectedMenu);

	// Determine if left panel should be visible based on auto-hide settings
	bool leftPanelVisible = sidebarVisible && ShouldShowLeftPanel();

	// Create the table with appropriate number of columns based on visibility
	int numColumns = leftPanelVisible ? 2 : 1;
	if (ImGui::BeginTable("Menus Table", numColumns, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
		if (leftPanelVisible) {
			const float sidebarWidth = ThemeManager::Constants::EDITOR_MENU_SIDEBAR_WIDTH * Util::GetUIScale();
			ImGui::TableSetupColumn("##ListOfMenus", editorLayout ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_None, editorLayout ? sidebarWidth : 2.0f);
			ImGui::TableSetupColumn("##MenuConfig", ImGuiTableColumnFlags_WidthStretch, editorLayout ? 1.0f : 8.0f);
			if (editorLayout && resetLayout)
				ImGui::TableSetColumnWidth(0, sidebarWidth);
			const auto previousSort = globals::menu->GetSettings().FeatureListSort;
			RenderLeftColumn(menuList, selectedMenu, featureSearch, categoryExpansionStates);
			if (globals::menu->GetSettings().FeatureListSort != previousSort) {
				// Sort order changed: rebuild now and re-select the same feature
				std::string reselect;
				if (const auto* feat = selectedMenu < menuList.size() ? std::get_if<Feature*>(&menuList[selectedMenu]) : nullptr)
					reselect = (*feat)->GetShortName();
				menuList = buildVisibleMenus();
				HandlePendingFeatureSelection(reselect, menuList, selectedMenu);
			}
			RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);
		} else {
			// When left panel is hidden, right column takes full width
			ImGui::TableSetupColumn("##MenuConfig", 0, 1);
			RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);
		}

		ImGui::EndTable();
	}

	ImGui::EndChild();
}

std::vector<FeatureListRenderer::MenuFuncInfo> FeatureListRenderer::BuildMenuList(
	const std::string& featureSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	// Build the menu list
	auto& featureList = Feature::GetFeatureList();
	auto sortedFeatureList{ featureList };  // need a copy so the load order is not lost
	std::ranges::sort(sortedFeatureList, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});

	// Filter features by search string
	if (!featureSearch.empty()) {
		auto it = std::remove_if(sortedFeatureList.begin(), sortedFeatureList.end(),
			[&featureSearch](Feature* feat) { return !Util::FeatureMatchesSearch(feat, featureSearch); });
		sortedFeatureList.erase(it, sortedFeatureList.end());
	}

	auto menuList = std::vector<MenuFuncInfo>{
		BuiltInMenu{ T("menu.features.home", "Home"), []() { HomePageRenderer::RenderHomePage(); }, "Home" },
		BuiltInMenu{ T("menu.features.presets", "Presets"), []() { PresetsPageRenderer::Render(); }, "Presets" },
		BuiltInMenu{ T("menu.features.general", "General"), drawGeneralSettings, "General" },
		BuiltInMenu{ T("menu.features.performance", "Performance"), [] { PerformanceRenderer::Render(); }, "Performance" },
		BuiltInMenu{ T("menu.features.advanced", "Advanced"), drawAdvancedSettings, "Advanced" },
		BuiltInMenu{ T("menu.features.profiling", "Profiling"), []() { ProfilingRenderer::RenderStatistics(); }, "Profiling" },
		BuiltInMenu{ T("menu.features.cs_editor", "OS Editor"), []() { globals::features::csEditor.DrawSettings(); }, "CSEditor" }
	};  // NOTE: The menu list is rebuilt every frame, so category expansion states
	// persist correctly. This is acceptable since the list is small and built
	// infrequently, but could be optimized if performance becomes an issue.

	const auto isFavorite = [](Feature* feat) {
		return feat->IsInMenu() && feat->loaded && globals::state->IsFeatureFavorite(feat->GetShortName());
	};

	// Favorites leave their category's count; recounted here since they change at runtime
	Menu::categoryCounts.clear();
	for (Feature* feat : featureList) {
		if (feat->IsInMenu() && feat->loaded && !isFavorite(feat))
			Menu::categoryCounts[std::string(feat->GetCategory())]++;
	}

	// Favorites are pinned above the rest, outside any category
	std::ranges::copy(sortedFeatureList | std::ranges::views::filter(isFavorite), std::back_inserter(menuList));

	// Group features by category, or list them flat when sorting alphabetically
	const auto sortMode = globals::menu->GetSettings().FeatureListSort;
	std::map<std::string, std::vector<Feature*>> categorizedFeatures;
	for (Feature* feat : sortedFeatureList) {
		if (feat->IsInMenu() && feat->loaded && !isFavorite(feat)) {
			switch (sortMode) {
			case Menu::FeatureSortMode::Alphabetical:
				menuList.push_back(feat);  // already in display-name order
				break;
			case Menu::FeatureSortMode::Category:
			default:  // unknown values from a hand-edited config fall back to categories
				categorizedFeatures[std::string(feat->GetCategory())].push_back(feat);
				break;
			}
		}
	}

	// Sort features within each category
	for (auto& [category, features] : categorizedFeatures) {
		std::ranges::sort(features, [](Feature* a, Feature* b) {
			return a->GetDisplayName() < b->GetDisplayName();
		});
	}

	// Add categorized features to menu with collapsible headers
	for (const auto categoryName : FeatureCategories::kMenuOrder) {
		const std::string category(categoryName);
		if (categorizedFeatures.find(category) != categorizedFeatures.end() && !categorizedFeatures[category].empty()) {
			// Initialize expansion state if not exists
			if (categoryExpansionStates.find(category) == categoryExpansionStates.end()) {
				categoryExpansionStates[category] = true;  // Default to expanded
			}

			// Add category header
			menuList.push_back(CategoryHeader{ category });

			// Add features only if category is expanded
			if (categoryExpansionStates[category]) {
				std::ranges::copy(categorizedFeatures[category], std::back_inserter(menuList));
			}
		}
	}

	// Add any categories not in the predefined order
	for (const auto& [category, features] : categorizedFeatures) {
		if (std::ranges::find(FeatureCategories::kMenuOrder, std::string_view(category)) == FeatureCategories::kMenuOrder.end() && !features.empty()) {
			// Initialize expansion state if not exists
			if (categoryExpansionStates.find(category) == categoryExpansionStates.end()) {
				categoryExpansionStates[category] = true;  // Default to expanded
			}

			// Add category header
			menuList.push_back(CategoryHeader{ category });

			// Add features only if category is expanded
			if (categoryExpansionStates[category]) {
				std::ranges::copy(features, std::back_inserter(menuList));
			}
		}
	}

	auto unloadedFeatures = sortedFeatureList | std::ranges::views::filter([](Feature* feat) {
		return !feat->loaded && feat->IsInMenu() && !feat->IsHiddenUnreleased() && (!FeatureIssues::IsObsoleteFeature(feat->GetShortName()) || globals::state->IsDeveloperMode());
	});
	if (std::ranges::distance(unloadedFeatures) != 0) {
		menuList.push_back(T("menu.features.unloaded_features", "Unloaded Features"));
		std::ranges::copy(unloadedFeatures, std::back_inserter(menuList));
	}
	// Add top section for feature issues (rejected features, obsolete info, etc.)
	if (FeatureIssues::HasFeatureIssues()) {
		menuList.insert(menuList.begin(), BuiltInMenu{ T("menu.features.feature_issues", "Feature Issues"), []() {
														  FeatureIssues::DrawFeatureIssuesUI();
													  } });
	}

	return menuList;
}

void FeatureListRenderer::HandlePendingFeatureSelection(
	std::string& pendingFeatureSelection,
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu)
{
	if (!pendingFeatureSelection.empty()) {
		for (size_t i = 0; i < menuList.size(); ++i) {
			if (std::holds_alternative<Feature*>(menuList[i])) {
				Feature* feature = std::get<Feature*>(menuList[i]);
				if (feature->GetShortName() == pendingFeatureSelection) {
					selectedMenu = i;
					logger::info("Navigated to {} feature menu", pendingFeatureSelection);
					break;
				}
			} else if (const auto* page = std::get_if<BuiltInMenu>(&menuList[i]); page && (page->canonicalId == pendingFeatureSelection || page->name == pendingFeatureSelection)) {
				// Built-in pages such as Presets are addressed by their display name.
				selectedMenu = i;
				logger::info("Navigated to {} page", pendingFeatureSelection);
				break;
			}
		}
		pendingFeatureSelection.clear();  // Clear after processing
	}
}

void FeatureListRenderer::RenderLeftColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	std::map<std::string, bool>& categoryExpansionStates)
{
	ImGui::TableNextColumn();
	// Draw the feature list
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
	if (ImGui::BeginListBox("##MenusList", { -FLT_MIN, -FLT_MIN })) {
		// Find where core built-in menus end (Home, General, Advanced, Display)
		size_t coreMenuCount = 0;
		for (size_t i = 0; i < menuList.size(); i++) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				const BuiltInMenu& menu = std::get<BuiltInMenu>(menuList[i]);
				if (IsCoreMenu(menu.canonicalId)) {
					coreMenuCount++;
				}
			}
		}

		// First render the core built-in menus (Home, General, Advanced, Display)
		size_t renderedCoreMenus = 0;
		for (size_t i = 0; i < menuList.size() && renderedCoreMenus < CORE_MENU_NAMES.size(); i++) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				const BuiltInMenu& menu = std::get<BuiltInMenu>(menuList[i]);
				if (IsCoreMenu(menu.canonicalId)) {
					std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates }, menuList[i]);
					renderedCoreMenus++;
				}
			}
		}

		Util::DrawSectionHeader(T("menu.features.features", "Features"), true);
		const float sortButtonWidth = ImGui::GetFrameHeight();
		Util::DrawFeatureSearchBar(featureSearch, ImGui::GetContentRegionAvail().x - sortButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		const float searchBarHeight = ImGui::GetItemRectSize().y;
		ImGui::SameLine();

		{
			auto style = Util::TransparentIconButtonStyle();
			const auto sortIcon = GetSortModeIcon(globals::menu->GetSettings().FeatureListSort);
			if (Icons::Button("###FeatureSort", sortIcon, ImVec2(sortButtonWidth, searchBarHeight)))
				ImGui::OpenPopup("##FeatureSortPopup");
		}

		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T("menu.features.sort_tooltip", "Sort features"));

		if (ImGui::BeginPopup("##FeatureSortPopup")) {
			using enum Menu::FeatureSortMode;
			auto& sort = globals::menu->GetSettings().FeatureListSort;
			if (ImGui::MenuItem(T("menu.features.sort_by_category", "By Category"), nullptr, sort == Category))
				sort = Category;
			if (ImGui::MenuItem(T("menu.features.sort_alphabetically", "Alphabetical"), nullptr, sort == Alphabetical))
				sort = Alphabetical;
			ImGui::EndPopup();
		}

		// Then render the rest (features and categories, but skip already rendered core menus)
		for (size_t i = 0; i < menuList.size(); i++) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				const BuiltInMenu& menu = std::get<BuiltInMenu>(menuList[i]);
				if (IsCoreMenu(menu.canonicalId)) {
					continue;  // Skip, already rendered
				}
			}
			std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates }, menuList[i]);
		}

		ImGui::EndListBox();
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

void FeatureListRenderer::RenderRightColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t selectedMenu,
	std::string& pendingFeatureSelection)
{
	ImGui::TableNextColumn();

	if (selectedMenu < menuList.size()) {
		std::visit(DrawMenuVisitor{ pendingFeatureSelection }, menuList[selectedMenu]);
	} else {
		ImGui::TextDisabled("%s", T("menu.features.select_item_left", "Please select an item on the left."));
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const BuiltInMenu& menu)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	// Use error color for Feature Issues menu item
	bool isFeatureIssues = (menu.name == T("menu.features.feature_issues", "Feature Issues"));
	bool isCSEditor = (menu.name == T("menu.features.cs_editor", "OS Editor"));
	bool isPresets = (menu.name == T("menu.features.presets", "Presets"));

	if (isFeatureIssues) {
		auto& themeSettings = globals::menu->GetSettings().Theme;
		ImGui::PushStyleColor(ImGuiCol_Text, themeSettings.StatusPalette.Error);

		if (PillSelectable(fmt::format(" {} ", menu.name), selectedMenuRef == listId))
			selectedMenuRef = listId;

		ImGui::PopStyleColor();
	} else if (isCSEditor) {
		// Launcher entry: opens the editor without leaving the current page.
		if (PillSelectable(fmt::format(" {} ", menu.name), false))
			CSEditor::OpenEditorWindow();
	} else if (isPresets) {
		if (PillSelectable(fmt::format(" {} ", menu.name), selectedMenuRef == listId))
			selectedMenuRef = listId;
	} else {
		if (PillSelectable(fmt::format(" {} ", menu.name), selectedMenuRef == listId))
			selectedMenuRef = listId;
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const std::string& label)
{
	// Style "Unloaded Features" to match category headers
	if (label == T("menu.features.unloaded_features", "Unloaded Features")) {
		Util::DrawSectionHeader(label.c_str(), true);
	} else {
		// Use default separator text for other labels - should be themed via ImGuiCol_Separator
		SeparatorTextWithFont(label, Menu::FontRole::Subheading);
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const CategoryHeader& header)
{
	// Get expansion state from static map
	bool isExpanded = categoryExpansionStates[header.name];

	// Draw category header with custom styling using util:UI function
	// Use Heading font for category headers
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
		int count = Menu::categoryCounts[std::string(header.name)];
		const auto categoryLabel = TranslateCategory(header.name);
		Util::DrawCategoryHeader(header.name.c_str(), categoryLabel.c_str(), isExpanded, count);
	}

	// Update expansion state
	categoryExpansionStates[header.name] = isExpanded;
}

void FeatureListRenderer::ListMenuVisitor::operator()(Feature* feat)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	auto& themeSettings = globals::menu->GetSettings().Theme;

	ImVec4 textColor;

	// Determine the text color based on the state
	if (isDisabled) {
		textColor = themeSettings.StatusPalette.Disable;
	} else if (isLoaded) {
		textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	} else if (hasFailedMessage) {
		textColor = feat->version.empty() ? themeSettings.StatusPalette.Disable : themeSettings.StatusPalette.Error;
	} else {
		// Installed but not loaded means the feature is only pending a restart (green),
		// otherwise it is simply missing (grey).
		textColor = feat->installed ? themeSettings.StatusPalette.RestartNeeded : themeSettings.StatusPalette.Disable;
	}

	// Create selectable item with semantic color
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	if (PillSelectable(fmt::format(" {} ", feat->GetDisplayName()), selectedMenuRef == listId)) {
		selectedMenuRef = listId;
	}
	ImGui::PopStyleColor();

	// Favorites are pinned at the top of the list, marked by a pin attached to the name (its label ends in a space)
	if (isLoaded && globals::state->IsFeatureFavorite(featureName)) {
		ImGui::SameLine(0.0f, 0.0f);
		const Icons::GlyphRef pin = Icons::TI(ICON_TI_PIN);
		const float lineHeight = ImGui::GetTextLineHeight();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		Icons::FontGuard pinFont(pin, 0.75f);
		const ImVec2 pinBox(ImGui::CalcTextSize(pin.utf8).x, lineHeight);
		ImGui::Dummy(pinBox);
		ImGui::GetWindowDrawList()->AddText(Icons::GetCenteredGlyphPos(origin, pinBox, pin.utf8),
			ImGui::GetColorU32(Util::Colors::GetAccent()), pin.utf8);
	}

	// Display the stage marker behind the name, regardless of loaded state
	if (const auto stage = feat->GetReleaseStage(); stage != Feature::ReleaseStage::Release) {
		ImGui::SameLine();
		ImGui::TextColored(Feature::GetReleaseStageColor(stage), "%s", Feature::GetReleaseStageTag(stage).c_str());
	}

	// A feature only authored for other scenes still gets a hollow dot, so its settings stay discoverable.
	if (auto* sceneManager = globals::sceneSettingsManager; sceneManager->HasAnySceneEntriesForFeature(featureName)) {
		const bool applying = sceneManager->IsFeatureOverridingBase(featureName);
		ImGui::SameLine();
		Util::DrawInlineIndicatorDot(ImGui::GetColorU32(Util::Colors::GetInfo()), applying);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(applying ?
									   T("menu.features.scene_indicator_overriding",
										   "Scene Manager is overriding settings here. Blue settings show its values.") :
									   T("menu.features.scene_indicator_configured",
										   "Has Scene Manager settings for other times, weathers or locations."));

		if (sceneManager->HasSketches(featureName)) {
			ImGui::SameLine();
			Util::DrawInlineIndicatorDot(ImGui::GetColorU32(Util::Colors::GetSuccess()), true);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T("menu.features.scene_indicator_sketched",
					"Has settings sketched over the Scene Manager's values. They revert once you leave this feature."));
		}
	}

	// Display version if loaded
	if (isLoaded) {
		ImGui::SameLine();
		std::string formattedVersion = feat->version;
		std::replace(formattedVersion.begin(), formattedVersion.end(), '-', '.');
		ImGui::TextDisabled(fmt::format("({})", formattedVersion).c_str());
	}
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const BuiltInMenu& menu)
{
	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		// Add spacing only for Home menu
		if (menu.name == T("menu.features.home", "Home")) {
			ImGui::Dummy(ImVec2(0, ThemeManager::Constants::BUTTON_SPACING));
		}
		menu.func();
	}
	ImGui::EndChild();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const std::string&)
{
	// std::unreachable() from c++23
	// you are not supposed to have selected a label!
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const CategoryHeader&)
{
	// Category headers are not selectable in the right panel
	ImGui::TextDisabled("%s", T("menu.features.select_feature_left", "Please select a feature from the left."));
}

void FeatureListRenderer::DrawMenuVisitor::operator()(Feature* feat)
{
	const bool disabled = globals::state->IsFeatureDisabled(feat->GetShortName());
	const auto& style = ImGui::GetStyle();
	const float railWidth = BrowserUI::IconButtonSize() + style.FramePadding.x * 2.0f;
	ImGui::PushID(feat->GetShortName().c_str());
	if (ImGui::BeginChild("##FeatureConfigFrame", ImVec2(0, 0), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		const float width = std::max(ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x - railWidth - style.ItemSpacing.x);
		if (ImGui::BeginChild("##FeaturePageContent", ImVec2(width, 0), false,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
			RenderFeatureHeader(feat, disabled, feat->loaded);
			SceneSettingsUI::DrawFeaturePageControls(feat, !disabled && feat->loaded);
			ImGui::Separator();
			if (ImGui::BeginChild("##FeatureSettingsScroll"))
				RenderFeatureSettings(feat, disabled, feat->loaded, !feat->failedLoadedMessage.empty());
			ImGui::EndChild();
		}
		ImGui::EndChild();
		ImGui::SameLine();
		RenderFeatureActions(feat, disabled, feat->loaded);
	}
	ImGui::EndChild();
	ImGui::PopID();
	FeatureOverwritesPanel::DrawExport();
	DrawConstraintWarningDialog(pendingFeatureSelection);
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureHeader(Feature* feat, bool, bool loaded)
{
	auto [description, details] = feat->GetFeatureSummary();
	const auto stage = feat->GetReleaseStage();
	DrawFeatureHeader(feat->GetDisplayName(), loaded ? feat->version : "", description,
		Feature::GetReleaseStageTag(stage), Feature::GetReleaseStageColor(stage));
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureActions(Feature* feat, bool disabled, bool loaded)
{
	const auto name = feat->GetShortName();
	const auto& style = ImGui::GetStyle();
	const float size = BrowserUI::IconButtonSize();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, std::max(style.ChildRounding, style.FrameRounding));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, style.Colors[ImGuiCol_FrameBg]);
	const bool visible = ImGui::BeginChild("##FeatureActionRail", ImVec2(size + style.FramePadding.x * 2.0f, 0),
		ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	if (visible) {
		const auto action = [&](const char* id, Icons::GlyphRef icon, const char* label, bool enabled, bool selected, auto&& run) {
			ImGui::BeginDisabled(!enabled);
			if (BrowserUI::IconButton(id, icon, nullptr, selected))
				run();
			ImGui::EndDisabled();
			DrawFeatureActionHint(label);
		};
		if (!feat->IsAlwaysEnabled())
			action("##Boot", Icons::TI(ICON_TI_BULB), T("menu.features.enable_at_boot", "Enable at Boot"), true, !disabled,
				[&] { feat->ToggleAtBootSetting(); });
		const bool favorite = globals::state->IsFeatureFavorite(name);
		action("##Favorite", Icons::TI(ICON_TI_PIN), favorite ? T("menu.features.remove_from_favorites", "Remove from Favorites") : T("menu.features.add_to_favorites", "Add to Favorites"), loaded, favorite, [&] {
			if (globals::state->SetFeatureFavorite(name, !favorite))
				globals::menu->SelectFeatureMenu(name);
		});
		if (SceneSettingsUI::CanEditFeaturePage(feat))
			action("##Scene", Icons::TI(ICON_TI_STACK_2), T("feature.scene_manager.name", "Scene Manager"), loaded && !disabled,
				SceneSettingsUI::IsFeaturePageEditing(feat), [&] {
					if (SceneSettingsUI::IsFeaturePageEditing(feat))
						SceneSettingsUI::HideFeaturePageEditing();
					else
						SceneSettingsUI::BeginFeaturePageEditing(feat);
				});
		auto* editor = EditorWindow::GetSingleton();
		const auto undoLabel = I18n::GetSingleton()->Format("cs_editor.undo_states",
			{ { "count", std::to_string(editor->undoHistory.GetUndoCount()) } }, "Undo (Ctrl+Z) - {count} states");
		action("##Undo", Icons::TI(ICON_TI_ARROW_BACK_UP), undoLabel.c_str(), editor->undoHistory.CanUndo(), false,
			[&] { editor->PerformUndo(); });
		const auto redoLabel = I18n::GetSingleton()->Format("cs_editor.redo_states",
			{ { "count", std::to_string(editor->undoHistory.GetRedoCount()) } }, "Redo (Ctrl+Y) - {count} states");
		action("##Redo", Icons::TI(ICON_TI_ARROW_FORWARD_UP), redoLabel.c_str(), editor->undoHistory.CanRedo(), false,
			[&] { editor->PerformRedo(); });
		if (feat->UsesMainSettings()) {
			action("##Defaults", Icons::TI(ICON_TI_RESTORE), T("menu.features.restore_defaults", "Restore Defaults"), loaded && !disabled,
				false, [&] { globals::state->RestoreFeatureLayers(feat); });
			if (globals::state->HasEditsUnderBaselines(feat))
				action("##Baseline", Icons::TI(ICON_TI_ARROWS_EXCHANGE), T("menu.presets.use_values", "Use preset values"), loaded && !disabled,
					false, [&] { globals::state->ClearFeatureEditsUnderBaselines(feat); });
			action("##ExportBaseline", Icons::TI(ICON_TI_FILE_EXPORT), T("cs_editor.baseline_export", "Export Baseline Preset..."),
				loaded && !disabled, false, [&] { ScenePresetExport::OpenBaseline(name); });
		}
		if (auto* overrides = SettingsOverrideManager::GetSingleton(); feat->UsesMainSettings() && overrides->HasFeatureOverrides(name))
			action("##ApplyOverride", Icons::TI(ICON_TI_REFRESH), T("menu.features.apply_override", "Apply Override"), loaded && !disabled,
				false, [&] { feat->ReapplyCurrentPageOverrideSettings(); });
		if (FeatureOverwritesPanel::HasExportableSettings(feat))
			action("##ExportOverwrite", Icons::TI(ICON_TI_FILE_EXPORT), T("cs_editor.feature_overwrite_export", "Export Feature Overwrite..."),
				loaded && !disabled, false, [&] { FeatureOverwritesPanel::BeginExport(feat); });
	}
	ImGui::EndChild();
}

void FeatureListRenderer::DrawFeatureBody(Feature* feat)
{
	assert(feat && feat->loaded);

	const ImVec2 cursorPosBefore = ImGui::GetCursorPos();
	{
		// Sketches live only while their feature is on screen; the next Update drops any other.
		globals::sceneSettingsManager->RetainSketches(feat->GetShortName());
		SceneWidgetInterceptor::Scope baselineScope({ .feature = feat, .baseline = true });
		// ImGui raises this flag for any value edit; set only inside the call, it means this feature changed.
		// Buttons that rewrite settings (a reset, a preset) raise no flag, so a finished click is checked instead.
		const auto& context = *ImGui::GetCurrentContext();
		const bool editedBefore = context.ActiveIdHasBeenEditedThisFrame;
		feat->DrawSettings();
		if (feat->UsesMainSettings()) {
			auto* state = globals::state;
			const bool edited = !editedBefore && context.ActiveIdHasBeenEditedThisFrame;
			if (edited)
				state->MarkSettingsChanged();
			if (Util::InteractionEndedThisFrame()) {
				// Those buttons also bypass the interceptor, so the scene layer's cached apply document must
				// take their values before anything replays it.
				globals::sceneSettingsManager->CaptureExternalFeatureChanges(feat, true);
				if (!edited)
					state->CheckFeatureSettingsChanged(feat);
				state->InvalidateEditsUnderBaselines(feat);
			}
		}
	}
	const ImVec2 cursorPosAfter = ImGui::GetCursorPos();

	DetectReactiveConstraints();

	const float cursorEpsilon = 0.1f;
	bool cursorMoved = (std::abs(cursorPosAfter.x - cursorPosBefore.x) > cursorEpsilon ||
						std::abs(cursorPosAfter.y - cursorPosBefore.y) > cursorEpsilon);
	if (!cursorMoved) {
		ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Disable, "%s", T("menu.features.no_settings_available", "There are no settings available for this feature."));
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;

	if (isDisabled) {
		ImGui::TextColored(themeSettings.StatusPalette.Disable, "%s", T("menu.features.settings_hidden_disabled", "Feature settings are hidden because this feature is disabled at boot."));
		ImGui::Spacing();
		ImGui::Text("%s", T("menu.features.enable_to_access_config", "Enable the feature above to access its configuration options."));
	} else {
		if (isLoaded) {
			if (SceneSettingsUI::IsFeaturePageEditing(feat))
				SceneSettingsUI::DrawFeaturePageBody(feat);
			else
				DrawFeatureBody(feat);

			if (feat != &globals::features::csEditor &&
				ProfilingRenderer::PrepareFeatureTimers(feat->GetShortName(),
					ProfilingRenderer::IsFeatureProfilingAvailable(feat->GetShortName())) > 0.0f) {
				ImGui::SeparatorText(T("menu.features.profiling", "Profiling"));
				ProfilingRenderer::RenderFeatureTimers(feat->GetShortName());
			}
		} else {
			if (FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
				feat->DrawUnloadedUI();
			} else if (feat->installed) {
				ImGui::Text("%s", T("menu.features.available_after_restart", "This feature will be available after restart."));
			} else {
				feat->DrawUnloadedUI();
				if (!feat->GetFeatureModLink().empty()) {
					ImGui::Spacing();
					auto featureModLink = feat->GetFeatureModLink();
					const auto downloadText = std::vformat(
						T("menu.features.download_link", "Click here to download this feature ({})"), std::make_format_args(featureModLink));
					if (ImGui::Selectable(downloadText.c_str())) {
						ShellExecuteA(NULL, "open", featureModLink.c_str(), NULL, NULL, SW_SHOWNORMAL);
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T("menu.features.download_tooltip", "Download the feature from the mod page."));
					}
				}
			}
		}
	}

	if (hasFailedMessage && feat->DrawFailLoadMessage() && !FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
		ImGui::Spacing();
		SeparatorTextWithFont(T("menu.features.error_header", "Error"), Menu::FontRole::Subheading);
		ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", feat->failedLoadedMessage.c_str());
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderUseBaselinesButton(Feature* feat)
{
	const float size = ImGui::GetFrameHeight();
	{
		auto style = Util::TransparentIconButtonStyle();
		if (Icons::Button("##UseBaselines", Icons::TI(ICON_TI_STACK_2), ImVec2(size, size)))
			globals::state->ClearFeatureEditsUnderBaselines(feat);
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.features.use_baselines_tooltip",
							  "Use Baseline preset values: your changes override settings your Baseline presets set. "
							  "Click to drop those changes and use the presets' values. Your other changes stay."));
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderRestoreDefaultsButton(Feature* feat)
{
	const float size = ImGui::GetFrameHeight();
	{
		auto style = Util::TransparentIconButtonStyle();
		if (Icons::Button("##RestoreDefaults", Icons::TI(ICON_TI_RESTORE), ImVec2(size, size))) {
			// Clears the user's edits, so the feature shows what defaults and Baseline packs set.
			globals::state->RestoreFeatureLayers(feat);
		}
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.features.restore_defaults_tooltip", "Restore defaults: clears your edits to this feature, back to what defaults and your Baseline presets set"));
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderFavoriteButton(Feature* feat)
{
	const auto featureName = feat->GetShortName();
	const bool isFavorite = globals::state->IsFeatureFavorite(featureName);
	const float size = ImGui::GetFrameHeight();
	{
		auto style = Util::TransparentIconButtonStyle();
		if (Icons::Button("###Favorite", Icons::TI(isFavorite ? ICON_TI_PINNED_OFF : ICON_TI_PIN), ImVec2(size, size))) {
			globals::state->SetFeatureFavorite(featureName, !isFavorite);
			globals::state->CheckCoreSettingsChanged();
			// The favorites list shifts the menu; keep this feature selected
			globals::menu->SelectFeatureMenu(featureName);
		}
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", isFavorite ? T("menu.features.remove_from_favorites_tooltip", "Remove from favorites: moves this feature back into the main list") : T("menu.features.add_to_favorites_tooltip", "Add to favorites: lists this feature at the top of the feature list"));
	}
}

void FeatureListRenderer::DrawConstraintWarningDialog(std::string& pendingFeatureSelection)
{
	if (!g_reactiveWarningShow) {
		return;
	}

	constexpr const char* popupId = "###SettingChangeWarning";
	const std::string popupTitle = fmt::format("{}{}", T("menu.features.setting_change_warning_title", "Setting Change Warning"), popupId);

	// OpenPopup is idempotent while the popup is already open, so calling it
	// every frame while the flag is set is safe and ensures we don't miss the
	// one-frame window where ImGui expects it.
	ImGui::OpenPopup(popupId);

	// Center the popup (ImGuiCond_Always matches the Clear Cache dialog pattern)
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	if (Util::BeginPopupModalWithRoundedClose(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", T("menu.features.settings_adjusted_warning", "Some of your settings have been automatically adjusted due to feature incompatibilities."));
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		std::string navigationTarget;  // Set on a link click so the table and popup still end cleanly.

		// Table columns: Impacted Feature | Setting | Constrained By | Forced To
		if (ImGui::BeginTable("##ReactiveConstraintTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T("menu.features.col_impacted_feature", "Impacted Feature"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_setting", "Setting"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_constrained_by", "Constrained By"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_forced_to", "Forced To"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			size_t rowIndex = 0;
			for (const auto& [settingId, result] : g_reactiveWarningConstraints) {
				ImGui::TableNextRow();

				// --- Column 0: Impacted Feature (clickable -> navigate to that feature) ---
				ImGui::TableSetColumnIndex(0);
				{
					// Look up the display name of the target feature from its short name
					std::string targetDisplayName = settingId.featureShortName;
					for (auto* f : Feature::GetFeatureList()) {
						if (f->GetShortName() == settingId.featureShortName) {
							targetDisplayName = f->GetDisplayName();
							break;
						}
					}
					if (ImGui::Selectable(fmt::format("{}##imp{}", targetDisplayName, rowIndex).c_str())) {
						navigationTarget = settingId.featureShortName;
						break;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), targetDisplayName.c_str());
					}
				}

				// --- Column 1: Setting name ---
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", settingId.settingPath.c_str());

				// --- Column 2: Constrained By (source features, clickable) ---
				ImGui::TableSetColumnIndex(2);
				if (!result.sources.empty()) {
					if (ImGui::Selectable(fmt::format("{}##src{}", result.sources[0].featureName, rowIndex).c_str())) {
						navigationTarget = result.sources[0].featureShortName;
						break;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), result.sources[0].featureName.c_str());
						if (result.sources.size() > 1) {
							ImGui::Separator();
							for (size_t i = 1; i < result.sources.size(); ++i) {
								ImGui::Text(T("menu.features.also_feature", "Also: %s"), result.sources[i].featureName.c_str());
							}
						}
						ImGui::Separator();
						ImGui::Text("%s", result.sources[0].reason.c_str());
					}
				}

				// --- Column 3: Forced value ---
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%s", FeatureConstraints::FormatConstraintValue(result.forcedValue).c_str());

				rowIndex++;
			}

			ImGui::EndTable();
		}

		if (!navigationTarget.empty()) {
			pendingFeatureSelection = navigationTarget;
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"%s",
			T("menu.features.constraints_explanation",
				"These settings are disabled in their respective feature menus while the constraints are active. "
				"Adjust the constraining features to remove them."));

		ImGui::Spacing();

		// "Don't show again" checkbox -- same pattern as Clear Cache dialog
		ImGui::Checkbox(T("menu.features.dont_show_warning", "Don't show this warning again"), &g_dontShowAgainCheckbox);

		ImGui::Spacing();

		// Centered OK button
		constexpr float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH;
		const float windowWidth = ImGui::GetWindowWidth();
		const float offset = (windowWidth - buttonWidth) * 0.5f;
		if (offset > 0)
			ImGui::SetCursorPosX(offset);

		if (ImGui::Button(T("menu.features.ok_button", "OK"), ImVec2(buttonWidth, 0))) {
			if (g_dontShowAgainCheckbox) {
				if (auto* menu = globals::menu) {
					menu->GetSettings().SkipConstraintWarning = true;
				}
			}
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		// Popup was closed externally (e.g. clicked outside), reset state
		g_reactiveWarningShow = false;
		g_reactiveWarningConstraints.clear();
	}
}
