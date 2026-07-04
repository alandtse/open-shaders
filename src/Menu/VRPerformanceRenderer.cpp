#include "VRPerformanceRenderer.h"

#include <imgui.h>

#include "Feature.h"
#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "menu.vr_performance."

void VRPerformanceRenderer::Render()
{
	ImGui::TextWrapped("%s", T(TKEY("intro"),
								 "VR performance settings from across all features, gathered in one place. "
								 "Each section is the same control shown in that feature's own panel — "
								 "changes here take effect there too. Settings marked as requiring a restart "
								 "latch at game launch."));
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
