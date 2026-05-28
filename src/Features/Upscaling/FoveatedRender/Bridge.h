#pragma once

// FoveatedRenderImpl::Bridge — single point of contact between the FoveatedRender
// subsystem and the rest of Community Shaders (Upscaling, Streamline).
//
// All "is FoveatedRender active?", "what settings should DLSS use?", and
// "what happened at boot?" questions are answered here, so consumers never
// need to #include FoveatedRender.h or poke globals::features::upscaling.foveatedRender
// directly.
//
// IMPORTANT: when the FoveatedRender route is inactive every query returns a
// neutral / identity value — callers must still check IsRouteActive() and
// fall back to their own settings when it returns false.

#include <cstdint>

namespace FoveatedRenderImpl::Bridge
{
	// True when VR + DLSS available + FoveatedRender enabled-at-boot.
	bool IsRouteActive();

	// Settings forwarding (live values from FoveatedRender GUI).
	uint32_t GetQualityMode();
	uint32_t GetPresetDLSS();
	float GetSharpnessDLSS();

	// Boot-time latches. Run once during BSShaderRenderTargets::Create.
	// Latches enable + qualityMode so settings cannot drift mid-frame.
	void BootSequence();

	// Compute motion-vector scale for Streamline constants.
	// Returns {1,1} when route is inactive or subrect is full-eye.
	void ComputeMvecScale(float& outX, float& outY);

	// Set/cleared by ExecuteVRDlssCore to indicate that the foveated subrect
	// execute path is actually running this frame. SetConstants checks this so
	// mvecScale correction is not applied to the standard full-frame DLSS path
	// (e.g. menus, frames where foveated is skipped).
	inline bool foveatedEvaluating = false;

	// Render-to-display scale for a quality mode index (1=Quality .. 4=UltraPerf).
	// Delegates to the FFX SDK ratio table.
	float GetRenderScaleForQuality(uint32_t qualityMode);

	// Quality mode latched at boot (resource sizing decisions consult this so
	// they don't shift mid-game when the user changes the live setting).
	uint32_t GetQualityModeAtBoot();
}
