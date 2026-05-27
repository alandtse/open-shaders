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

// Bridge.h contract: when the route is inactive, getters return a neutral /
// identity value so callers that forget to check IsRouteActive() don't
// silently pick up FoveatedRender values.

uint32_t FoveatedRenderImpl::Bridge::GetQualityMode()
{
	if (!IsRouteActive())
		return 0u;
	return globals::features::upscaling.foveatedRender.GetActiveQualityMode();
}

uint32_t FoveatedRenderImpl::Bridge::GetPresetDLSS()
{
	if (!IsRouteActive())
		return 0u;
	return globals::features::upscaling.foveatedRender.GetActivePresetDLSS();
}

float FoveatedRenderImpl::Bridge::GetSharpnessDLSS()
{
	if (!IsRouteActive())
		return 0.0f;
	return globals::features::upscaling.foveatedRender.GetActiveSharpnessDLSS();
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

float FoveatedRenderImpl::Bridge::GetRenderScaleForQuality(uint32_t qualityMode)
{
	return FoveatedRender::GetRenderScaleForQuality(qualityMode);
}

uint32_t FoveatedRenderImpl::Bridge::GetQualityModeAtBoot()
{
	if (!IsRouteActive())
		return 0u;
	return globals::features::upscaling.foveatedRender.GetQualityModeAtBoot();
}
