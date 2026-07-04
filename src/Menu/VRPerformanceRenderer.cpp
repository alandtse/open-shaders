#include "VRPerformanceRenderer.h"

#include <imgui.h>

#include "Feature.h"
#include "I18n/I18n.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "menu.vr_performance."

// Broadcasts one profile to every loaded feature; each maps it to its own settings.
static void ApplyProfile(Feature::VRPerfProfile profile)
{
	for (Feature* feature : Feature::GetFeatureList())
		if (feature->loaded)
			feature->ApplyVRPerformanceProfile(profile);
}

void VRPerformanceRenderer::Render()
{
	ImGui::TextWrapped("%s", T(TKEY("intro"),
								 "VR performance settings from across all features, gathered in one place. "
								 "Each section is the same control shown in that feature's own panel — "
								 "changes here take effect there too. Settings marked as requiring a restart "
								 "latch at game launch."));
	ImGui::Spacing();

	// Profiles: one click sets the whole VR perf stack (upscaler, foveation, reprojection)
	// coherently across features. The sections below tune from there.
	ImGui::TextUnformatted(T(TKEY("profiles_label"), "Profile:"));
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("profile_performance"), "Performance")))
		ApplyProfile(Feature::VRPerfProfile::Performance);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("profile_performance_tooltip"), "Lowest render resolution; foveation and reprojection on. Fastest."));
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("profile_balanced"), "Balanced")))
		ApplyProfile(Feature::VRPerfProfile::Balanced);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("profile_balanced_tooltip"), "Mid render resolution; reprojection on."));
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("profile_quality"), "Quality")))
		ApplyProfile(Feature::VRPerfProfile::Quality);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("profile_quality_tooltip"), "Higher render resolution; reprojection off for max fidelity. Some changes apply on restart."));

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Loaded features contribute their VR perf controls via the shared hook. Features
	// without VR perf knobs draw nothing, so the page shows only what is relevant.
	for (Feature* feature : Feature::GetFeatureList()) {
		if (!feature->loaded)
			continue;
		ImGui::PushID(feature);
		feature->DrawVRPerformanceSettings();
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX
