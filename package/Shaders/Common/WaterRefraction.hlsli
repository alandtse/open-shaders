#ifndef __WATER_REFRACTION_HLSLI__
#define __WATER_REFRACTION_HLSLI__

#include "Common/VR.hlsli"
#include "Common/WaterFinite.hlsli"

namespace WaterRefraction
{

	/** Finds UV bounds whose depth and colour footprints stay in the selected eye. */
	bool TryGetUVBounds(float2 refractionRenderSize, float2 depthRenderSize, uint eyeIndex, out float2 minUV, out float2 maxUV)
	{
		minUV = maxUV = 0.0;
#ifdef VR
		const float2 minimumSize = float2(2, 1);
#else
		const float2 minimumSize = float2(1, 1);
#endif
		if (!all(WaterFinite::IsFinite(refractionRenderSize)) || !all(WaterFinite::IsFinite(depthRenderSize)) ||
			any(refractionRenderSize < minimumSize) || any(depthRenderSize < minimumSize))
			return false;

		float2 eyeMinUV = Stereo::ConvertToStereoUV(float2(0, 0), eyeIndex);
		float2 eyeMaxUV = Stereo::ConvertToStereoUV(float2(1, 1), eyeIndex);
		// Flooring the boundaries preserves physical texel centres at fractional render scales.
		minUV = max(
			(floor(eyeMinUV * refractionRenderSize) + 0.5) / refractionRenderSize,
			(floor(eyeMinUV * depthRenderSize) + 0.5) / depthRenderSize);
		maxUV = min(
			(floor(eyeMaxUV * refractionRenderSize) - 0.5) / refractionRenderSize,
			(floor(eyeMaxUV * depthRenderSize) - 0.5) / depthRenderSize);
		return all(minUV <= maxUV);
	}

	/** Uses the undistorted position when projection fails, then bounds the sample footprint. */
	float2 ClampUV(float2 uv, float2 fallbackUV, float2 minUV, float2 maxUV)
	{
		if (!all(WaterFinite::IsFinite(uv)))
			uv = all(WaterFinite::IsFinite(fallbackUV)) ? fallbackUV : (minUV + maxUV) * 0.5;
		return clamp(uv, minUV, maxUV);
	}
}

#endif
