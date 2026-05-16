#include "Bridge.h"

#include "../../Globals.h"
#include "../DlssEnhancerFeature.h"

bool DlssEnhancer::Bridge::IsRouteActive()
{
	// IsActive() already checks: globals::game::isVR
	//                            && globals::features::upscaling.streamline.featureDLSS
	//                            && enabledAtBoot
	return globals::features::dlssEnhancer.IsActive();
}

uint32_t DlssEnhancer::Bridge::GetQualityMode()
{
	return globals::features::dlssEnhancer.settings.qualityMode;
}

uint32_t DlssEnhancer::Bridge::GetPresetDLSS()
{
	return globals::features::dlssEnhancer.settings.presetDLSS;
}

float DlssEnhancer::Bridge::GetSharpnessDLSS()
{
	return globals::features::dlssEnhancer.settings.sharpnessDLSS;
}

void DlssEnhancer::Bridge::BootSequence()
{
	auto& enhancer = globals::features::dlssEnhancer;
	enhancer.LatchEnabled();
	enhancer.LatchQualityMode();
}

void DlssEnhancer::Bridge::ComputeMvecScale(float& outX, float& outY)
{
	// Default: identity (caller's normal Streamline path).
	outX = 1.0f;
	outY = 1.0f;

	if (!IsRouteActive())
		return;

	auto& enhancer = globals::features::dlssEnhancer;
	const auto mode = enhancer.GetDlssMode();
	const auto& uv = enhancer.subrectController.GetUV();  // PR-1 stereo Subrect: GetUV() == left-eye in stereo mode
	const bool isFullEye = (uv.w >= 0.999f && uv.h >= 0.999f);

	if (isFullEye)
		return;

	// Extreme mode merges both eyes into one strip texture of width
	// 2*subOutW; motion vectors authored at full-eye resolution must shrink
	// by 1/(2·w) on x to land in strip space. Default and Faster keep their
	// per-eye texture sets, so x-scale is 1/UV.w.
	if (mode == DlssEnhancerFeature::DlssMode::kExtreme) {
		outX = (uv.w > 0.0f) ? (1.0f / (2.0f * uv.w)) : 1.0f;
	} else {
		outX = (uv.w > 0.0f) ? (1.0f / uv.w) : 1.0f;
	}
	outY = (uv.h > 0.0f) ? (1.0f / uv.h) : 1.0f;
}

float DlssEnhancer::Bridge::GetRenderScaleForQuality(uint32_t qualityMode)
{
	return DlssEnhancerFeature::GetRenderScaleForQuality(qualityMode);
}

uint32_t DlssEnhancer::Bridge::GetQualityModeAtBoot()
{
	return globals::features::dlssEnhancer.GetQualityModeAtBoot();
}
