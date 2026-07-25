#include "PerformanceRenderer.h"

#include <algorithm>
#include <exception>
#include <imgui.h>
#include <vector>

#include "Feature.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "menu.performance."

// Section header: the feature name as a jump link to its panel, or a plain
// SeparatorText for the host (a link there would navigate to this very page).
static void DrawSectionHeader(Feature* feature, bool linkable)
{
	const std::string label = feature->GetPerformanceSectionLabel();
	if (label.empty())
		return;
	if (!linkable) {
		ImGui::SeparatorText(label.c_str());
		return;
	}
	if (ImGui::TextLink(label.c_str())) {
		if (auto* menu = Menu::GetSingleton())
			menu->SelectFeatureMenu(feature->GetShortName());
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("open_feature_tooltip"), "Open this feature's full settings panel"));
	ImGui::Separator();
}

void PerformanceRenderer::Render(Feature* host)
{
	ImGui::TextWrapped("%s", T(TKEY("intro"),
								 "Performance settings from across all features, gathered in one place. "
								 "Each section is the same control shown in that feature's own panel; "
								 "changes here take effect there too. Settings that need a restart take "
								 "effect at the next game launch."));
	ImGui::Spacing();

	// The active profile is the one every feature's settings currently match (else Custom).
	const Feature::PerfProfile profiles[3] = {
		Feature::PerfProfile::Performance, Feature::PerfProfile::Balanced, Feature::PerfProfile::Quality
	};
	int activeIdx = -1;
	for (int i = 0; i < IM_ARRAYSIZE(profiles) && activeIdx < 0; ++i) {
		bool all = true;
		for (Feature* f : Feature::GetFeatureList())
			if (f->loaded && (globals::game::isVR || !f->PerformanceSectionRequiresVR()) &&
				!f->MatchesPerformanceProfile(profiles[i])) {
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
			Feature::ApplyPerformanceProfileToAll(profiles[i]);
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

	// Drawn in perf-impact order (GetPerformanceOrder), not feature-registration order.
	// Skips features whose whole section is VR-only outside VR (PerformanceSectionRequiresVR)
	// so authors of VR-only sections don't need to gate their draw code manually.
	std::vector<Feature*> ordered;
	for (Feature* feature : Feature::GetFeatureList())
		if (feature->loaded && (globals::game::isVR || !feature->PerformanceSectionRequiresVR()))
			ordered.push_back(feature);
	// stable_sort keeps registration order among equal ranks (e.g. the default 1000) deterministic.
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const Feature* a, const Feature* b) { return a->GetPerformanceOrder() < b->GetPerformanceOrder(); });
	for (Feature* feature : ordered) {
		ImGui::PushID(feature);
		DrawSectionHeader(feature, feature != host);
		// Isolate each feature's draw so one throwing hook can't blank the rest of the page.
		try {
			feature->DrawPerformanceSettings();
		} catch (const std::exception& e) {
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s: draw error (%s)", feature->GetDisplayName().c_str(), e.what());
		} catch (...) {
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s: draw error (unknown)", feature->GetDisplayName().c_str());
		}
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX
