#ifndef __SNOW_DEFORMATION_PACKED_SNOW_HLSLI__
#define __SNOW_DEFORMATION_PACKED_SNOW_HLSLI__

/** Shading of snow pressed below its surface, shared by the domain shader that measures it and the lighting shader that applies it. */
namespace PackedSnow
{
	static const float FullDepth = 4.0;
	static const float3 AlbedoTint = float3(0.86, 0.89, 0.93);
	static const float RoughnessScale = 0.8;
	static const float ShininessScale = 1.3;

	/** Packed amount from 0 to 1 for a surface offset from the unpressed snow, negative where pressed. */
	float Amount(float offset)
	{
		return saturate(-offset / FullDepth);
	}
}

#endif
