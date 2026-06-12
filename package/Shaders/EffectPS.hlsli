
#if !defined(VR)
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#endif  // !VR

cbuffer PerTechnique : register(b0)
{
	float4 CameraDataEffect : packoffset(c0);
	float2 VPOSOffset : packoffset(c1);
	float2 FilteringParam : packoffset(c1.z);
};

cbuffer PerMaterial : register(b1)
{
	float4 BaseColor : packoffset(c0);
	float4 BaseColorScale : packoffset(c1);
	float4 LightingInfluence : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#if !defined(VR)
	float4 PLightPositionX[1] : packoffset(c0);
	float4 PLightPositionY[1] : packoffset(c1);
	float4 PLightPositionZ[1] : packoffset(c2);
	float4 PLightingRadiusInverseSquared : packoffset(c3);
	float4 PLightColorR : packoffset(c4);
	float4 PLightColorG : packoffset(c5);
	float4 PLightColorB : packoffset(c6);
	float4 DLightColor : packoffset(c7);
	float4 PropertyColor : packoffset(c8);
	float4 AlphaTestRef : packoffset(c9);
	float4 MembraneRimColor : packoffset(c10);
	float4 MembraneVars : packoffset(c11);
#else
	float4 PLightPositionX[2] : packoffset(c0);
	float4 PLightPositionY[2] : packoffset(c2);
	float4 PLightPositionZ[2] : packoffset(c4);
	float4 PLightingRadiusInverseSquared : packoffset(c6);
	float4 PLightColorR : packoffset(c7);
	float4 PLightColorG : packoffset(c8);
	float4 PLightColorB : packoffset(c9);
	float4 DLightColor : packoffset(c10);
	float4 PropertyColor : packoffset(c11);  // VR should be 11; this could start earlier though
	float4 AlphaTestRef : packoffset(c12);
	float4 MembraneRimColor : packoffset(c13);
	float4 MembraneVars : packoffset(c14);
#endif
};

#define LinearSampler SampBaseSampler

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

#if defined(EXP_HEIGHT_FOG)
#	define SampColorSampler SampBaseSampler
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif

#include "Common/ShadowSampling.hlsli"

#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#endif

#if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

#include "EffectPS_Lighting.hlsli"

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout = (PS_OUTPUT)0;

#if !defined(VR)
	uint eyeIndex = 0;
#else
	uint eyeIndex = input.EyeIndex;
#endif  // !VR

	float4 fogMul = 1;
#if !defined(MULTBLEND)
	fogMul.xyz = input.FogAlpha;
#endif

#if defined(MEMBRANE)
#	if !defined(MOTIONVECTORS_NORMALS) && defined(ALPHA_TEST)
	float noiseAlpha = TexNoiseSampler.Sample(SampNoiseSampler, input.TexCoord0.zw).w;
#		if defined(VC)
	noiseAlpha *= input.Color.w;
#		endif
	if (noiseAlpha - AlphaTestRef.x < 0) {
		discard;
	}
#	endif

#	if defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS)
	float3 normal = input.ScreenSpaceNormal;
#	elif defined(NORMALS)
	float3 normal = input.TBN0;
#	else
	float3 normal = TexNormalSampler.Sample(SampNormalSampler, input.TexCoord0.zw).xzy * 2 - 1;
#		if defined(SKINNED)
	normal = mul(normal, transpose(float3x3(input.TBN0, input.TBN1, input.TBN2)));
#		endif
#	endif
	float NdotV = dot(normal, input.ViewVector.xyz);
	float membraneColorMul = pow(saturate(1 - NdotV), MembraneVars.x);
	float4 membraneColor = MembraneRimColor * membraneColorMul;
#elif defined(PROJECTED_UV) && defined(NORMALS)
	float2 noiseTexCoord = 0.00333333341 * input.TexCoord0.xy;
	float noise = TexNoiseSampler.Sample(SampNoiseSampler, noiseTexCoord).x * 0.2 + 0.4;
	if (dot(input.TBN0, input.TBN1) - noise < 0) {
		discard;
	}
#endif

	float softMul = 1;
	float depth = 1;
#if defined(SOFT)
	depth = TexDepthSamplerEffect.Load(int3(input.Position.xy, 0)).x;
	softMul = saturate(-input.TexCoord0.w + LightingInfluence.y / ((1 - depth) * CameraDataEffect.z + CameraDataEffect.y));
#endif

	float lightingInfluence = LightingInfluence.x;
	float3 propertyColor = Color::Effect(PropertyColor.xyz);
	float shadowVariance = 1.0;

	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

	float2 rotation;
	sincos(Math::TAU * screenNoise, rotation.y, rotation.x);
	float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);

#if defined(LIGHTING)
	propertyColor = GetLightingColor(input.MSPosition.xyz, input.WorldPosition.xyz, input.Position.xy, eyeIndex, shadowVariance);

#	if defined(LIGHT_LIMIT_FIX)
	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	bool inWorld = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld;

	uint numClusteredLights = 0;
	uint lightOffset = 0;
	uint clusterIndex = 0;
	uint numStrictLights = 0;
	if (inWorld) {
		// Gate strict lights behind inWorld too -- they live in
		// LightLimitFix::StrictLights which is populated from world-space
		// CB data. Including them on non-world passes (UI overlays, blood
		// splatter on screen-space surfaces, etc.) leaks world lighting
		// into effects that shouldn't be lit by point/spot lights at all.
		// Clustered lights are already inWorld-gated below; strict needs
		// the same treatment for symmetry.
		numStrictLights = LightLimitFix::NumStrictLights;
		if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
			numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
			lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		}
	}
	uint totalLightCount = numStrictLights + numClusteredLights;

	[loop] for (uint i = 0; i < totalLightCount; i++)
	{
		LightLimitFix::Light light;
		if (i < numStrictLights) {
			light = LightLimitFix::StrictLights[i];
		} else {
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + (i - numStrictLights)];
			light = LightLimitFix::lights[clusteredLightIndex];
			if (LightLimitFix::IsLightIgnored(light))
				continue;
		}

		// Effect meshes are alpha-blended and lack reliable occluder depth
		// at their visible surface, so sampling LLF's shadow atlas with the
		// effect's world position produces incorrect dark imprints from
		// nearby shadow-casting bulbs. Shadow-flagged lights still contribute
		// lighting through the other passes; only the effect-mesh shadow
		// attenuation is skipped here.
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow)
			continue;

		float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WorldPosition.xyz;
		float lightDist = length(lightDirection);

#		if defined(ISL)
		float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
		if (intensityMultiplier < 1e-5)
			continue;
#		else
		float intensityFactor = saturate(lightDist / light.radius);
		if (intensityFactor == 1)
			continue;
		float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#		endif

		const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
		float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * intensityMultiplier * 0.5 * light.fade * Color::EffectLightingMult();
		propertyColor += lightColor;
	}

#	endif
#elif defined(MEMBRANE)
	propertyColor *= 0;
	lightingInfluence = 0;
#endif

	float4 baseTexColor = float4(1, 1, 1, 1);
	float4 baseColor = float4(1, 1, 1, 1);
#if !defined(TEXTURE)
	[branch] if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
#endif
	{
		baseTexColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
		baseTexColor.xyz = Color::Effect(baseTexColor.xyz);
		baseColor *= baseTexColor;
		if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::IgnoreTexAlpha || Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha) {
			baseColor.w = 1;
		}
	}

#if defined(MEMBRANE)
	float4 baseColorMul = float4(1, 1, 1, 1);
#else
	float4 baseColorMul = BaseColor;
	baseColorMul.xyz = Color::Effect(baseColorMul.xyz);
#	if defined(VC) && !defined(PROJECTED_UV)
	baseColorMul *= float4(Color::Effect(input.Color.xyz), input.Color.w);
#	endif
#endif

#if defined(MEMBRANE)
	baseColor.w *= input.ViewVector.w;
#else
	baseColor.w *= input.TexCoord0.z;
#endif

	baseColor = baseColorMul * baseColor;
	baseColor.w *= softMul;

#if defined(SOFT) && !(defined(FALLOFF) && defined(MULTBLEND))
	if (baseColor.w - 0.003 < 0) {
		discard;
	}
#endif

	float alpha = baseColor.w;

#if defined(BLOOD)
	alpha = baseColor.y;
	float deltaY = saturate(baseColor.y - AlphaTestRef.x);
	float bloodMul = baseColor.z;
#	if defined(VC)
	bloodMul *= input.Color.w;
#	endif
	if (deltaY < AlphaTestRef.y) {
		bloodMul *= (deltaY / AlphaTestRef.y);
	}
	baseColor.xyz = saturate(float3(2, 1, 1) - bloodMul.xxx) * (-bloodMul * AlphaTestRef.z + 1);
#endif

	alpha *= PropertyColor.w;

	float baseColorScale = BaseColorScale.x;

#if defined(MEMBRANE)
	baseColor.xyz = (PropertyColor.xyz + baseColor.xyz) * alpha + membraneColor.xyz * membraneColor.w;
	alpha += membraneColor.w;
	baseColorScale = MembraneVars.z;
#endif

	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha)
		alpha = TexGrayscaleSampler.Sample(SampGrayscaleSampler, float2(baseTexColor.w, alpha)).w;

	[branch] if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor)
	{
		float2 grayscaleToColorUv = float2(baseTexColor.y, baseColorMul.x);
#if defined(MEMBRANE)
		grayscaleToColorUv.y = PropertyColor.x;
#endif
		baseColor.xyz = Color::Effect(baseColorScale * TexGrayscaleSampler.Sample(SampGrayscaleSampler, grayscaleToColorUv).xyz);
	}

	float3 lightColor = lerp(baseColor.xyz, propertyColor * baseColor.xyz, lightingInfluence);

#if !defined(MOTIONVECTORS_NORMALS)
	if (alpha * fogMul.w - AlphaTestRefRS < 0) {
		discard;
	}
#endif

#if !defined(LIGHTING) && defined(VC) && defined(TEXCOORD) && defined(NORMALS) && defined(TEXTURE) && defined(FALLOFF) && defined(SOFT)
	if (Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha && lightingInfluence == 1.0)
		lightColor = GetLightingShadow(lightColor, input.WorldPosition.xyz, input.Position.xy, depth, eyeIndex, shadowVariance, screenNoise);
#endif

	lightColor = Color::EffectMult(lightColor);

#if !defined(MOTIONVECTORS_NORMALS)
	float fogFactor = Color::FogAlpha(input.FogParam.w);
	float3 fogColor = Color::Fog(input.FogParam.xyz);
#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#	endif
#	if defined(EXP_HEIGHT_FOG)
	float vanillaFogFactor = fogFactor;
	float3 vanillaFogColor = fogColor;
	float expFogFactor = 0;
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		expFogFactor = exponentialHeightFog.w;
#		if defined(ADDBLEND) || defined(MULTBLEND) || defined(MULTBLEND_DECAL)
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
#		else
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
		alpha *= 1 - exponentialHeightFog.w;
#		endif
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			vanillaFogColor = lightColor;
			vanillaFogFactor = 0;
		}
	}
#	endif
#	if defined(ADDBLEND)
#		if defined(EXP_HEIGHT_FOG)
	float3 blendedColor = lightColor * (1 - vanillaFogFactor) * (1 - expFogFactor);
#		else
	float3 blendedColor = lightColor * (1 - fogFactor);
#		endif
#	elif defined(MULTBLEND) || defined(MULTBLEND_DECAL)
#		if defined(EXP_HEIGHT_FOG)
	float3 blendedColor = lerp(lightColor, 1.0.xxx, saturate(1.5 * vanillaFogFactor).xxx);
	blendedColor = lerp(blendedColor, 1.0.xxx, saturate(1.5 * expFogFactor).xxx);
#		else
	float3 blendedColor = lerp(lightColor, 1.0.xxx, saturate(1.5 * fogFactor).xxx);
#		endif
#	else
#		if defined(EXP_HEIGHT_FOG)
	float3 blendedColor = lerp(lightColor, vanillaFogColor, vanillaFogFactor.xxx);
	blendedColor = lerp(blendedColor, fogColor, fogFactor.xxx);
#		else
	float3 blendedColor = lerp(lightColor, fogColor, fogFactor.xxx);
#		endif
#	endif
#else
	float3 blendedColor = lightColor.xyz;
#endif

	alpha = Color::EffectAlpha(alpha);

	float4 finalColor = float4(blendedColor, alpha);
#if defined(MULTBLEND_DECAL)
	finalColor.xyz *= alpha;
#else
	finalColor *= fogMul;
#endif
	psout.Diffuse = finalColor;
#if defined(LIGHTING) && defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = Color::TurboColormap(0.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = Color::TurboColormap(0.0);
		} else {
			psout.Diffuse.xyz = Color::TurboColormap((float)lightCount / MAX_CLUSTER_LIGHTS);
		}
	}
#endif

#if defined(DEFERRED)

#	if defined(MOTIONVECTORS_NORMALS)
#		if (defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
	float3 screenSpaceNormal = normalize(input.TBN0);
#		else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#		endif
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), 0.0, psout.Diffuse.w);
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);
	psout.MotionVectors = float4(screenMotionVector, 0.0, psout.Diffuse.w);
#	endif

#	if defined(MULTBLEND) || defined(MULTBLEND_DECAL)
	psout.Specular = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Albedo = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Reflectance = float4(psout.Diffuse.xyz, finalColor.w);
	psout.Masks = float4(Color::RGBToLuminance(psout.Diffuse.xyz).xxx, finalColor.w);
#	else
	psout.Albedo = float4(0, 0, 0, finalColor.w);
	psout.Specular = float4(0, 0, 0, finalColor.w);
	psout.Reflectance = float4(0, 0, 0, finalColor.w);
	psout.Masks = float4(0, 0, 0, finalColor.w);
#	endif

#elif defined(MOTIONVECTORS_NORMALS)
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);
	psout.MotionVectors = screenMotionVector;

#	if (defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
	float3 screenSpaceNormal = normalize(input.TBN0);
#	else
	float3 screenSpaceNormal = normalize(input.ScreenSpaceNormal);
#	endif

	screenSpaceNormal.z = max(0.001, sqrt(8 + -8 * screenSpaceNormal.z));
	screenSpaceNormal.xy /= screenSpaceNormal.zz;
	psout.ScreenSpaceNormals.xy = screenSpaceNormal.xy + 0.5.xx;
	psout.ScreenSpaceNormals.zw = 0.0.xx;
#else
	psout.Color2 = finalColor;
#endif

#if !defined(HDR_OUTPUT)
	if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) && SharedData::linearLightingSettings.enableLinearLighting) {
		psout.Diffuse.xyz = Color::LinearToSrgb(psout.Diffuse.xyz);
	}
#endif
	return psout;
}
