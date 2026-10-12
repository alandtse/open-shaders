#ifndef EFFECTS11_SKY_SCATTERING_HLSLI
#define EFFECTS11_SKY_SCATTERING_HLSLI

#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUDS) && defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

// Namespace constants change FXC code generation for VR clouds, so preserve these as named literals.
#define SKY_SCATTERING_PLANET_RADIUS 6371e3
#define SKY_SCATTERING_DIRECTION_LENGTH_SQ_EPSILON 1e-8
#define SKY_SCATTERING_BILLBOARD_COS_EPSILON 1e-3
#define SKY_SCATTERING_CLOUD_VISIBILITY_EPSILON 1e-6
#define SKY_SCATTERING_HORIZON_CROP_SCALE 40.0
#define SKY_SCATTERING_SKY_HORIZON_SCALE 10.0
#define SKY_SCATTERING_MOON_GLOW_RADIUS_SCALE 12.0
#define SKY_SCATTERING_MOON_GLOW_FLOOR 0.005
#define SKY_SCATTERING_MOON_EDGE_RADIUS_SCALE 8.0
#define SKY_SCATTERING_CLOUD_SHADOW_RAY_SCALE 32.0
#define SKY_SCATTERING_CLOUD_SHADOW_JITTER 0.01
#define SKY_SCATTERING_SUN_CLOUD_OCCLUSION_SCALE 1.2
#define SKY_SCATTERING_SUN_EDGE_OCCLUSION_SCALE 1.1
#define SKY_SCATTERING_SUN_CLOUD_WEIGHT_SCALE 4.0
#define SKY_SCATTERING_CLOUD_HORIZON_FLATTENING 8.0
#define SKY_SCATTERING_TWILIGHT_HEIGHT 0.1
#define SKY_SCATTERING_TWILIGHT_THRESHOLD_SCALE 5.0
#define SKY_SCATTERING_TWILIGHT_SPREAD_SCALE 3.0
#define SKY_SCATTERING_SHADOW_THRESHOLD_BASE 0.8
#define SKY_SCATTERING_SHADOW_THRESHOLD_RANGE 0.3
#define SKY_SCATTERING_SHADOW_FACING_THRESHOLD 0.95
#define SKY_SCATTERING_SHADOW_SPREAD_SCALE 4.0
#define SKY_SCATTERING_SHADOW_SPREAD_FLOOR 0.1
#define SKY_SCATTERING_SHADOW_FACING_SCALE 4.0
#define SKY_SCATTERING_CLOUD_PHASE_OFFSET 8.194068e-01
#define SKY_SCATTERING_CLOUD_PHASE_EXPONENT float3(-6.5e+01, -8.370334e+01, 7.810083e+00)
#define SKY_SCATTERING_CLOUD_PHASE_BIAS 5.5e+01
#define SKY_SCATTERING_CLOUD_PHASE_WEIGHT float3(9.805233e-06, 1.388198e-01, 2.054747e-03)
#define SKY_SCATTERING_CLOUD_PHASE_AMBIENT 2.600563e-02

namespace SkyScattering
{
	static const float2 ScaleHeight = float2(8e3, 1.2e3);
	static const float2 AirmassSlope = sqrt(0.5 * SKY_SCATTERING_PLANET_RADIUS / ScaleHeight);
	static const float3 RayleighExtinction = float3(6.6049e-6, 12.345e-6, 29.413e-6);
	static const float AerosolExtinction = 4.44e-5;

	float3 SafeNormalize(float3 v)
	{
		return v * rsqrt(max(dot(v, v), SKY_SCATTERING_DIRECTION_LENGTH_SQ_EPSILON));
	}

	float4 GetCelestialExtinction(float3 viewDirection)
	{
		float2 y = AirmassSlope * saturate(viewDirection.z);
		float2 depth = 2.0 * AirmassSlope * ScaleHeight * (1.0 / (y + sqrt(y * y + 4.0 / Math::PI)) - 1.0 / (AirmassSlope + sqrt(AirmassSlope * AirmassSlope + 4.0 / Math::PI)));
		return exp(-float4((RayleighExtinction - RayleighExtinction.r) * depth.x, RayleighExtinction.r * depth.x + AerosolExtinction * depth.y));
	}

	float3 Pow32(float3 x)
	{
		x *= x;
		x *= x;
		x *= x;
		x *= x;
		return x * x;
	}

	float3 GetSunDirection()
	{
		return SharedData::enbSettings.SkyScatteringSunDirection;
	}

	float GetFacing(float3 viewDirection, float3 lightDirection)
	{
		return saturate(dot(viewDirection, lightDirection) * 0.5 + 0.5);
	}

	float GetElevation(float3 viewDirection, float sunHeight)
	{
		return 1.0 - saturate(1.0 - viewDirection.z) * saturate(1.0 + sunHeight);
	}

	float GetHorizonCrop(float viewHeight)
	{
		float below = saturate(1.0 - viewHeight * SKY_SCATTERING_HORIZON_CROP_SCALE);
		return 1.0 - below * below;
	}

	float GetDustBand(float elevation)
	{
		return Pow32(saturate(1.0 - elevation * SharedData::enbSettings.SkyScatteringDustVolume)).x;
	}

	float GetEarthShadow(float elevation, float facing, float sunHeight)
	{
		float twilight = SKY_SCATTERING_TWILIGHT_HEIGHT - sunHeight;
		float threshold = lerp(SKY_SCATTERING_SHADOW_THRESHOLD_BASE + SKY_SCATTERING_SHADOW_THRESHOLD_RANGE * saturate(twilight * SKY_SCATTERING_TWILIGHT_THRESHOLD_SCALE), SKY_SCATTERING_SHADOW_FACING_THRESHOLD + saturate(sunHeight * -0.5), facing);
		float spread = saturate(1.0 - twilight * SKY_SCATTERING_TWILIGHT_SPREAD_SCALE);
		float shadow = saturate(1.0 + elevation - threshold) * (SKY_SCATTERING_SHADOW_SPREAD_SCALE * (SKY_SCATTERING_SHADOW_SPREAD_FLOOR + spread * spread)) * (1.0 + facing * SKY_SCATTERING_SHADOW_FACING_SCALE);
		return smoothstep(0.0, 1.0, smoothstep(0.0, 1.0, shadow));
	}

	float3 GetScatteringColor(float dustBand)
	{
		return SharedData::enbSettings.SkyScatteringColor * SharedData::enbSettings.SkyScatteringIntensity * Pow32(saturate(1.0 - dustBand * SharedData::enbSettings.SkyScatteringDustTint));
	}

	float3 GetScatteringColorAt(float3 direction)
	{
		return GetScatteringColor(GetDustBand(GetElevation(direction, GetSunDirection().z))) * saturate(SharedData::enbSettings.SkyScatteringAmount);
	}

	float3 ApplySkyScattering(float3 skyColor, float3 topColor, float3 viewDirection)
	{
		float3 sunDirection = GetSunDirection();
		float facing = GetFacing(viewDirection, sunDirection);
		float away = 1.0 - facing;
		float elevation = GetElevation(viewDirection, sunDirection.z);
		float dustBand = GetDustBand(elevation);
		float aboveHorizon = saturate(1.0 + viewDirection.z * SKY_SCATTERING_SKY_HORIZON_SCALE);
		float sunAboveHorizon = saturate(1.0 + viewDirection.z * SKY_SCATTERING_HORIZON_CROP_SCALE);
		aboveHorizon *= aboveHorizon;
		sunAboveHorizon *= sunAboveHorizon;

		float spread = lerp(SharedData::enbSettings.SkyScatteringHorizonRange * away * away, SharedData::enbSettings.SkyScatteringAtmosphereThickness, elevation * elevation);
		float3 scatteringColor = GetScatteringColor(dustBand) * (aboveHorizon * (1.0 - dustBand * SharedData::enbSettings.SkyScatteringDustDarkening));
		float shadow = lerp(1.0, GetEarthShadow(elevation, facing, sunDirection.z), SharedData::enbSettings.SkyScatteringShadowAmount);

		float3 result = skyColor * (1.0 + SharedData::enbSettings.SkyScatteringAirGlowIntensity / (1.0 + away * SharedData::enbSettings.SkyScatteringAirGlowRange));
		result = lerp(result, scatteringColor, saturate(SharedData::enbSettings.SkyScatteringAmount / (1.0 + away * spread)));
		result *= 1.0 + SharedData::enbSettings.SkyScatteringSunGlowIntensity * sunAboveHorizon / (1.0 + away * SharedData::enbSettings.SkyScatteringSunGlowRange);
		return lerp(skyColor, lerp(topColor * 0.5, result, shadow), aboveHorizon);
	}

	float GetBillboardRadius(float3 viewDirection, float3 centerDirection, float halfTan)
	{
		float cosAngle = dot(viewDirection, centerDirection);
		if (cosAngle <= SKY_SCATTERING_BILLBOARD_COS_EPSILON || halfTan <= 0.0)
			return 2.0;
		return (1.0 - cosAngle * cosAngle) / (cosAngle * cosAngle * halfTan * halfTan);
	}

	float3 GetMoonGlow(float3 viewDirection, float3 moonDirection, float3 moonColor, float moonHalfTan)
	{
		float radius = saturate(GetBillboardRadius(viewDirection, SafeNormalize(moonDirection), SKY_SCATTERING_MOON_GLOW_RADIUS_SCALE * moonHalfTan));
		return max(moonColor, 0.0) * (saturate(1.0 / (1.0 + radius * SharedData::enbSettings.SkyScatteringMoonGlowRange) - SKY_SCATTERING_MOON_GLOW_FLOOR) * (1.0 - radius));
	}

	float3 GetMoonGlow(float3 viewDirection)
	{
		float3 glow = 0.0;
		[branch] if (SharedData::enbSettings.SkyScatteringMoonGlowAmount > 0.0)
		{
			glow = GetMoonGlow(viewDirection, SharedData::MasserDirection.xyz, SharedData::MasserColor.xyz, SharedData::enbSettings.MasserBillboardTan);
			glow += GetMoonGlow(viewDirection, SharedData::SecundaDirection.xyz, SharedData::SecundaColor.xyz, SharedData::enbSettings.SecundaBillboardTan);
			glow *= SharedData::enbSettings.SkyScatteringMoonGlowAmount * SharedData::enbSettings.CloudsEdgeMoonMultiplier * GetHorizonCrop(viewDirection.z);
		}
		return glow;
	}

#if defined(CLOUDS)
	float GetCloudPhase(float3 viewDirection, float3 lightDirection, float cloudAlpha)
	{
		float cosTheta = dot(viewDirection, lightDirection);
		float p1 = cosTheta + SKY_SCATTERING_CLOUD_PHASE_OFFSET;
		float phase = dot(exp(float3(SKY_SCATTERING_CLOUD_PHASE_EXPONENT.x * cosTheta - SKY_SCATTERING_CLOUD_PHASE_BIAS, SKY_SCATTERING_CLOUD_PHASE_EXPONENT.y * p1 * p1, SKY_SCATTERING_CLOUD_PHASE_EXPONENT.z * cosTheta)), SKY_SCATTERING_CLOUD_PHASE_WEIGHT) + SKY_SCATTERING_CLOUD_PHASE_AMBIENT;
		return max(0.0, 1.0 + SharedData::enbSettings.CloudsLightingForwardScattering * (1.0 - cloudAlpha) * (phase * Math::PI - 1.0));
	}

	float GetCloudLightOcclusion(float3 viewDirection, float3 lightDirection, SamplerState textureSampler)
	{
		static const float3 PoissonDisc[4] = {
			float3(0.460921, 0.615192, 0.887539),
			float3(0.757347, 0.911008, 0.189581),
			float3(0.548753, 0.145482, 0.0548723),
			float3(0.90051, 0.157048, 0.623493)
		};

		float occlusion = 0.0;
		[unroll] for (uint i = 0; i < 4; i++)
		{
			float3 sampleDirection = normalize(lerp(viewDirection, lightDirection, (float(i) + 0.5) / SKY_SCATTERING_CLOUD_SHADOW_RAY_SCALE)) + (PoissonDisc[i] * 2.0 - 1.0) * SKY_SCATTERING_CLOUD_SHADOW_JITTER;
			if (sampleDirection.z < 0.0)
				occlusion += -sampleDirection.z;
#	if defined(CLOUD_SHADOWS)
			else
				occlusion += CloudShadows::CloudSelfShadowTexture.SampleLevel(textureSampler, sampleDirection, 0);
#	endif
		}
		return saturate(occlusion * 0.25);
	}

	float GetCloudLightVisibility(float3 viewDirection, float3 lightDirection, float occlusionScale, SamplerState textureSampler)
	{
		float visibility = saturate(1.0 - GetCloudLightOcclusion(viewDirection, lightDirection, textureSampler) * occlusionScale);
		return pow(max(visibility * visibility, SKY_SCATTERING_CLOUD_VISIBILITY_EPSILON), SharedData::enbSettings.CloudsLightingDensity);
	}

	float3 DesaturateCloudLight(float3 color)
	{
		return max(lerp(color, dot(color, 1.0 / 3.0), SharedData::enbSettings.CloudsLightingDesaturation), 0.0);
	}

	float GetCloudEdgeFade(float radius)
	{
		return pow(saturate(1.0 - radius), SharedData::enbSettings.CloudsEdgeFadePower);
	}

	void AddCloudMoonLight(inout float3 light, inout float3 edge, float3 viewDirection, float3 moonDirection, float3 moonColor, float halfTan, bool lit, bool rim, bool scattering, SamplerState textureSampler)
	{
		float3 direction = SafeNormalize(moonDirection);
		float3 color = max(moonColor, 0.0);
		float radius = GetBillboardRadius(viewDirection, direction, SKY_SCATTERING_MOON_EDGE_RADIUS_SCALE * halfTan);
		lit = lit && any(color > 0.0);
		rim = rim && radius < 1.0;
		[branch] if (lit || rim)
		{
			float visibility = 1.0;
			if (scattering)
				visibility = GetCloudLightVisibility(viewDirection, direction, 1.0, textureSampler);
			if (lit)
				light += color * visibility;
			if (rim)
				edge += color * (SharedData::enbSettings.CloudsEdgeMoonMultiplier * visibility * GetCloudEdgeFade(radius));
		}
	}

	float3 ShadeCloud(float3 cloudColor, float textureAlpha, float textureGray, float3 viewDirection, SamplerState textureSampler)
	{
		bool scattering = SharedData::enbSettings.EnableCloudsScattering && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::NoSkyScattering);
		float edgeWeight = saturate(1.0 - textureAlpha - SharedData::enbSettings.CloudsEdgeClamp);
		float3 result = cloudColor;
		float3 edge = 0.0;

		float sunWeight = saturate(SharedData::SunColor.w * SKY_SCATTERING_SUN_CLOUD_WEIGHT_SCALE);
		[branch] if (scattering && sunWeight > 0.0)
		{
			float3 sunDirection = GetSunDirection();
			float facing = GetFacing(viewDirection, sunDirection);
			float planetShadow = GetEarthShadow(GetElevation(viewDirection, sunDirection.z), facing, sunDirection.z);

			float sunVisibility = SharedData::enbSettings.SkyScatteringSunVisibility;
			if (sunVisibility < 1.0) {
				float3 flattened = normalize(float3(viewDirection.xy, viewDirection.z * SKY_SCATTERING_CLOUD_HORIZON_FLATTENING));
				planetShadow *= saturate(dot(flattened.xy, sunDirection.xy) * 0.5 + 0.5 + sunVisibility * 2.0 - 1.0);
			}

			float3 sunLit = result * lerp(1.0 - 0.5 * SharedData::enbSettings.SkyScatteringShadowAmount, 1.0, planetShadow);

			[branch] if (SharedData::enbSettings.CloudsLightingSunIntensity > 0.0)
			{
				float3 sunLight = DesaturateCloudLight(GetScatteringColorAt(sunDirection));
				float visibility = GetCloudLightVisibility(viewDirection, sunDirection, SKY_SCATTERING_SUN_CLOUD_OCCLUSION_SCALE, textureSampler);
				float phase = GetCloudPhase(viewDirection, sunDirection, textureAlpha);
				sunLit += sunLight * (visibility * phase * planetShadow * textureGray * SharedData::enbSettings.CloudsLightingSunIntensity);
			}

			result = lerp(result, sunLit, sunWeight);
		}

		[branch] if (edgeWeight > 0.0 && SharedData::SunColor.w > 0.0)
		{
			float3 sunDirection = SafeNormalize(SharedData::SunDirection.xyz);
			float radius = GetBillboardRadius(viewDirection, sunDirection, SharedData::enbSettings.SunBillboardTan);
			[branch] if (radius < 1.0)
			{
				edge = max(SharedData::SunColor.xyz, 0.0);
				if (scattering && SharedData::enbSettings.CalculateCloudsEdgeFromScattering)
					edge = GetScatteringColorAt(viewDirection) * (SharedData::enbSettings.SkyScatteringSunIntensity * saturate(SharedData::SunColor.w));
				if (scattering)
					edge *= GetCloudLightVisibility(viewDirection, sunDirection, SKY_SCATTERING_SUN_EDGE_OCCLUSION_SCALE, textureSampler);
				edge *= GetCloudEdgeFade(radius);
			}
		}

		float3 moonLight = 0.0;
		bool moonLit = scattering && SharedData::enbSettings.EnableCloudsLightingFromMoon && SharedData::enbSettings.CloudsLightingMoonIntensity != 0.0;
		bool moonRim = edgeWeight > 0.0 && SharedData::enbSettings.CloudsEdgeMoonMultiplier > 0.0;
		[branch] if (moonLit || moonRim)
		{
			AddCloudMoonLight(moonLight, edge, viewDirection, SharedData::MasserDirection.xyz, SharedData::MasserColor.xyz, SharedData::enbSettings.MasserBillboardTan, moonLit, moonRim, scattering, textureSampler);
			AddCloudMoonLight(moonLight, edge, viewDirection, SharedData::SecundaDirection.xyz, SharedData::SecundaColor.xyz, SharedData::enbSettings.SecundaBillboardTan, moonLit, moonRim, scattering, textureSampler);
		}

		float horizonCrop = GetHorizonCrop(viewDirection.z);
		result += DesaturateCloudLight(moonLight) * (SharedData::enbSettings.CloudsEdgeMoonMultiplier * SharedData::enbSettings.CloudsLightingMoonIntensity * horizonCrop * textureGray);
		return result + edge * (SharedData::enbSettings.CloudsEdgeIntensity * horizonCrop * dot(cloudColor, 1.0 / 3.0) * edgeWeight);
	}
#endif
}

#undef SKY_SCATTERING_PLANET_RADIUS
#undef SKY_SCATTERING_DIRECTION_LENGTH_SQ_EPSILON
#undef SKY_SCATTERING_BILLBOARD_COS_EPSILON
#undef SKY_SCATTERING_CLOUD_VISIBILITY_EPSILON
#undef SKY_SCATTERING_HORIZON_CROP_SCALE
#undef SKY_SCATTERING_SKY_HORIZON_SCALE
#undef SKY_SCATTERING_MOON_GLOW_RADIUS_SCALE
#undef SKY_SCATTERING_MOON_GLOW_FLOOR
#undef SKY_SCATTERING_MOON_EDGE_RADIUS_SCALE
#undef SKY_SCATTERING_CLOUD_SHADOW_RAY_SCALE
#undef SKY_SCATTERING_CLOUD_SHADOW_JITTER
#undef SKY_SCATTERING_SUN_CLOUD_OCCLUSION_SCALE
#undef SKY_SCATTERING_SUN_EDGE_OCCLUSION_SCALE
#undef SKY_SCATTERING_SUN_CLOUD_WEIGHT_SCALE
#undef SKY_SCATTERING_CLOUD_HORIZON_FLATTENING
#undef SKY_SCATTERING_TWILIGHT_HEIGHT
#undef SKY_SCATTERING_TWILIGHT_THRESHOLD_SCALE
#undef SKY_SCATTERING_TWILIGHT_SPREAD_SCALE
#undef SKY_SCATTERING_SHADOW_THRESHOLD_BASE
#undef SKY_SCATTERING_SHADOW_THRESHOLD_RANGE
#undef SKY_SCATTERING_SHADOW_FACING_THRESHOLD
#undef SKY_SCATTERING_SHADOW_SPREAD_SCALE
#undef SKY_SCATTERING_SHADOW_SPREAD_FLOOR
#undef SKY_SCATTERING_SHADOW_FACING_SCALE
#undef SKY_SCATTERING_CLOUD_PHASE_OFFSET
#undef SKY_SCATTERING_CLOUD_PHASE_EXPONENT
#undef SKY_SCATTERING_CLOUD_PHASE_BIAS
#undef SKY_SCATTERING_CLOUD_PHASE_WEIGHT
#undef SKY_SCATTERING_CLOUD_PHASE_AMBIENT

#endif
