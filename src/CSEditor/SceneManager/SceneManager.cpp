#include "SceneManager.h"

#include <imgui.h>

#include "../Browser/BrowserWidgets.h"
#include "SceneEntryTable.h"
#include "SceneSettingsManager.h"
#include "SceneSettingsUI.h"
#include "Utils/UI.h"

std::pair<std::string, std::vector<std::string>> SceneManager::GetFeatureSummary()
{
	return {
		T("feature.scene_manager.description",
			"Applies selected {brand} settings by interior, time of day, weather, and location."),
		{
			T("feature.scene_manager.key_feature_1", "Blends exterior settings across time of day and weather transitions"),
			T("feature.scene_manager.key_feature_2", "Applies interior settings separately from exterior settings"),
			T("feature.scene_manager.key_feature_3", "Supports location and cell overrides with per-setting precedence"),
		},
	};
}

void SceneManager::DrawTransitions()
{
	struct TransitionRow
	{
		TransitionControl control;
		const char* label;
		const char* format;
		ImGuiSliderFlags flags;
		const char* tooltip;
	};
	const TransitionRow rows[] = {
		{ TransitionControl::TimeOfDayHours,
			T("feature.scene_manager.time_of_day_transition", "Time of Day Transition"), "%.2f h", ImGuiSliderFlags_None,
			T("feature.scene_manager.time_of_day_transition_tooltip",
				"Hours spent blending between time of day periods, kept inside the sky's own colour transitions.\n"
				"0 switches between periods instantly.\n"
				"The active preset supplies this until you set your own.") },
		{ TransitionControl::LocationSeconds,
			T("feature.scene_manager.location_transition", "Location Transition"), "%.1f s", ImGuiSliderFlags_Logarithmic,
			T("feature.scene_manager.location_transition_tooltip",
				"Seconds location overrides take to ease in and out when you move between places.\n"
				"A single setting can override this from its own transition field in Scene Manager.\n"
				"The active preset supplies this until you set your own.") },
		{ TransitionControl::WeatherScale,
			T("feature.scene_manager.weather_transition_length", "Weather Transition Length"), "%.2fx", ImGuiSliderFlags_Logarithmic,
			T("feature.scene_manager.weather_transition_length_tooltip",
				"Multiplies how long every weather takes to blend into the next, keeping each weather's own pacing.\n"
				"A longer transition also delays a queued weather change, which waits for the running one to finish.\n"
				"The active preset supplies this until you set your own.") },
		{ TransitionControl::Ease,
			T("feature.scene_manager.transition_ease", "Transition Easing"), "%.2f", ImGuiSliderFlags_None,
			T("feature.scene_manager.transition_ease_tooltip",
				"Softens the start and end of time of day, location and weather transitions, sky included.\n"
				"0 keeps a constant-speed blend.\n"
				"The active preset supplies this until you set your own.") },
	};

	const auto& style = ImGui::GetStyle();
	const float iconSize = BrowserUI::IconButtonSize();
	const auto actionCount = SceneEntryTable::kRestoreActions.size();
	const float actionsWidth = actionCount * iconSize + (actionCount - 1) * style.ItemSpacing.x;
	float labelWidth = 0.0f;
	for (const auto& row : rows)
		labelWidth = std::max(labelWidth, ImGui::CalcTextSize(row.label).x);
	const float controlWidth = ImGui::GetContentRegionAvail().x - actionsWidth - 4.0f * style.CellPadding.x;
	labelWidth = std::clamp(labelWidth, 1.0f, std::max(1.0f, controlWidth * 0.5f));

	if (!ImGui::BeginTable("##TransitionDefaults", 3, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
		return;
	ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
	ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
	for (const auto& [control, label, format, flags, tooltip] : rows) {
		const auto& spec = GetTransitionControlSpec(control);
		ImGui::PushID(static_cast<int>(control));
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextWrapped("%s", label);
		Util::AddTooltip(tooltip);

		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-FLT_MIN);
		auto value = GetTransitionControl(control);
		if (ImGui::SliderFloat("##Value", &value, spec.min, spec.max, format, ImGuiSliderFlags_AlwaysClamp | flags))
			SetTransitionControl(control, value, true);
		// A drag held still would otherwise outlive the debounce and write the file mid-gesture.
		if (ImGui::IsItemActive())
			HoldDeferredSceneChanges();
		Util::AddTooltip(tooltip);
		ImGui::TableNextColumn();
		for (const auto action : SceneEntryTable::kRestoreActions) {
			if (action != SceneEntryTable::kRestoreActions.front())
				ImGui::SameLine();
			const bool defaults = action == RestoreSource::Defaults;
			const auto target = defaults ? std::optional{ spec.defaultValue } : GetPresetTransitionControl(control);
			const bool canRestore = target && GetTransitionControl(control) != *target;
			if (SceneEntryTable::DrawRestoreAction(action, canRestore, iconSize))
				SetTransitionControl(control, defaults ? target : std::nullopt);
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void SceneManager::SetupResources()
{
	LoadAll();
}

void SceneManager::DataLoaded()
{
	SceneSettingsManager::OnDataLoaded();
	MenuOpenCloseEventHandler::Register();
}

void SceneManager::Update()
{
	SceneSettingsManager::Update();
}

void SceneManager::DrawSettings()
{
	SceneSettingsUI::DrawStandalonePage();
}
