#include "PerformanceRenderer.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <imgui.h>
#include <vector>

#include "Feature.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "menu.performance."

// Shared by DrawSectionHeader and DrawSubsectionLink for a consistent link + tooltip.
static void DrawFeatureLink(const char* label, Feature* feature, const char* sectionAnchor = "")
{
	if (ImGui::TextLink(label)) {
		if (auto* menu = Menu::GetSingleton())
			menu->SelectFeatureMenu(feature->GetShortName(), sectionAnchor);
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("open_feature_tooltip"), "Open this feature's full settings panel"));
}

// Section header: the feature name as a jump link to its panel, or a plain
// SeparatorText for the host (a link there would navigate to this very page).
// Draws no trailing divider of its own -- the caller closes out each section
// with a separator AFTER its content, so dividers read as "end of this
// feature's block" rather than appearing to split the link from its own body.
static void DrawSectionHeader(Feature* feature, bool linkable)
{
	const std::string label = feature->GetPerformanceSectionLabel();
	if (label.empty())
		return;
	if (!linkable) {
		ImGui::SeparatorText(label.c_str());
		return;
	}
	DrawFeatureLink(label.c_str(), feature);
}

void PerformanceRenderer::DrawSubsectionLink(const char* label, Feature* feature, const char* sectionAnchor)
{
	// Indent BEFORE drawing the link, not after -- otherwise only content following
	// the link (often nothing) ends up indented, and the link itself, the one thing
	// meant to visually nest, stays flush with the top-level section header above it.
	ImGui::Indent();
	DrawFeatureLink(label, feature, sectionAnchor);
}

// Draws a Performance/Balanced/Quality button row, highlighting the active index
// (-1 = Custom). Shared by the global and per-feature rows so both stay identical.
static void DrawProfileButtonRow(const Feature::PerfProfile (&profiles)[3], const char* const (&labels)[3],
	const char* const (&tooltips)[3], int activeIdx, const std::function<void(Feature::PerfProfile)>& apply)
{
	for (int i = 0; i < IM_ARRAYSIZE(profiles); ++i) {
		if (i > 0)
			ImGui::SameLine();
		const bool active = i == activeIdx;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(labels[i]))
			apply(profiles[i]);
		if (active)
			ImGui::PopStyleColor();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", tooltips[i]);
	}
	if (activeIdx < 0) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", T(TKEY("profile_custom"), "(Custom)"));
	}
}

void PerformanceRenderer::Render(Feature* host)
{
	ImGui::TextWrapped("%s", T(TKEY("intro"),
								 "Performance settings from across all features, gathered in one place. "
								 "Each section is the same control shown in that feature's own panel; "
								 "changes here take effect there too. Settings that need a restart take "
								 "effect at the next game launch."));
	ImGui::Spacing();

	// VR-only sections are skipped everywhere below (PerformanceSectionRequiresVR), and a
	// feature needs a non-empty section label to opt in at all -- else it'd get an empty node.
	std::vector<Feature*> ordered;
	for (Feature* feature : Feature::GetFeatureList())
		if (feature->loaded && (globals::game::isVR || !feature->PerformanceSectionRequiresVR()) &&
			!feature->GetPerformanceSectionLabel().empty())
			ordered.push_back(feature);
	// stable_sort keeps registration order among equal ranks (e.g. the default 1000) deterministic.
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const Feature* a, const Feature* b) { return a->GetPerformanceOrder() < b->GetPerformanceOrder(); });

	// The active profile is the one every feature's settings currently match (else Custom).
	const Feature::PerfProfile profiles[3] = {
		Feature::PerfProfile::Performance, Feature::PerfProfile::Balanced, Feature::PerfProfile::Quality
	};
	int activeIdx = -1;
	for (int i = 0; i < IM_ARRAYSIZE(profiles) && activeIdx < 0; ++i) {
		bool all = true;
		for (Feature* f : ordered)
			if (!f->MatchesPerformanceProfile(profiles[i])) {
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
	// Foveation/reprojection tooltips only apply in VR. All three set Upscaling's
	// restart-gated qualityMode, so every tooltip carries that caveat, not just Quality's.
	const char* tooltips[3] = {
		globals::game::isVR ? T(TKEY("profile_performance_tooltip"), "Lowest render resolution; foveation and reprojection on. Fastest. Some changes apply on restart.") : T(TKEY("profile_performance_tooltip_flat"), "Lowest render resolution. Fastest. Some changes apply on restart."),
		globals::game::isVR ? T(TKEY("profile_balanced_tooltip"), "Mid render resolution; reprojection on. Some changes apply on restart.") : T(TKEY("profile_balanced_tooltip_flat"), "Mid render resolution. Some changes apply on restart."),
		globals::game::isVR ? T(TKEY("profile_quality_tooltip"), "Higher render resolution; reprojection off for max fidelity. Some changes apply on restart.") : T(TKEY("profile_quality_tooltip_flat"), "Higher render resolution for max fidelity. Some changes apply on restart.")
	};
	ImGui::TextUnformatted(T(TKEY("profiles_label"), "Profile:"));
	ImGui::SameLine();
	DrawProfileButtonRow(profiles, labels, tooltips, activeIdx,
		[](Feature::PerfProfile p) { Feature::ApplyPerformanceProfileToAll(p); });

	// Generic per-section tooltips: unlike the global row above, a section's row can't
	// claim specifics (render resolution, foveation) that only apply to SOME features.
	const char* sectionTooltips[3] = {
		T(TKEY("profile_performance_section_tooltip"), "Apply this section's Performance-tier settings only."),
		T(TKEY("profile_balanced_section_tooltip"), "Apply this section's Balanced-tier settings only."),
		T(TKEY("profile_quality_section_tooltip"), "Apply this section's Quality-tier settings only.")
	};

	// Surface the restart need here, not just inside each feature's collapsed section --
	// otherwise clicking a preset with a restart-gated field looks like it did nothing.
	bool anyPendingRestart = false;
	for (Feature* f : ordered)
		if (f->HasAnyPendingRestart()) {
			anyPendingRestart = true;
			break;
		}
	if (anyPendingRestart)
		Util::Text::RestartNeeded("%s", T(TKEY("pending_restart_banner"), "Some changes below need a restart to take effect."));

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Drawn in perf-impact order (GetPerformanceOrder), set up in `ordered` above.
	for (Feature* feature : ordered) {
		ImGui::PushID(feature);
		DrawSectionHeader(feature, feature != host);
		// Uniform, actionable preset row for every section: applies the profile to just
		// THIS feature, unlike the global broadcast row above.
		int featureActiveIdx = -1;
		for (int i = 0; i < IM_ARRAYSIZE(profiles) && featureActiveIdx < 0; ++i)
			if (feature->MatchesPerformanceProfile(profiles[i]))
				featureActiveIdx = i;
		DrawProfileButtonRow(profiles, labels, sectionTooltips, featureActiveIdx,
			[feature](Feature::PerfProfile p) { feature->ApplyPerformanceProfile(p); });
		// Presets stay visible: they're the primary surface, same as the global
		// buttons above. Only raw sliders/knobs collapse into Advanced below.
		try {
			feature->DrawPerformancePresets();
		} catch (const std::exception& e) {
			logger::error("PerformanceRenderer: {} presets threw: {}", feature->GetShortName(), e.what());
			Util::Text::WrappedError("%s: draw error (%s)", feature->GetDisplayName().c_str(), e.what());
		} catch (...) {
			logger::error("PerformanceRenderer: {} presets threw (unknown)", feature->GetShortName());
			Util::Text::WrappedError("%s: draw error (unknown)", feature->GetDisplayName().c_str());
		}
		// Collapsed by default: for verifying what a preset changed or fine-tuning past it.
		if (ImGui::TreeNodeEx(T(TKEY("section_advanced"), "Advanced"), ImGuiTreeNodeFlags_None)) {
			// Isolate each feature's draw so one throwing hook can't blank the rest of the page.
			// Logged, not just shown inline, so a reported red box is diagnosable from the log.
			try {
				feature->DrawPerformanceSettings();
			} catch (const std::exception& e) {
				logger::error("PerformanceRenderer: {} threw: {}", feature->GetShortName(), e.what());
				Util::Text::WrappedError("%s: draw error (%s)", feature->GetDisplayName().c_str(), e.what());
			} catch (...) {
				logger::error("PerformanceRenderer: {} threw (unknown)", feature->GetShortName());
				Util::Text::WrappedError("%s: draw error (unknown)", feature->GetDisplayName().c_str());
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
		// Closes out this feature's block, not the header above -- keeps the divider
		// meaning consistent ("end of a section") no matter how much content it drew.
		ImGui::Separator();
		ImGui::Spacing();
	}
}

#undef I18N_KEY_PREFIX
