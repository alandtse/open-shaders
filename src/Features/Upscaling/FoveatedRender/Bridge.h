#pragma once

// FoveatedRenderImpl::Bridge — single point of contact between the FoveatedRender
// subsystem and the rest of Community Shaders (Upscaling, Streamline).
//
// Consumers read FoveatedRender settings directly from
// globals::features::upscaling.foveatedRender; Bridge exposes only the
// cross-cutting concerns that require route-aware logic (active check, boot
// sequence, mvec scale, execute-frame flag).

#include <cstdint>

namespace FoveatedRenderImpl::Bridge
{
	// True when VR + DLSS available + FoveatedRender enabled-at-boot.
	bool IsRouteActive();

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
}
