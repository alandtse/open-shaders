#ifndef CLOUD_RELIGHT_HLSLI
#define CLOUD_RELIGHT_HLSLI

#include "CloudRelight/Draine.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

namespace CloudRelight
{
	static const float kMinimumTransmittance = 1e-3;
	static const float kSilverScatterScale = 1.35;
	static const float kEdgeFadeInStart = 0.08;
	static const float kEdgeFadeInEnd = 0.35;
	static const float kEdgeFadeOutStart = 0.45;
	static const float kEdgeFadeOutEnd = 0.85;
	static const float kSilverExtinction = 0.5;

	float GetOpticalDepth(float cloudDensity)
	{
		return -log(max(1.0 - saturate(cloudDensity), kMinimumTransmittance));
	}

	float GetBodyScatter(float opticalDepth)
	{
		return 1.0 - exp(-opticalDepth);
	}

	float GetDirectSingleScatter(float opticalDepth)
	{
		static const float kDirectScatterScale = 0.9;
		static const float kDirectExtinction = 0.75;
		return kDirectScatterScale * opticalDepth * exp(-kDirectExtinction * opticalDepth);
	}

	float GetSilverDensity(float cloudDensity, float spread)
	{
		float normalizedDensity = saturate(cloudDensity);
		float normalizedSpread = clamp(spread, -1.0, 1.0);
		float fadeStart = max(normalizedSpread, 0.0);
		float fadeEnd = min(1.0 + normalizedSpread, 1.0);
		if (fadeStart == fadeEnd)
			return normalizedSpread < 0.0 ? float(normalizedDensity > 0.0) : float(normalizedDensity >= 1.0);

		return saturate((normalizedDensity - fadeStart) / (fadeEnd - fadeStart));
	}

	float GetBroadSilverDensityWeight(float cloudDensity, float spread)
	{
		return 1.0 - GetSilverDensity(cloudDensity, spread);
	}

	float GetSilverSingleScatter(float opticalDepth, float cloudDensity, float spread)
	{
		float silverDensity = GetSilverDensity(cloudDensity, spread);
		float edgeMask =
			smoothstep(kEdgeFadeInStart, kEdgeFadeInEnd, cloudDensity) *
			(1.0 - smoothstep(kEdgeFadeOutStart, kEdgeFadeOutEnd, silverDensity));
		return kSilverScatterScale * edgeMask * (1.0 - exp(-opticalDepth)) * exp(-kSilverExtinction * opticalDepth);
	}

	float GetInnerShadowOpacity(float capturedOpacity)
	{
		return capturedOpacity * capturedOpacity;
	}

	namespace Phase
	{
		static const float kCoreForwardG = 0.94;
		static const float kAureoleG = 0.78;
		static const float kAureoleAlpha = 2.0;
		static const float kAureoleWeight = 0.45;

		float BroadSilverLining(float cosTheta)
		{
			static const float kBackwardG = -0.151765;
			static const float kForwardG = 0.611521;
			static const float kForwardAlpha = 60.0;
			static const float kForwardWeight = 0.923579;
			return (1.0 - kForwardWeight) * evalDraine(cosTheta, kBackwardG, 0.0) +
			       kForwardWeight * evalDraine(cosTheta, kForwardG, kForwardAlpha);
		}

		float SilverLining(float cosTheta)
		{
			float isotropicPhase = 0.25 * Math::INV_PI;
			float forwardCore = max(0.0, evalDraine(cosTheta, kCoreForwardG, 0.0) - isotropicPhase);
			float forwardAureole = max(0.0, evalDraine(cosTheta, kAureoleG, kAureoleAlpha) - isotropicPhase);
			return forwardCore + kAureoleWeight * forwardAureole;
		}
	}

	float GetPhaseRelighting(float cosTheta, float3 scatter)
	{
		float isotropicPhase = 0.25 * Math::INV_PI;
		float broadSilverPhase = max(0.0, Phase::BroadSilverLining(cosTheta) - isotropicPhase);
		return scatter.x + scatter.y * broadSilverPhase + scatter.z * Phase::SilverLining(cosTheta);
	}

#if defined(CLOUD_SHADOWS)
	float GetInnerShadow(float3 viewDir, float3 dirLightDir, float cloudDensity, SamplerState textureSampler)
	{
		static const float kRayStep = 1.0 / 32.0;
		float rayPos = kRayStep * 0.5;
		float4 raySelfShadow = 0.0;
		float4 rayCompletedShadow = 0.0;

		static const float3 kPoissonDisc[4] = {
			float3(0.460921f, 0.615192f, 0.887539f),
			float3(0.757347f, 0.911008f, 0.189581f),
			float3(0.548753f, 0.145482f, 0.0548723f),
			float3(0.90051f, 0.157048f, 0.623493f)
		};

		[unroll] for (int i = 0; i < 4; i++)
		{
			float3 raySample = normalize(lerp(viewDir, dirLightDir, rayPos));
			raySample += (kPoissonDisc[i] * 2.0 - 1.0) * 0.01;

			if (raySample.z < 0.0) {
				raySelfShadow[i] += -raySample.z;
				rayCompletedShadow[i] += -raySample.z;
			} else {
				float selfShadowOpacity = CloudShadows::CloudSelfShadowTexture.SampleLevel(textureSampler, raySample, 0).x;
				float completedShadowOpacity = CloudShadows::CloudShadowsTexture.SampleLevel(textureSampler, raySample, 0).x;
				raySelfShadow[i] = max(raySelfShadow[i], GetInnerShadowOpacity(selfShadowOpacity));
				rayCompletedShadow[i] = max(rayCompletedShadow[i], GetInnerShadowOpacity(completedShadowOpacity));
			}

			rayPos += kRayStep;
		}

		float selfShadowLight = 1.0 - saturate(dot(raySelfShadow, 0.25));
		float completedShadowLight = 1.0 - saturate(dot(rayCompletedShadow, 0.25));
		return lerp(selfShadowLight, max(selfShadowLight, completedShadowLight), saturate(cloudDensity));
	}

	float GetDirectionalRelighting(float3 viewDir, float3 lightDir, float cloudDensity, float3 scatter, SamplerState textureSampler)
	{
		float cosTheta = clamp(dot(viewDir, lightDir), -1.0, 1.0);
		return GetInnerShadow(viewDir, lightDir, cloudDensity, textureSampler) * GetPhaseRelighting(cosTheta, scatter);
	}

	float GetCelestialRelighting(float3 viewDir, float3 dirLightDir, float cloudDensity, float3 scatter, float3 lightWeights, SamplerState textureSampler)
	{
		float relighting = 0.0;
		// A negative sun weight preserves the engine light when Sky Sync is inactive.
		[branch] if (lightWeights.x < 0.0)
		{
			relighting = GetDirectionalRelighting(viewDir, dirLightDir, cloudDensity, scatter, textureSampler);
		}
		else
		{
			float3 lightDirections[3] = {
				SharedData::SunDirection.xyz,
				SharedData::MasserDirection.xyz,
				SharedData::SecundaDirection.xyz
			};
			float fallbackWeight = 1.0 - saturate(dot(lightWeights, 1.0));
			[unroll] for (int i = 0; i < 3; i++)
			{
				[branch] if (lightWeights[i] > 0.0)
				{
					float directionLengthSquared = dot(lightDirections[i], lightDirections[i]);
					[branch] if (directionLengthSquared > EPSILON_LENGTH_SQ)
					{
						float3 lightDir = lightDirections[i] * rsqrt(directionLengthSquared);
						relighting += lightWeights[i] * GetDirectionalRelighting(viewDir, lightDir, cloudDensity, scatter, textureSampler);
					}
					else
					{
						fallbackWeight += lightWeights[i];
					}
				}
			}

			[branch] if (fallbackWeight > EPSILON_DIVISION)
				relighting += fallbackWeight * GetInnerShadow(viewDir, dirLightDir, cloudDensity, textureSampler) * scatter.x;
		}
		return relighting;
	}

	float3 RelightCloud(float4 baseColor, float3 viewDir, SamplerState textureSampler)
	{
		if (baseColor.w <= 0.0)
			return baseColor.rgb;

		SharedData::CloudRelightSettings data = SharedData::cloudRelightSettings;

		float3 dirLightDir = normalize(SharedData::DirLightDirection.xyz);
		float linearLightingDirLightMultiplier =
			(SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0;
		float3 dirLightColor =
			Color::DirectionalLight(SharedData::DirLightColor.rgb / max(linearLightingDirLightMultiplier, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * linearLightingDirLightMultiplier * Color::VanillaNormalization();
		float isotropicPhase = 0.25 * Math::INV_PI;
		float opticalDepth = GetOpticalDepth(baseColor.a);
		float bodyScatter = GetBodyScatter(opticalDepth);
		float directSingleScatter = GetDirectSingleScatter(opticalDepth);
		float broadSilverDensityWeight = GetBroadSilverDensityWeight(baseColor.a, data.silverLiningSpread);
		float silverSingleScatter = GetSilverSingleScatter(opticalDepth, baseColor.a, data.silverLiningSpread);
		float bodyRelighting = bodyScatter * isotropicPhase * Math::TAU;
		float broadSilverRelighting = directSingleScatter * broadSilverDensityWeight * Math::TAU * data.silverLiningMix;
		float silverRelighting = silverSingleScatter * data.silverLiningMix;

		float3 cloudColor = baseColor.rgb * data.cloudOriginalMix;

		float3 scatter = float3(bodyRelighting, broadSilverRelighting, silverRelighting);
		float relighting = GetCelestialRelighting(viewDir, dirLightDir, baseColor.a, scatter, data.celestialLightWeights, textureSampler);
		cloudColor += baseColor.rgb * dirLightColor * data.cloudRelightMix * relighting;

		return cloudColor;
	}
#endif
}

#endif
