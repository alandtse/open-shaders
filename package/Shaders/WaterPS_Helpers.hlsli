#if defined(VR)
/**
Calculates the depthMultiplier as used in Water.hlsl

VR appears to require use of CameraProjInverse and does not use ProjData
@param uv UV coords to convert
@param depth The calculated depth
@param eyeIndex The eyeIndex; 0 is left, 1 is right
@returns depthMultiplier
*/
float CalculateDepthMultFromUV(float2 uv, float depth, uint eyeIndex = 0)
{
	float4 temp;
	temp.xy = (uv * 2 - 1);
	temp.z = depth;
	temp.w = 1;
	temp = mul(FrameBuffer::CameraProjInverse[eyeIndex], temp.xyzw);
	temp.xyz /= temp.w;
	return length(temp.xyz);
}
#endif  // VR

#define SampColorSampler Normals01Sampler
#define LinearSampler Normals01Sampler

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(EXP_HEIGHT_FOG)
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif

#include "Common/ShadowSampling.hlsli"

#if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
float GetWaterFogFade(uint eyeIndex)
{
#	if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		return ExponentialHeightFog::GetVanillaFogFade(PosAdjust[eyeIndex].w);
	}
#	endif
	return PosAdjust[eyeIndex].w;
}

#	if defined(FLOWMAP)

/**
 * Structure containing complete flowmap information
 */
struct FlowmapData
{
	float4 color;       // Raw flowmap color (R=flow_x, G=flow_y, B=flow_strength, A=flow_mask)
	float2 flowVector;  // Flow vector (coordinate space depends on source function)
};

/**
 * Gets raw flowmap data before UV-space coordinate transformation
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for sampling the flowmap texture
 * @return FlowmapData with raw components:
 *         - color: Raw flowmap texture sample (RG=rotation, B=strength, A=mask)
 *         - flowVector: Base flow vector before any coordinate transformation
 *                      Ready for direct application of rotation matrix for world positioning
 *
 * @details This function provides flowmap data in its original coordinate space, suitable
 *          for world-space positioning effects (like ripple movement). The flowVector has
 *          NOT been transformed for UV-space normal sampling - that transformation is only
 *          applied in GetFlowmapDataUV() which uses transpose for UV coordinate perturbation.
 *
 *          Use this function when you need to apply the rotation matrix directly for
 *          world-space effects without needing to reverse any existing transformations.
 *
 * @see GetFlowmapDataUV() for UV-space normal sampling (applies transpose transformation)
 */
FlowmapData GetFlowmapDataTextureSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data;
	data.color = FlowMapTex.SampleLevel(FlowMapSampler, input.TexCoord2.zw + uvShift, 0);
	data.flowVector = (64 * input.TexCoord3.xy) * sqrt(1.01 - data.color.z);
	// NOTE: flowVector is NOT transformed yet - this is the raw vector before rotation matrix
	return data;
}
/**
 * Samples flowmap texture and calculates UV-space flow data for texture sampling
 *
 * @param input Pixel shader input containing texture coordinates and world position data
 * @param uvShift UV offset for sampling the flowmap texture (used for animation/variation)
 * @return FlowmapData Complete flowmap information with UV-space flow vector
 *
 * @details This function:
 *          - Samples the flowmap texture at the specified UV coordinates
 *          - Decodes flow direction from RG channels (remapped from [0,1] to [-1,1])
 *          - Calculates flow strength using the blue channel with sqrt falloff
 *          - Applies transpose rotation matrix to transform flow direction to UV space
 *          - Scales flow vector by world position and strength factors
 *
 * @note Flowmap format:
 *       - Red channel: Flow direction X component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Green channel: Flow direction Y component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Blue channel: Flow strength (0 = no flow, 1 = maximum flow)
 *       - Alpha channel: Flow mask/intensity multiplier
 */
FlowmapData GetFlowmapDataUV(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowSinCos = data.color.xy * 2 - 1;
	float2x2 flowRotationMatrix = float2x2(flowSinCos.x, flowSinCos.y, -flowSinCos.y, flowSinCos.x);
	data.flowVector = mul(transpose(flowRotationMatrix), data.flowVector);
	return data;
}
// ----------------------------------------------------------------
// Flowmap Parallax Functions
// ----------------------------------------------------------------

/**
 * Samples height from flowmap texture using the same 4-sample blend as flowmap normals
 * This ensures height transitions match the normal transitions exactly
 *
 * @param input PS_INPUT for flowmap coordinate access
 * @param normalMul The blend weights from the flowmap system (same as used for normals)
 * @param uvShift The UV shift value (1 / (128 * flowmapDimensions))
 * @param mipLevel Mip level for texture sampling
 */
float GetFlowmapHeightBlended(PS_INPUT input, float2 normalMul, float2 uvShift, float mipLevel)
{
	// Sample height using the EXACT same UV computation as GetFlowmapNormal
	// This ensures the height blending matches the normal blending perfectly

	// Sample 0: uvShift, multiplier=9.92, offset=0
	FlowmapData flowData0 = GetFlowmapDataUV(input, uvShift);
	float2 uv0 = 0 + (flowData0.flowVector - float2(9.92 * ((0.001 * ReflectionColor.w) * flowData0.color.w), 0));
	float height0 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv0, mipLevel).w;

	// Sample 1: float2(0, uvShift.y), multiplier=10.64, offset=0.27
	FlowmapData flowData1 = GetFlowmapDataUV(input, float2(0, uvShift.y));
	float2 uv1 = 0.27 + (flowData1.flowVector - float2(10.64 * ((0.001 * ReflectionColor.w) * flowData1.color.w), 0));
	float height1 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv1, mipLevel).w;

	// Sample 2: 0.0.xx, multiplier=8, offset=0
	FlowmapData flowData2 = GetFlowmapDataUV(input, 0.0.xx);
	float2 uv2 = 0 + (flowData2.flowVector - float2(8 * ((0.001 * ReflectionColor.w) * flowData2.color.w), 0));
	float height2 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv2, mipLevel).w;

	// Sample 3: float2(uvShift.x, 0), multiplier=8.48, offset=0.62
	FlowmapData flowData3 = GetFlowmapDataUV(input, float2(uvShift.x, 0));
	float2 uv3 = 0.62 + (flowData3.flowVector - float2(8.48 * ((0.001 * ReflectionColor.w) * flowData3.color.w), 0));
	float height3 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv3, mipLevel).w;

	// Use the EXACT same blending formula as flowmap normals
	float blendedHeight =
		normalMul.y * (normalMul.x * height2 + (1 - normalMul.x) * height3) +
		(1 - normalMul.y) * (normalMul.x * height1 + (1 - normalMul.x) * height0);

	return blendedHeight;
}

// Keep this for compatibility - just forwards to the proper function
float GetFlowmapHeightBarycentric(PS_INPUT input, float2 flowmapDimensions, float2 baseUV, float mipLevel)
{
	// This is now unused - we use GetFlowmapHeightBlended directly
	return FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, baseUV, mipLevel).w;
}

/**
 * Computes mip level for flowmap texture sampling
 */
float GetFlowmapMipLevel(float2 flowmapUV)
{
	float2 textureDims;
	FlowMapNormalsTex.GetDimensions(textureDims.x, textureDims.y);

#		if defined(VR)
	textureDims /= 16.0;
#		else
	textureDims /= 8.0;
#		endif

	float2 texCoordsPerSize = flowmapUV * textureDims;
	float2 dxSize = ddx(texCoordsPerSize);
	float2 dySize = ddy(texCoordsPerSize);
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;
	float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);
	return max(0.5 * log2(minTexCoordDelta), 0);
}

/**
 * Samples height from flowmap texture (riverflow.dds alpha channel)
 * Uses the same UV calculation as GetFlowmapNormal for consistency
 */

/**
 * Generates flowmap-based normal (no parallax - flowmap normals are not parallax-shifted)
 * Uses mip clamping to preserve detail at distance and prevent over-blurring
 */
float3 GetFlowmapNormal(PS_INPUT input, float2 uvShift, float multiplier, float offset)
{
	FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
	float2 uv = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	float2 dx = ddx(uv);
	float2 dy = ddy(uv);
	float mipLevel = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));
	mipLevel = clamp(mipLevel + SharedData::MipBias, 0, 5);

	float mipScale = exp2(-mipLevel);
	float2 scaledFlowVector = flowData.flowVector * mipScale;
	float2 scaledUv = offset + (scaledFlowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	return float3(FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, scaledUv, mipLevel).xy, flowData.color.z);
}

/**
 * Gets flowmap data with world-space flow vector for positioning effects
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for flowmap sampling (used for animation phases)
 * @return FlowmapData Complete flowmap information with world-space flow vector
 *
 * @details This function:
 *          - Samples raw flowmap data (before UV-space transformations)
 *          - Decodes flow direction from flowmap RG channels
 *          - Applies component-wise directional transformation
 *          - Returns complete flowmap data with world-space flow vector
 *
 * @note Use this for effects that need to move with water current (ripples, debris, foam, etc.)
 *       For UV-space normal sampling, use GetFlowmapDataUV() instead
 */
FlowmapData GetFlowmapDataWorldSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}

/**
 * Converts existing texture-space flowmap data to world-space (avoids duplicate sampling)
 *
 * @param textureSpaceData FlowmapData from GetFlowmapDataTextureSpace()
 * @return FlowmapData Complete flowmap data with world-space flow vector
 *
 * @note Use this overload when you already have texture-space flowmap data to avoid duplicate texture sampling
 */
FlowmapData GetFlowmapDataWorldSpace(FlowmapData textureSpaceData)
{
	FlowmapData data = textureSpaceData;
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}
#	endif

#	if defined(LOD)
#		undef WATER_EFFECTS
#		undef WETNESS_EFFECTS
#	endif

#	if defined(WATER_EFFECTS) && !defined(VC)
#		define WATER_PARALLAX
#		include "WaterEffects/WaterParallax.hlsli"
#	endif

#	if defined(DYNAMIC_CUBEMAPS)
#		include "DynamicCubemaps/DynamicCubemaps.hlsli"
#	endif

#	if defined(WETNESS_EFFECTS)
#		include "WetnessEffects/WetnessEffects.hlsli"
#	endif

// Structure to return both normal and ripple/splash color information
struct WaterNormalData
{
	float3 normal;
	float4 rippleInfo;  // xyz = scaled ripple normal (normalized normal * intensity), w = splash effect intensity
};

WaterNormalData GetWaterNormal(PS_INPUT input, float distanceFactor, float normalsDepthFactor, float3 viewDirection, float depth, uint eyeIndex, float wetnessOcclusion)
{
	WaterNormalData result;
	result.rippleInfo = float4(0, 0, 0, 0);

	float3 normalScalesRcp = rcp(input.NormalsScale.xyz);

#	if defined(WATER_PARALLAX)
	float2 parallaxOffset = WaterEffects::GetParallaxOffset(input, normalScalesRcp);
#	endif

#	if defined(FLOWMAP)
#		if defined(UNIFIED_WATER)
	float2 flowmapDimensions = input.TexCoord4.xy;
#		else
	float2 flowmapDimensions = input.TexCoord4.xx;
#		endif
	float2 uvShift = 1 / (128 * flowmapDimensions);

	// Compute flowmap parallax and create parallaxed input for normal sampling
	PS_INPUT flowmapInput = input;
	float2 flowmapParallaxOffset = float2(0, 0);
#		if defined(WATER_PARALLAX) && !defined(LOD)
	float parallaxAmount = WaterEffects::GetFlowmapParallaxAmount(input, flowmapDimensions, viewDirection);
	float2 parallaxDir = viewDirection.xy / -viewDirection.z;
	parallaxDir.y = -parallaxDir.y;
	float viewDotUp = -viewDirection.z;
	parallaxDir *= 0.008 * saturate(viewDotUp * 2.0);
	flowmapInput.TexCoord3.xy = input.TexCoord3.xy + parallaxAmount * parallaxDir;
	flowmapParallaxOffset = WaterEffects::GetFlowmapParallaxOffset(input, flowmapDimensions, viewDirection, normalScalesRcp);
#		endif

	// Calculate cell blend weights using parallaxed input
	float2 normalMul = 0.5 + -(-0.5 + abs(frac(flowmapInput.TexCoord2.zw * (64 * flowmapDimensions)) * 2 - 1));

	// Sample flowmap normals with parallax applied
	float3 flowmapNormal0 = GetFlowmapNormal(flowmapInput, uvShift, 9.92, 0);
	float3 flowmapNormal1 = GetFlowmapNormal(flowmapInput, float2(0, uvShift.y), 10.64, 0.27);
	float3 flowmapNormal2 = GetFlowmapNormal(flowmapInput, 0.0.xx, 8, 0);
	float3 flowmapNormal3 = GetFlowmapNormal(flowmapInput, float2(uvShift.x, 0), 8.48, 0.62);

	float2 flowmapNormalWeighted =
		normalMul.y * (normalMul.x * flowmapNormal2.xy + (1 - normalMul.x) * flowmapNormal3.xy) +
		(1 - normalMul.y) *
			(normalMul.x * flowmapNormal1.xy + (1 - normalMul.x) * flowmapNormal0.xy);
	float2 flowmapDenominator = sqrt(normalMul * normalMul + (1 - normalMul) * (1 - normalMul));
	float3 flowmapNormal =
		float3(((-0.5 + flowmapNormalWeighted) / (flowmapDenominator.x * flowmapDenominator.y)) *
				   max(0.4, normalsDepthFactor),
			0);
	flowmapNormal.z =
		sqrt(1 - flowmapNormal.x * flowmapNormal.x - flowmapNormal.y * flowmapNormal.y);
	float2 baseNormalUv = input.TexCoord1.xy;
#		if defined(WATER_PARALLAX)
	// Use flowmap-derived parallax offset for base normals
	baseNormalUv += flowmapParallaxOffset.xy * normalScalesRcp.x;
#		endif
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, baseNormalUv, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#	endif  // End of FLOWMAP block

#	if !defined(FLOWMAP)
#		if defined(WATER_PARALLAX)
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy + parallaxOffset.xy * normalScalesRcp.x, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#		else
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#		endif
#	endif  // End of !FLOWMAP block
#	if defined(FLOWMAP) && !defined(BLEND_NORMALS)
#		ifdef DISABLE_FLOWMAP_NORMALS
	// FLOWMAP NORMALS DISABLED: Using only base normals (flow system still active for ripples/splashes)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#		else
	// FLOWMAP NORMALS ENABLED: Blending flow-based normals with base normals
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor));
#		endif
#	elif !defined(LOD)

#		if defined(WATER_PARALLAX)
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw + parallaxOffset.xy * normalScalesRcp.y, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy + parallaxOffset.xy * normalScalesRcp.z, SharedData::MipBias).xyz * 2.0 - 1.0;
#		else
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy, SharedData::MipBias).xyz * 2.0 - 1.0;
#		endif

	float3 blendedNormal = normalize(float3(0, 0, 1) + NormalsAmplitude.x * normals1 +
									 NormalsAmplitude.y * normals2 + NormalsAmplitude.z * normals3);
#		if defined(UNDERWATER)
	float3 finalNormal = blendedNormal;
#		else
	float3 finalNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, normalsDepthFactor));
#		endif

#		if defined(FLOWMAP)
	float normalBlendFactor =
		normalMul.y * ((1 - normalMul.x) * flowmapNormal3.z + normalMul.x * flowmapNormal2.z) +
		(1 - normalMul.y) * (normalMul.x * flowmapNormal1.z + (1 - normalMul.x) * flowmapNormal0.z);
	finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), normalize(lerp(finalNormal, flowmapNormal, normalBlendFactor)), distanceFactor));
#		endif
#	else
	float3 finalNormal =
		normalize(float3(0, 0, 1) + NormalsAmplitude.xxx * normals1);
#	endif

#	if defined(WADING)
#		if defined(FLOWMAP)
	float2 displacementUv = input.TexCoord3.zw;
#		else
	float2 displacementUv = input.TexCoord3.xy;
#		endif
	float3 displacement = normalize(float3(NormalsAmplitude.w * (-0.5 + DisplacementTex.Sample(DisplacementSampler, displacementUv).zw),
		0.04));
	finalNormal = lerp(displacement, finalNormal, displacement.z);
#	endif

#	if defined(WETNESS_EFFECTS)
	// Wetness Effects Debug System:
	// DEBUG_WETNESS_EFFECTS Color Legend:
	// - BRIGHT MAGENTA: Ripples, BRIGHT GREEN: Splashes, CYAN: Both effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	float maxRainDropDistance = SharedData::wetnessEffectsSettings.RaindropFxRange * SharedData::wetnessEffectsSettings.RaindropFxRange * 3;
	float rainDropDistance = dot(input.WPosition.xyz, input.WPosition.xyz);
	float distanceFadeout = saturate((1 - saturate(rainDropDistance / maxRainDropDistance)) * 3);
	if (finalNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx &&
		(rainDropDistance < maxRainDropDistance) && wetnessOcclusion > 0.05) {
		float rippleStrengthModifier = (wetnessOcclusion * wetnessOcclusion) * distanceFadeout;
		float3 rippleWPosition = input.WPosition.xyz + finalNormal * 16;
#		if defined(WATER_PARALLAX)
		rippleWPosition.xy += parallaxOffset;
#		endif
#		if defined(FLOWMAP)
		// Flow-following ripple enhancement: Makes raindrops follow water current
		FlowmapData worldFlowData = GetFlowmapDataWorldSpace(input, float2(0, 0));

		// Calculate flow-aware ripple offset using centralized timing logic
		// Parameters: avgFlowmapMultiplier=9.26 (average of GetWaterNormal flowmap normal multipliers: 9.92, 10.64, 8, 8.48)
		// uvToWorldScale=0.125 (1/8 - relates to 64× texture coordinate scaling factor)
		float2 flowOffset = WetnessEffects::GetFlowAwareRippleOffset(
			worldFlowData.flowVector,
			worldFlowData.color.w,      // Flow strength from flowmap alpha
			0.001 * ReflectionColor.w,  // Reflection timing scale (matches GetFlowmapNormal)
			9.26,                       // Average flowmap normal multiplier
			0.125                       // UV-to-world scale factor (1/8)
		);

		rippleWPosition.xy += flowOffset;
#		endif
		raindropInfo = WetnessEffects::GetRainDrops(rippleWPosition + FrameBuffer::CameraPosAdjust[eyeIndex].xyz, SharedData::wetnessEffectsSettings.Time, finalNormal, rippleStrengthModifier);

		// Calculate ripple and splash color intensities
		float rippleIntensity = length(raindropInfo.xy) * rippleStrengthModifier;
		float splashIntensity = raindropInfo.w * distanceFadeout;

		// Store ripple and splash information for color effects
		result.rippleInfo.xyz = raindropInfo.xyz * rippleIntensity;
		result.rippleInfo.w = splashIntensity;
	}
	float3 rippleNormal = normalize(raindropInfo.xyz);
	finalNormal = WetnessEffects::ReorientNormal(rippleNormal, finalNormal);
#	endif

	result.normal = finalNormal;
	return result;
}

float3 GetWaterSpecularColor(PS_INPUT input, float3 normal, float3 viewDirection, float distanceFactor, float skylightingSpecular)
{
	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections))
		return ReflectionColor.xyz * VarAmounts.y;

	float3 R = reflect(viewDirection, WaterParams.y * normal + float3(0, 0, 1 - WaterParams.y));
	float3 reflectionColor = CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz;

#	if defined(DYNAMIC_CUBEMAPS)
	float3 dynamicCubemap;
	if (SharedData::InInterior) {
		dynamicCubemap = DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, 0).xyz;
	} else {
		float3 specularIrradiance = 1.0;
		if (skylightingSpecular < 1.0)
			specularIrradiance = Color::IrradianceToLinear(DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, 0).xyz);

		float3 specularIrradianceReflections = 1.0;
		if (skylightingSpecular > 0.0)
			specularIrradianceReflections = Color::IrradianceToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, 0).xyz);

		dynamicCubemap = Color::IrradianceToGamma(lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular));
	}

	float reflectionAmount = saturate(length(input.WPosition.xyz) / 1024.0);

#		if defined(VR)
	// Reflection cubemap is incorrect for interiors in VR, ignore it
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior || SharedData::HideSky)
		reflectionAmount = 0.0;
#		else
	if (SharedData::HideSky)
		reflectionAmount = 0.0;
#		endif
	reflectionColor = lerp(dynamicCubemap, reflectionColor, reflectionAmount);
#	endif

#	if !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
	float pointingDirection = dot(viewDirection, R);
	float pointingAlignment = dot(reflect(viewDirection, float3(0, 0, 1)), R);
	float ssrAmount = min(pointingAlignment, pointingDirection);
	if (SSRParams.x > 0.0 && ssrAmount > 0.0) {
		float2 ssrReflectionUv = ((FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy) * SSRParams.zw) + 0.05 * normal.xy;
		float2 ssrReflectionUvDR = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(ssrReflectionUv);
		float4 ssrReflectionColorBlurred = SSRReflectionTex.Sample(SSRReflectionSampler, ssrReflectionUvDR);
		float4 ssrReflectionColorRaw = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
		float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7);
		float3 finalSsrReflectionColor = max(0, ssrReflectionColor.xyz);
		float ssrFraction = saturate(ssrReflectionColor.w * distanceFactor * ssrAmount);
		reflectionColor = lerp(reflectionColor, finalSsrReflectionColor, ssrFraction);
	}
#	endif

	return reflectionColor;
}

float GetScreenDepthWater(float2 screenPosition, uint a_useVR = 0)
{
	float depth = DepthTex.Load(float3(screenPosition, 0)).x;
#	if defined(VR)  // VR appears to use hard coded values
	return depth * 1.01 + -0.01;
#	else
	return (CameraDataWater.w / (-depth * CameraDataWater.z + CameraDataWater.x));
#	endif
}

float3 GetLdotN(float3 normal)
{
#	if defined(UNDERWATER)
	return 1;
#	else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 1;
	return saturate(dot(SunDir.xyz, normal));
#	endif
}

float GetFresnelValue(float3 normal, float3 viewDirection)
{
#	if defined(UNDERWATER)
	float3 actualNormal = -normal;
#	else
	float3 actualNormal = normal;
#	endif
	float viewAngle = 1 - saturate(dot(-viewDirection, actualNormal));
	return (1 - FresnelRI.x) * pow(viewAngle, 5) + FresnelRI.x;
}

struct DiffuseOutput
{
	float3 refractionColor;
	float3 refractionDiffuseColor;
	float depth;
	float refractionMul;
	float3 refractedViewDirection;
};

DiffuseOutput GetWaterDiffuseColor(PS_INPUT input, float3 normal, float3 viewDirection, inout float4 distanceMul, float refractionsDepthFactor, float fresnel, uint eyeIndex, float3 viewPosition, float depth)
{
#	if defined(REFRACTIONS)
	float4 refractionNormal = mul(transpose(TextureProj[eyeIndex]), float4((VarAmounts.w * refractionsDepthFactor * normal.xy) + input.MPosition.xy, input.MPosition.z, 1));

	float2 refractionUvRaw = float2(refractionNormal.x, refractionNormal.w - refractionNormal.y) / refractionNormal.ww;
	refractionUvRaw = Stereo::ConvertToStereoUV(refractionUvRaw, eyeIndex);  // need to convert here for VR due to refractionNormal values

#		if defined(VR)
	float2 refractionUvRawNoStereo = Stereo::ConvertFromStereoUV(refractionUvRaw, eyeIndex, 1);
#		endif

	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

	float2 refractionScreenPosition = FrameBuffer::DynamicResolutionParams1.xy * (refractionUvRaw / VPOSOffset.xy);
	float4 refractionWorldPosition = float4(input.WPosition.xyz * depth / viewPosition.z, 0);

#		if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float refractionDepth = GetScreenDepthWater(refractionScreenPosition);
	depth = refractionDepth;
#			if !defined(VR)
	float refractionDepthMul = length(float3((((VPOSOffset.zw + refractionUvRaw) * 2 - 1)) * refractionDepth / ProjData.xy, refractionDepth));
#			else
	float refractionDepthMul = CalculateDepthMultFromUV(refractionUvRawNoStereo, refractionDepth, eyeIndex);
#			endif  //VR

	float3 refractionDepthAdjustedViewDirection = -viewDirection * refractionDepthMul;
	float refractionViewSurfaceAngle = dot(refractionDepthAdjustedViewDirection, ReflectPlane[eyeIndex].xyz);

	float refractionPlaneMul = (1 - ReflectPlane[eyeIndex].w / refractionViewSurfaceAngle);

	if (refractionPlaneMul < 0.0) {
		refractionUvRaw = FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;  // This value is already stereo converted for VR
	} else {
		distanceMul = saturate(refractionPlaneMul * float4(length(refractionDepthAdjustedViewDirection).xx, abs(refractionViewSurfaceAngle).xx) / FogParam.z);

#			if defined(VR)
		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4((refractionUvRawNoStereo * 2 - 1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
#			else
		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4((refractionUvRaw * 2 - 1) * float2(1, -1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
#			endif
		refractionWorldPosition.xyz /= refractionWorldPosition.w;
	}
#		endif

	float2 refractionUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(refractionUvRaw);
	float3 refractionColor = RefractionTex.Sample(RefractionSampler, refractionUV).xyz;
	float3 refractionDiffuseColor = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), distanceMul.y);

#		if defined(UNDERWATER)
	float refractionMul = 0;
#		else
	float refractionMul = 1 - pow(saturate((-distanceMul.x * FogParam.z + FogParam.z) / FogParam.w), FogNearColor.w);
#		endif

	DiffuseOutput output;
	output.refractionColor = refractionColor;
	output.refractionDiffuseColor = refractionDiffuseColor;
	output.depth = depth;
	output.refractionMul = refractionMul;
	output.refractedViewDirection = normalize(refractionWorldPosition.xyz - input.WPosition.xyz);
	return output;
#	else
	DiffuseOutput output;
	output.refractionColor = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), fresnel) * GetLdotN(normal);
	output.refractionDiffuseColor = output.refractionColor;
	output.depth = 1;
	output.refractionMul = 1;
	output.refractedViewDirection = viewDirection;
	return output;
#	endif
}

float3 GetSunColor(float3 normal, float3 viewDirection, float3 worldPosition, uint eyeIndex)
{
#	if defined(UNDERWATER)
	return 0.0.xxx;
#	else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 0.0.xxx;

	float3 reflectionDirection = reflect(viewDirection, normal);
	float reflectionMul = exp2(VarAmounts.x * log2(saturate(dot(reflectionDirection, SunDir.xyz))));

	float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 sunColor = Color::DirectionalLight((SunColor.xyz * SunDir.w) / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * (1.0 - exp(-DeepColor.w)) * llDirLightMult;
#		if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		sunColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	}
#		endif
	return reflectionMul * sunColor;
#	endif
}
#endif
