#include "MenuManager.h"

#include "EffectManager.h"
#include "Features/Effects11.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "PresetManager.h"
#include "SettingManager.h"
#include "State.h"
#include "TextureManager.h"
#include "Utils/UI.h"

namespace
{
	// Finds a_location's own state for a_flagName, or nullptr if it has no entry for it.
	const PresetEffectStatus* FindStatus(const PresetLocation& a_location, const std::string& a_flagName)
	{
		for (const auto& status : a_location.effectStatus) {
			if (status.flagName == a_flagName)
				return &status;
		}
		return nullptr;
	}

	// Hover tooltip for a preset-location picker entry: header comment, then a captioned
	// diff against a_active (or plain state if a_active is nullptr or is a_location).
	void RenderPresetTooltip(const PresetLocation& a_location, const PresetLocation* a_active)
	{
		if (!ImGui::IsItemHovered())
			return;

		ImGui::BeginTooltip();
		if (!a_location.headerComment.empty())
			ImGui::TextUnformatted(a_location.headerComment.c_str());

		const bool isActiveLocation = a_active && a_active->root == a_location.root;
		const auto& palette = globals::menu->GetSettings().Theme.StatusPalette;

		if (isActiveLocation) {
			// Its real settings are already live in the panel below -- repeating them
			// here (with no diff to show against itself) would just be noise.
			ImGui::TextColored(palette.InfoColor, "%s", T("feature.effects11.preset_tooltip_is_active", "(currently selected -- see settings below)"));
		} else if (a_active) {
			ImGui::TextColored(palette.InfoColor, T("feature.effects11.preset_tooltip_diff_vs", "Compared to selected preset (%s):"), a_active->label.c_str());
			for (const auto& status : a_location.effectStatus) {
				const PresetEffectStatus* activeStatus = FindStatus(*a_active, status.flagName);
				const bool activeEnabled = activeStatus && activeStatus->enabled;

				if (status.enabled && !status.fileExists) {
					ImGui::TextColored(palette.Error, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_broken", "on, file missing"));
				} else if (status.enabled && !activeEnabled) {
					ImGui::TextColored(palette.SuccessColor, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_added", "on (added)"));
				} else if (!status.enabled && activeEnabled) {
					ImGui::TextColored(palette.Error, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_removed", "off (active has it on)"));
				} else if (status.enabled) {
					ImGui::Text("%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_on", "on"));
				} else {
					ImGui::TextColored(palette.Disable, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_off", "off"));
				}
			}
		} else {
			// No active preset to diff against (nothing loaded yet), so nothing below
			// duplicates this -- show plain state instead of a diff.
			for (const auto& status : a_location.effectStatus) {
				if (status.enabled && !status.fileExists)
					ImGui::TextColored(palette.Error, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_broken", "on, file missing"));
				else if (status.enabled)
					ImGui::Text("%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_on", "on"));
				else
					ImGui::TextColored(palette.Disable, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_off", "off"));
			}
		}
		ImGui::EndTooltip();
	}
}

MenuManager& MenuManager::GetSingleton()
{
	static MenuManager instance;
	return instance;
}

bool MenuManager::RenderPresetSelector()
{
	bool changed = false;
	auto& settingManager = SettingManager::GetSingleton();
	auto& effectManager = EffectManager::GetSingleton();
	auto& effects11 = globals::features::effects11;
	auto& presetManager = PresetManager::GetSingleton();

	const auto& locations = presetManager.GetDiscoveredLocations();
	const auto* active = presetManager.GetActiveLocation();

	if (locations.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Warning);
		ImGui::TextWrapped("%s", T("feature.effects11.no_preset_found",
									 "No ENB preset found (checked game root, Data, and Data subfolders)."));
		ImGui::PopStyleColor();
	} else {
		const std::string savedLocationLabel = effects11.settings.presetLocation == "." ? T("feature.effects11.preset_game_root", "Game root") : effects11.settings.presetLocation;
		const std::string previewLabel = active ? active->label : (effects11.settings.presetLocation.empty() ? T("feature.effects11.preset_none_selected", "(none selected)") : T("feature.effects11.preset_missing", "(missing) ") + savedLocationLabel);
		if (ImGui::BeginCombo(T("feature.effects11.preset_location", "Preset location"), previewLabel.c_str())) {
			for (const auto& loc : locations) {
				const bool isSelected = active && active->root == loc.root;
				if (Util::RoundedSelectable(loc.label.c_str(), isSelected)) {
					presetManager.SetActiveLocation(loc.root);
					effects11.settings.presetLocation = presetManager.ToRelativeKey(loc.root);
					// After Load()/Apply(), not before: Save() piggybacks an ENB save for
					// whatever preset is currently loaded, which would still be the old one.
					settingManager.Load();
					effectManager.Apply();
					globals::state->Save();
					changed = true;
				}
				RenderPresetTooltip(loc, active);
			}
			ImGui::EndCombo();
		}
	}
	ImGui::SameLine();
	if (Util::Button(T("feature.effects11.rescan_presets", "Rescan"))) {
		presetManager.Rescan();
		changed = true;
	}
	return changed;
}
