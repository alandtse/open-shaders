#ifndef FOVEATED_SHADER_DETAIL_HLSLI
#define FOVEATED_SHADER_DETAIL_HLSLI

// Per-pixel detail weight for foveated effects: pass the mode (0=off/1=feathered/2=hard, see
// FoveatedCommon::GetShaderMode) + mask params, get a 0..1 weight (0 outside the mask = skip).

#include "Common/FoveatedMask.hlsli"

static const float FOVEATED_SHADER_DETAIL_MODE_FEATHERED = 1.0;
static const float FOVEATED_SHADER_DETAIL_MODE_HARD_CUTOFF = 2.0;

float FoveatedEvaluateShaderDetailWeight(float mode, float2 eyeUv, float centerScale, float centerFeather, float centerHorizontalScale, float2 centerOffset)
{
	bool detailModeEnabled = mode >= FOVEATED_SHADER_DETAIL_MODE_FEATHERED;
	if (!detailModeEnabled)
		return 1.0f;

	bool hardCutoffMode = mode >= FOVEATED_SHADER_DETAIL_MODE_HARD_CUTOFF;
	if (hardCutoffMode) {
		float edgeDistance = FoveatedComputeMaskDistance(eyeUv, centerScale, centerHorizontalScale, centerOffset);
		return edgeDistance > 1.0f ? 0.0f : 1.0f;
	}

	return FoveatedComputeCenterBlendWeight(eyeUv, centerScale, centerFeather, centerHorizontalScale, centerOffset);
}

bool FoveatedIsShaderDetailActive(float detailWeight)
{
	return detailWeight > 0.0001f;
}

/**
* @brief Combines one eye's foveation detail weight with the other eye's.
*
* Keeps both eyes on one decision for the same surface: the result is the lower of
* the two weights, so neither eye can run the detailed path where the other falls
* back. An invalid reprojection keeps this eye's own weight.
*
* @param[in] ownWeight   Detail weight of the current eye, [0,1].
* @param[in] otherWeight Detail weight of the other eye at the reprojected position, [0,1].
* @param[in] otherValid  True when otherWeight was reprojected validly.
* @return Combined detail weight, [0,1].
*/
float FoveatedCombineEyeWeights(float ownWeight, float otherWeight, bool otherValid)
{
	return otherValid ? min(ownWeight, otherWeight) : ownWeight;
}

#endif
