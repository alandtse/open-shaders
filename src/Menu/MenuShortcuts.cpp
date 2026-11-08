#include "Menu/MenuShortcuts.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Fonts.h"
#include "Menu/MenuHeaderRenderer.h"
#include "Menu/MenuLinks.h"
#include "Utils/UI.h"

#include <algorithm>

namespace MenuShortcuts
{
	namespace
	{
		/// Fires while the main menu window, or a child of it, is focused, wherever in it Dispatch() runs.
		bool helpRequested = false;
		constexpr const char* kHelpPopupId = "##MenuShortcutHelp";

		constexpr ImGuiInputFlags kRouteFlags = ImGuiInputFlags_RouteFocused | ImGuiInputFlags_RouteFromRootWindow;

		const char* KeyName(ImGuiKey key)
		{
			switch (key) {
			case ImGuiKey_PageUp:
				return "PgUp";
			case ImGuiKey_PageDown:
				return "PgDn";
			default:
				return ImGui::GetKeyName(key);
			}
		}
	}

	std::vector<Shortcut> BuildTable(const Actions& actions)
	{
		std::vector<Shortcut> table;
		const auto add = [&table](std::string_view id, ImGuiKeyChord chord, const char* (*label)(),
							 const std::function<bool()>& enabled, const std::function<void()>& run, bool repeat = false) {
			if (run)
				table.push_back({ id, chord, label, enabled, run, repeat });
		};

		add(Ids::kSave, ImGuiMod_Ctrl | ImGuiKey_S, [] { return T("menu.shortcuts.save", "Save settings"); }, {}, actions.save);
		add(Ids::kUndo, ImGuiMod_Ctrl | ImGuiKey_Z, [] { return T("menu.shortcuts.undo", "Undo"); }, {}, actions.undo);
		add(Ids::kRedo, ImGuiMod_Ctrl | ImGuiKey_Y, [] { return T("menu.shortcuts.redo", "Redo"); }, {}, actions.redo);
		add(Ids::kRedo, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, [] { return T("menu.shortcuts.redo", "Redo"); }, {}, actions.redo);
		add(Ids::kGoTo, ImGuiMod_Ctrl | ImGuiKey_K, [] { return T("menu.shortcuts.go_to", "Go to..."); }, {}, actions.openGoTo);
		add(Ids::kFocusSearch, ImGuiMod_Ctrl | ImGuiKey_F, [] { return T("menu.shortcuts.focus_search", "Search features"); }, {}, actions.focusSearch);

		if (actions.stepFeature) {
			const auto step = actions.stepFeature;
			add(Ids::kPreviousFeature, ImGuiMod_Ctrl | ImGuiKey_PageUp, [] { return T("menu.shortcuts.previous_feature", "Previous feature"); }, {}, [step] { step(-1); }, true);
			add(Ids::kNextFeature, ImGuiMod_Ctrl | ImGuiKey_PageDown, [] { return T("menu.shortcuts.next_feature", "Next feature"); }, {}, [step] { step(1); }, true);
		}

		add(Ids::kTogglePin, ImGuiMod_Ctrl | ImGuiKey_P, [] { return T("menu.shortcuts.toggle_pin", "Pin or unpin the current feature"); }, actions.canTogglePin, actions.togglePin);

		if (actions.openPage) {
			const auto open = actions.openPage;
			add(Ids::kHome, ImGuiMod_Ctrl | ImGuiKey_1, [] { return T("menu.shortcuts.page_home", "Go to Home"); }, {}, [open] { open(MenuPageIds::kHome); });
			add(Ids::kPresets, ImGuiMod_Ctrl | ImGuiKey_2, [] { return T("menu.shortcuts.page_presets", "Go to Presets"); }, {}, [open] { open(MenuPageIds::kPresets); });
			add(Ids::kGeneral, ImGuiMod_Ctrl | ImGuiKey_3, [] { return T("menu.shortcuts.page_general", "Go to General"); }, {}, [open] { open(MenuPageIds::kGeneral); });
		}

		add(Ids::kOpenEditor, ImGuiMod_Ctrl | ImGuiKey_E, [] { return T("menu.shortcuts.open_editor", "Open the OS Editor"); }, actions.canOpenEditor, actions.openEditor);
		add(Ids::kHelp, ImGuiKey_F1, [] { return T("menu.shortcuts.help", "Help and shortcuts"); }, {}, actions.openHelp);

		return table;
	}

	void Dispatch(const std::vector<Shortcut>& table)
	{
		for (const auto& shortcut : table) {
			const ImGuiInputFlags flags = kRouteFlags | (shortcut.repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None);
			if (ImGui::Shortcut(shortcut.chord, flags) && (!shortcut.enabled || shortcut.enabled()))
				shortcut.run();
		}
	}

	std::string FormatChord(ImGuiKeyChord chord)
	{
		std::string text;
		if (chord & ImGuiMod_Ctrl)
			text += "Ctrl+";
		if (chord & ImGuiMod_Shift)
			text += "Shift+";
		if (chord & ImGuiMod_Alt)
			text += "Alt+";
		text += KeyName(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_));
		return text;
	}

	std::string GetHint(const std::vector<Shortcut>& table, std::string_view id)
	{
		std::string hint;
		for (const auto& shortcut : table) {
			if (shortcut.id != id)
				continue;
			if (!hint.empty())
				hint += " / ";
			hint += FormatChord(shortcut.chord);
		}
		return hint;
	}

	std::vector<HelpRow> BuildHelpRows(const std::vector<Shortcut>& table)
	{
		std::vector<HelpRow> rows;
		std::vector<std::string_view> ids;
		for (const auto& shortcut : table) {
			if (std::ranges::find(ids, shortcut.id) != ids.end())
				continue;
			ids.push_back(shortcut.id);
			rows.push_back({ shortcut.label(), GetHint(table, shortcut.id) });
		}
		return rows;
	}
	void RequestHelp()
	{
		helpRequested = true;
	}

	void DrawHelp(const std::vector<Shortcut>& table)
	{
		if (std::exchange(helpRequested, false))
			ImGui::OpenPopup(kHelpPopupId);
		if (!ImGui::BeginPopup(kHelpPopupId))
			return;

		// Escape closes the popover, not the menu
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			globals::menu->ConsumeEscape();
			ImGui::CloseCurrentPopup();
		}

		{
			MenuFonts::FontRoleGuard heading(Menu::FontRole::Subheading);
			ImGui::SeparatorText(T("menu.help.shortcuts", "Keyboard Shortcuts"));
		}
		if (ImGui::BeginTable("##HelpShortcuts", 2, ImGuiTableFlags_SizingFixedFit)) {
			for (const auto& row : BuildHelpRows(table)) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.label);
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", row.chords.c_str());
			}
			ImGui::EndTable();
		}

		{
			MenuFonts::FontRoleGuard heading(Menu::FontRole::Subheading);
			ImGui::SeparatorText(T("menu.help.help", "Help"));
		}
		for (size_t i = 0; i < MenuLinks::kAll.size(); ++i) {
			const auto link = MenuLinks::kAll[i];
			if (i > 0) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", "\xC2\xB7");  // middle dot
				ImGui::SameLine();
			}
			ImGui::PushID(static_cast<int>(link));
			if (ImGui::TextLink(MenuLinks::GetLabel(link)))
				MenuLinks::Open(link);
			ImGui::PopID();
			Util::HoverTip(MenuLinks::GetUrl(link));
		}

		{
			MenuFonts::FontRoleGuard heading(Menu::FontRole::Subheading);
			ImGui::SeparatorText(T("menu.help.about", "About"));
		}
		ImGui::TextUnformatted(MenuHeaderRenderer::GetTitle().c_str());

		ImGui::EndPopup();
	}
}
