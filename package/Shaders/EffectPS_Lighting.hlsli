float3 GetEffectAmbientLighting(float skylightingDiffuse)
{
	float3 ambientColor = ShadowSampling::GetRawAmbientLighting(ShadowSampling::LightingSampleNormal);

#if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
#	if defined(SKYLIGHTING)
		ambientColor = ImageBasedLighting::GetDiffuseIBLOccluded(ambientColor, ShadowSampling::ImageBasedLightingNormal, skylightingDiffuse);
#	else
		ambientColor = ImageBasedLighting::GetDiffuseIBL(ambientColor, ShadowSampling::ImageBasedLightingNormal);
#	endif
	}
#endif

	return ambientColor;
}

void ExtractEffectLighting(float3 inputColor, out float3 dirColor, out float3 ambientColor, float skylightingDiffuse)
{
	float3 ambientColorAmb = GetEffectAmbientLighting(skylightingDiffuse);
	float3 dirLightColorDir = ShadowSampling::GetDirectionalLighting();

	float inputLuma = Color::RGBToLuminance(inputColor);
	float ambientLuma = Color::RGBToLuminance(ambientColorAmb);
	float dirLightLuma = Color::RGBToLuminance(dirLightColorDir);

	float totalLuma = ambientLuma + dirLightLuma;

	if (totalLuma > 0.0 && ambientLuma > 0.0)
		ambientColorAmb *= inputLuma / totalLuma;

	float3 dirLightColorAmb = max(0.0, inputColor - ambientColorAmb);

	dirColor = dirLightColorAmb;
	ambientColor = ambientColorAmb;
}

#if defined(LIGHTING)
float3 GetLightingColor(float3 msPosition, float3 worldPosition, float2 screenPosition, uint eyeIndex, inout float shadowVariance)
{
	float3 color = DLightColor.xyz * Color::EffectLightingMult();
	bool suppressExternalEmittance = SharedData::InInterior && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::SuppressExternalEmittance);
	if (suppressExternalEmittance) {
		color = GetEffectAmbientLighting(1.0) + ShadowSampling::GetDirectionalLighting();
	}

#	if defined(SKYLIGHTING)
	float skylightingDiffuse = 1.0;
	if (!SharedData::InInterior) {
#		if defined(VR)
		float3 positionMSSkylight = worldPosition + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
		float3 positionMSSkylight = worldPosition;
#		endif

		sh2 skylightingSH = Skylighting::SampleNoBias(positionMSSkylight);
		skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, float3(0, 0, 1), Skylighting::GetFadeOutFactor(positionMSSkylight));
	}
#	endif

	float3 dirColor;
	float3 ambientColor;
#	if defined(SKYLIGHTING)
	ExtractEffectLighting(color, dirColor, ambientColor, skylightingDiffuse);
#	else
	ExtractEffectLighting(color, dirColor, ambientColor, 1.0);
#	endif

	float3 viewDirection = normalize(worldPosition.xyz);

	float unusedSurfaceShadow;
	float dirShadow = 1.0;

	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

	if (inWorld && ShadowSampling::HasDirectionalShadows())
		dirShadow = ShadowSampling::Get3DFilteredShadow(worldPosition.xyz, viewDirection, screenPosition, eyeIndex, unusedSurfaceShadow);

	shadowVariance = 1.0 - sqrt(saturate(fwidth(dirShadow)));

	dirColor *= dirShadow;

#	if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	}
#	endif

#	if defined(SKYLIGHTING)
#		if defined(IBL)
	if (!SharedData::iblSettings.EnableIBL)
#		endif
	{
		ambientColor = Color::IrradianceToLinear(ambientColor);
		ambientColor *= skylightingDiffuse;
		ambientColor = Color::IrradianceToGamma(ambientColor);
	}
#	endif

	color = dirColor + ambientColor;

#	if defined(LIGHT_LIMIT_FIX)
	if (!(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld))
#	endif
	{
		float4 lightDistanceSquared = (PLightPositionX[eyeIndex] - msPosition.xxxx) * (PLightPositionX[eyeIndex] - msPosition.xxxx) + (PLightPositionY[eyeIndex] - msPosition.yyyy) * (PLightPositionY[eyeIndex] - msPosition.yyyy) + (PLightPositionZ[eyeIndex] - msPosition.zzzz) * (PLightPositionZ[eyeIndex] - msPosition.zzzz);
		float4 lightFadeMul = 1.0.xxxx - saturate(PLightingRadiusInverseSquared * lightDistanceSquared);
		color.x += dot(Color::PointLight(PLightColorR.xxx).x * lightFadeMul * Color::EffectLightingMult(), 1.0.xxxx);
		color.y += dot(Color::PointLight(PLightColorG.xxx).x * lightFadeMul * Color::EffectLightingMult(), 1.0.xxxx);
		color.z += dot(Color::PointLight(PLightColorB.xxx).x * lightFadeMul * Color::EffectLightingMult(), 1.0.xxxx);
	}

	return color;
}
#else
float3 GetLightingShadow(float3 color, float3 worldPosition, float2 screenPosition, float depth, uint eyeIndex, inout float shadowVariance, float noise)
{
	float3 dirColor;
	float3 ambientColor;
	float skylightingDiffuse = 1.0;
#	if defined(SKYLIGHTING)
	ExtractEffectLighting(color, dirColor, ambientColor, skylightingDiffuse);
#	else
	ExtractEffectLighting(color, dirColor, ambientColor, 1.0);
#	endif

	static const uint sampleCount = 8;
	static const float rcpSampleCount = 1.0 / float(sampleCount);

	// Enough for sky statics
	float maxDistance = max(0, SharedData::GetScreenDepth(depth));
	float viewRayLength = 2048.0;
	float3 viewDirection = normalize(worldPosition);
	float3 startPosition = worldPosition - viewDirection * viewRayLength;
	float3 endPosition = worldPosition + viewDirection * min(maxDistance, viewRayLength);

	float shadow = 1.0;

	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

	if (inWorld && !SharedData::InInterior) {
		shadow = 0.0;
		for (uint i = 0; i < sampleCount; i++) {
			float t = (float(i) + noise) * rcpSampleCount;
			float3 samplePositionWS = lerp(startPosition, endPosition, t);
			shadow += ShadowSampling::GetWorldShadow(samplePositionWS, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);
		}
		shadow *= rcpSampleCount;
	}

	shadowVariance = 1.0 - sqrt(saturate(fwidth(shadow)));

	dirColor *= shadow;

#	if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	}
#	endif

	return dirColor + ambientColor;
}
#endif
