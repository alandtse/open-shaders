#include "SceneManager.h"

#include <imgui.h>

#include "SceneSettingsManager.h"
#include "SceneSettingsUI.h"
#include "Utils/UI.h"

namespace
{
	/** @brief Reset button on the line of the control before it. @return Whether it was clicked. */
	bool DrawResetButton(const char* label, const char* tooltip)
	{
		ImGui::SameLine();
		const bool clicked = Util::WarningButton(label);
		Util::AddTooltip(tooltip);
		return clicked;
	}
}

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
	const auto drawTransitionControl = [this](TransitionControl control, const char* label, const char* format,
										   ImGuiSliderFlags flags, const char* tooltip, const char* resetLabel) {
		const auto& spec = GetTransitionControlSpec(control);
		auto value = GetTransitionControl(control);
		if (ImGui::SliderFloat(label, &value, spec.min, spec.max, format, ImGuiSliderFlags_AlwaysClamp | flags))
			SetTransitionControl(control, value, true);
		// A drag held still would otherwise outlive the debounce and write the file mid-gesture.
		if (ImGui::IsItemActive())
			HoldDeferredSceneChanges();
		Util::AddTooltip(tooltip);
		if (GetUserTransitionControl(control) &&
			DrawResetButton(resetLabel, T("feature.scene_manager.transition_reset_tooltip",
											"Drop your value and use the active preset's, or the default when it sets none.")))
			SetTransitionControl(control, std::nullopt);
	};

	drawTransitionControl(TransitionControl::TimeOfDayHours,
		T("feature.scene_manager.time_of_day_transition", "Time of Day Transition"), "%.2f h", ImGuiSliderFlags_None,
		T("feature.scene_manager.time_of_day_transition_tooltip",
			"Hours spent blending between time of day periods, kept inside the sky's own colour transitions.\n"
			"0 switches between periods instantly.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.time_of_day_transition_reset", "Reset##TimeOfDayTransition"));
	drawTransitionControl(TransitionControl::LocationSeconds,
		T("feature.scene_manager.location_transition", "Location Transition"), "%.1f s", ImGuiSliderFlags_Logarithmic,
		T("feature.scene_manager.location_transition_tooltip",
			"Seconds location overrides take to ease in and out when you move between places.\n"
			"A single setting can override this from its own transition field in Scene Manager.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.location_transition_reset", "Reset##LocationTransition"));
	drawTransitionControl(TransitionControl::WeatherScale,
		T("feature.scene_manager.weather_transition_length", "Weather Transition Length"), "%.2fx", ImGuiSliderFlags_Logarithmic,
		T("feature.scene_manager.weather_transition_length_tooltip",
			"Multiplies how long every weather takes to blend into the next, keeping each weather's own pacing.\n"
			"A longer transition also delays a queued weather change, which waits for the running one to finish.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.weather_transition_length_reset", "Reset##WeatherTransitionLength"));
	drawTransitionControl(TransitionControl::Ease,
		T("feature.scene_manager.transition_ease", "Transition Easing"), "%.2f", ImGuiSliderFlags_None,
		T("feature.scene_manager.transition_ease_tooltip",
			"Softens the start and end of time of day, location and weather transitions, sky included.\n"
			"0 keeps a constant-speed blend.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.transition_ease_reset", "Reset##TransitionEase"));
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
