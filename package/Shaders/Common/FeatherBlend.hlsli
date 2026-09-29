#ifndef FEATHER_BLEND_HLSLI
#define FEATHER_BLEND_HLSLI

namespace FeatherBlend
{
	// curve below 1 widens the outer part of the band, above 1 keeps it near the edge.
	float ShapeRamp(float normalizedDistance, float curve)
	{
		return pow(saturate(normalizedDistance), curve);
	}

	// Zero slope at both ends, so a cropped region meets its surroundings without a visible seam.
	float SmoothRamp(float ramp)
	{
		return ramp * ramp * (3.0 - 2.0 * ramp);
	}
}

#endif
