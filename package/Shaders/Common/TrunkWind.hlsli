#ifndef __TRUNK_WIND_DEPENDENCY_HLSL__
#define __TRUNK_WIND_DEPENDENCY_HLSL__

namespace TrunkWind
{
	float2 GetWorldDisplacement(float localHeight, float time, float2 windVector)
	{
		const float flexibleHeight = 4096.0;
		const float maximumDisplacement = 512.0;

		float height = saturate(max(localHeight, 0.0) / flexibleHeight);
		float flexibility = height * height;
		float gust = 0.55 + 0.3 * sin(time * 1.25) + 0.15 * sin(time * 2.7 + 0.9);
		return windVector * (maximumDisplacement * flexibility * gust);
	}
}

#endif  // __TRUNK_WIND_DEPENDENCY_HLSL__
