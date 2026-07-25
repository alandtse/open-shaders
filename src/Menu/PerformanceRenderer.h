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
};
