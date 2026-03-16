#ifndef FOVEATED_MASK_HLSLI
#define FOVEATED_MASK_HLSLI

#ifndef FOVEATED_CENTER_AREA_MIN
#	define FOVEATED_CENTER_AREA_MIN 0.25
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

float FoveatedComputeCenterBlendWeight(float2 eyeUv, float centerScale, float centerFeather)
{
	float halfSize = saturate(centerScale) * 0.5;
	float2 outside = abs(eyeUv - 0.5) - halfSize.xx;
	float distanceOutside = max(outside.x, outside.y);
	return 1.0 - smoothstep(0.0, max(centerFeather, FOVEATED_CENTER_FEATHER_MIN), distanceOutside);
}

#endif
