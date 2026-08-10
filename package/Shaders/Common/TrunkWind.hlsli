#ifndef __TRUNK_WIND_DEPENDENCY_HLSL__
#define __TRUNK_WIND_DEPENDENCY_HLSL__

namespace TrunkWind
{
	float2 GetDisplacement(float3 positionMS, float time)
	{
		const float flexibleHeight = 4096.0;
		const float maximumDisplacement = 512.0;
		const float2 windDirection = normalize(float2(1.0, 0.35));

		float height = saturate(max(positionMS.z, 0.0) / flexibleHeight);
		float flexibility = height * height;
		float gust = sin(time * 1.25) + 0.3 * sin(time * 2.7 + 0.9);
		return windDirection * (maximumDisplacement * flexibility * gust);
	}
}

#endif  // __TRUNK_WIND_DEPENDENCY_HLSL__
