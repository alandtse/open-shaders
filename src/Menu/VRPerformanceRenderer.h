#pragma once

/**
 * @brief Renders the "VR Performance" page: a single place that aggregates every
 * feature's VR performance controls (upscaling/PerfMode, foveation, stereo
 * reprojection, culling) so users can find them without visiting each feature panel.
 *
 * The page owns no settings state. It loops the loaded features and calls
 * Feature::DrawVRPerformanceSettings() on each; the controls are the same ones
 * (bound to the same settings) each feature draws in its own panel. A feature that
 * does not override the hook contributes nothing (fail-safe; no registry to maintain).
 */
struct Feature;

class VRPerformanceRenderer
{
public:
	/** @brief Draws the VR Performance page. VR-only; caller gates on isVR.
	 *  @param host The feature whose panel embeds the page; its section header is not
	 *              drawn as a jump link (it would navigate to the page itself). */
	static void Render(Feature* host);
};
