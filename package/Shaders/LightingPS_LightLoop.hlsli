#if !defined(LOD)
#	if !defined(LIGHT_LIMIT_FIX)
[loop] for (uint lightIndex = 0; lightIndex < numLights; lightIndex++)
{
	float3 lightDirection = PointLightPosition[eyeIndex * numLights + lightIndex].xyz - input.WorldPosition.xyz;
	float lightDist = length(lightDirection);
	float intensityFactor = saturate(lightDist / PointLightPosition[lightIndex].w);
	if (intensityFactor == 1)
		continue;

	float intensityMultiplier = 1 - intensityFactor * intensityFactor;
	float3 lightColor = Color::PointLight(PointLightColor[lightIndex].xyz) * intensityMultiplier;
	float lightShadow = 1.f;
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) {
		if (lightIndex < numShadowLights) {
			lightShadow *= shadowColor[ShadowLightMaskSelect[lightIndex]];
		}
	}

	float3 normalizedLightDirection = normalize(lightDirection);

	DirectContext pointLightContext;
	DirectLightingOutput pointLightOutput;
#		if defined(TRUE_PBR)
	{
		float3 refractedLightDirection = normalizedLightDirection;
#			if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			if (dot(normalizedLightDirection, coatWorldNormal) > 0)
				refractedLightDirection = -refract(-normalizedLightDirection, coatWorldNormal, eta);
		}
#			endif
		pointLightContext = CreateDirectLightingContext(worldNormal.xyz, coatWorldNormal, vertexNormal.xyz, refractedViewDirection, viewDirection, refractedLightDirection, normalizedLightDirection, lightColor, lightShadow, lightShadow);
	}
#		else
	pointLightContext = CreateDirectLightingContext(worldNormal.xyz, vertexNormal.xyz, viewDirection, normalizedLightDirection, lightColor, lightShadow, lightShadow);
#			if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, normalizedLightDirection, screenNoise, eyeIndex);
		pointLightContext.hairShadow = hairShadow;
	}
#			endif
#		endif
	EvaluateLighting(pointLightContext, material, tbnTr, uvOriginal, uvOriginal_ddx, uvOriginal_ddy, pointLightOutput);
#		if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		EvaluateWetnessLighting(wetnessNormal, pointLightContext, waterRoughnessSpecular, pointLightOutput);
#		endif
	lightsDiffuseColor += pointLightOutput.diffuse;
	lightsSpecularColor += pointLightOutput.specular;
#		if defined(TRUE_PBR)
	coatLightsDiffuseColor += pointLightOutput.coatDiffuse;
#		endif
	transmissionColor += pointLightOutput.transmission;
}

#	else

uint numClusteredLights = 0;
uint totalLightCount = LightLimitFix::NumStrictLights;
uint clusterIndex = 0;
uint lightOffset = 0;
if (inWorld && LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
	numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
	totalLightCount += numClusteredLights;
	lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
}

#		if defined(LLFDEBUG)
LightLimitFix::LLFDebugInfo llfDebug = LightLimitFix::LLFDebugInfoInit();
#		endif

#		if defined(DEFERRED)
// Contact-shadow setup, gated on the runtime toggle so we don't pay the
// noise hash + step-count math for every pixel when the feature is off
// (it defaults off). The step count and noise are reused across every
// clustered light in this pixel so we hoist them out of the per-light loop.
uint contactShadowSteps = 0;
float contactShadowNoise = 0.0;
[branch] if (SharedData::lightLimitFixSettings.EnableContactShadows)
{
	contactShadowSteps = round(SharedData::lightLimitFixSettings.ContactShadowMaxSteps *
							   (1.0 - saturate(viewPosition.z / SharedData::lightLimitFixSettings.ContactShadowMaxDistance)));
	// The helper stays stereo-stable in VR — see
	// LightLimitFix::GetContactShadowNoiseCoord for the eye-buffer math.
	contactShadowNoise = Random::InterleavedGradientNoise(
		LightLimitFix::GetContactShadowNoiseCoord(input.Position.xy, screenUV),
		SharedData::FrameCount);
}
#		endif

[loop] for (uint lightIndex = 0; lightIndex < totalLightCount; lightIndex++)
{
	LightLimitFix::Light light;
	if (lightIndex < LightLimitFix::NumStrictLights) {
		light = LightLimitFix::StrictLights[lightIndex];
	} else {
		uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + (lightIndex - LightLimitFix::NumStrictLights)];
		light = LightLimitFix::lights[clusteredLightIndex];

		if (LightLimitFix::IsLightIgnored(light))
			continue;
	}

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
	float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * intensityMultiplier * light.fade;
	float lightShadow = 1.0;

	float shadowComponent = 1.0;
	bool shadowCoverage = false;
	if (inWorld && !inReflection) {
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
			shadowComponent = LightLimitFix::GetShadowLightShadow(light.shadowMapIndex, worldPositionWS, rotationMatrix, shadowCoverage);
			lightShadow *= shadowComponent;
		}
	}

#		if defined(LLFDEBUG)
	uint llfShadowType = (light.lightFlags & LightLimitFix::LightFlags::Shadow &&
							 light.shadowMapIndex < SharedData::lightLimitFixSettings.ShadowMapSlots) ?
	                         (uint)LightLimitFix::Shadows[light.shadowMapIndex].ShadowLightParam.x :
	                         0;
	LightLimitFix::LLFDebugAccumulate(llfDebug, light, shadowComponent, shadowCoverage, llfShadowType);
#		endif

	float3 normalizedLightDirection = normalize(lightDirection);
	float lightAngle = dot(worldNormal.xyz, normalizedLightDirection.xyz);

	float contactShadow = 1.0;

#		if defined(DEFERRED)
	// Outer guard: contactShadowSteps > 0 covers both "feature off" and "pixel past
	// MaxDistance", so all per-light intensity-gate math is paid only when a raymarch
	// is actually possible. Without this, the falloff math fires for every clustered
	// light even in the default-off case.
	[branch] if (contactShadowSteps > 0)
	{
		// Strict lights always raymarch -- skip the falloff math for them entirely.
		// Clustered lights need a normalized falloff to compare against MinIntensity;
		// derive it from intensityMultiplier on the non-ISL path (where it IS already
		// 1 - (d/r)^2) and re-compute on the ISL path (where GetAttenuation isn't
		// [0,1]-normalized, so the threshold would mean different things otherwise).
		const bool isClusteredLight = lightIndex >= LightLimitFix::NumStrictLights;
		bool passesIntensityGate = !isClusteredLight;
		if (isClusteredLight) {
#			if defined(ISL)
			float falloffFactor = saturate(lightDist * light.invRadius);
			passesIntensityGate = (1.0 - falloffFactor * falloffFactor) >
			                      SharedData::lightLimitFixSettings.ContactShadowMinIntensity;
#			else
			passesIntensityGate = intensityMultiplier >
			                      SharedData::lightLimitFixSettings.ContactShadowMinIntensity;
#			endif
		}

		// Particle lights carry both Simple and Particle bits. Simple-only lights are
		// clustered fallbacks (no real emitter) and never trace; particle lights trace
		// only when the user opted in via EnableParticleContactShadows.
		const bool isParticleLight = (light.lightFlags & LightLimitFix::LightFlags::Particle) != 0;
		const bool canShadow = isParticleLight ?
		                           SharedData::lightLimitFixSettings.EnableParticleContactShadows :
		                           !(light.lightFlags & LightLimitFix::LightFlags::Simple);
		[branch] if (
			canShadow &&
			shadowComponent != 0.0 &&
			lightAngle > 0.0 &&
			passesIntensityGate)
		{
			// Derive view-space position via CameraView; the Light struct only carries positionWS
			// (camera-relative) so the matrix multiply here is the cheapest path until positionVS
			// is added to the struct + populated CPU-side.
			float3 lightPositionVS = mul(FrameBuffer::CameraView[eyeIndex], float4(light.positionWS[eyeIndex].xyz, 1)).xyz;
			float3 normalizedLightDirectionVS = normalize(lightPositionVS - viewPosition.xyz);
			contactShadow = LightLimitFix::ContactShadows(viewPosition, contactShadowNoise, normalizedLightDirectionVS, contactShadowSteps, eyeIndex);
		}
	}
#		endif

	float3 refractedLightDirection = normalizedLightDirection;
#		if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
	{
		if (dot(normalizedLightDirection, coatWorldNormal) > 0)
			refractedLightDirection = -refract(-normalizedLightDirection, coatWorldNormal, eta);
	}
#		endif

	float parallaxShadow = 1;

#		if defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
	[branch] if (
		SharedData::extendedMaterialSettings.EnableShadows &&
		!(light.lightFlags & LightLimitFix::LightFlags::Simple) &&
		lightAngle > 0.0 &&
		shadowComponent != 0.0 &&
		contactShadow != 0.0)
	{
		float3 lightDirectionTS = normalize(mul(refractedLightDirection, tbn).xyz);
#			if defined(PARALLAX)
		[branch] if (SharedData::extendedMaterialSettings.EnableParallax)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams);
#			elif defined(LANDSCAPE)
#				if defined(TRUE_PBR)
		if (SharedData::extendedMaterialSettings.EnableParallax)
#				else
		if (SharedData::extendedMaterialSettings.EnableTerrainParallax || (SharedData::extendedMaterialSettings.EnableParallax && Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement))
#				endif
#				if defined(TERRAIN_VARIATION)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, lightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams, sharedOffset, dx, dy);
#				else
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, lightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams);
#				endif
#			elif defined(EMAT_ENVMAP)
		[branch] if (complexMaterialParallax)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexEnvMaskSampler, SampEnvMaskSampler, 3, parallaxShadowQuality, screenNoise, displacementParams);
#			elif defined(TRUE_PBR) && !defined(LODLANDSCAPE) && !defined(FACEGEN)
		[branch] if (PBRParallax)
			parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams);
#			endif
	}
#		endif  // defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

	DirectContext pointLightContext;
	DirectLightingOutput pointLightOutput;
	float pointLightShadow = lightShadow * parallaxShadow * contactShadow;
#		if defined(TRUE_PBR)
	pointLightContext = CreateDirectLightingContext(worldNormal.xyz, coatWorldNormal, vertexNormal.xyz, refractedViewDirection, viewDirection, refractedLightDirection, normalizedLightDirection, lightColor, pointLightShadow, pointLightShadow);
#		else
	pointLightContext = CreateDirectLightingContext(worldNormal.xyz, vertexNormal.xyz, viewDirection, normalizedLightDirection, lightColor, pointLightShadow, pointLightShadow);
#			if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, normalizedLightDirection, screenNoise, eyeIndex);
		pointLightContext.hairShadow = hairShadow;
	}
#			endif
#		endif
	EvaluateLighting(pointLightContext, material, tbnTr, uvOriginal, uvOriginal_ddx, uvOriginal_ddy, pointLightOutput);
#		if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		EvaluateWetnessLighting(wetnessNormal, pointLightContext, waterRoughnessSpecular, pointLightOutput);
#		endif

	lightsDiffuseColor += pointLightOutput.diffuse;
	lightsSpecularColor += pointLightOutput.specular;
#		if defined(TRUE_PBR)
	coatLightsDiffuseColor += pointLightOutput.coatDiffuse;
#		endif
	transmissionColor += pointLightOutput.transmission;
}
#	endif
#endif
