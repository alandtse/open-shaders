#ifndef __VALUE_NOISE_DEPENDENCY_HLSL__
#define __VALUE_NOISE_DEPENDENCY_HLSL__

#include "Common/Random.hlsli"

namespace ValueNoise
{
	/// Three independent [0,1) values per integer lattice point from the PCG 3D hash.
	float3 LatticeValue(int3 lattice)
	{
		return float3(Random::pcg3d(asuint(lattice)) >> 8u) * (1.0f / 16777216.0f);
	}

	/// Trilinear value noise with a quintic fade; the integer lattice stays exact at large world coordinates.
	float3 Evaluate3(float3 position)
	{
		int3 lattice = int3(floor(position));
		float3 blend = frac(position);
		blend = blend * blend * blend * (blend * (blend * 6.0f - 15.0f) + 10.0f);
		float3 front = lerp(
			lerp(LatticeValue(lattice), LatticeValue(lattice + int3(1, 0, 0)), blend.x),
			lerp(LatticeValue(lattice + int3(0, 1, 0)), LatticeValue(lattice + int3(1, 1, 0)), blend.x), blend.y);
		float3 back = lerp(
			lerp(LatticeValue(lattice + int3(0, 0, 1)), LatticeValue(lattice + int3(1, 0, 1)), blend.x),
			lerp(LatticeValue(lattice + int3(0, 1, 1)), LatticeValue(lattice + int3(1, 1, 1)), blend.x), blend.y);
		return lerp(front, back, blend.z);
	}
}

#endif
