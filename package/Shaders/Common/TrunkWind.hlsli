#ifndef __TRUNK_WIND_DEPENDENCY_HLSL__
#define __TRUNK_WIND_DEPENDENCY_HLSL__

namespace TrunkWind
{
	float GetInstanceResponse(float2 instanceOriginWS)
	{
		float2 hash = frac(instanceOriginWS * float2(0.1031, 0.1030));
		hash += dot(hash, hash.yx + 33.33);
		float responseMin = min(Permutation::TrunkWindInstanceResponseMin, Permutation::TrunkWindInstanceResponseMax);
		float responseMax = max(Permutation::TrunkWindInstanceResponseMin, Permutation::TrunkWindInstanceResponseMax);
		return lerp(responseMin, responseMax, frac((hash.x + hash.y) * hash.x));
	}

	float2 GetWorldDisplacement(float localHeight, float2 windVector, float gustStrength, float instanceResponse)
	{
		float height = saturate(max(localHeight, 0.0) / max(Permutation::TrunkWindFlexibleHeight, 1.0));
		float flexibility = height * height;
		return windVector *
		       (max(Permutation::TrunkWindMaximumDisplacement, 0.0) * flexibility * gustStrength * instanceResponse);
	}
}

#endif  // __TRUNK_WIND_DEPENDENCY_HLSL__
