#include "WaterPS_Resources.hlsli"
// Resources must precede Helpers (the comment also blocks clang-format include sorting).
#include "WaterPS_Helpers.hlsli"

#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#endif

#if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	uint eyeIndex = Stereo::GetEyeIndexPS(input.HPosition, VPOSOffset);
	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

#if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
	float3 viewDirection = normalize(input.WPosition.xyz);

	float distanceFactor = saturate(lerp(FrameBuffer::FrameParams.w, 1, (length(input.WPosition.xyz) - 8192) / (WaterParams.x - 8192)));
	float4 distanceMul = saturate(lerp(VarAmounts.z, 1, -(distanceFactor - 1))).xxxx;
	float distanceBlendFactor = distanceFactor;
#	if defined(UNIFIED_WATER)
	distanceBlendFactor = 1.0f;
#	endif

	bool isSpecular = false;

	float depth = 0;

#	if defined(DEPTH)
#		if defined(VERTEX_ALPHA_DEPTH)
#			if defined(VC)
	distanceMul = saturate(input.TexCoord3.z);
#			endif
#		else
	distanceMul = 0;

	depth = GetScreenDepthWater(screenPosition);
	float2 depthOffset =
		FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
#			if !defined(VR)
	float depthMul = length(float3((depthOffset * 2 - 1) * depth / ProjData.xy, depth));
#			else
	float depthMul = CalculateDepthMultFromUV(Stereo::ConvertFromStereoUV(depthOffset, eyeIndex, 1), depth, eyeIndex);
#			endif  //VR
	float3 depthAdjustedViewDirection = -viewDirection * depthMul;
	float viewSurfaceAngle = dot(depthAdjustedViewDirection, ReflectPlane[eyeIndex].xyz);

	float planeMul = (1 - ReflectPlane[eyeIndex].w / viewSurfaceAngle);
	distanceMul = saturate(
		planeMul * float4(length(depthAdjustedViewDirection).xx, abs(viewSurfaceAngle).xx) /
		FogParam.z);
#		endif
#	endif

#	if defined(UNDERWATER)
	float4 depthControl = float4(0, 1, 1, 0);
#	elif defined(LOD)
	float4 depthControl = float4(1, 0, 0, 1);
#	elif defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float4 depthControl = float4(0, 0, 1, 0);
#	else
	float4 depthControl = DepthControl * (distanceMul - 1) + 1;
#	endif
	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

#	if defined(SKYLIGHTING)
	float wetnessOcclusion = 1.0;

#		if defined(VR)
	float3 positionMSSkylight = input.WPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
	float3 positionMSSkylight = input.WPosition.xyz;
#		endif

	sh2 skylightingSH = Skylighting::SampleNoBias(positionMSSkylight);
	float skylighting = SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1));

	float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, float3(0, 0, 1), Skylighting::GetFadeOutFactor(input.WPosition.xyz));

	wetnessOcclusion = inWorld ? pow(saturate(skylighting), 2) : 0;
#	endif

#	if defined(SKYLIGHTING)
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, eyeIndex, wetnessOcclusion);
#	else
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, eyeIndex, inWorld);
#	endif

	float3 normal = waterData.normal;

#	if defined(SKYLIGHTING)
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#	endif

	float fresnel = GetFresnelValue(normal, viewDirection);

#	if defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float3 finalColor = 0.0.xxx;

	[unroll] for (int lightIndex = 0; lightIndex < NUM_SPECULAR_LIGHTS; ++lightIndex)
	{
		float3 lightVector = LightPos[lightIndex].xyz - (PosAdjust[eyeIndex].xyz + input.WPosition.xyz);
		float3 lightDirection = normalize(normalize(lightVector) - viewDirection);
		float lightFade = saturate(length(lightVector) / LightPos[lightIndex].w);
		float lightColorMul = (1 - lightFade * lightFade);
		float LdotN = saturate(dot(lightDirection, normal));
		float3 lightColor = (Color::PointLight(LightColor[lightIndex].xyz) * pow(LdotN, FresnelRI.z)) * lightColorMul;
		finalColor += lightColor;
	}

	finalColor *= fresnel;
#		if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override specular color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorSpecular(waterData.rippleInfo, 2.5, 4.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#		endif

	isSpecular = true;
#	else

#		if defined(SKYLIGHTING)
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceFactor, skylightingSpecular);
#		else
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceFactor, 1.0);
#		endif

	DiffuseOutput diffuseOutput = GetWaterDiffuseColor(input, normal, viewDirection, distanceMul, depthControl.y, fresnel, eyeIndex, viewPosition, depth);

	float surfaceShadow;
	float dirShadow = ShadowSampling::Get3DFilteredShadow(input.WPosition.xyz, diffuseOutput.refractedViewDirection, input.HPosition.xy, eyeIndex, surfaceShadow);

	float3 dirColor;
	float3 ambientColor;
#		if defined(SKYLIGHTING) && !defined(INTERIOR)
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, dirColor, ambientColor, skylightingDiffuse);
#		else
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, dirColor, ambientColor);
#		endif

	dirColor *= dirShadow;

#		if defined(SKYLIGHTING)
	ambientColor = Color::IrradianceToLinear(ambientColor);
	ambientColor *= skylightingDiffuse;
	ambientColor = Color::IrradianceToGamma(ambientColor);
#		endif

	diffuseOutput.refractionDiffuseColor = dirColor + ambientColor;

	float3 diffuseColor = lerp(diffuseOutput.refractionColor, diffuseOutput.refractionDiffuseColor, diffuseOutput.refractionMul);

	depthControl = DepthControl * (distanceMul - 1) + 1;

	float3 specularLighting = 0;

#		if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;

	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			if (LightLimitFix::IsLightIgnored(light) || light.lightFlags & LightLimitFix::LightFlags::Shadow) {
				continue;
			}

			float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WPosition.xyz;
			float lightDist = length(lightDirection);

#			if defined(ISL)
			float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
#			else
			float intensityFactor = saturate(lightDist / light.radius);
			float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#			endif

			float3 normalizedLightDirection = normalize(lightDirection);

			float3 H = normalize(normalizedLightDirection - viewDirection);
			float HdotN = saturate(dot(H, normal));

			const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
			float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * pow(HdotN, FresnelRI.z) * light.fade;
			specularLighting += lightColor * intensityMultiplier;
		}
	}
	specularColor += specularLighting * 3;
#		endif

#		if defined(UNDERWATER)
	float3 finalSpecularColor = lerp(Color::Water(ShallowColor.xyz), specularColor, 0.5);
	float3 finalColor = saturate(1 - length(input.WPosition.xyz) * 0.002) * ((1 - fresnel) * (diffuseColor - finalSpecularColor)) + finalSpecularColor;
	// Add ripple and splash color effects for underwater
#			if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization (darker for underwater)
	float3 debugColor = WetnessEffects::GetDebugWetnessColorUnderwater(waterData.rippleInfo, 1.5, 2.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#			endif
#		else

	float3 sunColor = GetSunColor(normal, viewDirection, input.WPosition.xyz, eyeIndex) * surfaceShadow;

#			if defined(VC)
	float specularFraction = lerp(1, fresnel * diffuseOutput.refractionMul, distanceBlendFactor);
	float3 finalColorPreFog = lerp(diffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;

#				if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 fogColor = Color::Fog(input.FogParam.xyz);
#				else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 fogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogDistanceFactor));
#				endif

	fogDistanceFactor = Color::FogAlpha(fogDistanceFactor);

#				if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#				endif
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			fogColor = exponentialHeightFog.xyz;
			fogColor *= GetWaterFogFade(eyeIndex);
			finalColorPreFog = lerp(finalColorPreFog, fogColor, exponentialHeightFog.w);
		} else {
			fogColor *= GetWaterFogFade(eyeIndex);
			finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
			float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade(eyeIndex);
			finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
		}
	} else {
		fogColor *= GetWaterFogFade(eyeIndex);
		finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
	}
#				else
	fogColor *= GetWaterFogFade(eyeIndex);
	finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
#				endif

	float3 finalColor = finalColorPreFog;

#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif

#			else
	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float3 finalColorPreFog = lerp(diffuseOutput.refractionDiffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;

#				if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 preFogColor = Color::Fog(input.FogParam.xyz);
#				else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 preFogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogDistanceFactor));
#				endif

	fogDistanceFactor = Color::FogAlpha(fogDistanceFactor);

#				if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		preFogColor = ImageBasedLighting::GetFogIBLColor(preFogColor);
	}
#				endif
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, preFogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			preFogColor = exponentialHeightFog.xyz;
			preFogColor *= GetWaterFogFade(eyeIndex);
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, exponentialHeightFog.w);
		} else {
			preFogColor *= GetWaterFogFade(eyeIndex);
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
			float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade(eyeIndex);
			finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
		}
	} else {
		preFogColor *= GetWaterFogFade(eyeIndex);
		finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
	}
#				else
	preFogColor *= GetWaterFogFade(eyeIndex);

	finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
#				endif

	float3 refractionColor = diffuseOutput.refractionColor;

	float fogFactor = min(FogParam.w, pow(saturate(-diffuseOutput.depth * FogParam.y - FogParam.x), FogParam.z));
	float3 fogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogFactor));
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled && ExponentialHeightFog::ShouldDisableVanillaFog()) {
		fogFactor = 0;
	}
#				endif
#				if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#				endif
	refractionColor = lerp(refractionColor, fogColor, Color::FogAlpha(fogFactor));

	float3 finalColor = lerp(refractionColor, finalColorPreFog, diffuseOutput.refractionMul);
#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif
#			endif

#		endif
#	endif
	psout.Lighting = float4(finalColor, isSpecular);
#endif

#if defined(STENCIL)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	float3 normal =
		normalize(cross(ddx_coarse(input.WorldPosition.xyz), ddy_coarse(input.WorldPosition.xyz)));
	float VdotN = dot(viewDirection, normal);
	psout.WaterMask = float4(0, 0, VdotN, 0);

	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);
#endif

	return psout;
}
