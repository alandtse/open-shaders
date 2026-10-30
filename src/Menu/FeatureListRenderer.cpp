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
#include <utility>

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
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "Menu/Icons/helpers/SettingsActionIcons.h"
#include "Menu/MenuIcons.h"
#include "Menu/PerformanceRenderer.h"
#include "Menu/PresetsPageRenderer.h"
#include "Menu/ProfilingRenderer.h"
#include "Menu/SettingReveal.h"
#include "Menu/SettingsPageRenderer.h"
#include "Menu/ThemeManager.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"
#include "Utils/UIAnim.h"

namespace
{
	constexpr char kUnloadedCategory[] = "Unloaded Features";

	std::string GetMenuId(const FeatureListRenderer::MenuFuncInfo& item)
	{
		if (const auto* feature = std::get_if<Feature*>(&item))
			return (*feature)->GetShortName();
		if (const auto* menu = std::get_if<FeatureListRenderer::BuiltInMenu>(&item))
			return menu->canonicalId;
		return {};
	}

	bool DrawNavRow(const FeatureListRenderer::BuiltInMenu& menu, bool selected, bool disabled, float width = 0.0f)
	{
		using Constants = ThemeManager::Constants;
		const auto icon = menu.canonicalId == "Presets"       ? MenuIcons::kPresets :
		                  menu.canonicalId == "CSEditor"      ? MenuIcons::kCSEditor :
		                  menu.canonicalId == "Screenshot"    ? MenuIcons::kScreenshot :
		                  menu.canonicalId == "WeatherPicker" ? MenuIcons::kSceneSelector :
		                  menu.canonicalId == "Performance"   ? MenuIcons::kProfiling :
		                                                        MenuIcons::kSettings;
		const auto glyph = Icons::TI(icon);

		const float scale = Util::GetUIScale();
		const float lineHeight = ImGui::GetTextLineHeight();
		const float height = ImGui::GetFrameHeight();
		const float rowWidth = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;

		bool clicked = false;
		{
			// The pill draws its own highlight, so the list highlight stays out of it
			Util::Anim::NoListHighlight ownHighlight;
			// A dimmed pill still takes the click: the caller explains why it cannot act
			ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32_BLACK_TRANS);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32_BLACK_TRANS);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32_BLACK_TRANS);
			clicked = ImGui::Selectable("##NavRow", false, ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(rowWidth, height));
			ImGui::PopStyleColor(3);
		}
		auto* drawList = ImGui::GetWindowDrawList();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float rounding = ImGui::GetStyle().FrameRounding;
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
		if (ImGui::IsItemHovered() && !disabled)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		const auto blend = Util::Anim::ItemBlend(ImGui::GetItemID(), ImGui::IsItemHovered() && !disabled, ImGui::IsItemActive(), selected);
		Util::Anim::FillLayer(drawList, min, max, rounding, ImGuiCol_HeaderHovered, blend.hover);
		Util::Anim::FillLayer(drawList, min, max, rounding, ImGuiCol_HeaderActive, blend.press);
		if (selected || blend.selected > 0.0f) {
			// The selected pill is one shape that glides between the pills; one that was just left fades out in place
			const auto pill = selected ? Util::Anim::SelectionGlide(ImHashStr("NavPillGlide"), min, max) : Util::Anim::GlideRect{ min, max };
			const float alpha = selected ? 1.0f : blend.selected;
			ImVec4 accent = Util::Colors::GetAccent();
			accent.w = Util::Surface::FillOpacity(Constants::SEGMENT_SELECTED_FILL_ALPHA) * alpha;
			drawList->AddRectFilled(pill.min, pill.max, ImGui::GetColorU32(accent), rounding);
			accent.w = Constants::SEGMENT_SELECTED_OUTLINE_ALPHA * alpha;
			const float stroke = Constants::CONTROL_OUTLINE_WIDTH * scale;
			const float halfStroke = stroke * 0.5f;
			drawList->AddRect(ImVec2(pill.min.x + halfStroke, pill.min.y + halfStroke), ImVec2(pill.max.x - halfStroke, pill.max.y - halfStroke),
				ImGui::GetColorU32(accent), std::max(0.0f, rounding - halfStroke), ImDrawFlags_None, stroke);
		}

		const ImVec4 textColor = ImGui::GetStyleColorVec4(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
		const ImVec4 iconColor = !disabled ? Util::Colors::GetAccent() : textColor;
		const float iconGap = Constants::NAV_ROW_ICON_GAP * scale;

		// Narrow buttons keep the icon readable and reveal the full label on hover.
		const float fontSize = ImGui::GetFontSize();
		const float available = std::max(0.0f, (max.x - min.x) - ImGui::GetStyle().FramePadding.x * 2.0f);
		const float labelWidth = ImGui::CalcTextSize(menu.name.c_str()).x;
		const float iconSize = glyph ? std::min(lineHeight, available) : 0.0f;
		const float iconWidth = glyph ? iconSize + iconGap : 0.0f;
		const float textWidth = std::max(0.0f, available - iconWidth);
		const float fit = labelWidth > textWidth && labelWidth > 0.0f ? textWidth / labelWidth : 1.0f;
		const bool iconOnly = glyph && fit < Constants::NAV_PILL_MIN_CONTENT_SCALE;
		float x = iconOnly ? min.x + ((max.x - min.x) - iconSize) * 0.5f : min.x + ImGui::GetStyle().FramePadding.x;
		drawList->PushClipRect(min, max, true);
		if (glyph) {
			Icons::DrawCenteredGlyph(drawList, ImVec2(x, min.y), ImVec2(iconSize, max.y - min.y), fontSize * iconSize / lineHeight, glyph, ImGui::GetColorU32(iconColor));
			x += iconWidth;
		}
		if (!iconOnly) {
			// Keep the label's ink centred when it shrinks to fit.
			const float textY = min.y + (max.y - min.y) * 0.5f - Util::GetTextInkCenterY() * fit;
			drawList->AddText(ImGui::GetFont(), fontSize * fit, ImVec2(x, textY), ImGui::GetColorU32(textColor), menu.name.c_str());
		}
		drawList->PopClipRect();
		if (iconOnly && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", menu.name.c_str());
		return clicked;
	}

	struct FeaturePageLayoutState
	{
		bool measured = false;
		float materialHeight = 0.0f;
		float contentHeight = 0.0f;
		float scrollY = 0.0f;
		float scrollMaxY = 0.0f;
	};

	std::unordered_map<std::string, FeaturePageLayoutState> g_featurePageLayouts;

	std::string* currentSettingHint = nullptr;

	void DrawFeatureActionHint(const char* label, const char* details = nullptr, bool show = false)
	{
		if (!show && !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			return;
		const auto itemMin = ImGui::GetItemRectMin();
		const auto itemMax = ImGui::GetItemRectMax();
		ImGui::PushID(ImGui::GetItemID());
		const SKSE::stl::scope_exit restoreID([]() noexcept { ImGui::PopID(); });
		auto* storage = ImGui::GetStateStorage();
		const ImGuiID progressId = ImGui::GetID("##ActionHintProgress");
		const ImGuiID frameId = ImGui::GetID("##ActionHintFrame");
		const int frame = ImGui::GetFrameCount();
		const float previous = storage->GetInt(frameId, -1) == frame - 1 ? storage->GetFloat(progressId) : 0.0f;
		const float progress = std::min(previous + ImGui::GetIO().DeltaTime / ThemeManager::Constants::FEATURE_ACTION_HINT_FADE_SECONDS, 1.0f);
		storage->SetFloat(progressId, progress);
		storage->SetInt(frameId, frame);
		const float easedProgress = Util::Smoothstep(0.0f, 1.0f, progress);
		auto* viewport = ImGui::GetWindowViewport();
		const auto& style = ImGui::GetStyle();
		const float scale = Util::GetUIScale();
		const ImVec2 padding(ThemeManager::Constants::FEATURE_ACTION_HINT_PADDING_X * scale,
			ThemeManager::Constants::FEATURE_ACTION_HINT_PADDING_Y * scale);
		const float right = itemMin.x - style.FramePadding.x - ThemeManager::Constants::FEATURE_ACTION_HINT_GAP * scale;
		MenuFonts::FontRoleGuard font(Menu::FontRole::Subtext);
		if (right - viewport->WorkPos.x < padding.x * 2.0f + ImGui::GetFontSize())
			return;
		const float wrapWidth = std::min(ImGui::GetFontSize() * ThemeManager::Constants::FEATURE_ACTION_HINT_WIDTH_EM,
			std::max(ImGui::GetFontSize(), right - viewport->WorkPos.x - padding.x * 2.0f));
		const auto labelSize = label ? ImGui::CalcTextSize(label, nullptr, false, wrapWidth) : ImVec2();
		const auto detailSize = details ? ImGui::CalcTextSize(details, nullptr, false, wrapWidth) : ImVec2();
		const float textGap = label && details ? style.ItemSpacing.y : 0.0f;
		const ImVec2 size(std::max(labelSize.x, detailSize.x) + padding.x * 2.0f,
			labelSize.y + detailSize.y + textGap + padding.y * 2.0f);
		const float left = std::max(viewport->WorkPos.x, right - size.x);
		const float top = std::clamp((itemMin.y + itemMax.y - size.y) * 0.5f, viewport->WorkPos.y,
			std::max(viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - size.y));
		const float slide = ThemeManager::Constants::FEATURE_ACTION_HINT_SLIDE * scale * (1.0f - easedProgress);
		const float animatedLeft = std::max(viewport->WorkPos.x, left - slide);
		const ImVec2 origin(animatedLeft, top);
		const ImVec2 end(animatedLeft + size.x, top + size.y);
		auto background = style.Colors[ImGuiCol_FrameBg];
		background.w = easedProgress;
		auto* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y), false);
		const SKSE::stl::scope_exit restoreClip([drawList]() noexcept { drawList->PopClipRect(); });
		const float rounding = ThemeManager::Constants::FEATURE_ACTION_HINT_ROUNDING * scale;
		drawList->AddRectFilled(origin, end, ImGui::GetColorU32(background), rounding);
		drawList->AddRect(origin, end, ImGui::GetColorU32(ImGuiCol_ButtonHovered, easedProgress), rounding,
			ImDrawFlags_None, ThemeManager::Constants::CONTROL_OUTLINE_WIDTH * scale);
		if (label)
			drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x + padding.x, origin.y + padding.y),
				ImGui::GetColorU32(ImGuiCol_Text, easedProgress), label, nullptr, wrapWidth);
		if (details)
			drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
				ImVec2(origin.x + padding.x, origin.y + padding.y + labelSize.y + textGap),
				ImGui::GetColorU32(ImGuiCol_TextDisabled, easedProgress), details, nullptr, wrapWidth);
	}

	bool IsUtilityFeature(Feature* feature)
	{
		return feature->GetCategory() == FeatureCategories::kUtility;
	}

	/** @brief "< Features" row heading the Settings section list. @return True when clicked. */
	bool DrawBackRow()
	{
		MenuFonts::FontRoleGuard labelFont(Menu::FontRole::Subheading);
		const float lineHeight = ImGui::GetTextLineHeight();
		const float padX = ImGui::CalcTextSize(" ").x;
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const bool clicked = Util::Selectable("##BackToFeatures", false, ImGuiSelectableFlags_None, ImVec2(0.0f, lineHeight));
		auto* drawList = ImGui::GetWindowDrawList();
		Icons::DrawCenteredGlyph(drawList, ImVec2(pos.x + padX, pos.y), ImVec2(lineHeight, lineHeight), Icons::TI(ICON_TI_CHEVRON_LEFT),
			ImGui::GetColorU32(Util::Colors::GetSecondary()));
		drawList->AddText(ImVec2(pos.x + padX + lineHeight + ThemeManager::Constants::NAV_ROW_ICON_GAP * Util::GetUIScale(), pos.y),
			ImGui::GetColorU32(ImGuiCol_Text), T("menu.features.back_to_features", "Features"));
		return clicked;
	}

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
		const float activationZoneWidth = ThemeManager::Constants::AUTOHIDE_ACTIVATION_ZONE_WIDTH * Util::GetUIScale();
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
		const float headerRight = startPos.x + ImGui::GetContentRegionAvail().x;

		// Calculate title size and draw feature name with Title font
		ImVec2 titleSize;
		ImVec2 titleMax;
		float titleBaseline;
		{
			MenuFonts::FontRoleGuard titleGuard(Menu::FontRole::Title);
			ImGui::SetWindowFontScale(titleScale);
			ImGui::TextWrapped("%s", featureName.c_str());
			titleSize = ImGui::GetItemRectSize();
			titleMax = ImGui::GetItemRectMax();
			titleBaseline = ImGui::GetItemRectMin().y + ImGui::GetFontBaked()->Ascent;
			ImGui::SetWindowFontScale(1.0f);
		}

		// Store the title-only height for return value
		float titleOnlyHeight = titleSize.y;

		// Running x for baseline-aligned annotations (stage tag, then version) to the right of the title
		float annotationX = titleMax.x + ImGui::GetStyle().ItemSpacing.x;

		// Draw stage marker ([ALPHA]/[BETA]) on the title baseline
		if (!stageTag.empty()) {
			ImVec2 tagSize;
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				tagSize = ImGui::CalcTextSize(stageTag.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}

			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				if (annotationX + tagSize.x > headerRight) {
					annotationX = startPos.x;
					titleBaseline = startPos.y + titleSize.y + ImGui::GetStyle().ItemInnerSpacing.y + ImGui::GetFontBaked()->Ascent;
					titleSize.y = titleBaseline - ImGui::GetFontBaked()->Ascent + ImGui::GetFontSize() - startPos.y;
				}
				ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
					ImVec2(annotationX, titleBaseline - ImGui::GetFontBaked()->Ascent), ImGui::GetColorU32(stageColor), stageTag.c_str());
				annotationX += tagSize.x + ImGui::GetStyle().ItemSpacing.x;
				ImGui::SetWindowFontScale(1.0f);
			}
		}

		// Draw version with the title font on the same baseline if version exists
		if (!version.empty()) {
			// Format version: replace dashes with dots for consistency
			std::string formattedVersion = version;
			std::replace(formattedVersion.begin(), formattedVersion.end(), '-', '.');

			// Position version text right of the stage tag (or title)
			float versionX = annotationX;

			// Use dimmed text color for version
			ImVec4 versionColor = palette.Text;
			versionColor.w *= ThemeManager::Constants::VERSION_TEXT_OPACITY;

			{
				MenuFonts::FontRoleGuard titleGuard(Menu::FontRole::Title);
				ImGui::SetWindowFontScale(titleScale * ThemeManager::Constants::FEATURE_VERSION_TEXT_SCALE);
				const float versionWidth = ImGui::CalcTextSize(("v" + formattedVersion).c_str()).x;
				if (versionX + versionWidth > headerRight) {
					versionX = startPos.x;
					titleBaseline = startPos.y + titleSize.y + ImGui::GetStyle().ItemInnerSpacing.y + ImGui::GetFontBaked()->Ascent;
					titleSize.y = titleBaseline - ImGui::GetFontBaked()->Ascent + ImGui::GetFontSize() - startPos.y;
				}
				ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
					ImVec2(versionX, titleBaseline - ImGui::GetFontBaked()->Ascent), ImGui::GetColorU32(versionColor), ("v" + formattedVersion).c_str());
				ImGui::SetWindowFontScale(1.0f);
			}
		}
		// Reset cursor to after the title block
		ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + titleSize.y + ImGui::GetStyle().ItemSpacing.y * 0.25f));

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
	if (category == kUnloadedCategory)
		return T("menu.features.unloaded_features", "Unloaded Features");
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
	std::string& selectedMenuId,
	std::string& featureSearch,
	std::string& pendingFeatureSelection,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawSettings,
	const std::function<void()>& drawSettingsNav, bool editorLayout, bool sidebarVisible, bool resetLayout, Hooks* hooks)
{
	ImGui::BeginChild("Menus Table", ImVec2(0, -footerHeight));

	// A search filter or a collapsed category could hide the feature being navigated to
	if (!pendingFeatureSelection.empty()) {
		featureSearch.clear();
		if (const auto* feature = Feature::FindFeatureByShortName(pendingFeatureSelection))
			categoryExpansionStates[feature->loaded ? std::string(feature->GetCategory()) : kUnloadedCategory] = true;
	}

	const auto buildVisibleMenus = [&] {
		auto menus = BuildMenuList(categoryExpansionStates, drawSettings, drawSettingsNav);
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
	size_t selectedMenu = 0;
	for (size_t i = 0; i < menuList.size(); ++i)
		if (GetMenuId(menuList[i]) == selectedMenuId) {
			selectedMenu = i;
			break;
		}

	HandlePendingFeatureSelection(pendingFeatureSelection, menuList, selectedMenu);
	if (hooks && hooks->featureStep && !menuList.empty()) {
		const int direction = hooks->featureStep > 0 ? 1 : -1;
		for (size_t step = 1; step <= menuList.size(); ++step) {
			const auto next = static_cast<size_t>((static_cast<ptrdiff_t>(selectedMenu) + direction * static_cast<ptrdiff_t>(step) +
													  static_cast<ptrdiff_t>(menuList.size())) %
												  static_cast<ptrdiff_t>(menuList.size()));
			if (std::holds_alternative<Feature*>(menuList[next])) {
				selectedMenu = next;
				break;
			}
		}
		hooks->featureStep = 0;
	}

	// Determine if left panel should be visible based on auto-hide settings
	const bool settingsOpen = GetMenuId(menuList[selectedMenu]) == MenuPageIds::kSettings;
	const bool leftPanelVisible = settingsOpen || (sidebarVisible && ShouldShowLeftPanel());
	auto& sidebar = globals::menu->GetSidebarState();
	const float progress = settingsOpen ? 1.0f : editorLayout ? (leftPanelVisible ? 1.0f : 0.0f) :
	                                                            sidebar.progress;
	const float easedProgress = Util::Smoothstep(0.0f, 1.0f, progress);
	const ImVec2 available = ImGui::GetContentRegionAvail();
	const float expandedWidth = available.x + (editorLayout ? 0.0f : sidebar.windowOffset);
	const bool initialSidebarWidth = sidebar.availableWidth <= 0.0f;
	const float sidebarWidth = editorLayout ? ThemeManager::Constants::EDITOR_MENU_SIDEBAR_WIDTH * Util::GetUIScale() :
	                                          expandedWidth * sidebar.widthRatio;
	const float fullSidebarWidth = !editorLayout && sidebar.width > 0.0f ? sidebar.width + sidebarWidth - sidebar.contentWidth :
	                                                                       sidebarWidth + ImGui::GetStyle().ItemSpacing.x;
	const bool retractWindow = !editorLayout && !ImGui::GetCurrentWindow()->RootWindow->DockIsActive;
	const float slideOffset = settingsOpen ? 0.0f : retractWindow ? sidebar.windowOffset :
	                                                                std::floor(fullSidebarWidth * (1.0f - easedProgress));
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 tableOrigin(origin.x - slideOffset, origin.y);
	ImGui::SetCursorScreenPos(tableOrigin);
	if (ImGui::BeginTable("Menus Table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable, ImVec2(available.x + slideOffset, 0.0f))) {
		if (!editorLayout) {
			if (auto* savedLayout = ImGui::TableSettingsFindByID(ImGui::GetCurrentTable()->ID);
				savedLayout && savedLayout->ColumnsCount == 2 && savedLayout->GetColumnSettings()[0].IsStretch) {
				auto* columns = savedLayout->GetColumnSettings();
				const float totalWeight = columns[0].WidthOrWeight + columns[1].WidthOrWeight;
				const float ratio = totalWeight > 0.0f ? columns[0].WidthOrWeight / totalWeight : sidebar.widthRatio;
				columns[0].WidthOrWeight = std::floor(expandedWidth * ratio);
				columns[0].IsStretch = false;
				savedLayout->RefScale = ImGui::GetFontSize();
				ImGui::TableLoadSettings(ImGui::GetCurrentTable());
			}
		}
		ImGui::TableSetupColumn("##ListOfMenus", ImGuiTableColumnFlags_WidthFixed, sidebarWidth);
		ImGui::TableSetupColumn("##MenuConfig", ImGuiTableColumnFlags_WidthStretch);
		// BeginTable clears ResizedColumn, so LastResizedColumn preserves the just-applied drag width.
		if ((!editorLayout && !initialSidebarWidth && ImGui::GetCurrentTable()->LastResizedColumn != 0) || (editorLayout && resetLayout))
			ImGui::TableSetColumnWidth(0, sidebarWidth);
		if (progress > 0.0f) {
			const auto previousSort = globals::menu->GetSettings().FeatureListSort;
			RenderLeftColumn(menuList, selectedMenu, featureSearch, categoryExpansionStates, !editorLayout, drawSettingsNav);
			if (globals::menu->GetSettings().FeatureListSort != previousSort) {
				// Sort order changed: rebuild now and re-select the same feature
				std::string reselect;
				if (const auto* feat = selectedMenu < menuList.size() ? std::get_if<Feature*>(&menuList[selectedMenu]) : nullptr)
					reselect = (*feat)->GetShortName();
				menuList = buildVisibleMenus();
				HandlePendingFeatureSelection(reselect, menuList, selectedMenu);
			}
		} else {
			// When left panel is hidden, right column takes full width
			ImGui::TableNextColumn();
		}
		RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection, hooks);
		if (!editorLayout) {
			const auto* table = ImGui::GetCurrentTable();
			const float columnWidth = table->Columns[0].WidthRequest;
			if ((initialSidebarWidth || table->LastResizedColumn == 0) && expandedWidth > 0.0f)
				sidebar.widthRatio = columnWidth / expandedWidth;
			sidebar.contentWidth = columnWidth;
			sidebar.availableWidth = expandedWidth;
			sidebar.width = table->Columns[1].WorkMinX - tableOrigin.x;
		}

		ImGui::EndTable();
	}
	if (!editorLayout) {
		const float step = ImGui::GetIO().DeltaTime * Util::Anim::Speed() / ThemeManager::Constants::SIDEBAR_SLIDE_DURATION;
		sidebar.progress = Util::Anim::ReducedMotion() ? (leftPanelVisible ? 1.0f : 0.0f) :
		                                                 std::clamp(sidebar.progress + (leftPanelVisible ? step : -step), 0.0f, 1.0f);
	}

	if (selectedMenu < menuList.size())
		selectedMenuId = GetMenuId(menuList[selectedMenu]);
	ImGui::EndChild();
}

std::vector<FeatureListRenderer::MenuFuncInfo> FeatureListRenderer::BuildMenuList(
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawSettings,
	const std::function<void()>&)
{
	// Build the menu list
	auto& featureList = Feature::GetFeatureList();
	auto sortedFeatureList{ featureList };  // need a copy so the load order is not lost
	std::ranges::sort(sortedFeatureList, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});

	auto menuList = std::vector<MenuFuncInfo>{
		BuiltInMenu{ T("menu.features.home", "Home"), []() { HomePageRenderer::RenderHomePage(); }, "Home" },
		BuiltInMenu{ T("menu.features.presets", "Presets"), []() { PresetsPageRenderer::Render(); }, "Presets" },
		BuiltInMenu{ T("menu.features.settings", "Settings"), drawSettings, "Settings" },
		BuiltInMenu{ T("menu.features.performance", "Performance"), [] { PerformanceRenderer::Render(); }, "Performance" },
		BuiltInMenu{ T("menu.features.cs_editor", "OS Editor"), []() { globals::features::csEditor.DrawSettings(); }, "CSEditor" }
	};  // NOTE: The menu list is rebuilt every frame, so category expansion states
	// persist correctly. This is acceptable since the list is small and built
	// infrequently, but could be optimized if performance becomes an issue.

	const auto isFavorite = [](Feature* feat) {
		return feat->IsInMenu() && feat->loaded && globals::state->IsFeatureFavorite(feat->GetShortName());
	};

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
				categoryExpansionStates[category] = true;
			}

			// Add category header
			menuList.push_back(CategoryHeader{ category });

			std::ranges::copy(categorizedFeatures[category], std::back_inserter(menuList));
		}
	}

	// Add any categories not in the predefined order
	for (const auto& [category, features] : categorizedFeatures) {
		if (std::ranges::find(FeatureCategories::kMenuOrder, std::string_view(category)) == FeatureCategories::kMenuOrder.end() && !features.empty()) {
			// Initialize expansion state if not exists
			if (categoryExpansionStates.find(category) == categoryExpansionStates.end()) {
				categoryExpansionStates[category] = true;
			}

			// Add category header
			menuList.push_back(CategoryHeader{ category });

			std::ranges::copy(features, std::back_inserter(menuList));
		}
	}

	auto unloadedFeatures = sortedFeatureList | std::ranges::views::filter([](Feature* feat) {
		return !feat->loaded && feat->IsInMenu() && !feat->IsHiddenUnreleased() && (!FeatureIssues::IsObsoleteFeature(feat->GetShortName()) || globals::state->IsDeveloperMode());
	});
	if (std::ranges::distance(unloadedFeatures) != 0) {
		categoryExpansionStates.try_emplace(kUnloadedCategory, true);
		menuList.push_back(CategoryHeader{ kUnloadedCategory });
		std::ranges::copy(unloadedFeatures, std::back_inserter(menuList));
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
	std::map<std::string, bool>& categoryExpansionStates, bool showVersions, const std::function<void()>& drawSettingsNav)
{
	ImGui::TableNextColumn();
	if (ImGui::BeginChild("##Navigation", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
		const float scale = Util::GetUIScale();
		const float inset = ThemeManager::Constants::LEFT_PANE_EDGE_INSET * scale;
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + inset);
		ImGui::Indent(inset);
		const auto pageIndex = [&](std::string_view id) {
			for (size_t i = 0; i < menuList.size(); ++i)
				if (GetMenuId(menuList[i]) == id)
					return i;
			return menuList.size();
		};
		const auto drawPage = [&](std::string_view id, float width = 0.0f) {
			const auto index = pageIndex(id);
			if (index < menuList.size()) {
				if (const auto* builtIn = std::get_if<BuiltInMenu>(&menuList[index]))
					RenderBuiltInRow(*builtIn, index, selectedMenu, width);
				else if (const auto* feature = std::get_if<Feature*>(&menuList[index]))
					RenderBuiltInRow(BuiltInMenu{ (*feature)->GetDisplayName(), {}, (*feature)->GetShortName() }, index, selectedMenu, width);
			}
		};
		const float width = ImGui::GetContentRegionAvail().x - inset;
		const float gap = std::max(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y);
		const float rowHeight = ImGui::GetFrameHeight();
		const auto drawPair = [&](std::string_view left, std::string_view right) {
			const bool hasRight = pageIndex(right) < menuList.size();
			const ImVec2 track = ImGui::GetCursorScreenPos();
			const float pillWidth = hasRight ? (width - gap) * 0.5f : width;
			ImGui::SetCursorScreenPos(track);
			drawPage(left, pillWidth);
			if (hasRight) {
				ImGui::SetCursorScreenPos(ImVec2(track.x + pillWidth + gap, track.y));
				drawPage(right, pillWidth);
			}
			ImGui::SetCursorScreenPos(ImVec2(track.x, track.y + rowHeight + gap));
		};
		drawPair(MenuPageIds::kPresets, MenuPageIds::kCSEditor);
		drawPair("Screenshot", "WeatherPicker");
		drawPair(MenuPageIds::kProfiling, MenuPageIds::kSettings);
		ImGui::Unindent(inset);
		ImGui::Spacing();

		static std::string previousPage = "Home";
		const bool settingsOpen = GetMenuId(menuList[selectedMenu]) == MenuPageIds::kSettings;
		if (!settingsOpen)
			previousPage = GetMenuId(menuList[selectedMenu]);
		if (settingsOpen) {
			if (DrawBackRow()) {
				const auto back = pageIndex(previousPage);
				selectedMenu = back < menuList.size() ? back : pageIndex(MenuPageIds::kHome);
			}
			if (ImGui::BeginChild("##SettingsNavScroll")) {
				drawSettingsNav();
				ImGui::Dummy(ImVec2(0.0f, inset));
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
					ImGui::GetCurrentContext()->ActiveIdPreviousFrame == 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
					globals::menu->ConsumeEscape();
					const auto back = pageIndex(previousPage);
					selectedMenu = back < menuList.size() ? back : pageIndex(MenuPageIds::kHome);
				}
			}
			ImGui::EndChild();
		} else {
			const auto sortSize = BrowserUI::IconButtonSize();
			const ImVec2 listStart = ImGui::GetCursorScreenPos();
			const ImVec2 listSize = ImGui::GetContentRegionAvail();
			const float listHeight = std::max(1.0f, listSize.y - sortSize - inset - ImGui::GetStyle().ItemSpacing.y);
			float listRight = listStart.x + listSize.x;
			if (ImGui::BeginChild("##FeatureListScroll", ImVec2(0.0f, listHeight))) {
				listRight = ImGui::GetCurrentWindow()->InnerRect.Max.x;
				const auto filter = globals::menu->GetSettings().FeatureListFilter & FeatureListFilter::kKnown;
				if (filter != 0)
					globals::state->PrimeUserEdits();
				const auto matches = [&](Feature* feature) {
					if (feature->GetShortName() == "Screenshot" || feature->GetShortName() == "WeatherPicker")
						return false;
					return Util::FeatureMatchesSearch(feature, featureSearch) &&
					       PassesFilter(GetFilterBits(feature), filter);
				};
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + inset);
				bool expanded = true;
				bool hasMatches = false;
				for (size_t i = 0; i < menuList.size(); ++i) {
					if (std::holds_alternative<BuiltInMenu>(menuList[i]))
						continue;
					if (const auto* category = std::get_if<CategoryHeader>(&menuList[i])) {
						const bool unloaded = category->name == kUnloadedCategory;
						if (!std::ranges::any_of(menuList, [&](const MenuFuncInfo& item) {
								const auto* feature = std::get_if<Feature*>(&item);
								return feature && matches(*feature) && (unloaded ? !(*feature)->loaded : (*feature)->loaded && (*feature)->GetCategory() == category->name);
							}))
							continue;
						std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, showVersions }, menuList[i]);
						expanded = categoryExpansionStates[category->name];
						continue;
					}
					if (std::holds_alternative<std::string>(menuList[i])) {
						expanded = true;
						std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, showVersions }, menuList[i]);
						continue;
					}
					if (const auto* feature = std::get_if<Feature*>(&menuList[i]); feature && matches(*feature)) {
						hasMatches = true;
						if (expanded)
							std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates, showVersions }, menuList[i]);
					}
				}
				if (!hasMatches && filter != 0) {
					BrowserUI::EmptyState(T("menu.features.no_matches", "No features match"),
						T("menu.features.no_matches_hint", "Turn off a filter to list more features."));
					if (BrowserUI::Link("##ClearFilters", T("menu.features.clear_filters", "Clear filters")))
						globals::menu->GetSettings().FeatureListFilter = 0;
				}
				ImGui::Dummy(ImVec2(0.0f, inset));
			}
			ImGui::EndChild();
			ImGui::SetCursorScreenPos(ImVec2(std::max(listStart.x, listRight - sortSize - ImGui::GetStyle().ItemSpacing.x),
				listStart.y + std::max(0.0f, listSize.y - sortSize - inset)));
			if (ImGui::BeginChild("##FloatingSort", ImVec2(sortSize, sortSize), ImGuiChildFlags_None,
					ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
				const bool lit = ImGui::IsWindowHovered() || ImGui::GetFocusID() == ImGui::GetID("##SortFeatures") || ImGui::IsPopupOpen("##FeatureSortPopup");
				const float alpha = std::lerp(ThemeManager::Constants::PALETTE_DIMMED_ALPHA, 1.0f, Util::Anim::Toggle(ImGui::GetID("##Visibility"), lit));
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
				if (BrowserUI::IconButton("##SortFeatures", GetSortModeIcon(globals::menu->GetSettings().FeatureListSort), T("menu.features.sort_tooltip", "Sort features")))
					ImGui::OpenPopup("##FeatureSortPopup");
				ImGui::PopStyleVar();
				ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y - ImGui::GetStyle().ItemSpacing.y),
					ImGuiCond_Appearing, ImVec2(1.0f, 1.0f));
				RenderSortPopup("##FeatureSortPopup");
			}
			ImGui::EndChild();
		}
	}
	ImGui::EndChild();
}

void FeatureListRenderer::RenderBuiltInRow(const BuiltInMenu& menu, size_t listId, size_t& selectedMenu, float width)
{
	ImGui::PushID(menu.canonicalId.c_str());
	if (menu.canonicalId == MenuPageIds::kCSEditor) {
		const bool canOpen = EditorWindow::CanBeOpen();
		if (DrawNavRow(menu, false, !canOpen, width)) {
			if (canOpen)
				CSEditor::OpenEditorWindow();
			else
				EditorWindow::GetSingleton()->WarnOpeningBlocked();
		}
	} else if (DrawNavRow(menu, selectedMenu == listId, false, width))
		selectedMenu = listId;
	ImGui::PopID();
}

void FeatureListRenderer::RenderRightColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t selectedMenu,
	std::string& pendingFeatureSelection, const Hooks* hooks)
{
	ImGui::TableNextColumn();

	if (selectedMenu < menuList.size()) {
		Util::Anim::PageFade fade(ImGui::GetID(GetMenuId(menuList[selectedMenu]).c_str()));
		std::visit(DrawMenuVisitor{ pendingFeatureSelection, hooks }, menuList[selectedMenu]);
	} else {
		ImGui::TextDisabled("%s", T("menu.features.select_item_left", "Please select an item on the left."));
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const BuiltInMenu& menu)
{
	RenderBuiltInRow(menu, listId, selectedMenuRef);
}

void FeatureListRenderer::ListMenuVisitor::operator()(const std::string& label)
{
	// Use default separator text for other labels - should be themed via ImGuiCol_Separator
	SeparatorTextWithFont(label, Menu::FontRole::Subheading);
}

void FeatureListRenderer::ListMenuVisitor::operator()(const CategoryHeader& header)
{
	// Get expansion state from the persisted map
	bool isExpanded = categoryExpansionStates[header.name];

	// Draw category header with custom styling using util:UI function
	// Use Heading font for category headers
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
		const auto categoryLabel = TranslateCategory(header.name);
		Util::DrawCategoryHeader(header.name.c_str(), categoryLabel.c_str(), isExpanded);
	}

	// Update expansion state
	if (categoryExpansionStates[header.name] != isExpanded)
		ImGui::MarkIniSettingsDirty();
	categoryExpansionStates[header.name] = isExpanded;
}

void FeatureListRenderer::ListMenuVisitor::operator()(Feature* feat)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const auto featureName = feat->GetShortName();
	ImGui::PushID(featureName.c_str());
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	auto& themeSettings = globals::menu->GetSettings().Theme;

	ImVec4 textColor;

	// Determine the text color based on the state
	if (isDisabled) {
		textColor = themeSettings.StatusPalette.Disable;
	} else if (feat->GetUnavailableMessage()) {
		textColor = themeSettings.StatusPalette.Error;
	} else if (isLoaded) {
		textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	} else if (hasFailedMessage) {
		textColor = feat->version.empty() ? themeSettings.StatusPalette.Disable : themeSettings.StatusPalette.Error;
	} else if (!feat->conflictMessage.empty()) {
		textColor = themeSettings.StatusPalette.Warning;
	} else {
		// Installed but not loaded means the feature is only pending a restart (green),
		// otherwise it is simply missing (grey).
		textColor = feat->installed ? themeSettings.StatusPalette.RestartNeeded : themeSettings.StatusPalette.Disable;
	}

	// Create selectable item with semantic color
	const float edgeInset = std::max(ImGui::GetStyle().FramePadding.x, ThemeManager::Constants::ANIM_SELECTED_BAR_WIDTH * Util::GetUIScale());
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + edgeInset);
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	if (Util::Selectable(fmt::format("{} ###{}", feat->GetDisplayName(), featureName).c_str(), selectedMenuRef == listId, ImGuiSelectableFlags_SpanAllColumns)) {
		selectedMenuRef = listId;
	}
	ImGui::PopStyleColor();
	const float rowTop = ImGui::GetItemRectMin().y;
	const float rowHeight = ImGui::GetItemRectSize().y;
	RenderRowContextMenu(feat);

	if (showVersions && globals::menu->GetSettings().FeatureListSort == Menu::FeatureSortMode::Alphabetical) {
		if (const auto icon = Util::GetCategoryIcon(feat->GetCategory())) {
			ImGui::SameLine();
			const float iconSize = ImGui::GetTextLineHeight();
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop + (rowHeight - iconSize) * 0.5f));
			ImGui::Image(icon, ImVec2(iconSize, iconSize));
		}
	}

	// Display the stage marker behind the name, regardless of loaded state
	if (const auto stage = feat->GetReleaseStage(); stage != Feature::ReleaseStage::Release) {
		MenuFonts::FontRoleGuard badgeFont(Menu::FontRole::Subtext);
		BrowserUI::InlineBadge(stage == Feature::ReleaseStage::Alpha ? T("menu.features.badge_alpha", "Alpha") : T("menu.features.badge_beta", "Beta"),
			Feature::GetReleaseStageColor(stage), rowHeight, rowTop);
	}

	// Indicator icons stack after the name and badge: a standard gap before the first, a space between the rest
	const float lineHeight = ImGui::GetTextLineHeight();
	const float spaceWidth = ImGui::CalcTextSize(" ").x;
	bool firstIcon = true;
	const auto nextIconBox = [&](float width) {
		ImGui::SameLine(0.0f, firstIcon ? -1.0f : spaceWidth);
		firstIcon = false;
		const ImVec2 origin(ImGui::GetCursorScreenPos().x, rowTop + (rowHeight - lineHeight) * 0.5f);
		ImGui::SetCursorScreenPos(origin);
		ImGui::Dummy(ImVec2(width, lineHeight));
		return origin;
	};
	const auto indicatorDot = [&](ImU32 color, bool filled) {
		const float radius = lineHeight * ThemeManager::Constants::SCENE_INDICATOR_RADIUS_RATIO;
		const ImVec2 origin = nextIconBox(radius * 2.0f);
		Util::DrawIconCircle(ImVec2(origin.x + radius, origin.y + lineHeight * 0.5f), radius, color, filled);
	};

	// Favorites are pinned at the top of the list, marked by a pin
	if (isLoaded && globals::state->IsFeatureFavorite(featureName)) {
		const Icons::GlyphRef pin = Icons::TI(ICON_TI_PIN);
		Icons::FontGuard pinFont(pin, ThemeManager::Constants::FEATURE_ROW_PIN_SCALE);
		const ImVec2 pinBox(ImGui::CalcTextSize(pin.utf8).x, lineHeight);
		const ImVec2 origin = nextIconBox(pinBox.x);
		ImGui::GetWindowDrawList()->AddText(Icons::GetCenteredGlyphPos(origin, pinBox, pin.utf8),
			ImGui::GetColorU32(Util::Colors::GetAccent()), pin.utf8);
	}

	// A feature only authored for other scenes still gets a hollow dot, so its settings stay discoverable.
	if (auto* sceneManager = globals::sceneSettingsManager; sceneManager->HasAnySceneEntriesForFeature(featureName)) {
		const bool applying = sceneManager->IsFeatureOverridingBase(featureName);
		indicatorDot(ImGui::GetColorU32(Util::Colors::GetInfo()), applying);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(applying ?
									   T("menu.features.scene_indicator_overriding",
										   "Scene Manager is overriding settings here. Blue settings show its values.") :
									   T("menu.features.scene_indicator_configured",
										   "Has Scene Manager settings for other times, weathers or locations."));

		if (sceneManager->HasSketches(featureName)) {
			indicatorDot(ImGui::GetColorU32(Util::Colors::GetSuccess()), true);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T("menu.features.scene_indicator_sketched",
					"Has settings sketched over the Scene Manager's values. They revert once you leave this feature."));
		}
	}
	ImGui::PopID();
}

void FeatureListRenderer::ListMenuVisitor::RenderRowContextMenu(Feature* feat) const
{
	if (!ImGui::BeginPopupContextItem("##RowMenu"))
		return;

	{
		MenuFonts::FontRoleGuard menuFont(Menu::FontRole::Body);
		auto* state = globals::state;
		const auto featureName = feat->GetShortName();
		const bool isLoaded = feat->loaded;
		const bool isDisabled = state->IsFeatureDisabled(featureName);

		const bool isFavorite = state->IsFeatureFavorite(featureName);
		if (ImGui::MenuItem(isFavorite ? T("menu.features.unpin", "Unpin") : T("menu.features.pin", "Pin to Top"), nullptr, false, isLoaded))
			ToggleFavorite(feat);
		if (!isLoaded)
			Util::AddTooltip(T("menu.features.pin_needs_loaded", "Only loaded features can be pinned"), Util::kTooltipWhenDisabled);

		// The same condition as the feature page's Restore Defaults, plus something to restore
		const bool canRestore = isLoaded && !isDisabled && feat->UsesMainSettings();
		if (ImGui::MenuItem(T("menu.features.restore_defaults", "Restore Defaults"), nullptr, false, canRestore && state->HasUserEdits(feat))) {
			globals::state->RestoreFeatureLayers(feat);
		}
		Util::AddTooltip(canRestore ? T("menu.features.restore_defaults_tooltip", "Restore defaults: clears your edits to this feature, back to what defaults and your Base Settings presets set") :
									  T("menu.features.restore_unavailable", "Only loaded features that keep their settings in the user config can be restored"),
			Util::kTooltipWhenDisabled);

		if (ImGui::MenuItem(T("menu.features.load_at_boot", "Load at Boot"), nullptr, !isDisabled)) {
			const bool enabled = feat->ToggleAtBootSetting();
			logger::info("{}: {} at boot.", featureName, enabled ? "Enabled" : "Disabled");
		}
		Util::AddTooltip(T("menu.features.load_at_boot_tooltip", "Whether the game loads this feature at startup. Takes effect after a restart."));

		ImGui::Separator();
		if (ImGui::MenuItem(T("menu.features.copy_name", "Copy Name")))
			ImGui::SetClipboardText(feat->GetDisplayName().c_str());
	}
	ImGui::EndPopup();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const BuiltInMenu& menu)
{
	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		// Add spacing only for Home menu
		if (menu.name == T("menu.features.home", "Home")) {
			ImGui::Dummy(ImVec2(0, ThemeManager::Constants::BUTTON_SPACING * Util::GetUIScale()));
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
	std::string hint;
	const auto previousHint = std::exchange(currentSettingHint, &hint);
	const SKSE::stl::scope_exit restoreHint([previousHint]() noexcept { currentSettingHint = previousHint; });
	if (hooks && hooks->onFeatureShown)
		hooks->onFeatureShown(feat);
	const auto featureName = feat->GetShortName();
	const bool disabled = globals::state->IsFeatureDisabled(featureName);
	if (feat->loaded && feat->UsesMainSettings() && !disabled && !SceneSettingsUI::IsFeaturePageEditing(feat))
		globals::menu->WatchFeatureUndo(feat);
	const bool featureProfilingAvailable = feat != &globals::features::csEditor && !disabled && feat->loaded &&
	                                       ProfilingRenderer::IsFeatureProfilingAvailable(featureName);
	const float profilingHeight = ProfilingRenderer::PrepareFeatureTimers(featureName, featureProfilingAvailable);
	const auto& style = ImGui::GetStyle();
	const float railWidth = BrowserUI::IconButtonSize() + style.FramePadding.x * 2.0f;
	ImGui::PushID(featureName.c_str());
	if (ImGui::BeginChild("##FeatureConfigFrame", ImVec2(0, 0), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		const float width = std::max(ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x - railWidth - style.ItemSpacing.x);
		if (ImGui::BeginChild("##FeaturePageContent", ImVec2(width, 0), false,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground)) {
			RenderFeatureHeader(feat, feat->loaded);
			SceneSettingsUI::DrawFeaturePageControls(feat, !disabled && feat->loaded);
			const float pageViewportHeight = std::max(ImGui::GetContentRegionAvail().y, 0.0f);
			if (!featureProfilingAvailable) {
				g_featurePageLayouts.erase(featureName);
			} else {
				auto& pageLayout = g_featurePageLayouts[featureName];
				const float predictedProfilingStart = std::max(pageLayout.materialHeight, pageViewportHeight - profilingHeight);
				const float predictedContentHeight = predictedProfilingStart + profilingHeight;
				const float contentHeightDelta = predictedContentHeight - pageLayout.contentHeight;
				const float scale = Util::GetUIScale();
				const float bottomTolerance = ThemeManager::Constants::FEATURE_PAGE_BOTTOM_TOLERANCE * scale;
				const bool profilingWasAtBottom = pageLayout.scrollMaxY <= bottomTolerance ||
				                                  pageLayout.scrollY >= pageLayout.scrollMaxY - bottomTolerance;
				ImGui::SetNextWindowContentSize(ImVec2(0.0f, predictedContentHeight));
				if (pageLayout.measured && profilingWasAtBottom &&
					std::abs(contentHeightDelta) >= ThemeManager::Constants::FEATURE_PAGE_LAYOUT_EPSILON * scale)
					ImGui::SetNextWindowScroll(ImVec2(-1.0f, std::max(pageLayout.scrollY + contentHeightDelta, 0.0f)));
			}
			if (ImGui::BeginChild("##FeatureSettingsScroll", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground)) {
				const float materialStartY = ImGui::GetCursorPosY();
				RenderFeatureSettings(feat, disabled, feat->loaded, !feat->failedLoadedMessage.empty());
				if (featureProfilingAvailable) {
					auto& pageLayout = g_featurePageLayouts[featureName];
					const float materialHeight = std::max(ImGui::GetCursorPosY() - materialStartY, 0.0f);
					const float profilingStart = std::max(materialHeight, pageViewportHeight - profilingHeight);
					ImGui::SetCursorPosY(materialStartY + profilingStart);
					ProfilingRenderer::RenderFeatureTimers(featureName);
					pageLayout.measured = true;
					pageLayout.materialHeight = materialHeight;
					pageLayout.contentHeight = profilingStart + profilingHeight;
					pageLayout.scrollY = ImGui::GetScrollY();
					pageLayout.scrollMaxY = ImGui::GetScrollMaxY();
				}
			}
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

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureHeader(Feature* feat, bool loaded)
{
	auto [description, details] = feat->GetFeatureSummary();
	const auto stage = feat->GetReleaseStage();
	DrawFeatureHeader(feat->GetDisplayName(), loaded && !IsUtilityFeature(feat) ? feat->version : "", description,
		Feature::GetReleaseStageTag(stage), Feature::GetReleaseStageColor(stage));
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureActions(Feature* feat, bool disabled, bool loaded)
{
	const auto name = feat->GetShortName();
	const auto& style = ImGui::GetStyle();
	const float size = BrowserUI::IconButtonSize();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, style.Colors[ImGuiCol_FrameBg]);
	const bool visible = ImGui::BeginChild("##FeatureActionRail", ImVec2(size + style.FramePadding.x * 2.0f, 0),
		ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	if (visible) {
		const float actionsHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - size - style.ItemSpacing.y);
		if (ImGui::BeginChild("##FeatureActionButtons", ImVec2(0.0f, actionsHeight), ImGuiChildFlags_None,
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
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
			if (!IsUtilityFeature(feat))
				action("##Favorite", Icons::TI(ICON_TI_PIN), favorite ? T("menu.features.remove_from_favorites", "Unpin Feature") : T("menu.features.add_to_favorites", "Pin Feature"), loaded, favorite, [&] {
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
			if (feat->UsesMainSettings()) {
				action("##Defaults", SettingsActionIcons::kRestoreDefaults, T("menu.features.restore_defaults", "Restore Defaults"), loaded && !disabled,
					false, [&] { globals::state->RestoreFeatureLayers(feat); });
				action("##RestoreSaved", SettingsActionIcons::kRestoreSaved, T("menu.restore_settings", "Restore Saved Settings"), loaded && !disabled,
					false, [&] { globals::state->RestoreSavedFeatureSettings(feat); });
			}
			if (auto* overrides = SettingsOverrideManager::GetSingleton(); feat->UsesMainSettings() && overrides->HasFeatureOverrides(name))
				action("##ApplyOverride", SettingsActionIcons::kReapplyOverride, T("menu.features.apply_override", "Reapply Override"), loaded && !disabled,
					false, [&] { feat->ReapplyCurrentPageOverrideSettings(); });
			if (FeatureOverwritesPanel::HasExportableSettings(feat))
				action("##ExportOverwrite", Icons::TI(ICON_TI_FILE_EXPORT), T("cs_editor.feature_overwrite_export", "Export Feature Override..."),
					loaded && !disabled, false, [&] { FeatureOverwritesPanel::BeginOverrideExport(feat); });
			if (feat->UsesMainSettings()) {
				action("##ExportBaseline", SceneActionIcons::kExport, T("cs_editor.baseline_export", "Export to Base Settings Preset..."),
					loaded && !disabled, false, [&] { ScenePresetExport::OpenBaseline(name); });
				if (globals::state->HasEditsUnderBaselines(feat))
					action("##Baseline", Icons::TI(ICON_TI_ARROWS_EXCHANGE), T("menu.presets.use_values", "Use preset values"), loaded && !disabled,
						false, [&] { globals::state->ClearFeatureEditsUnderBaselines(feat); });
			}
		}
		ImGui::EndChild();
		const bool hasHint = currentSettingHint && !currentSettingHint->empty();
		BrowserUI::IconButton("##SettingInformation", Icons::TI(ICON_TI_EXCLAMATION_CIRCLE), nullptr, false,
			ImGui::GetColorU32(hasHint ? Util::Colors::GetInfo() : style.Colors[ImGuiCol_TextDisabled]));
		if (hasHint) {
			DrawFeatureActionHint(nullptr, currentSettingHint->c_str(), true);
		} else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
			DrawFeatureActionHint(nullptr,
				T("menu.features.setting_information_hint", "Hover a setting to see its source or scene-editing restrictions."), true);
		}
	}
	ImGui::EndChild();
}

void FeatureListRenderer::SetSettingHint(std::string message)
{
	if (!currentSettingHint)
		return;
	*currentSettingHint = std::move(message);
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
		SettingReveal::BeginFeatureBody(feat);
		feat->DrawSettings();
		SettingReveal::EndFeatureBody();
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
		BrowserUI::EmptyState(T("menu.features.settings_hidden_disabled", "Feature settings are hidden because this feature is disabled at boot."),
			T("menu.features.enable_at_boot_hint", "Load it at boot to use it the next time the game starts."));
		ImGui::Spacing();
		const char* label = T("menu.features.enable_at_boot", "Enable at Boot");
		const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
		if (Util::Button(label))
			feat->ToggleAtBootSetting();
	} else if (const auto unavailableMessage = feat->GetUnavailableMessage()) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(themeSettings.StatusPalette.Error, "%s", unavailableMessage);
		ImGui::PopTextWrapPos();
	} else {
		if (isLoaded) {
			if (SceneSettingsUI::IsFeaturePageEditing(feat))
				SceneSettingsUI::DrawFeaturePageBody(feat);
			else
				DrawFeatureBody(feat);
		} else {
			if (FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
				feat->DrawUnloadedUI();
			} else if (!feat->conflictMessage.empty()) {
				ImGui::TextColored(themeSettings.StatusPalette.Warning, "%s", feat->conflictMessage.c_str());
			} else if (feat->installed) {
				ImGui::Text("%s", T("menu.features.available_after_restart", "This feature will be available after restart."));
			} else {
				feat->DrawUnloadedUI();
				if (!feat->GetFeatureModLink().empty()) {
					ImGui::Spacing();
					auto featureModLink = feat->GetFeatureModLink();
					const auto downloadText = std::vformat(
						T("menu.features.download_link", "Click here to download this feature ({})"), std::make_format_args(featureModLink));
					if (Util::RoundedSelectable(downloadText.c_str(), false)) {
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
							  "Use Base Settings preset values: your changes override settings your Base Settings presets set. "
							  "Click to drop those changes and use the presets' values. Your other changes stay."));
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderRestoreDefaultsButton(Feature* feat)
{
	const float size = ImGui::GetFrameHeight();
	{
		auto style = Util::TransparentIconButtonStyle();
		if (Icons::Button("##RestoreDefaults", SettingsActionIcons::kRestoreDefaults, ImVec2(size, size))) {
			// Clears the user's edits, so the feature shows what defaults and Baseline packs set.
			globals::state->RestoreFeatureLayers(feat);
		}
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("menu.features.restore_defaults_tooltip", "Restore defaults: clears your edits to this feature, back to what defaults and your Base Settings presets set"));
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
		ImGui::Text("%s", isFavorite ? T("menu.features.remove_from_favorites_tooltip", "Unpin this feature from the top of the feature list") : T("menu.features.add_to_favorites_tooltip", "Pin this feature to the top of the feature list"));
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
		const float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH * Util::GetUIScale();
		const float windowWidth = ImGui::GetWindowWidth();
		const float offset = (windowWidth - buttonWidth) * 0.5f;
		if (offset > 0)
			ImGui::SetCursorPosX(offset);

		if (Util::Button(T("menu.features.ok_button", "OK"), ImVec2(buttonWidth, 0))) {
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

std::uint32_t FeatureListRenderer::GetFilterBits(Feature* feat)
{
	auto* sceneManager = globals::sceneSettingsManager;
	const auto shortName = feat->GetShortName();
	std::uint32_t bits = 0;
	if (feat->loaded && globals::state->IsFeatureFavorite(shortName))
		bits |= FeatureListFilter::kFavorites;
	if (globals::state->HasUserEdits(feat))
		bits |= FeatureListFilter::kModified;
	if (sceneManager && sceneManager->HasAnySceneEntriesForFeature(shortName))
		bits |= FeatureListFilter::kScene;
	if (feat->GetReleaseStage() != Feature::ReleaseStage::Release)
		bits |= FeatureListFilter::kPreRelease;
	if (!feat->loaded)
		bits |= FeatureListFilter::kUnloaded;
	return bits;
}

bool FeatureListRenderer::PassesFilter(std::uint32_t bits, std::uint32_t filter)
{
	const std::uint32_t required = filter & FeatureListFilter::kKnown;
	return (bits & required) == required;
}

void FeatureListRenderer::ToggleFavorite(Feature* feat)
{
	const auto name = feat->GetShortName();
	globals::state->SetFeatureFavorite(name, !globals::state->IsFeatureFavorite(name));
}

void FeatureListRenderer::RenderSortPopup(const char* popupId)
{
	if (!ImGui::BeginPopup(popupId))
		return;
	auto& menuSettings = globals::menu->GetSettings();

	using enum Menu::FeatureSortMode;
	auto& sort = menuSettings.FeatureListSort;
	if (ImGui::MenuItem(T("menu.features.sort_by_category", "By Category"), nullptr, sort == Category))
		globals::state->SetFeatureListSort(false);
	if (ImGui::MenuItem(T("menu.features.sort_alphabetically", "Alphabetical"), nullptr, sort == Alphabetical))
		globals::state->SetFeatureListSort(true);

	ImGui::EndPopup();
}

void FeatureListRenderer::RenderFilterPopup(const char* popupId)
{
	if (!ImGui::BeginPopup(popupId))
		return;
	struct FilterCounts
	{
		int favorites = 0, modified = 0, scene = 0, preRelease = 0, unloaded = 0;
	} counts;
	globals::state->PrimeUserEdits();
	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->IsInMenu() || feature->IsHiddenUnreleased())
			continue;
		const auto bits = GetFilterBits(feature);
		counts.favorites += (bits & FeatureListFilter::kFavorites) != 0;
		counts.modified += (bits & FeatureListFilter::kModified) != 0;
		counts.scene += (bits & FeatureListFilter::kScene) != 0;
		counts.preRelease += (bits & FeatureListFilter::kPreRelease) != 0;
		counts.unloaded += (bits & FeatureListFilter::kUnloaded) != 0;
	}
	auto& menuSettings = globals::menu->GetSettings();

	auto& filter = menuSettings.FeatureListFilter;
	struct Item
	{
		std::uint32_t bit;
		int count;
		const char* label;
		const char* tooltip;
	};
	const Item items[] = {
		{ FeatureListFilter::kFavorites, counts.favorites, T("menu.features.filter_label_favorites", "Pinned"),
			T("menu.features.filter_favorites", "Pinned features") },
		{ FeatureListFilter::kModified, counts.modified, T("menu.features.filter_label_modified", "Modified"),
			T("menu.features.filter_modified", "Modified: settings differ from defaults and your Base Settings presets") },
		{ FeatureListFilter::kScene, counts.scene, T("menu.features.filter_label_scene", "Scene"),
			T("menu.features.filter_scene", "Scene: has Scene Manager settings") },
		{ FeatureListFilter::kPreRelease, counts.preRelease, T("menu.features.filter_label_pre_release", "Pre-release"),
			T("menu.features.filter_pre_release", "Pre-release: alpha and beta features") },
		{ FeatureListFilter::kUnloaded, counts.unloaded, T("menu.features.filter_label_unloaded", "Unloaded"),
			T("menu.features.filter_unloaded", "Unloaded: missing, failed, or disabled at boot. Only these are listed while it is on") },
	};
	// Filters combine, so toggling one leaves the popup open
	ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
	for (const auto& item : items) {
		const bool active = (filter & item.bit) != 0;
		const std::string countText = std::to_string(item.count);
		// A filter with nothing to show stays greyed unless it is on, so it can still be turned off
		if (ImGui::MenuItem(item.label, countText.c_str(), active, item.count > 0 || active))
			filter ^= item.bit;
		Util::AddTooltip(item.tooltip, Util::kTooltipWhenDisabled);
	}
	ImGui::PopItemFlag();

	ImGui::Separator();
	if (ImGui::MenuItem(T("menu.features.clear_filter_chips", "Clear filters"), nullptr, false, (filter & FeatureListFilter::kKnown) != 0))
		filter = 0;
	ImGui::EndPopup();
}
