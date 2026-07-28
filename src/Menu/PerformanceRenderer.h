#pragma once

/**
 * @brief Renders the "Performance" page: a central place that aggregates every
 * feature's performance controls (upscaling, shadow budgets, foveation, stereo
 * reprojection, and culling) so users can manage performance in one place.
 *
 * The page owns no settings state. It iterates loaded features and calls
 * Feature::DrawPerformanceSettings() on each; controls bind directly to feature settings.
 * Intelligently includes VR-specific sections when running in VR mode.
 */
struct Feature;

class PerformanceRenderer
{
public:
	/**
	 * @brief Draws the central Performance page.
	 * @param host The feature whose panel embeds the page (if any); its section header
	 *             is not drawn as a jump link to prevent redundant navigation.
	 */
	static void Render(Feature* host = nullptr);

	/**
	 * @brief Draws an indented, clickable heading for a named sub-part of a feature's
	 * hub section (e.g. LightLimitFix's "Shadow Limit Fix" impact-cull controls, which
	 * are named differently from the feature's own display name). Unlike the top-level
	 * section header, this draws no full-width divider, so it reads as nested under the
	 * feature rather than as a sibling top-level section; the link still navigates to
	 * the same feature panel. Caller must pair with ImGui::Unindent() once its
	 * subsection content is drawn.
	 * @param label The subsection's display name (e.g. "Shadow Limit Fix").
	 * @param feature The feature to navigate to when the link is clicked.
	 * @param sectionAnchor Optional anchor id (see Menu::SelectFeatureMenu) the
	 *        feature's own DrawSettings can check to scroll to this subsection.
	 */
	static void DrawSubsectionLink(const char* label, Feature* feature, const char* sectionAnchor = "");
};
