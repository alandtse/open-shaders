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
	float Remap(float x, float inMin, float inMax, float outMin, float outMax)
	{
		if (inMin == inMax)
			return outMin;

		return lerp(outMin, outMax, saturate((x - inMin) / (inMax - inMin)));
	}

	namespace Phase
	{
		// HG+Draine blend (see Draine.hlsli), parameters least-squares fit to this feature's
		// original silver-lining shape over the forward-scattering hemisphere.
		float SilverLining(float cosTheta)
		{
			static const float kGHG = -0.151765;
			static const float kGD = 0.611521;
			static const float kAlpha = 60.0;
			static const float kWeightD = 0.923579;
			return (1.0 - kWeightD) * evalDraine(cosTheta, kGHG, 0.0) + kWeightD * evalDraine(cosTheta, kGD, kAlpha);
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
				raySelfShadow[i] = max(raySelfShadow[i], CloudShadows::CloudSelfShadowTexture.SampleLevel(textureSampler, raySample, 0).x);
				rayCompletedShadow[i] = max(rayCompletedShadow[i], CloudShadows::CloudShadowsTexture.SampleLevel(textureSampler, raySample, 0).x);
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
		float silverLiningPhase = max(isotropicPhase, Phase::SilverLining(cosTheta));

		float phaseCloud =
			Remap(
				baseColor.w,
				data.silverLiningSpread > 0.0 ? data.silverLiningSpread : 0.0,
				data.silverLiningSpread < 0.0 ? 1.0 + data.silverLiningSpread : 1.0,
				lerp(isotropicPhase, silverLiningPhase, data.silverLiningMix),
				isotropicPhase) *
			Math::TAU * data.cloudRelightMix;
		phaseCloud = min(phaseCloud, 2.0);

		float directionalLightIntensity = saturate(Color::RGBToLuminance(dirLightColor));
		float vanillaMix = lerp(1.0, data.cloudOriginalMix, directionalLightIntensity);
		float3 cloudColor = baseColor.rgb * vanillaMix;

		float3 relitColor = baseColor.a * baseColor.rgb * phaseCloud * GetInnerShadow(viewDir, dirLightDir, baseColor.a, textureSampler) * dirLightColor;
		cloudColor += relitColor;

		return cloudColor;
	}
#endif
}

#endif
