#ifndef __SNOW_DEFORMATION_COVERAGE_HLSLI__
#define __SNOW_DEFORMATION_COVERAGE_HLSLI__

#include "Common/SharedData.hlsli"

#if defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t2
#	include "Skylighting/Skylighting.hlsli"
#endif

Texture2D<float> SnowMapTexture : register(t1);
SamplerState LinearSampler : register(s0);

float SnowCoverAt(float2 worldPosition, float2 windowPosition, float landHeight, float normalZ)
{
	float amount = 0.0;
	[branch] if (Options.x > 0.5)
	{
		float2 uv = worldPosition * SharedData::snowCoverSettings.mapScale + SharedData::snowCoverSettings.mapOffset;
		float height = landHeight - SharedData::snowCoverSettings.SnowHeightOffset + (SnowMapTexture.SampleLevel(LinearSampler, uv, 0) - 0.5) * SharedData::snowCoverSettings.mapZscale;
		float environment = (height + SharedData::snowCoverSettings.SeasonalAltitude) / max(SharedData::snowCoverSettings.BlendSmoothness, 1.0);
		float timeSnowing = SharedData::snowCoverSettings.TimeSnowing;
		float weather = timeSnowing * timeSnowing * timeSnowing * max(500.0, SharedData::snowCoverSettings.SnowingDensity) / 500.0;
		weather = clamp(weather * max(SharedData::snowCoverSettings.minAngle, normalZ), -1.0, 1.0);

		float2 cameraRelative = windowPosition + EyeOffset.xy;
		float2 cellFraction = frac(worldPosition / 4096.0 + 64.0);
		int2 waterCell = (int2)round(cameraRelative / 4096.0 + 2.5 - cellFraction);
		float waterHeight = -2147483648.0;
		[flatten] if (all(waterCell >= 0) && all(waterCell < 5))
			waterHeight = SharedData::WaterData[waterCell.x + waterCell.y * 5].w;
		float waterFade = smoothstep(-64.0, 8.0, waterHeight - (landHeight + EyeOffset.z));

		amount = max(saturate(max(environment, weather)) - waterFade, 0.0) * smoothstep(SharedData::snowCoverSettings.minAngle, SharedData::snowCoverSettings.maxAngle, normalZ);
	}

#if defined(SKYLIGHTING)
	[branch] if (amount > 0.0 && Options.y > 0.5)
	{
		float3 cameraRelative = float3(windowPosition + EyeOffset.xy, landHeight + EyeOffset.z);
		sh2 skylighting = Skylighting::Sample(cameraRelative, float3(0.0, 0.0, 1.0));
		amount *= smoothstep(0.0, 0.75, saturate(SphericalHarmonics::Unproject(skylighting, float3(0.0, 0.0, 1.0))));
	}
#endif

	float melt = 0.0;
	uint fireCount = amount > 0.0 ? min(SharedData::snowCoverSettings.FireMeltCount, 8u) : 0u;
	[loop] for (uint i = 0; i < fireCount; i++)
	{
		float4 fire = SharedData::snowCoverSettings.FireMeltSpheres[i];
		float radius = fire.w * SharedData::snowCoverSettings.FireMeltRadiusScale;
		float3 delta = float3(worldPosition, landHeight) - fire.xyz;
		melt = max(melt, 1.0 - smoothstep(radius * radius * 0.25, radius * radius, dot(delta, delta)));
	}
	return saturate(amount * (1.0 - melt * SharedData::snowCoverSettings.FireMeltStrength));
}

#endif
