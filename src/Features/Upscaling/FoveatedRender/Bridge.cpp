#include "Bridge.h"

#include "../../../Globals.h"
#include "../../Upscaling.h"
#include "../FoveatedRender.h"

bool FoveatedRenderImpl::Bridge::IsRouteActive()
{
	// IsActive() already checks: enabledAtBoot && isVR
	//                            && GetUpscaleMethod() is kDLSS or kFSR (selected, not just available)
	return globals::features::upscaling.foveatedRender.IsActive();
}

void FoveatedRenderImpl::Bridge::BootSequence()
{
	auto& enhancer = globals::features::upscaling.foveatedRender;
	enhancer.LatchEnabled();
	enhancer.LatchQualityMode();
}

void FoveatedRenderImpl::Bridge::ComputeMvecScale(uint32_t eyeIndex, float& outX, float& outY)
{
	// Default: identity (caller's normal Streamline path).
	outX = 1.0f;
	outY = 1.0f;

	if (!IsRouteActive())
		return;

	auto& enhancer = globals::features::upscaling.foveatedRender;
	// Stereo Subrect: GetUV() == left-eye, GetRightEyeUV() == right-eye. Asymmetric
	// presets (e.g. Nasal Convergence) size the two eyes differently, so the scale must
	// be computed per-eye rather than always reading the left eye's UV.
	const auto& uv = (eyeIndex == 1) ? enhancer.subrectController.GetRightEyeUV() : enhancer.subrectController.GetUV();
	const bool isFullEye = uv.IsFullEye();

	if (isFullEye)
		return;

	// Default + Faster both use per-eye DLSS calls (not strip-merged), so
	// motion vectors scale by 1/UV.w on x.
	outX = (uv.w > 0.0f) ? (1.0f / uv.w) : 1.0f;
	outY = (uv.h > 0.0f) ? (1.0f / uv.h) : 1.0f;
}
