#ifndef FOVEATED_MASK_HLSLI
#define FOVEATED_MASK_HLSLI

// Superellipse (shape power 4 = squircle) foveation mask: a smooth center->periphery weight that
// avoids the hard seam of a rect mask. CPU mirror of the clamping is in src/Features/FoveatedCommon.h.

#ifndef FOVEATED_CENTER_SCALE_MIN
#	define FOVEATED_CENTER_SCALE_MIN 0.25
#endif
#ifndef FOVEATED_CENTER_SCALE_MAX
#	define FOVEATED_CENTER_SCALE_MAX 1.0
#endif
#ifndef FOVEATED_CENTER_FEATHER_MIN
#	define FOVEATED_CENTER_FEATHER_MIN 1e-4
#endif
#ifndef FOVEATED_CENTER_HORIZONTAL_SCALE_MIN
#	define FOVEATED_CENTER_HORIZONTAL_SCALE_MIN 1.0
#endif
#ifndef FOVEATED_CENTER_HORIZONTAL_SCALE_MAX
#	define FOVEATED_CENTER_HORIZONTAL_SCALE_MAX 2.0
#endif
#ifndef FOVEATED_MASK_SHAPE_POWER
#	define FOVEATED_MASK_SHAPE_POWER 4
#endif

float FoveatedClampCenterScale(float centerScale)
{
	return clamp(centerScale, FOVEATED_CENTER_SCALE_MIN, FOVEATED_CENTER_SCALE_MAX);
}

float2 FoveatedComputeCenterUV(float2 centerOffset)
{
	return clamp(float2(0.5, 0.5) + centerOffset, float2(0.0, 0.0), float2(1.0, 1.0));
}

float FoveatedClampCenterHorizontalScale(float centerHorizontalScale)
{
	return clamp(centerHorizontalScale, FOVEATED_CENTER_HORIZONTAL_SCALE_MIN, FOVEATED_CENTER_HORIZONTAL_SCALE_MAX);
}

float2 FoveatedComputeMaskRadii(float centerScale, float centerHorizontalScale)
{
	float clampedCenterScale = FoveatedClampCenterScale(centerScale);
	float clampedCenterHorizontalScale = FoveatedClampCenterHorizontalScale(centerHorizontalScale);
	float2 radii = float2(clampedCenterScale * clampedCenterHorizontalScale * 0.5, clampedCenterScale * 0.5);
	return max(radii, FOVEATED_CENTER_FEATHER_MIN.xx);
}

float FoveatedComputeNormalizedFeather(float centerScale, float centerFeather, float centerHorizontalScale)
{
	float2 radii = FoveatedComputeMaskRadii(centerScale, centerHorizontalScale);
	float baseRadius = max(min(radii.x, radii.y), FOVEATED_CENTER_FEATHER_MIN);
	return max(centerFeather, FOVEATED_CENTER_FEATHER_MIN) / baseRadius;
}

float FoveatedComputeMaskDistance(float2 eyeUv, float centerScale, float centerHorizontalScale, float2 centerOffset)
{
	float2 radii = FoveatedComputeMaskRadii(centerScale, centerHorizontalScale);
	float2 centerUv = FoveatedComputeCenterUV(centerOffset);
	float2 normalized = (eyeUv - centerUv) / radii;
	float2 absoluteNormalized = abs(normalized);
#if FOVEATED_MASK_SHAPE_POWER == 4
	// Squircle fast path: pow(t,4) == (t*t)^2 and pow(sum,1/4) == sqrt(sqrt(sum)),
	// so the default shape avoids three transcendental pow() calls per pixel in
	// the SSR path that is explicitly trying to save GPU time.
	float2 sq = absoluteNormalized * absoluteNormalized;
	float pNorm = sq.x * sq.x + sq.y * sq.y;
	return sqrt(sqrt(max(pNorm, 0.0)));
#else
	float shapePower = max((float)FOVEATED_MASK_SHAPE_POWER, 1.0);
	float invShapePower = 1.0 / shapePower;
	float pNorm = pow(absoluteNormalized.x, shapePower) + pow(absoluteNormalized.y, shapePower);
	return pow(max(pNorm, 0.0), invShapePower);
#endif
}

float FoveatedComputeCenterBlendWeight(float2 eyeUv, float centerScale, float centerFeather, float centerHorizontalScale, float2 centerOffset)
{
	float normalizedFeather = FoveatedComputeNormalizedFeather(centerScale, centerFeather, centerHorizontalScale);
	float maskDistance = FoveatedComputeMaskDistance(eyeUv, centerScale, centerHorizontalScale, centerOffset);
	return 1.0 - smoothstep(1.0, 1.0 + normalizedFeather, maskDistance);
}

#endif
