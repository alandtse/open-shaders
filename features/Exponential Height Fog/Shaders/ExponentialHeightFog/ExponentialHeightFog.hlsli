#ifndef __EXPONENTIAL_HEIGHT_FOG_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "ExponentialHeightFog/VolumetricFogCommon.hlsli"

#if defined(DYNAMIC_CUBEMAPS)
#	include "DynamicCubemaps/DynamicCubemaps.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

Texture3D<float4> ExponentialHeightFogIntegratedLightScattering : register(t19);
Texture3D<float4> ExponentialHeightFogIntegratedLightScatteringFar : register(t22);

namespace ExponentialHeightFog
{
	static const float MinDistanceHazeFadeDistance = 1.0f;

	float4 ApplyDistanceHaze(float4 heightFog, float3 hazeColor, float3 viewToPos)
	{
		[branch] if (SharedData::exponentialHeightFogSettings.distanceHazeMaxOpacity > 0.0f)
		{
			float horizontalDistance = length(viewToPos.xy);
			float hazeDistance = max(horizontalDistance - SharedData::exponentialHeightFogSettings.distanceHazeStartDistance, 0.0f);
			float fadeDistance = max(SharedData::exponentialHeightFogSettings.distanceHazeFadeDistance, MinDistanceHazeFadeDistance);
			float hazeOpacity = saturate(SharedData::exponentialHeightFogSettings.distanceHazeMaxOpacity) * smoothstep(0.0f, fadeDistance, hazeDistance);
			float visibleHazeOpacity = hazeOpacity * (1.0f - heightFog.a);
			float combinedOpacity = heightFog.a + visibleHazeOpacity;
			if (visibleHazeOpacity > 0.0f)
				heightFog = float4((heightFog.rgb * heightFog.a + hazeColor * visibleHazeOpacity) / combinedOpacity, combinedOpacity);
		}
		return heightFog;
	}

	float GetVanillaFogFade(float vanillaFogFade)
	{
		return SharedData::exponentialHeightFogSettings.respectVanillaFogFade != 0 ? vanillaFogFade : 1.0f;
	}

	float GetLinearVanillaFogFade(float vanillaFogFade)
	{
		float fogFade = GetVanillaFogFade(vanillaFogFade);
		return ENABLE_LL ? Color::AuthoredGammaToLinear(fogFade.xxx).x : fogFade;
	}

	bool ShouldDisableVanillaFog()
	{
		return SharedData::exponentialHeightFogSettings.enabled && SharedData::exponentialHeightFogSettings.disableVanillaFog != 0;
	}

	uint GetEyeIndexFromCameraWS(float3 cameraWS)
	{
#if defined(VR)
		return distance(cameraWS, FrameBuffer::CameraPosAdjust[1].xyz) < distance(cameraWS, FrameBuffer::CameraPosAdjust[0].xyz) ? 1u : 0u;
#else
		return 0u;
#endif
	}

	bool ShouldApplyVolumetricFog(out uint3 volumeSize)
	{
		volumeSize = 0u.xxx;
		bool applyVolumetricFog = false;
		[branch] if (SharedData::exponentialHeightFogSettings.enabled != 0 &&
					 SharedData::exponentialHeightFogSettings.volumetricFogEnabled != 0 &&
					 SharedData::exponentialHeightFogSettings.volumetricFogDistance > GetVolumetricStartDistance() + 1.0f)
		{
			ExponentialHeightFogIntegratedLightScattering.GetDimensions(volumeSize.x, volumeSize.y, volumeSize.z);
			applyVolumetricFog = all(volumeSize > 0u);
		}
		return applyVolumetricFog;
	}

	float GetSceneDepthForFog(float3 positionWS, uint eyeIndex, out float2 volumeUV)
	{
		volumeUV = 0.0f.xx;
		float sceneDepth = 0.0f;
		float4 clipPosition = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(positionWS, 1.0f));
		[branch] if (clipPosition.w > 0.0f)
		{
			sceneDepth = max(clipPosition.w, SharedData::CameraData.y);
			volumeUV = saturate(clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f);
		}
		return sceneDepth;
	}

	float4 SampleFogGrid(Texture3D<float4> volume, uint3 volumeSize, float2 volumeUV, float sceneDepth, uint eyeIndex)
	{
		float slice = ComputeVolumetricNormalizedSlice(sceneDepth, float(volumeSize.z));
		float3 texelCenter = 0.5f / float3(volumeSize);
		float2 uvMin = texelCenter.xy;
		float2 uvMax = 1.0f.xx - texelCenter.xy;
#if defined(VR)
		uvMin.x = (eyeIndex == 0u ? 0.0f : 0.5f) + texelCenter.x;
		uvMax.x = (eyeIndex == 0u ? 0.5f : 1.0f) - texelCenter.x;
#endif
		// Integrated values live at slice back faces, not at material sample centers.
		float z = clamp(slice - texelCenter.z, texelCenter.z, 1.0f - texelCenter.z);
		float4 fog = volume.SampleLevel(SampColorSampler, float3(clamp(volumeUV, uvMin, uvMax), z), 0);
		float startDistance = GetVolumetricStartDistance();
		float3 gridZ = GetVolumetricGridZParams(float(volumeSize.z));
		float firstBackDepth = (exp2(1.0f / gridZ.z) - gridZ.y) / max(gridZ.x, 1e-20f);
		float nearWeight = saturate((sceneDepth - startDistance) / max(firstBackDepth - startDistance, EPSILON_DIVISION));
		return lerp(float4(0.0f.xxx, 1.0f), fog, nearWeight);
	}

	float4 SampleFarFogGrid(float2 volumeUV, float sceneDepth, uint eyeIndex)
	{
		uint3 volumeSize;
		ExponentialHeightFogIntegratedLightScatteringFar.GetDimensions(volumeSize.x, volumeSize.y, volumeSize.z);
		float nearGridEnd = GetVolumetricNearGridEndDistance();
		if (any(volumeSize == 0u) || nearGridEnd + 1.0f >= GetVolumetricTotalFarPlane() || sceneDepth <= nearGridEnd)
			return float4(0.0f.xxx, 1.0f);

		float slice = ComputeVolumetricFarNormalizedSlice(sceneDepth, float(volumeSize.z));
		float3 texelCenter = 0.5f / float3(volumeSize);
		float2 uvMin = texelCenter.xy;
		float2 uvMax = 1.0f.xx - texelCenter.xy;
#if defined(VR)
		uvMin.x = (eyeIndex == 0u ? 0.0f : 0.5f) + texelCenter.x;
		uvMax.x = (eyeIndex == 0u ? 0.5f : 1.0f) - texelCenter.x;
#endif
		float z = clamp(slice - texelCenter.z, texelCenter.z, 1.0f - texelCenter.z);
		float4 fog = ExponentialHeightFogIntegratedLightScatteringFar.SampleLevel(SampColorSampler, float3(clamp(volumeUV, uvMin, uvMax), z), 0);
		float3 gridZ = GetVolumetricFarGridZParams(float(volumeSize.z));
		float firstBackDepth = (exp2(1.0f / gridZ.z) - gridZ.y) / max(gridZ.x, 1e-20f);
		float nearWeight = saturate((sceneDepth - nearGridEnd) / max(firstBackDepth - nearGridEnd, EPSILON_DIVISION));
		return lerp(float4(0.0f.xxx, 1.0f), fog, nearWeight);
	}

	float4 SampleFogGrids(uint3 volumeSize, float2 volumeUV, float sceneDepth, uint eyeIndex)
	{
		float4 nearFog = SampleFogGrid(ExponentialHeightFogIntegratedLightScattering, volumeSize, volumeUV, sceneDepth, eyeIndex);
		float4 farFog = SampleFarFogGrid(volumeUV, sceneDepth, eyeIndex);
		return float4(nearFog.rgb + nearFog.a * farFog.rgb, nearFog.a * farFog.a);
	}

	float4 CompositeFogScattering(float4 analyticalFog, float4 volume)
	{
		float opacity = saturate(1.0f - volume.a * (1.0f - analyticalFog.a));
		// Both RGB inputs already include opacity; multiplying it again darkens the analytical tail.
		float3 premultiplied = volume.rgb + volume.a * analyticalFog.rgb;
		return float4(opacity > EPSILON_DIVISION ? premultiplied / opacity : 0.0f.xxx, opacity);
	}

	float4 CombineVolumetricFog(float4 analyticalFog, float3 positionWS, uint eyeIndex, uint3 volumeSize)
	{
		float2 volumeUV;
		float sceneDepth = GetSceneDepthForFog(positionWS, eyeIndex, volumeUV);
#if defined(VR)
		volumeUV = Stereo::ConvertToStereoUV(volumeUV, eyeIndex);
#endif
		float4 volume = sceneDepth > 0.0f ? SampleFogGrids(volumeSize, volumeUV, sceneDepth, eyeIndex) : float4(0.0f.xxx, 1.0f);
		return CompositeFogScattering(analyticalFog, volume);
	}

	float4 CombineVolumetricFog(float4 analyticalFog, float4 screenPosition, uint3 volumeSize)
	{
		float2 volumeUV = screenPosition.xy * SharedData::BufferDim.zw;
		uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(volumeUV);
		float sceneDepth = SharedData::GetScreenDepth(screenPosition.z);
		float2 coarseUV = volumeUV;
		if (SharedData::exponentialHeightFogSettings.volumetricUpsampleJitterMultiplier > 0.0f) {
			float2 noiseCoord = Stereo::EyeStableNoiseCoord(screenPosition.xy, SharedData::BufferDim.xy);
			float2 noise = float2(Random::InterleavedGradientNoise(noiseCoord, SharedData::FrameCount),
				Random::InterleavedGradientNoise(noiseCoord.yx + 19.19f, SharedData::FrameCount));
			coarseUV += (noise * 2.0f - 1.0f) * SharedData::exponentialHeightFogSettings.volumetricUpsampleJitterMultiplier /
			            max(float2(volumeSize.xy), 1.0f.xx);
		}
		float4 volume = SampleFogGrids(volumeSize, coarseUV, sceneDepth, eyeIndex);
		return CompositeFogScattering(analyticalFog, volume);
	}

	float4 GetExponentialHeightFogInternal(float3 positionWS, float3 cameraWS, float3 fogColor, bool useScreenPosition, float4 screenPosition, bool applyVolumetricFog)
	{
		float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
		float fogDensity = GetHeightFogDensity();
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0)
			fogDensity *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
		float fogHeightFalloff2 = SharedData::exponentialHeightFogSettings.fogHeightFalloff2 * 0.001f;
		float fogDensity2 = SharedData::exponentialHeightFogSettings.fogDensity2 * 0.001f;
		uint eyeIndex = GetEyeIndexFromCameraWS(cameraWS);
		if (fogDensity <= 0.0f && fogDensity2 <= 0.0f && SharedData::exponentialHeightFogSettings.distanceHazeMaxOpacity <= 0.0f)
			return 0.0f.xxxx;
		float3 viewToPos = positionWS;
		uint3 volumeSize = 0u.xxx;
		applyVolumetricFog = applyVolumetricFog && (fogDensity > 0.0f || fogDensity2 > 0.0f);
		if (applyVolumetricFog)
			applyVolumetricFog = ShouldApplyVolumetricFog(volumeSize);

		float viewToPosLength = length(viewToPos);
		float viewToPosLengthInv = rcp(max(viewToPosLength, 1e-4f));

		float rayOriginTerms = fogDensity * exp2(-fogHeightFalloff * max(cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight, 0));
		float rayOriginTerms2 = fogDensity2 * exp2(-fogHeightFalloff2 * max(cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight2, 0));
		float rayLength = viewToPosLength;
		float rayDirectionZ = viewToPos.z;

		float excludeDistance = SharedData::exponentialHeightFogSettings.startDistance;
		if (applyVolumetricFog) {
			float2 volumeUV;
			float sceneDepth = GetSceneDepthForFog(positionWS, eyeIndex, volumeUV);
			float cosAngle = sceneDepth * viewToPosLengthInv;
			float invCosAngle = cosAngle > 0.001f ? rcp(cosAngle) : 0.0f;
			excludeDistance = max(excludeDistance, GetVolumetricEndDistance() * invCosAngle);
		}

		if (excludeDistance > 0) {
			excludeDistance = min(excludeDistance, viewToPosLength);
			float excludeIntersectionTime = excludeDistance * viewToPosLengthInv;
			float cameraToExclusionIntersectionZ = excludeIntersectionTime * viewToPos.z;
			float exclusionIntersectionZ = cameraWS.z + cameraToExclusionIntersectionZ;
			rayLength = (1.0f - excludeIntersectionTime) * viewToPosLength;
			rayDirectionZ = viewToPos.z - cameraToExclusionIntersectionZ;
			float exponent = fogHeightFalloff * max(exclusionIntersectionZ - SharedData::exponentialHeightFogSettings.fogHeight, 0);
			rayOriginTerms = fogDensity * exp2(-exponent);
			float exponent2 = fogHeightFalloff2 * max(exclusionIntersectionZ - SharedData::exponentialHeightFogSettings.fogHeight2, 0);
			rayOriginTerms2 = fogDensity2 * exp2(-exponent2);
		}

		float falloff = fogHeightFalloff * rayDirectionZ;
		float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
		float lineIntegralTaylor = 0.69314718056f - 0.24022650695f * falloff;  // log(2) - (0.5 * (log(2)^2)) * falloff
		float falloff2 = fogHeightFalloff2 * rayDirectionZ;
		float lineIntegral2 = (1.0f - exp2(-falloff2)) / falloff2;
		float lineIntegralTaylor2 = 0.69314718056f - 0.24022650695f * falloff2;
		float exponentialHeightLineIntegralCalc =
			rayOriginTerms * (abs(falloff) > 0.01f ? lineIntegral : lineIntegralTaylor) +
			rayOriginTerms2 * (abs(falloff2) > 0.01f ? lineIntegral2 : lineIntegralTaylor2);
		float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength;

		float expFogFactor = (fogDensity > 0.0f || fogDensity2 > 0.0f) ? saturate(exp2(-exponentialHeightLineIntegral)) : 1.0f;

		float3 fogInscatteringColor = 0.0f.xxx;
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0) {
			fogInscatteringColor = GetFogAmbientColor(viewToPosLength);
		} else if (SharedData::exponentialHeightFogSettings.originalFogColorAmount > 0.0f) {
			if (!SharedData::InInterior) {
				fogColor = Color::Fog(SharedData::exponentialHeightFogSettings.vanillaFogNearColor.rgb);
#if defined(IBL)
				if (SharedData::iblSettings.EnableIBL)
					fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
#endif
			}
			fogInscatteringColor = fogColor * SharedData::exponentialHeightFogSettings.originalFogColorAmount;
		}
		fogInscatteringColor += Color::GamutTransform(SharedData::exponentialHeightFogSettings.fogInscatteringColor.rgb) * SharedData::exponentialHeightFogSettings.fogInscatteringColor.a;

#if defined(IBL) || defined(DYNAMIC_CUBEMAPS)
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings == 0) {
			float cubemapMipLevel = SharedData::exponentialHeightFogSettings.cubemapMipLevel;
			float3 cubemapDirection = normalize(lerp(positionWS, float3(0, 0, 1), saturate((cubemapMipLevel + 1) / 9)));
			float3 inscattering = 0.0;
			[branch] if (SharedData::InInterior)
			{
#	if defined(DYNAMIC_CUBEMAPS)
				if (SharedData::exponentialHeightFogSettings.useDynamicCubemaps > 0)
					inscattering = DynamicCubemaps::EnvTexture.SampleLevel(SampColorSampler, cubemapDirection, cubemapMipLevel).xyz;
#	endif
			}
			else
			{
#	if defined(IBL)
				if (SharedData::exponentialHeightFogSettings.useSkyIBL > 0)
					inscattering = ImageBasedLighting::GetSkyIBLColor(cubemapDirection);
#	endif
			}
			fogInscatteringColor += Color::ApplyLinearSrgbTint(inscattering, SharedData::exponentialHeightFogSettings.inscatteringTint.rgb) * SharedData::exponentialHeightFogSettings.inscatteringTint.a;
		}
#endif

		fogColor = fogInscatteringColor;

		float3 directionalInscattering = 0;

		float3 viewDirection = viewToPos * viewToPosLengthInv;

		// Calculate directional light inscattering using Henyey-Greenstein phase function
		if (SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier > 0) {
			float3 lightDirection = normalize(SharedData::DirLightDirection.xyz);
			float cosTheta = dot(lightDirection, viewDirection);
			float phase = HenyeyGreenstein(cosTheta, SharedData::exponentialHeightFogSettings.directionalInscatteringAnisotropy);
			float3 directionalLightInscattering = GetDirectionalLightColor() * phase;
			directionalInscattering = directionalLightInscattering * SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier;
			if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0)
				directionalInscattering *= SharedData::exponentialHeightFogSettings.fogLightingInfluence;
		}

		fogColor += directionalInscattering;
		float4 analyticalFog = float4(fogColor * (1.0f - expFogFactor), 1.0f - expFogFactor);
		float4 combinedFog;
		if (!applyVolumetricFog) {
			combinedFog = float4(analyticalFog.a > EPSILON_DIVISION ? analyticalFog.rgb / analyticalFog.a : 0.0f.xxx, analyticalFog.a);
		} else {
			combinedFog = useScreenPosition ? CombineVolumetricFog(analyticalFog, screenPosition, volumeSize) : CombineVolumetricFog(analyticalFog, positionWS, eyeIndex, volumeSize);
		}
		return ApplyDistanceHaze(combinedFog, fogColor, viewToPos);
	}

	float4 GetExponentialHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, false, 0.0f.xxxx, true);
	}

	float4 GetExponentialHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor, float4 screenPosition)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, true, screenPosition, true);
	}

	float4 GetExponentialHeightFogNoVolumetric(float3 positionWS, float3 cameraWS, float3 fogColor)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, false, 0.0f.xxxx, false);
	}

	float4 GetExponentialHeightFogNoVolumetric(float3 positionWS, float3 cameraWS, float3 fogColor, float4 screenPosition)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, true, screenPosition, false);
	}

	float GetSunlightFogAttenuation(float3 positionWS, float3 cameraWS)
	{
		float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
		float fogDensity = GetHeightFogDensity();
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0)
			fogDensity *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
		float fogHeightFalloff2 = SharedData::exponentialHeightFogSettings.fogHeightFalloff2 * 0.001f;
		float fogDensity2 = SharedData::exponentialHeightFogSettings.fogDensity2 * 0.001f;
		if (fogDensity <= 0.0f && fogDensity2 <= 0.0f) {
			return 1.0f;
		}

		float exponent = fogHeightFalloff * max(positionWS.z + cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight, 0.0f);
		float localDensity = fogDensity * exp2(-exponent);
		float exponent2 = fogHeightFalloff2 * max(positionWS.z + cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight2, 0.0f);
		float localDensity2 = fogDensity2 * exp2(-exponent2);

		float3 lightDir = SharedData::DirLightDirection.xyz;
		float lightDirZ = lightDir.z;

		float sunlightFogAttenuation = 0.0f;

		// Integral = Density * (1 - exp2(-slope * inf)) / slope
		if (lightDirZ > 0.001f) {
			float slope = max(fogHeightFalloff * lightDirZ, 1e-8f);
			float slope2 = max(fogHeightFalloff2 * lightDirZ, 1e-8f);
			float exponentialHeightLineIntegral = localDensity / slope + localDensity2 / slope2;
			sunlightFogAttenuation = saturate(exp2(-exponentialHeightLineIntegral));
		}

		float attenuationAmount = SharedData::exponentialHeightFogSettings.sunlightAttenuationAmount;
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0)
			attenuationAmount *= SharedData::exponentialHeightFogSettings.fogLightingInfluence;
		return lerp(1.0f, sunlightFogAttenuation, attenuationAmount);
	}
}
#endif
