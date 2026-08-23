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

	float GetBroadSilverDensityWeight(float cloudDensity, float spread)
	{
		float normalizedDensity = saturate(cloudDensity);
		float normalizedSpread = clamp(spread, -1.0, 1.0);
		float fadeStart = max(normalizedSpread, 0.0);
		float fadeEnd = min(1.0 + normalizedSpread, 1.0);
		if (fadeStart == fadeEnd)
			return 1.0;

		return 1.0 - saturate((normalizedDensity - fadeStart) / (fadeEnd - fadeStart));
	}

	float GetSilverSingleScatter(float opticalDepth, float cloudDensity)
	{
		static const float kSilverScatterScale = 1.35;
		static const float kEdgeFadeInStart = 0.08;
		static const float kEdgeFadeInEnd = 0.35;
		static const float kEdgeFadeOutStart = 0.45;
		static const float kEdgeFadeOutEnd = 0.85;
		static const float kSilverExtinction = 0.5;
		float edgeMask =
			smoothstep(kEdgeFadeInStart, kEdgeFadeInEnd, cloudDensity) *
			(1.0 - smoothstep(kEdgeFadeOutStart, kEdgeFadeOutEnd, cloudDensity));
		return kSilverScatterScale * edgeMask * (1.0 - exp(-opticalDepth)) * exp(-kSilverExtinction * opticalDepth);
	}

	float GetInnerShadowOpacity(float capturedOpacity)
	{
		return capturedOpacity * capturedOpacity;
	}

	namespace Phase
	{
		float BroadSilverLining(float cosTheta)
		{
			static const float kBackwardG = -0.151765;
			static const float kForwardG = 0.611521;
			static const float kForwardAlpha = 60.0;
			static const float kForwardWeight = 0.923579;
			return (1.0 - kForwardWeight) * evalDraine(cosTheta, kBackwardG, 0.0) +
			       kForwardWeight * evalDraine(cosTheta, kForwardG, kForwardAlpha);
		}

		float SilverLining(float cosTheta, float spread)
		{
			static const float kMinimumForwardG = 0.82;
			static const float kMaximumForwardG = 0.94;
			static const float kAureoleG = 0.78;
			static const float kAureoleAlpha = 2.0;
			static const float kAureoleWeight = 0.45;
			float isotropicPhase = 0.25 * Math::INV_PI;
			float forwardG = lerp(kMinimumForwardG, kMaximumForwardG, saturate(1.0 - abs(spread)));
			float forwardCore = max(0.0, evalDraine(cosTheta, forwardG, 0.0) - isotropicPhase);
			float forwardAureole = max(0.0, evalDraine(cosTheta, kAureoleG, kAureoleAlpha) - isotropicPhase);
			return forwardCore + kAureoleWeight * forwardAureole;
		}
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
		float cosTheta = dot(viewDir, dirLightDir);
		float isotropicPhase = 0.25 * Math::INV_PI;
		float broadSilverPhase = max(0.0, Phase::BroadSilverLining(cosTheta) - isotropicPhase);
		float silverLiningPhase = Phase::SilverLining(cosTheta, data.silverLiningSpread);
		float opticalDepth = GetOpticalDepth(baseColor.a);
		float bodyScatter = GetBodyScatter(opticalDepth);
		float directSingleScatter = GetDirectSingleScatter(opticalDepth);
		float broadSilverDensityWeight = GetBroadSilverDensityWeight(baseColor.a, data.silverLiningSpread);
		float silverSingleScatter = GetSilverSingleScatter(opticalDepth, baseColor.a);
		float bodyRelighting = bodyScatter * isotropicPhase * Math::TAU;
		float broadSilverRelighting = directSingleScatter * broadSilverDensityWeight * broadSilverPhase * Math::TAU * data.silverLiningMix;
		float silverRelighting = silverSingleScatter * silverLiningPhase * data.silverLiningMix;

		float3 cloudColor = baseColor.rgb * data.cloudOriginalMix;

		float directVisibility = GetInnerShadow(viewDir, dirLightDir, baseColor.a, textureSampler);
		float3 directCloudLight = baseColor.rgb * directVisibility * dirLightColor * data.cloudRelightMix;
		cloudColor += directCloudLight * bodyRelighting;
		cloudColor += directCloudLight * broadSilverRelighting;
		cloudColor += directCloudLight * silverRelighting;

		return cloudColor;
	}
#endif
}

#endif
