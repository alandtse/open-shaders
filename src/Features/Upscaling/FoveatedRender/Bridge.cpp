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
	const auto mode = enhancer.GetDlssMode();
	const auto& uv = enhancer.subrectController.GetUV();  // PR-1 stereo Subrect: GetUV() == left-eye in stereo mode
	const bool isFullEye = (uv.w >= 0.999f && uv.h >= 0.999f);

	if (isFullEye)
		return;

	// Extreme mode merges both eyes into one strip of width 2*subOutW;
	// motion vectors authored at full-eye resolution must shrink by 1/(2·w)
	// on x to land in strip space. Default and Faster keep per-eye sets, so
	// x-scale is 1/UV.w.
	if (mode == FoveatedRender::DlssMode::kExtreme) {
		outX = (uv.w > 0.0f) ? (1.0f / (2.0f * uv.w)) : 1.0f;
	} else {
		outX = (uv.w > 0.0f) ? (1.0f / uv.w) : 1.0f;
	}
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
