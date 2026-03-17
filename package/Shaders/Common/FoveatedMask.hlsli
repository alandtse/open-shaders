#ifndef FOVEATED_MASK_HLSLI
#define FOVEATED_MASK_HLSLI

#ifndef FOVEATED_CENTER_AREA_MIN
#	define FOVEATED_CENTER_AREA_MIN 0.45
#endif
#ifndef FOVEATED_CENTER_AREA_MAX
#	define FOVEATED_CENTER_AREA_MAX 1.0
#endif
#ifndef FOVEATED_CENTER_FEATHER_MIN
#	define FOVEATED_CENTER_FEATHER_MIN 1e-4
#endif

float FoveatedClampCenterArea(float centerScale)
{
	return clamp(centerScale, FOVEATED_CENTER_AREA_MIN, FOVEATED_CENTER_AREA_MAX);
}

float2 FoveatedComputeCenterUV(float2 centerOffset)
{
	return clamp(float2(0.5, 0.5) + centerOffset, float2(0.0, 0.0), float2(1.0, 1.0));
}

float FoveatedComputeEllipseDistance(float2 eyeUv, float centerScale, float2 centerOffset)
{
	float clampedCenterScale = FoveatedClampCenterArea(centerScale);
	float radius = max(clampedCenterScale * 0.5, FOVEATED_CENTER_FEATHER_MIN);
	float2 centerUv = FoveatedComputeCenterUV(centerOffset);
	float2 normalized = (eyeUv - centerUv) / radius.xx;
	return length(normalized);
}

float FoveatedComputeEllipseDistance(float2 eyeUv, float centerScale)
{
	return FoveatedComputeEllipseDistance(eyeUv, centerScale, float2(0.0, 0.0));
}

float FoveatedComputeCenterBlendWeight(float2 eyeUv, float centerScale, float centerFeather, float2 centerOffset)
{
	float clampedCenterScale = FoveatedClampCenterArea(centerScale);
	float radius = max(clampedCenterScale * 0.5, FOVEATED_CENTER_FEATHER_MIN);
	float normalizedFeather = max(centerFeather, FOVEATED_CENTER_FEATHER_MIN) / radius;
	float ellipseDistance = FoveatedComputeEllipseDistance(eyeUv, centerScale, centerOffset);
	return 1.0 - smoothstep(1.0, 1.0 + normalizedFeather, ellipseDistance);
}

float FoveatedComputeCenterBlendWeight(float2 eyeUv, float centerScale, float centerFeather)
{
	return FoveatedComputeCenterBlendWeight(eyeUv, centerScale, centerFeather, float2(0.0, 0.0));
}

#endif
