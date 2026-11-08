#include "WeatherPickerWindow.h"

#include "../I18n/I18n.h"
#include "EditorWindow.h"
#include "Features/CSEditor.h"
#include "Globals.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/MenuIcons.h"
#include "Menu/ThemeManager.h"
#include "PaletteWindow.h"
#include "Utils/Game.h"
#include "Utils/ImGuiLayout.h"
#include "Utils/UI.h"
#include "imgui_internal.h"

#include <format>

#define I18N_KEY_PREFIX "cs_editor."

void WeatherPickerWindow::Draw()
{
	if (!open)
		return;

	const auto* editor = EditorWindow::GetSingleton();
	const float scale = Util::GetUIScale();
	const float pad = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;
	const auto* viewport = ImGui::GetMainViewport();
	const ImRect workArea(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + pad),
		ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + viewport->WorkSize.y - pad));

	// Default spot: right-aligned, directly above the palette window. The palette only reports a
	// real position once it has been drawn, so keep re-anchoring until then (or until it is
	// known to be closed) and leave the window free to be dragged at a stable UI scale.
	float anchorX = workArea.Max.x;
	float anchorY = workArea.Max.y;
	bool anchorResolved = !PaletteWindow::GetSingleton()->open;
	bool paletteSettling = false;
	if (ImGuiWindow* palette = ImGui::FindWindowByName("##CSEditorPalette"); PaletteWindow::GetSingleton()->open && palette && palette->WasActive && palette->Size.y > 0.0f) {
		anchorX = palette->Pos.x + palette->Size.x;
		anchorY = palette->Pos.y - pad;
		anchorResolved = true;
		paletteSettling = Util::ImGuiLayout::AutoResizeAnchor::IsSettling(palette);
	}

	static bool positioned = false;
	static Util::ImGuiLayout::AutoResizeAnchor layout;
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(std::max(1.0f, workArea.GetWidth()), std::max(1.0f, workArea.GetHeight())));
	const bool reanchor = layout.Update(scale, workArea, editor->resetLayout || !positioned, ImGui::FindWindowByName("##WeatherPickerWindow"), paletteSettling);
	if (reanchor)
		ImGui::SetNextWindowPos(ImVec2(anchorX, anchorY), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	if (anchorResolved)
		positioned = true;

	if (ImGui::Begin("##WeatherPickerWindow", nullptr, CSEditorWindows::kFloatingPanelFlags))
		DrawContents();
	ImGui::End();
}

void WeatherPickerWindow::DrawSelection(float width, float height, bool fullWidthSearch)
{
	const float scale = Util::GetUIScale();
	const auto& style = ImGui::GetStyle();
	const float gap = style.ItemSpacing.x;
	const float inset = ThemeManager::Constants::LEFT_PANE_EDGE_INSET * scale;
	const float filtersWidth = CSEditor::GetWeatherFilterWidth() + inset * 2.0f;
	const bool stacked = width < filtersWidth + ThemeManager::Constants::CSE_PICKER_COMBO_WIDTH * scale + gap;
	const float listWidth = stacked ? width : width - filtersWidth - gap;
	CSEditor::SyncWeatherFilter();
	const auto* sky = globals::game::sky;
	constexpr auto searchId = "WeatherPickerSelection";
	ImGui::BeginGroup();
	std::string search;
	if (fullWidthSearch) {
		ImGui::SetNextItemWidth(width);
		search = Util::DrawComboSearchInput(searchId, false);
		height = std::max(ImGui::GetTextLineHeightWithSpacing(), height - ImGui::GetFrameHeightWithSpacing());
	}
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inset, inset));
	const bool showList = ImGui::BeginChild("##WeatherPickerSelection", ImVec2(listWidth, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (showList) {
		if (!fullWidthSearch) {
			ImGui::SetNextItemWidth(-FLT_MIN);
			search = Util::DrawComboSearchInput(searchId);
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inset, inset));
		const bool showWeathers = ImGui::BeginChild("##WeatherPickerList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		if (showWeathers) {
			const auto& weathers = CSEditor::GetFilteredWeathers();
			for (int index = 0; index < static_cast<int>(weathers.size()); ++index) {
				auto* weather = weathers[index];
				const auto editorId = Util::GetFormEditorID(weather);
				const auto name = weather->GetName() ? weather->GetName() : "";
				const auto formId = std::format("{:08X}", weather->GetFormID());
				if (!search.empty() && !Util::StringMatchesSearch(editorId, search) &&
					!Util::StringMatchesSearch(name, search) && !Util::StringMatchesSearch(formId, search))
					continue;
				const bool selected = sky && sky->currentWeather == weather;
				ImGui::PushID(static_cast<int>(weather->GetFormID()));  // Editor IDs can repeat across plugins
				ImGui::PushStyleColor(ImGuiCol_Text, CSEditor::GetWeatherTypeColor(weather));
				const bool clicked = Util::RoundedSelectable(editorId.empty() ? formId.c_str() : editorId.c_str(), selected);
				ImGui::PopStyleColor();
				ImGui::PopID();
				if (clicked)
					CSEditor::SelectWeather(globals::game::sky, index);
				if (auto tooltip = Util::HoverTooltipWrapper()) {
					ImGui::Text(T(TKEY("tooltip_weather_name"), "Weather: %s"), name);
					ImGui::Text(T(TKEY("tooltip_editor_id"), "Editor ID: %s"), editorId.c_str());
					ImGui::Text(T(TKEY("tooltip_form_id"), "Form ID: 0x%08X"), weather->GetFormID());
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndChild();
	}
	ImGui::EndChild();
	if (!stacked)
		ImGui::SameLine(0.0f, gap);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inset, inset));
	const bool showFilters = ImGui::BeginChild("##WeatherPickerFilters", ImVec2(filtersWidth, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (showFilters)
		CSEditor::DrawWeatherFilterChips();
	ImGui::EndChild();
	ImGui::EndGroup();
}

void WeatherPickerWindow::DrawContents(bool expanded)
{
	const float scale = Util::GetUIScale();
	const float spacing = ThemeManager::Constants::CSE_PICKER_ITEM_SPACING * scale;
	if (!expanded) {
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(spacing, ThemeManager::Constants::CSE_PICKER_FRAME_PADDING_Y * scale));
	}
	const SKSE::stl::scope_exit restoreStyle([expanded] { if (!expanded) ImGui::PopStyleVar(2); });
	auto* sky = globals::game::sky;
	if (!sky) {
		ImGui::TextDisabled("%s", T(TKEY("sky_not_available"), "Sky not available"));
		return;
	}
	const float comboWidth = ThemeManager::Constants::CSE_PICKER_COMBO_WIDTH * scale;
	const float listHeight = ImGui::GetTextLineHeightWithSpacing() * ThemeManager::Constants::CSE_PICKER_LIST_ROWS;
	if (expanded) {
		DrawSelection(ImGui::GetContentRegionAvail().x, listHeight, true);
	} else {
		const auto preview = sky->currentWeather ? Util::GetFormEditorID(sky->currentWeather) : std::string{};
		const float popupWidth = comboWidth + CSEditor::GetWeatherFilterWidth() + spacing +
		                         (ImGui::GetStyle().WindowPadding.x + ThemeManager::Constants::LEFT_PANE_EDGE_INSET * scale) * 2.0f;
		const auto& style = ImGui::GetStyle();
		const float popupHeight = std::min(listHeight + style.WindowPadding.y * 2.0f,
			ImGui::GetWindowViewport()->WorkSize.y - style.DisplaySafeAreaPadding.y * 2.0f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, popupHeight), ImVec2(popupWidth, popupHeight));
		ImGui::SetNextItemWidth(comboWidth);
		if (ImGui::BeginCombo("##WeatherPickerWindowCombo", preview.empty() ? T(TKEY("select_weather"), "Select Weather") : preview.c_str())) {
			DrawSelection(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
			ImGui::EndCombo();
		}
		ImGui::SameLine(0.0f, spacing);
		const float iconSize = ImGui::GetFrameHeight();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
		{
			auto iconStyle = Util::TransparentIconButtonStyle();
			if (Icons::Button("##WeatherPickerWindowReset", Icons::TI(MenuIcons::kRestore), ImVec2(iconSize, iconSize)))
				CSEditor::ResetWeatherToDefault(sky);
		}
		ImGui::PopStyleVar();
		Util::AddTooltip(T(TKEY("reset_weather_tooltip"), "Resets weather to default"));
	}
	if (expanded) {
		const auto* reset = T("feature.weather_picker.reset_weather", "Reset Weather");
		if (Util::Button(reset))
			CSEditor::ResetWeatherToDefault(sky);
		Util::AddTooltip(T(TKEY("reset_weather_tooltip"), "Resets weather to default"));
		ImGui::SameLine();
		auto* editor = EditorWindow::GetSingleton();
		const bool locked = editor->IsWeatherLockedByUser(sky->currentWeather);
		const auto* lock = locked ? T("feature.weather_picker.unlock_weather", "Unlock Weather") : T("feature.weather_picker.lock_weather", "Lock Weather");
		{
			const Util::DisableGuard disabled(!sky->currentWeather);
			if (locked)
				ImGui::PushStyleColor(ImGuiCol_Button, Menu::GetSingleton()->GetTheme().StatusPalette.SuccessColor);
			if (Util::Button(lock))
				editor->ToggleUserWeatherLock(sky->currentWeather);
			if (locked)
				ImGui::PopStyleColor();
			// Hooks failed to install, but MaintainWeatherLock still re-applies the lock every
			// frame, so the lock still works -- just without redirecting the engine's own
			// SetWeather/ForceWeather call sites, so weather may visibly flash before correcting.
			Util::AddTooltip(EditorWindow::GetWeatherLockTooltip(locked));
		}
	}
	const float transitionWidth = expanded ? -FLT_MIN : comboWidth + spacing + ImGui::GetFrameHeight();
	CSEditor::DrawWeatherTransitionBar(sky, transitionWidth, expanded);
}

#undef I18N_KEY_PREFIX
