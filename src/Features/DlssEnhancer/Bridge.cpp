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

float DlssEnhancer::Bridge::GetRenderScaleForQuality(uint32_t qualityMode)
{
	return DlssEnhancerFeature::GetRenderScaleForQuality(qualityMode);
}

uint32_t DlssEnhancer::Bridge::GetQualityModeAtBoot()
{
	return globals::features::dlssEnhancer.GetQualityModeAtBoot();
}
