#include "VRPerformanceRenderer.h"

#include <algorithm>
#include <imgui.h>
#include <vector>

#include "Feature.h"
#include "I18n/I18n.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "menu.vr_performance."

void VRPerformanceRenderer::Render()
{
	ImGui::TextWrapped("%s", T(TKEY("intro"),
								 "VR performance settings from across all features, gathered in one place. "
								 "Each section is the same control shown in that feature's own panel — "
								 "changes here take effect there too. Settings that need a restart take "
								 "effect at the next game launch."));
	ImGui::Spacing();

	// Profiles: one click sets the whole VR perf stack coherently across features. The active
	// profile is the one every feature's settings currently match (else Custom), so the buttons
	// show state instead of looking stateless.
	const Feature::VRPerfProfile profiles[3] = {
		Feature::VRPerfProfile::Performance, Feature::VRPerfProfile::Balanced, Feature::VRPerfProfile::Quality
	};
	int activeIdx = -1;
	for (int i = 0; i < IM_ARRAYSIZE(profiles) && activeIdx < 0; ++i) {
		bool all = true;
		for (Feature* f : Feature::GetFeatureList())
			if (f->loaded && !f->MatchesVRPerformanceProfile(profiles[i])) {
				all = false;
				break;
			}
		if (all)
			activeIdx = i;
	}

	const char* labels[3] = {
		T(TKEY("profile_performance"), "Performance"),
		T(TKEY("profile_balanced"), "Balanced"),
		T(TKEY("profile_quality"), "Quality")
	};
	const char* tooltips[3] = {
		T(TKEY("profile_performance_tooltip"), "Lowest render resolution; foveation and reprojection on. Fastest."),
		T(TKEY("profile_balanced_tooltip"), "Mid render resolution; reprojection on."),
		T(TKEY("profile_quality_tooltip"), "Higher render resolution; reprojection off for max fidelity. Some changes apply on restart.")
	};
	ImGui::TextUnformatted(T(TKEY("profiles_label"), "Profile:"));
	for (int i = 0; i < IM_ARRAYSIZE(profiles); ++i) {
		ImGui::SameLine();
		const bool active = i == activeIdx;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(labels[i]))
			Feature::ApplyVRPerformanceProfileToAll(profiles[i]);
		if (active)
			ImGui::PopStyleColor();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", tooltips[i]);
	}
	if (activeIdx < 0) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", T(TKEY("profile_custom"), "(Custom)"));
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Loaded features contribute their VR perf controls via the shared hook, drawn in
	// perf-impact order (GetVRPerformanceOrder), not feature-registration order. Features
	// without VR perf knobs draw nothing, so the page shows only what is relevant.
	std::vector<Feature*> ordered;
	for (Feature* feature : Feature::GetFeatureList())
		if (feature->loaded)
			ordered.push_back(feature);
	std::sort(ordered.begin(), ordered.end(),
		[](const Feature* a, const Feature* b) { return a->GetVRPerformanceOrder() < b->GetVRPerformanceOrder(); });
	for (Feature* feature : ordered) {
		ImGui::PushID(feature);
		feature->DrawVRPerformanceSettings();
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX
