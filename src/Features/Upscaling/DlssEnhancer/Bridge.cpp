#include "Bridge.h"

#include "../../../Globals.h"
#include "../../Upscaling.h"
#include "../DlssEnhancer.h"

bool DlssEnhancerImpl::Bridge::IsRouteActive()
{
	// IsActive() already checks: globals::game::isVR
	//                            && globals::features::upscaling.streamline.featureDLSS
	//                            && enabledAtBoot
	return globals::features::upscaling.dlssEnhancer.IsActive();
}

uint32_t DlssEnhancerImpl::Bridge::GetQualityMode()
{
	return globals::features::upscaling.settings.qualityMode;
}

uint32_t DlssEnhancerImpl::Bridge::GetPresetDLSS()
{
	return globals::features::upscaling.settings.presetDLSS;
}

float DlssEnhancerImpl::Bridge::GetSharpnessDLSS()
{
	return globals::features::upscaling.settings.sharpnessDLSS;
}

void DlssEnhancerImpl::Bridge::BootSequence()
{
	auto& enhancer = globals::features::upscaling.dlssEnhancer;
	enhancer.LatchEnabled();
	enhancer.LatchQualityMode();
}

void DlssEnhancerImpl::Bridge::ComputeMvecScale(float& outX, float& outY)
{
	// Default: identity (caller's normal Streamline path).
	outX = 1.0f;
	outY = 1.0f;

	if (!IsRouteActive())
		return;

	auto& enhancer = globals::features::upscaling.dlssEnhancer;
	const auto& uv = enhancer.subrectController.GetUV();  // PR-1 stereo Subrect: GetUV() == left-eye in stereo mode
	const bool isFullEye = (uv.w >= 0.999f && uv.h >= 0.999f);

	if (isFullEye)
		return;

	// MVP-B has only Default + Faster modes. Both use per-eye DLSS calls
	// (not strip-merged), so motion vectors scale by 1/UV.w on x. Extreme
	// mode's 1/(2·w) factor is deferred to PR-3b along with Modes::Extreme.
	outX = (uv.w > 0.0f) ? (1.0f / uv.w) : 1.0f;
	outY = (uv.h > 0.0f) ? (1.0f / uv.h) : 1.0f;
}

float DlssEnhancerImpl::Bridge::GetRenderScaleForQuality(uint32_t qualityMode)
{
	return DlssEnhancer::GetRenderScaleForQuality(qualityMode);
}

uint32_t DlssEnhancerImpl::Bridge::GetQualityModeAtBoot()
{
	return globals::features::upscaling.dlssEnhancer.GetQualityModeAtBoot();
}
