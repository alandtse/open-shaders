#include "Bridge.h"

#include "../../../Globals.h"
#include "../../Upscaling.h"
#include "../FoveatedRender.h"

bool FoveatedRenderImpl::Bridge::IsRouteActive()
{
	// IsActive() already checks: globals::game::isVR
	//                            && globals::features::upscaling.streamline.featureDLSS
	//                            && enabledAtBoot
	return globals::features::upscaling.foveatedRender.IsActive();
}

void FoveatedRenderImpl::Bridge::BootSequence()
{
	auto& enhancer = globals::features::upscaling.foveatedRender;
	enhancer.LatchEnabled();
	enhancer.LatchQualityMode();
}

void FoveatedRenderImpl::Bridge::ComputeMvecScale(float& outX, float& outY)
{
	// Default: identity (caller's normal Streamline path).
	outX = 1.0f;
	outY = 1.0f;

	if (!IsRouteActive())
		return;

	auto& enhancer = globals::features::upscaling.foveatedRender;
	const auto& uv = enhancer.subrectController.GetUV();  // PR-1 stereo Subrect: GetUV() == left-eye in stereo mode
	const bool isFullEye = (uv.w >= 0.999f && uv.h >= 0.999f);

	if (isFullEye)
		return;

	// Default + Faster both use per-eye DLSS calls (not strip-merged), so
	// motion vectors scale by 1/UV.w on x.
	outX = (uv.w > 0.0f) ? (1.0f / uv.w) : 1.0f;
	outY = (uv.h > 0.0f) ? (1.0f / uv.h) : 1.0f;
}
