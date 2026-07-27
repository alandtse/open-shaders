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

// Shared by the top-level section header and DrawSubsectionLink: a clickable link to
// a feature's own panel, with the standard "open this feature" tooltip.
static void DrawFeatureLink(const char* label, Feature* feature)
{
	if (ImGui::TextLink(label)) {
		if (auto* menu = Menu::GetSingleton())
			menu->SelectFeatureMenu(feature->GetShortName());
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

void PerformanceRenderer::DrawSubsectionLink(const char* label, Feature* feature)
{
	DrawFeatureLink(label, feature);
	ImGui::Indent();
}

// Draws a Performance/Balanced/Quality button row, highlighting whichever index is
// active (-1 = none, i.e. Custom), invoking apply(profile) on click. Shared by the
// global broadcast row and each per-feature row so both look and behave identically.
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

	// Features whose whole section is VR-only outside VR (PerformanceSectionRequiresVR)
	// are skipped everywhere below: active-profile detection, the restart banner, and
	// the per-feature list itself, so authors of VR-only sections don't need to gate
	// their draw code manually. A non-empty section label is what "opted into the hub"
	// means; without this check every loaded feature (including ones with no hub
	// content at all) would get an empty header-less "Advanced" node.
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
	// Foveation/reprojection are VR-only (their sections are hidden on Flat via
	// PerformanceSectionRequiresVR), so don't claim they're affected there. All
	// three set Upscaling's qualityMode, which is restart-gated whenever PerfMode
	// is engaged (the normal case for any DLSS/FSR user) -- so every tooltip
	// carries the same restart caveat, not just Quality's.
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
		// Uniform, actionable preset row for every section, computed here rather than
		// left to each feature to hand-roll: applies the profile to just THIS feature
		// (unlike the global row above, which broadcasts to all of them), so users can
		// nudge one section without disturbing the rest. Features with nothing else to
		// show (VR, ScreenSpaceGI, ScreenSpaceShadows) still get visible, working
		// controls instead of an empty gap before Advanced.
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
