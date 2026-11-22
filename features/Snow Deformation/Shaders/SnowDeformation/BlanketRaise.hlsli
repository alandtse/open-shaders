#ifndef __SNOW_DEFORMATION_BLANKET_RAISE_HLSLI__
#define __SNOW_DEFORMATION_BLANKET_RAISE_HLSLI__

#include "SnowDeformation/UpdateParams.hlsli"

Texture2D<float> BlanketRaise : register(t4);

/** Height of the rendered snow blanket at a world position, 0 outside the blanket window. */
float RaiseAt(float2 worldPosition, SamplerState raiseSampler)
{
	const float2 cell = worldPosition * (1.0 / BLANKET_CELL);
	const int2 inside = (int2)floor(cell) - BlanketCell.xy;
	float raise = 0.0;
	[branch] if (all(inside >= 0) && all(inside < BLANKET_TEXELS - 1))
		raise = BlanketRaise.SampleLevel(raiseSampler, (cell + 0.5) * (1.0 / BLANKET_TEXELS), 0);
	return raise;
}

#endif
