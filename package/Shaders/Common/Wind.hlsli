#ifndef __WIND_DEPENDENCY_HLSL__
#define __WIND_DEPENDENCY_HLSL__

#include "Common/Permutation.hlsli"

namespace Wind
{
	float GetWindIntensityOverrideScale()
	{
		return Permutation::OverrideWindIntensity != 0 ? Permutation::WindIntensityOverride : 1.0;
	}

	float GetWindVariationSample(float sampleIndex)
	{
		return frac(sin(sampleIndex * 12.9898 + 78.233) * 43758.5453);
	}

	float GetWindVariation(float time)
	{
		float variationMin = min(Permutation::TrunkWindVariationMin, Permutation::TrunkWindVariationMax);
		float variationMax = max(Permutation::TrunkWindVariationMin, Permutation::TrunkWindVariationMax);
		float samplePosition = time / max(Permutation::TrunkWindVariationInterval, 0.1);
		float sampleIndex = floor(samplePosition);
		float blend = frac(samplePosition);
		blend = blend * blend * (3.0 - 2.0 * blend);
		float variation = lerp(GetWindVariationSample(sampleIndex), GetWindVariationSample(sampleIndex + 1.0), blend);
		return lerp(variationMin, variationMax, variation);
	}

	float GetCurrentWindVariationScale()
	{
		return Permutation::TrunkWindGustStrength * GetWindVariation(Permutation::TrunkWindTimer);
	}

	float GetPreviousWindVariationScale()
	{
		return Permutation::TrunkWindPreviousGustStrength * GetWindVariation(Permutation::TrunkWindPreviousTimer);
	}

	float GetCurrentTrunkWindVariationScale()
	{
		return GetCurrentWindVariationScale() * Permutation::TrunkWindBendSensitivity;
	}

	float GetPreviousTrunkWindVariationScale()
	{
		return GetPreviousWindVariationScale() * Permutation::TrunkWindBendSensitivity;
	}

	float GetCurrentTrunkWindStrength()
	{
		return length(Permutation::TrunkWindVector) * GetCurrentTrunkWindVariationScale();
	}

	float GetPreviousTrunkWindStrength()
	{
		return length(Permutation::TrunkWindPreviousVector) * GetPreviousTrunkWindVariationScale();
	}

	float GetCurrentWindIntensityScale()
	{
		return GetWindIntensityOverrideScale() * GetCurrentWindVariationScale();
	}

	float GetPreviousWindIntensityScale()
	{
		return GetWindIntensityOverrideScale() * GetPreviousWindVariationScale();
	}

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

#endif  // __WIND_DEPENDENCY_HLSL__
