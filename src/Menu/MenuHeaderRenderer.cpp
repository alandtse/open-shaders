#include "MenuHeaderRenderer.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>

#include "CSEditor/Browser/BrowserWidgets.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "Menu/GoToPalette.h"
#include "Menu/HomePageRenderer.h"
#include "Menu/Icons/helpers/SettingsActionIcons.h"
#include "Menu/IssuesPageRenderer.h"
#include "Plugin.h"
#include "SaveManager.h"
#include "ShaderCache.h"
#include "State.h"
#include "ThemeManager.h"
#include "Util.h"

namespace
{
	Util::ConfirmationPopup restoreDefaults;

	void RequestRestoreDefaults()
	{
		restoreDefaults.title = T("menu.restore_defaults", "Restore All Defaults");
		restoreDefaults.message = T("menu.restore_defaults_confirm", "Reset all settings to their defaults? Your saved settings remain unchanged until you save.");
		restoreDefaults.confirmLabel = T("menu.restore_defaults", "Restore All Defaults");
		restoreDefaults.cancelLabel = T("ui.cancel", "Cancel");
		restoreDefaults.focus = Util::ConfirmationPopup::Focus::Cancel;
		restoreDefaults.Request();
	}

}

std::string MenuHeaderRenderer::GetTitle()
{
	auto version = Plugin::BUILD_DESCRIBE;
	if (version.starts_with('v'))
		version.remove_prefix(1);
	if (version.empty())
		return std::format("{} {}", Plugin::DISPLAY_NAME, Util::GetFormattedVersion(Plugin::VERSION));
	return std::format("{} {}", Plugin::DISPLAY_NAME, version);
}

void MenuHeaderRenderer::RenderHeader(Menu& menu, bool& sidebarVisible, float uiScale)
{
	const float size = BrowserUI::IconButtonSize();
	const float homeGap = ImGui::GetStyle().ItemInnerSpacing.x;
	if (ImGui::BeginTable("##MainMenuHeader", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("##ActionsAndTitle", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##Navigation", ImGuiTableColumnFlags_WidthFixed, size * 2.0f + homeGap + ImGui::GetStyle().ItemSpacing.x);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		const float top = ImGui::GetCursorPosY();
		ImGui::BeginGroup();
		bool afterFirst = false;
		const auto action = [&](const char* id, Icons::GlyphRef glyph, const char* tooltip, bool selected, bool enabled, auto&& run, ImU32 tint = 0) {
			if (afterFirst)
				BrowserUI::SameLineIfFits(size);
			afterFirst = true;
			ImGui::BeginDisabled(!enabled);
			if (BrowserUI::IconButton(id, glyph, tooltip, selected, tint))
				run();
			ImGui::EndDisabled();
		};
		action("##SidebarToggle", Icons::TI(ICON_TI_LAYOUT_COLUMNS), sidebarVisible ? T("menu.hide_sidebar", "Hide Sidebar") : T("menu.show_sidebar", "Show Sidebar"),
			sidebarVisible, true, [&] { sidebarVisible = !sidebarVisible; });
		action("##BackgroundToggle", Icons::TI(ICON_TI_EYE), menu.IsBackgroundVisible() ? T("menu.hide_background", "Hide Background") : T("menu.show_background", "Show Background"),
			!menu.IsBackgroundVisible(), true, [&] { menu.ToggleBackgroundVisibility(); });

		auto* shaderCache = globals::shaderCache;
		const bool canClear = !shaderCache->IsCapturingActiveShaders() && !shaderCache->IsAwaitingMenuCloseCapture();
		const auto clearScope = Util::ResolveShaderCacheClearScope();
		const auto* cacheTooltip = clearScope == Util::ShaderCacheClearScope::ActiveOnly ?
		                               T("menu.clear_active_shaders", "Clear Active Shaders") :
		                               T("menu.clear_shader_cache", "Clear Shader Cache");
		const auto cacheGlyph = clearScope == Util::ShaderCacheClearScope::ActiveOnly ? SettingsActionIcons::kClearActiveShaders : SettingsActionIcons::kClearCache;
		BrowserUI::SameLineIfFits(size);
		ImGui::BeginDisabled(!canClear);
		if (BrowserUI::PaintedIconButton("##ClearShaderCache", [cacheGlyph](ImDrawList* drawList, ImVec2, float, ImU32) {
				Icons::FontGuard font(cacheGlyph);
				const auto color = ImGui::IsItemHovered() ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : Util::Colors::GetSecondary();
				// Status marks change glyph bounds, so both cache icons share one placement reference.
				const auto position = Icons::GetCenteredGlyphPos(ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), SettingsActionIcons::kClearCache.utf8);
			drawList->AddText(position, ImGui::GetColorU32(color), cacheGlyph.utf8); }, 0, cacheTooltip))
			Util::RequestClearShaderCacheConfirmation(clearScope);
		ImGui::EndDisabled();
		const bool restoreAllDefaults = ImGui::GetIO().KeyShift;
		const auto* restoreTooltip = restoreAllDefaults ? T("menu.restore_defaults", "Restore All Defaults") : T("menu.restore_settings", "Restore Saved Settings");
		action("##RestoreSettings", restoreAllDefaults ? SettingsActionIcons::kRestoreDefaults : SettingsActionIcons::kRestoreSaved, restoreTooltip, false, true,
			[&] {
				if (restoreAllDefaults)
					RequestRestoreDefaults();
				else
					globals::state->Load();
			});
		const bool unsaved = globals::state->HasUnsavedChanges();
		{
			auto unsavedTint = Util::StateHoverButtonStyle(Util::Colors::GetUnsaved(), unsaved);
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
			const SKSE::stl::scope_exit restoreSaveHover([]() noexcept { ImGui::PopStyleColor(); });
			action("##SaveSettings", Icons::TI(ICON_TI_DEVICE_FLOPPY), unsaved ? T("menu.save_settings_unsaved", "Save Settings (unsaved changes)") : T("menu.save_settings", "Save Settings"), false, true, [] { SaveManager::SaveAll(); }, unsaved ? ImGui::GetColorU32(Util::Colors::GetUnsaved()) : 0);
		}

		{
			MenuFonts::FontRoleGuard titleFont(Menu::FontRole::Title);
			const auto title = GetTitle();
			const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
			BrowserUI::SameLineIfFits(titleWidth);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - titleWidth));
			ImGui::AlignTextToFramePadding();
			ImGui::TextWrapped("%s", title.c_str());
		}
		ImGui::EndGroup();
		const float height = ImGui::GetItemRectSize().y;
		ImGui::TableSetColumnIndex(1);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + homeGap);
		ImGui::SetCursorPosY(top + std::max(0.0f, (height - size) * 0.5f));
		const auto issueCount = IssuesPageRenderer::GetFeatureIssueCount();
		const auto homeTooltip = issueCount == 0 ? std::string(T("menu.features.home", "Home")) :
		                         issueCount == 1 ? std::string(T("menu.home.feature_issue_one", "1 feature issue")) :
		                                           I18n::GetSingleton()->Format("menu.home.feature_issue_many", { { "count", std::to_string(issueCount) } }, "{count} feature issues");
		const auto errorColor = Util::Colors::GetError();
		ImGui::PushStyleColor(ImGuiCol_Header, issueCount > 0 ? ImVec4(errorColor.x, errorColor.y, errorColor.z, ThemeManager::Constants::SEGMENT_SELECTED_FILL_ALPHA) : ImGui::GetStyleColorVec4(ImGuiCol_Header));
		const SKSE::stl::scope_exit restoreIssueAccent([]() noexcept { ImGui::PopStyleColor(); });
		if (BrowserUI::IconButton("##Home", Icons::TI(ICON_TI_HOME), homeTooltip.c_str(), issueCount > 0 || menu.GetSelectedMenuId() == "Home",
				issueCount > 0 ? ImGui::GetColorU32(errorColor) : 0)) {
			menu.SelectFeatureMenu("Home");
			if (issueCount > 0)
				HomePageRenderer::FocusNeedsAttention();
		}
		ImGui::SameLine();
		if (BrowserUI::IconButton("##Search", Icons::TI(ICON_TI_SEARCH), T("menu.palette.title", "Go to..."), GoToPalette::IsOpen()))
			GoToPalette::Open();
		ImGui::EndTable();
	}

	if (restoreDefaults.Draw()) {
		globals::state->Load(State::ConfigMode::DEFAULT);
		globals::state->MarkSettingsChanged();
	}

	ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS * uiScale);
	ImGui::Spacing();
	if (globals::shaderCache->GetFailedTasks()) {
		if (Util::Button(T("menu.toggle_error_message", "Toggle Error Message"), { -1, 0 }))
			globals::shaderCache->ToggleErrorMessages();
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("menu.toggle_error_message_tooltip",
				"Hide or show the shader failure message. "
				"Your installation is broken and will likely see errors in game. "
				"Please double check you have updated all features and that your load order is correct. "
				"See OpenShaders.log for details and check the Nexus Mods page or Discord server."));
		ImGui::Spacing();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS * uiScale);
		ImGui::Spacing();
	}
}
