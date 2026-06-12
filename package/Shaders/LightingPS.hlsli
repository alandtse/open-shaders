#include "LightingPS_Resources.hlsli"
// Resources must precede Helpers (the comment also blocks clang-format include sorting).
#include "LightingPS_Helpers.hlsli"

#if defined(LOD)
#	undef EXTENDED_MATERIALS
#	undef WATER_BLENDING
#	undef LIGHT_LIMIT_FIX
#	undef WETNESS_EFFECTS
#	undef DYNAMIC_CUBEMAPS
#	undef WATER_EFFECTS
#endif

#if defined(WORLD_MAP)
#	undef CLOUD_SHADOWS
#	undef SKYLIGHTING
#endif

#include "Common/LightingCommon.hlsli"

#if defined(WATER_EFFECTS)
#	include "WaterEffects/WaterCaustics.hlsli"
#endif

#if defined(EYE)
#	undef WETNESS_EFFECTS
#endif

#if defined(EXTENDED_MATERIALS) && !defined(LOD) && (defined(PARALLAX) || defined(LANDSCAPE) || defined(ENVMAP) || defined(TRUE_PBR))
#	define EMAT
#endif

#if defined(EMAT) && (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#	define EMAT_ENVMAP
#endif

#if defined(DYNAMIC_CUBEMAPS)
#	include "DynamicCubemaps/DynamicCubemaps.hlsli"
#endif

#if defined(TRUE_PBR)
#	include "Common/PBR.hlsli"
#endif

#if defined(EMAT)
#	include "ExtendedMaterials/ExtendedMaterials.hlsli"
#endif

#if defined(SCREEN_SPACE_SHADOWS)
#	include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#endif

#if defined(TREE_ANIM)
#	undef WETNESS_EFFECTS
#endif

#if defined(WETNESS_EFFECTS)
#	include "WetnessEffects/WetnessEffects.hlsli"
#endif

#if defined(TERRAIN_BLENDING)
#	include "TerrainBlending/TerrainBlending.hlsli"
#endif

#if defined(SSS) && defined(SKIN) && defined(DEFERRED)
#	undef SOFT_LIGHTING
#endif

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(HAIR) && defined(CS_HAIR)
#	include "Hair/Hair.hlsli"
#endif

#if defined(TERRAIN_VARIATION)
#	include "TerrainVariation/TerrainVariation.hlsli"
#endif

#if defined(EXTENDED_TRANSLUCENCY) && !(defined(LOD) || defined(SKIN) || defined(HAIR) || defined(EYE) || defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS) || defined(DEPTH_WRITE_DECALS))
#	include "ExtendedTranslucency/ExtendedTranslucency.hlsli"
#	define ANISOTROPIC_ALPHA
#endif

#if defined(CS_SKIN)
#	include "Skin/Skin.hlsli"
#endif

#define LinearSampler SampColorSampler

#include "Common/ShadowSampling.hlsli"

#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#endif

#if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

#if defined(EXP_HEIGHT_FOG)
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif

#include "Common/LightingEval.hlsli"

PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout;
	uint eyeIndex = Stereo::GetEyeIndexPS(input.Position, VPOSOffset);

	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WorldPosition.xyz, 1)).xyz;
	float3 viewDirection = -normalize(input.WorldPosition.xyz);

	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

#if defined(DEFERRED)
	const bool inWorld = true;
#else
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
#endif
	const bool inReflection = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection;

	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewPosition.z);

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	float3x3 tbn = float3x3(input.TBN0.xyz, input.TBN1.xyz, input.TBN2.xyz);

#	if !defined(TREE_ANIM) && !defined(LOD)
	// Fix incorrect vertex normals on double-sided meshes
	if (!frontFace)
		tbn = lerp(tbn, -tbn, nearFactor);
#	endif

	float3x3 tbnTr = transpose(tbn);

	tbnTr[0] = normalize(tbnTr[0]);
	tbnTr[1] = normalize(tbnTr[1]);
	tbnTr[2] = normalize(tbnTr[2]);

	tbn = transpose(tbnTr);

#endif  // defined (SKINNED) || !defined (MODELSPACENORMALS)

#if !defined(TRUE_PBR)
#	if defined(LANDSCAPE)
	float shininess = dot(input.LandBlendWeights1, LandscapeTexture1to4IsSpecPower) + input.LandBlendWeights2.x * LandscapeTexture5to6IsSpecPower.x + input.LandBlendWeights2.y * LandscapeTexture5to6IsSpecPower.y;
#	elif defined(SPECULAR)
	float shininess = SpecularColor.w;
#	else
	float shininess = 0.0;
#	endif  // defined (LANDSCAPE)
#endif

#if defined(TERRAIN_BLENDING)
	float blendFactorTerrain = 0.0;
	[flatten] if (SharedData::terrainBlendingSettings.Enabled)
	{
		float depthSampled = TerrainBlending::TerrainBlendingMaskTexture[input.Position.xy].x;

		float depthSampledLinear = SharedData::GetScreenDepth(depthSampled);
		float depthPixelLinear = SharedData::GetScreenDepth(input.Position.z);

		blendFactorTerrain = saturate((depthSampledLinear - depthPixelLinear) / 5.0);

		if (input.Position.z == depthSampled)
			blendFactorTerrain = 1;
	}
#endif

	float2 uv = input.TexCoord0.xy;
	float2 uvOriginal = uv;

#if defined(EMAT)
	float parallaxShadowQuality = sqrt(1.0 - saturate(viewPosition.z / 2048.0));
#endif

#if defined(LANDSCAPE)
	float mipLevels[6];
#else
	float mipLevel = 0;
#endif  // LANDSCAPE
	float sh0 = 0;
	float pixelOffset = 0;

#if defined(EMAT)
#	if defined(LANDSCAPE)
	DisplacementParams displacementParams[6];
	displacementParams[0].DisplacementScale = 1.f;
	displacementParams[0].DisplacementOffset = 0.f;
	displacementParams[0].HeightScale = 1;
	displacementParams[0].FlattenAmount = 0;
#	else
	DisplacementParams displacementParams;
	displacementParams.DisplacementScale = 1.f;
	displacementParams.DisplacementOffset = 0.f;
	displacementParams.HeightScale = 1;
	displacementParams.FlattenAmount = 0;
#	endif

#endif

	float curvature = 0;
	float normalSmoothness = 0;

#if !defined(MODELSPACENORMALS)
	float3 vertexNormal = tbnTr[2];
#	if defined(EMAT)

	if (SharedData::extendedMaterialSettings.EnableParallaxWarpingFix) {
		float3 ndx = ddx(vertexNormal);
		float3 ndy = ddy(vertexNormal);
		float3 fdx = ddx(input.WorldPosition.xyz);
		float3 fdy = ddy(input.WorldPosition.xyz);
		float fragSize = rcp(length(max(abs(fdx), abs(fdy))));
		curvature = pow(length(max(abs(ndx), abs(ndy))) * fragSize, 0.5);
		float3 flatWorldNormal = normalize(-cross(ddx(input.WorldPosition.xyz), ddy(input.WorldPosition.xyz)));
		normalSmoothness = (1 - dot(vertexNormal, flatWorldNormal));
#		if defined(LANDSCAPE)
		displacementParams[0].HeightScale = saturate(1 - curvature);
		displacementParams[0].FlattenAmount = (normalSmoothness + curvature);
#		else
		displacementParams.HeightScale = saturate(1 - curvature);
		displacementParams.FlattenAmount = (normalSmoothness + curvature);
#		endif
	}
#	endif
#endif

	float3 entryNormal = 0;
	float3 entryNormalTS = 0;
	float eta = 1;
	float3 refractedViewDirection = viewDirection;
	float4 sampledCoatColor = PBRParams2;
	float3 complexSpecular = 1.0;  // Declare complexSpecular at a higher scope so it's available throughout the shader (NEEDED FOR STOCH. FIX)

#if defined(EMAT)
#	if defined(PARALLAX) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
	if (SharedData::extendedMaterialSettings.EnableParallax) {
		mipLevel = ExtendedMaterials::GetMipLevel(uv, TexParallaxSampler, screenNoise);
		uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, viewDirection, tbnTr, screenNoise, TexParallaxSampler, SampParallaxSampler, 0, displacementParams, pixelOffset);
		if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows))
			sh0 = TexParallaxSampler.SampleLevel(SampParallaxSampler, uv, mipLevel).x;
	}
#	endif  // defined(PARALLAX) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

	bool complexMaterial = false;
	bool complexMaterialParallax = false;
	float4 complexMaterialColor = 1.0;

#	if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float4 envMaskSample = TexEnvMaskSampler.Sample(SampEnvMaskSampler, uv);
	float envMaskBase = envMaskSample.x;
	if (SharedData::extendedMaterialSettings.EnableComplexMaterial) {
		const float kMaskEpsilon = (4.0 / 255.0);

		const float4 mipSample = TexEnvMaskSampler.SampleLevel(SampEnvMaskSampler, uv, 15);
		complexMaterial = mipSample.w < (1.0 - kMaskEpsilon);

		const bool grayscaleMask = (abs(mipSample.x - mipSample.y) < kMaskEpsilon) &&
		                           (abs(mipSample.x - mipSample.z) < kMaskEpsilon) &&
		                           (abs(mipSample.y - mipSample.z) < kMaskEpsilon);
		// Preserve height-only masks while rejecting grayscale environment masks
		const bool solidBlackHeightMask = all(mipSample.xyz < kMaskEpsilon) &&
		                                  mipSample.w > kMaskEpsilon &&
		                                  mipSample.w < (1.0 - kMaskEpsilon);
		if (grayscaleMask && !solidBlackHeightMask)
			complexMaterial = false;

		if (complexMaterial) {
			if (envMaskSample.w > kMaskEpsilon && envMaskSample.w < (1.0 - kMaskEpsilon)) {
				complexMaterialParallax = true;
				mipLevel = ExtendedMaterials::GetMipLevel(uv, TexEnvMaskSampler, screenNoise);
				uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, viewDirection, tbnTr, screenNoise, TexEnvMaskSampler, SampTerrainParallaxSampler, 3, displacementParams, pixelOffset);
				if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows))
					sh0 = TexEnvMaskSampler.SampleLevel(SampEnvMaskSampler, uv, mipLevel).w;
				complexMaterialColor = TexEnvMaskSampler.Sample(SampEnvMaskSampler, uv);
			} else {
				complexMaterialColor = envMaskSample;
			}
			envMaskBase = complexMaterialColor.x;
		}
	}
#	endif  // ENVMAP

#	if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	bool PBRParallax = false;
	[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
	{
		float4 sampledCoatProperties = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);
		sampledCoatColor.rgb *= Color::Diffuse(sampledCoatProperties.rgb);
		sampledCoatColor.a *= sampledCoatProperties.a;
	}
#		if !defined(FACEGEN)
	[branch] if (SharedData::extendedMaterialSettings.EnableParallax && (PBRFlags & PBR::Flags::HasDisplacement) != 0)
	{
		PBRParallax = true;
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			displacementParams.HeightScale = PBRParams1.y;
			displacementParams.DisplacementScale = 0.5;
			displacementParams.DisplacementOffset = -0.25;

			eta = lerp(1.0, (1 - sqrt(MultiLayerParallaxData.y)) / (1 + sqrt(MultiLayerParallaxData.y)), sampledCoatColor.w);
			[branch] if ((PBRFlags & PBR::Flags::CoatNormal) != 0)
			{
				entryNormalTS = normalize(TransformNormal(TexBackLightSampler.Sample(SampBackLightSampler, uvOriginal).xyz));
			}
			else
			{
				entryNormalTS = normalize(TransformNormal(TexNormalSampler.Sample(SampNormalSampler, uvOriginal).xyz));
			}
			entryNormal = normalize(mul(tbn, entryNormalTS));
			refractedViewDirection = -refract(-viewDirection, entryNormal, eta);
		}
		else
		{
			displacementParams.HeightScale *= PBRParams1.y;
		}
		mipLevel = ExtendedMaterials::GetMipLevel(uv, TexParallaxSampler, screenNoise);
		uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, refractedViewDirection, tbnTr, screenNoise, TexParallaxSampler, SampParallaxSampler, 0, displacementParams, pixelOffset);
		if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows))
			sh0 = TexParallaxSampler.SampleLevel(SampParallaxSampler, uv, mipLevel).x;
	}
#		endif  // !FACEGEN
#	endif      // TRUE_PBR

#elif defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float envMaskBase = TexEnvMaskSampler.Sample(SampEnvMaskSampler, uv).x;
#endif  // EMAT

#if defined(SNOW)
	bool useSnowSpecular = true;
#else
	bool useSnowSpecular = false;
#endif  // SNOW

#if defined(SPARKLE) || !defined(PROJECTED_UV)
	bool useSnowDecalSpecular = true;
#else
	bool useSnowDecalSpecular = false;
#endif  // defined(SPARKLE) || !defined(PROJECTED_UV)

	float2 diffuseUv = uv;

#if defined(SPARKLE)
	diffuseUv = ProjectedUVParams2.yy * input.TexCoord0.zw;
#endif  // SPARKLE

#if defined(LANDSCAPE)
	// Normalise blend weights
	float totalWeight = input.LandBlendWeights1.x + input.LandBlendWeights1.y + input.LandBlendWeights1.z +
	                    input.LandBlendWeights1.w + input.LandBlendWeights2.x + input.LandBlendWeights2.y;
	if (totalWeight > 0.0) {
		input.LandBlendWeights1 /= totalWeight;
		input.LandBlendWeights2.xy /= totalWeight;
	}
	float3 blendedRGB = 0;
	float blendedAlpha = 0;
	float3 blendedNormalRGB = 0;
	float blendedNormalAlpha = 0;

#	if defined(TRUE_PBR)
	float4 blendedRMAOS = 0;
#	endif

	// Compute stochastic offsets and derivatives once for all layers (only when terrain variation is enabled)
#	if defined(TERRAIN_VARIATION)
	bool useTerrainVariation = SharedData::terrainVariationSettings.enableTilingFix;
	// Initialise dx, dy, and sharedOffset for when Terrain Variation is disabled via enableTilingFix but still #defined
	float2 dx = 0, dy = 0;
	StochasticOffsets sharedOffset;
	sharedOffset.offset1 = float2(0, 0);
	sharedOffset.offset2 = float2(0, 0);
	sharedOffset.offset3 = float2(0, 0);
	sharedOffset.weights = float3(0, 0, 0);
	[branch] if (useTerrainVariation)
	{
		dx = ddx(input.TexCoord0.zw);
		dy = ddy(input.TexCoord0.zw);
		sharedOffset = ComputeStochasticOffsets(input.TexCoord0.zw);
	}
#	endif

#	if defined(EMAT)
#		if defined(TRUE_PBR)
	if (SharedData::extendedMaterialSettings.EnableParallax) {
#		else
	if (SharedData::extendedMaterialSettings.EnableTerrainParallax || (SharedData::extendedMaterialSettings.EnableParallax && Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement)) {
#		endif
		mipLevels[0] = ExtendedMaterials::GetMipLevel(uv, TexColorSampler, screenNoise);
		mipLevels[1] = ExtendedMaterials::GetMipLevel(uv, TexLandColor2Sampler, screenNoise);
		mipLevels[2] = ExtendedMaterials::GetMipLevel(uv, TexLandColor3Sampler, screenNoise);
		mipLevels[3] = ExtendedMaterials::GetMipLevel(uv, TexLandColor4Sampler, screenNoise);
		mipLevels[4] = ExtendedMaterials::GetMipLevel(uv, TexLandColor5Sampler, screenNoise);
		mipLevels[5] = ExtendedMaterials::GetMipLevel(uv, TexLandColor6Sampler, screenNoise);

		displacementParams[1] = displacementParams[0];
		displacementParams[2] = displacementParams[0];
		displacementParams[3] = displacementParams[0];
		displacementParams[4] = displacementParams[0];
		displacementParams[5] = displacementParams[0];
#		if defined(TRUE_PBR)
		displacementParams[0].HeightScale *= PBRParams1.y;
		displacementParams[1].HeightScale *= LandscapeTexture2PBRParams.y;
		displacementParams[2].HeightScale *= LandscapeTexture3PBRParams.y;
		displacementParams[3].HeightScale *= LandscapeTexture4PBRParams.y;
		displacementParams[4].HeightScale *= LandscapeTexture5PBRParams.y;
		displacementParams[5].HeightScale *= LandscapeTexture6PBRParams.y;
#		endif

		float weights[6];
		// Initialize weights array
		weights[0] = weights[1] = weights[2] = weights[3] = weights[4] = weights[5] = 0.0;
#		if defined(TERRAIN_VARIATION)
		uv = ExtendedMaterials::GetParallaxCoords(input, viewPosition.z, uv, mipLevels, viewDirection, tbnTr, screenNoise, displacementParams, sharedOffset, dx, dy, pixelOffset,
			weights);
#		else
		uv = ExtendedMaterials::GetParallaxCoords(input, viewPosition.z, uv, mipLevels, viewDirection, tbnTr, screenNoise, displacementParams, pixelOffset,
			weights);
#		endif
		if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
			input.LandBlendWeights1.x = weights[0];
			input.LandBlendWeights1.y = weights[1];
			input.LandBlendWeights1.z = weights[2];
			input.LandBlendWeights1.w = weights[3];
			input.LandBlendWeights2.x = weights[4];
			input.LandBlendWeights2.y = weights[5];
		}
		if (SharedData::extendedMaterialSettings.EnableShadows && (parallaxShadowQuality > 0.0f || SharedData::extendedMaterialSettings.ExtendShadows)) {
#		if defined(TERRAIN_VARIATION)
			sh0 = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, parallaxShadowQuality, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, dx, dy, weights);
			float shadowMultiplier = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, DirLightDirection, sh0, parallaxShadowQuality, screenNoise, displacementParams, sharedOffset, dx, dy);
#		else
			sh0 = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, parallaxShadowQuality, input.LandBlendWeights1, input.LandBlendWeights2.xy, weights);
#		endif
		}
	}
#		if defined(TERRAIN_VARIATION)
	else if (useTerrainVariation) {
		// Calculate proper mip levels for terrain variation when parallax is disabled but EMAT is available
		mipLevels[0] = ExtendedMaterials::GetMipLevel(uv, TexColorSampler, screenNoise);
		mipLevels[1] = ExtendedMaterials::GetMipLevel(uv, TexLandColor2Sampler, screenNoise);
		mipLevels[2] = ExtendedMaterials::GetMipLevel(uv, TexLandColor3Sampler, screenNoise);
		mipLevels[3] = ExtendedMaterials::GetMipLevel(uv, TexLandColor4Sampler, screenNoise);
		mipLevels[4] = ExtendedMaterials::GetMipLevel(uv, TexLandColor5Sampler, screenNoise);
		mipLevels[5] = ExtendedMaterials::GetMipLevel(uv, TexLandColor6Sampler, screenNoise);
	}
#		endif
#	else
	// Initialize mip levels for non-EMAT case
	mipLevels[0] = mipLevels[1] = mipLevels[2] = mipLevels[3] = mipLevels[4] = mipLevels[5] = 0.0;
#	endif  // EMAT
#endif      // LANDSCAPE

#if defined(SPARKLE)
	diffuseUv = ProjectedUVParams2.yy * (input.TexCoord0.zw + (uv - uvOriginal));
#else
	diffuseUv = uv;
#endif  // SPARKLE

	float4 baseColor = 0;
	float4 normal = 0;
	float glossiness = 0;
#if defined(CS_SKIN)
	const bool skinEnabled = SharedData::skinData.skinParams.w > 0.0f;
#	if defined(SKIN)
	float skinRoughness = 0;
	float skinSpecular = 0;
	float skinFuzzMask = 1;
	float skinWetMask = 1;
	float skinAO = 1;
	bool skinRoughnessSet = false;
#	endif
#endif

	float4 rawRMAOS = 0;

	float4 glintParameters = 0;

#if defined(SNOW)  // Earlier snow definition for Terrain Variation rework.
#	if !defined(TRUE_PBR)
	float landSnowMask = 0.0;
#		if defined(LANDSCAPE)
	landSnowMask = GetLandSnowMaskValue(baseColor.w);
#		endif
#	endif
#endif

#include "LightingPS_Landscape.hlsli"

#if defined(LOD_BLENDING)
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
	baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODObjectGamma) * SharedData::lodBlendingSettings.LODObjectBrightness;
#	elif defined(LODLANDSCAPE)
// First apply terrain variation if enabled
#		if defined(TERRAIN_VARIATION)
	if (SharedData::terrainVariationSettings.enableLODTerrainTilingFix) {
		float2 dx = ddx(uv);
		float2 dy = ddy(uv);
		StochasticOffsets lodOffset = ComputeStochasticOffsetsLOD(uv);
		float4 lodStochasticColor = StochasticSampleLOD(screenNoise, TexColorSampler, SampColorSampler, uv, lodOffset, dx, dy);

		// Apply the stochastic result directly
		baseColor.xyz = Color::Diffuse(lodStochasticColor.rgb);
	}
#		endif
	baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#	endif
#endif  // LOD_BLENDING

#if defined(SKIN) && defined(CS_SKIN)
	float4 skinsk = 0;
	float4 skinExtra = 0;
	float4 skinWetnessSample = 0;
	uint2 skinExtraDimensions = uint2(0, 0);
	uint2 wetnessDimensions = uint2(0, 0);
	bool hasSkinExtra = false;
	bool hasSkinWetness = false;
	if (skinEnabled) {
		skinsk = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);
		TexSkinExtraSampler.GetDimensions(skinExtraDimensions.x, skinExtraDimensions.y);
		TexSkinWetnessSampler.GetDimensions(wetnessDimensions.x, wetnessDimensions.y);
		hasSkinExtra = skinExtraDimensions.x > 32 && skinExtraDimensions.y > 32;
		hasSkinWetness = wetnessDimensions.x > 32 && wetnessDimensions.y > 32;
	}
	float4 skinWetnessNormal = float4(0.f, 0.f, 0.f, 1.f);

	if (hasSkinExtra && SharedData::skinData.skinParams.x > 0.0f) {
		skinExtra = TexSkinExtraSampler.Sample(SampColorSampler, uv);
		skinRoughness = skinExtra.x;
		skinFuzzMask = skinExtra.y;
		skinAO = skinExtra.z;
		skinSpecular = skinExtra.w;
		skinRoughnessSet = true;
	} else {
		skinRoughnessSet = false;
	}
	if (hasSkinWetness && skinEnabled) {
		skinWetnessSample = TexSkinWetnessSampler.Sample(SampColorSampler, uv);
		if ((skinWetnessSample.y == 0 && skinWetnessSample.z == 0) || (skinWetnessSample.x == skinWetnessSample.y && skinWetnessSample.y == skinWetnessSample.z && skinWetnessSample.w >= 0.99f)) {
			skinWetMask = skinWetnessSample.x;
			skinWetnessNormal.xyz = Skin::CalculateNormalFromHeight(skinWetMask, SharedData::skinData.wetParams.w * 0.0001, uv) * 0.5 + 0.5;
		} else {
			skinWetnessNormal.xyz = skinWetnessSample.xyz;
			skinWetMask = skinWetnessSample.w;
		}
	} else {
		skinWetMask = 1.0;
	}
#endif

	float landSnowMask1 = GetLandSnowMaskValue(baseColor.w);

#if defined(MODELSPACENORMALS)
#	if defined(LODLANDNOISE)
	normal.xyz = normal.xzy - 0.5.xxx;
	float lodLandNoiseParameter = GetLodLandBlendParameter(baseColor.xyz);
	float noise = TexLandLodNoiseSampler.Sample(SampLandLodNoiseSampler, uv * 3.0.xx).x;
	float lodLandNoiseMultiplier = GetLodLandBlendMultiplier(lodLandNoiseParameter, noise);
	baseColor.xyz *= lodLandNoiseMultiplier;
	normal.xyz *= 2;
	normal.w = 1;
	glossiness = 0;
#	elif defined(LODLANDSCAPE)
	normal.xyz = 2.0.xxx * (-0.5.xxx + normal.xzy);
	normal.w = 1;
	glossiness = 0;
#	else
	normal.xyz = normal.xzy * 2.0.xxx + -1.0.xxx;
	normal.w = 1;
	glossiness = TexSpecularSampler.Sample(SampSpecularSampler, uv).x;
#	endif  // LODLANDNOISE
#elif (defined(SNOW) && defined(LANDSCAPE))
	normal.xyz = GetLandNormal(landSnowMask1, normal.xyz, uv, SampNormalSampler, TexNormalSampler);
	glossiness = normal.w;
#else
	normal.xyz = TransformNormal(normal.xyz);
	glossiness = normal.w;
#endif  // MODELSPACENORMALS

#if defined(WORLD_MAP)
	normal.xyz = GetWorldMapNormal(input, normal.xyz, rawBaseColor.xyz);
#endif  // WORLD_MAP

#if defined(LANDSCAPE)
#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask = LandscapeTexture1to4IsSnow.x * input.LandBlendWeights1.x;
#	endif  // SNOW
#endif      // LANDSCAPE

#if defined(EMAT_ENVMAP)
	complexMaterial = complexMaterial && complexMaterialColor.y > (4.0 / 255.0);
	shininess = lerp(shininess, shininess * complexMaterialColor.y, complexMaterial);
	if (complexMaterial) {
		complexSpecular = lerp(1.0, baseColor.xyz, complexMaterialColor.z);
		baseColor.xyz = lerp(baseColor.xyz, 0.0, complexMaterialColor.z);
	}
#endif  // defined (EMAT) && defined(ENVMAP)

#if defined(FACEGEN)
	if (!SharedData::linearLightingSettings.enableLinearLighting) {
		baseColor.xyz = GetFacegenBaseColor(baseColor.xyz, uv);
	} else {
		baseColor.xyz = Color::SkyrimGammaToLinear(GetFacegenBaseColor(Color::LinearToSkyrimGamma(baseColor.xyz), uv));
	}
#elif defined(FACEGEN_RGB_TINT)
	if (!SharedData::linearLightingSettings.enableLinearLighting) {
		baseColor.xyz = GetFacegenRGBTintBaseColor(baseColor.xyz, uv);
	} else {
		baseColor.xyz = Color::SkyrimGammaToLinear(GetFacegenRGBTintBaseColor(Color::LinearToSkyrimGamma(baseColor.xyz), uv));
	}
#endif  // FACEGEN

#if defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled) {
		baseColor.xyz = baseColor.xyz * SharedData::skinData.skinParams2.w;
	}
#endif  // CS_SKIN

#if defined(HAIR) && defined(CS_HAIR)
	float3 hairTint = 0;

	if (SharedData::hairSpecularSettings.Enabled) {
		hairTint = lerp(1, Color::Diffuse(TintColor.xyz), Color::ColorToLinear(input.Color.y));
		baseColor.xyz *= hairTint;
		baseColor.xyz = Hair::Saturation(baseColor.xyz, SharedData::hairSpecularSettings.HairSaturation);
		baseColor.xyz *= SharedData::hairSpecularSettings.BaseColorMult;
		baseColor.xyz = SharedData::hairSpecularSettings.HairMode == 1 ? baseColor.xyz * baseColor.xyz : baseColor.xyz;  // To match color for Marschner
	}

	float3 sampledHairFlow = 0;
	bool useHairFlowMap = false;
#	if defined(BACK_LIGHTING)
	if (SharedData::hairSpecularSettings.Enabled) {
		uint2 hairFlowDimensions = uint2(0, 0);
		sampledHairFlow = float3(TexBackLightSampler.Sample(SampBackLightSampler, uv).xy, 0.5f);
		TexBackLightSampler.GetDimensions(hairFlowDimensions.x, hairFlowDimensions.y);
		useHairFlowMap = (sampledHairFlow.x > 0.0 || sampledHairFlow.y > 0.0) && hairFlowDimensions.x > 32 && hairFlowDimensions.y > 32;
		sampledHairFlow = useHairFlowMap ? sampledHairFlow * 2.0f - 1.0f : float3(0.5f, 0.5f, 0.5f);
	}
#	endif
#endif

#if defined(LOD_LAND_BLEND)
	float4 lodLandColor;

#	if defined(TERRAIN_VARIATION)
	if (SharedData::terrainVariationSettings.enableLODTerrainTilingFix) {
		// Apply stochastic sampling to LOD_LAND_BLEND color texture
		float2 blendColorUV = input.TexCoord0.zw;
		float2 dx = ddx(blendColorUV);
		float2 dy = ddy(blendColorUV);
		StochasticOffsets lodBlendColorOffset = ComputeStochasticOffsetsLOD(blendColorUV);
		lodLandColor = StochasticSampleLOD(screenNoise, TexLandLodBlend1Sampler, SampLandLodBlend1Sampler, blendColorUV, lodBlendColorOffset, dx, dy);
	} else {
		lodLandColor = TexLandLodBlend1Sampler.Sample(SampLandLodBlend1Sampler, input.TexCoord0.zw);
	}
#	else
	lodLandColor = TexLandLodBlend1Sampler.Sample(SampLandLodBlend1Sampler, input.TexCoord0.zw);
#	endif

	lodLandColor.xyz = Color::ColorToLinear(lodLandColor.xyz) * Color::VanillaDiffuseColorMult();
#	if defined(LOD_BLENDING)
	lodLandColor.xyz = pow(abs(lodLandColor.xyz), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#	endif  // LOD_BLENDING
	float lodBlendParameter = GetLodLandBlendParameter(lodLandColor.xyz);
	float lodBlendMask = TexLandLodBlend2Sampler.Sample(SampLandLodBlend2Sampler, 3.0.xx * input.TexCoord0.zw).x;
	float lodLandFadeFactor = GetLodLandBlendMultiplier(lodBlendParameter, lodBlendMask);
	float lodLandBlendFactor = LODTexParams.z * input.LandBlendWeights2.w;
	normal.xyz = lerp(normal.xyz, float3(0, 0, 1), lodLandBlendFactor);

#	if !defined(TRUE_PBR)
	baseColor.w = 0;
	baseColor = lerp(baseColor, lodLandColor * lodLandFadeFactor, lodLandBlendFactor);
	glossiness = lerp(glossiness, 0, lodLandBlendFactor);
#	endif
#endif  // LOD_LAND_BLEND

#if defined(SNOW) && !defined(TRUE_PBR)
	useSnowSpecular = landSnowMask != 0.0;
#endif  // SNOW

#if defined(BACK_LIGHTING)
	float4 backLightColor = TexBackLightSampler.Sample(SampBackLightSampler, uv);
#	if defined(HAIR) && defined(CS_HAIR)
	if (useHairFlowMap) {
		backLightColor = 0.0f;
	}
#	endif
#endif  // BACK_LIGHTING

#if (defined(RIM_LIGHTING) || defined(SOFT_LIGHTING) || defined(LOAD_SOFT_LIGHTING))
	float4 rimSoftLightColor = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);
#endif  // RIM_LIGHTING || SOFT_LIGHTING

	uint numLights = min(7, uint(NumLightNumShadowLight.x));
	uint numShadowLights = min(4, uint(NumLightNumShadowLight.y));

#if defined(MODELSPACENORMALS) && !defined(SKINNED)
	float3 worldNormal = normal.xyz;
	float3x3 tbnTr = ReconstructTBN(input.WorldPosition.xyz, worldNormal, screenUV);
#else
	float3 worldNormal = normalize(mul(tbn, normal.xyz));
#	if defined(TREE_ANIM) && !defined(VR)
	float3 viewNormal = normalize(FrameBuffer::WorldToView(worldNormal, false, eyeIndex));
	viewNormal = float3(viewNormal.xy, -abs(viewNormal.z));
	worldNormal = normalize(FrameBuffer::ViewToWorld(viewNormal, false, eyeIndex));
#	elif defined(TREE_ANIM)
	// VR must keep tree normals eye/view independent.
#	endif

#	if defined(SPARKLE)
	float3 projectedNormal = normalize(mul(tbn, float3(ProjectedUVParams2.xx * normal.xy, normal.z)));
#	endif  // SPARKLE
#endif      // defined (MODELSPACENORMALS) && !defined (SKINNED)

#if defined(SKIN) && defined(CS_SKIN)
#	if defined(WETNESS_EFFECTS)
	float3 skinWetNormal = worldNormal.xyz;
#		if defined(FACEGEN)
	float2 wetUV = uv;
#		else
	float2 wetUV = uv * SharedData::skinData.skinDetailParams.y;
#		endif
	float2 dynamicWet = Skin::GetWetness(input.WorldPosition.z + FrameBuffer::CameraPosAdjust[eyeIndex].z, worldNormal.xyz);
	float skinWetness = Skin::PerlinNoise(wetUV, SharedData::skinData.wetParams.x, SharedData::skinData.wetParams.y, SharedData::skinData.wetParams.z, clamp(dynamicWet.x + dynamicWet.y + SharedData::skinData.skinParams2.y, 0.f, 2.f) * (hasSkinWetness ? 1.0 : 0.5));
	if ((SharedData::skinData.skinDetailParams.w > 0.0f || skinWetness > 0.0f) && skinEnabled)
#	else
	if (SharedData::skinData.skinDetailParams.w > 0.0f && skinEnabled)
#	endif
	{
#	if defined(FACEGEN)
		float2 detailUV = input.TexCoord0.xy * SharedData::skinData.skinDetailParams.x;
#	else
		float2 detailUV = input.TexCoord0.xy * SharedData::skinData.skinDetailParams.x * SharedData::skinData.skinDetailParams.y;
#	endif  // FACEGEN
#	if defined(MODELSPACENORMALS)
		const float3x3 tbnTr = Skin::ReconstructTBN(input.WorldPosition.xyz, worldNormal, screenUV);
		const float3x3 tbn = transpose(tbnTr);
		const float3 tangentNormal = mul(tbnTr, worldNormal.xyz);
#	else
		const float3 tangentNormal = normal.xyz;
#	endif  // MODELSPACENORMALS
		float3 detailNormal = float3(Skin::TexSkinDetailNormal.SampleBias(SampNormalSampler, detailUV, SharedData::MipBias - 1.0f).xy, 0.5f);
		skinAO *= Skin::TexSkinDetailNormal.Sample(SampNormalSampler, detailUV).w;
		detailNormal = (detailNormal * 2.0 - 1.0) * SharedData::skinData.skinDetailParams.z;
		float3 combinedTangentNormal = normalize(float3(Skin::ReorientNormal(detailNormal, tangentNormal).xy, tangentNormal.z));
		float3 combinedNormal = normalize(mul(tbn, combinedTangentNormal));
		if (SharedData::skinData.skinDetailParams.w > 0.0f)
			worldNormal.xyz = combinedNormal;
#	if defined(WETNESS_EFFECTS)
		if (skinWetness > 0.0f) {
			float3 wetNormal = Skin::CalculateNormalFromHeight(skinWetness, SharedData::skinData.wetParams.w * 0.0005, uv);
			if (hasSkinWetness) {
				// float3 wetMaskNormal = Skin::CalculateNormalFromHeight(skinWetMask, SharedData::skinData.wetParams.w * 0.00005, uv);
				float3 wetMaskNormal = (skinWetnessNormal.xyz * 2.0 - 1.0);
				wetNormal = Skin::ReorientNormal(wetMaskNormal, wetNormal);
			}
			if (SharedData::skinData.skinParams2.y > 1.0f) {
				wetNormal = lerp(wetNormal, tangentNormal, saturate(SharedData::skinData.skinParams2.y - 1.0f));
			}
			float3 combinedWetNormal = skinWetMask ? wetNormal : combinedTangentNormal;
			skinWetNormal = normalize(mul(tbn, combinedWetNormal));
			skinWetNormal = lerp(worldNormal.xyz, skinWetNormal, skinWetness > 0 ? 1 : 0);
		}
#	endif
	}
#endif  // CS_SKIN

	float projectedMaterialWeight = 0;

	float projWeight = 0;

#if defined(PROJECTED_UV)
	float3 projWorldPos = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float3 triFaceNormal = normalize(-cross(ddx(input.WorldPosition.xyz), ddy(input.WorldPosition.xyz)));
	float3 triWeights = Triplanar::GetWeights(tbnTr[2], triFaceNormal);
	float projNoise = Triplanar::Sample(TexCharacterLightProjNoiseSampler, SampCharacterLightProjNoiseSampler, projWorldPos, triWeights, ProjectedUVParams.z).x;
	float3 texProj = normalize(input.TexProj);
#	if defined(TREE_ANIM) || defined(LODOBJECTSHD)
	float vertexAlpha = 1;
#	else
	float vertexAlpha = input.Color.w;
#	endif  // defined (TREE_ANIM) || defined (LODOBJECTSHD)
	projWeight = -ProjectedUVParams.x * projNoise + (dot(worldNormal.xyz, texProj) * vertexAlpha - ProjectedUVParams.w);
#	if defined(LODOBJECTSHD)
	projWeight += (-0.5 + input.Color.w) * 2.5;
#	endif  // LODOBJECTSHD
#	if defined(SPARKLE)
	if (projWeight < 0)
		discard;

	rawBaseColor = Triplanar::SampleStochasticBias(TexColorSampler, SampColorSampler, projWorldPos, triWeights, ProjectedUVParams2.y, SharedData::MipBias, screenNoise);
	baseColor = float4(Color::Diffuse(rawBaseColor.rgb), rawBaseColor.a);
	worldNormal.xyz = projectedNormal;
#	elif !defined(FACEGEN) && !defined(MULTI_LAYER_PARALLAX) && !defined(PARALLAX) && !defined(SPARKLE)
	if (ProjectedUVParams3.w > 0.5) {
		float diffuseNormalScale = ProjectedUVParams3.x * ProjectedUVParams.z;
		float3 projNormal = TransformNormal(Triplanar::SampleStochastic(TexProjNormalSampler, SampProjNormalSampler, projWorldPos, triWeights, diffuseNormalScale, screenNoise).xyz);
		float detailNormalScale = ProjectedUVParams3.y * ProjectedUVParams.z;
		float3 projDetailNormal = Triplanar::SampleStochastic(TexProjDetail, SampProjDetailSampler, projWorldPos, triWeights, detailNormalScale, screenNoise).xyz;
		float3 finalProjNormal = normalize(TransformNormal(projDetailNormal) * float3(1, 1, projNormal.z) + float3(projNormal.xy, 0));
		float3 projBaseColor = Color::ColorToLinear(Triplanar::SampleStochastic(TexProjDiffuseSampler, SampProjDiffuseSampler, projWorldPos, triWeights, diffuseNormalScale, screenNoise).xyz) * Color::ColorToLinear(ProjectedUVParams2.xyz);
		projectedMaterialWeight = smoothstep(0, 1, 5 * (0.1 + projWeight));
#		if defined(TRUE_PBR)
		projBaseColor = max(0, projBaseColor.xyz * MaterialObjectRGBScale);
		rawRMAOS.xyw = lerp(rawRMAOS.xyw, float3(ParallaxOccData.x, 0, ParallaxOccData.y), projectedMaterialWeight);
		float4 projectedGlintParameters = 0;
		if ((PBRFlags & PBR::Flags::ProjectedGlint) != 0) {
			projectedGlintParameters = SparkleParams;
		}
		glintParameters = lerp(glintParameters, projectedGlintParameters, projectedMaterialWeight);
#		else
		projBaseColor *= Color::VanillaDiffuseColorMult();
#		endif  // TRUE_PBR
#		if defined(LOD_BLENDING) && (defined(LODOBJECTS) || defined(LODOBJECTSHD))
		projBaseColor.xyz = pow(abs(projBaseColor.xyz), SharedData::lodBlendingSettings.LODObjectSnowGamma) * SharedData::lodBlendingSettings.LODObjectSnowBrightness;
#		endif  // LOD_BLENDING
		normal.xyz = lerp(normal.xyz, finalProjNormal, projectedMaterialWeight);
		baseColor.xyz = lerp(baseColor.xyz, projBaseColor, projectedMaterialWeight);

#		if defined(SNOW)
		useSnowDecalSpecular = true;
#		endif  // SNOW
	} else {
		if (projWeight > 0) {
			baseColor.xyz = Color::Diffuse(ProjectedUVParams2.xyz);
#		if defined(SNOW)
			useSnowDecalSpecular = true;
#		endif  // SNOW
		}
	}

#		if defined(SPECULAR)
	useSnowSpecular = useSnowDecalSpecular;
#		endif  // SPECULAR
#	endif      // SPARKLE

#endif  // SNOW

#if defined(WORLD_MAP)
	baseColor.xyz = GetWorldMapBaseColor(rawBaseColor.xyz, baseColor.xyz, projWeight);
#endif  // WORLD_MAP

#if defined(MODELSPACENORMALS)
	float3 vertexNormal = worldNormal;
#endif

	float3 screenSpaceNormal = normalize(FrameBuffer::WorldToView(worldNormal, false, eyeIndex));

#if defined(HAIR) && defined(CS_HAIR)
	float3 Bitangent = normalize(float3(input.TBN0.y, input.TBN1.y, input.TBN2.y));
	float3 hairT = 0;
#	if defined(BACK_LIGHTING)
	hairT = useHairFlowMap ? normalize(mul(tbn, sampledHairFlow)) : Bitangent;
#	else
	hairT = Bitangent;
#	endif
	hairT = Hair::ReorientTangent(hairT, worldNormal);

	if (SharedData::hairSpecularSettings.Enabled) {
		if (SharedData::hairSpecularSettings.EnableTangentShift && SharedData::hairSpecularSettings.HairMode != 1) {
			float3 shiftedNormal = Hair::ShiftWorldNormal(hairT, worldNormal, 0, uv);
			screenSpaceNormal = normalize(FrameBuffer::WorldToView(shiftedNormal, false, eyeIndex));
		}
	}
#endif

	MaterialProperties material = (MaterialProperties)0;

	material.F0 = 0;
	material.Roughness = 1;

#if defined(TRUE_PBR)
	material.Noise = screenNoise;

	material.Roughness = clamp(rawRMAOS.x, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
	material.Metallic = saturate(rawRMAOS.y);
	material.AO = rawRMAOS.z;

	// Apply vertex color to base color so PBR metals use it. On LANDSCAPE,
	// honor DisableTerrainVertexColors (as the non-PBR path does) by
	// neutralizing the source color so terrain vertex colors don't tint PBR;
	// a white source yields VertexAO == 1, i.e. no AO darkening either.
	float3 pbrVertexColorSrc = input.Color.xyz;
#	if defined(LANDSCAPE)
	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		pbrVertexColorSrc = 1;
#	endif
	float3 pbrVertexColor = Color::SrgbToLinear(pbrVertexColorSrc);
	float pbrVertexAO = max(max(pbrVertexColor.x, pbrVertexColor.y), pbrVertexColor.z);
	pbrVertexColor = pbrVertexAO == 0.0f ? 1.0f : pbrVertexColor * lerp(1 / max(pbrVertexAO, 0.001), 1, SharedData::truePBRSettings.VertexAOStrength);

	if (!SharedData::linearLightingSettings.enableLinearLighting) {
		baseColor.xyz = Color::SrgbToLinear(baseColor.xyz) * pbrVertexColor;
		material.F0 = lerp(rawRMAOS.w, baseColor.xyz, material.Metallic);
		baseColor.xyz = Color::LinearToSrgb(baseColor.xyz);
	} else {
		baseColor.xyz *= pbrVertexColor;
		material.F0 = lerp(rawRMAOS.w, baseColor.xyz, material.Metallic);
	}

	material.GlintScreenSpaceScale = max(1, glintParameters.x);
	material.GlintLogMicrofacetDensity = clamp(PBR::Constants::MaxGlintDensity - glintParameters.y, PBR::Constants::MinGlintDensity, PBR::Constants::MaxGlintDensity);
	material.GlintMicrofacetRoughness = clamp(glintParameters.z, PBR::Constants::MinGlintRoughness, PBR::Constants::MaxGlintRoughness);
	material.GlintDensityRandomization = clamp(glintParameters.w, PBR::Constants::MinGlintDensityRandomization, PBR::Constants::MaxGlintDensityRandomization);

#	if defined(GLINT)
	float glintNoise = Random::R1Modified(float(SharedData::FrameCount), (Random::pcg2d(uint2(input.Position.xy)) / 4294967296.0).x);
	Glints::PrecomputeGlints(glintNoise, uvOriginal, ddx(uvOriginal), ddy(uvOriginal), material.GlintScreenSpaceScale, material.GlintCache);
#	endif

	baseColor.xyz *= 1 - material.Metallic;

	material.BaseColor = baseColor.xyz;

	float3 coatWorldNormal = worldNormal;

#	if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
	{
		material.SubsurfaceColor = PBRParams2.xyz;
		material.Thickness = PBRParams2.w;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
		{
			float4 sampledSubsurfaceProperties = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);

			// If LL is off, Diffuse returns sRGB
			material.SubsurfaceColor *= Color::Diffuse(sampledSubsurfaceProperties.xyz);

			if (!SharedData::linearLightingSettings.enableLinearLighting) {
				material.SubsurfaceColor = Color::LinearToSrgb(
					Color::SrgbToLinear(material.SubsurfaceColor) * pbrVertexColor);
			} else {
				material.SubsurfaceColor *= pbrVertexColor;
			}

			material.Thickness *= sampledSubsurfaceProperties.w;
		}
		material.Thickness = lerp(material.Thickness, 1, projectedMaterialWeight);
	}
	else if ((PBRFlags & PBR::Flags::TwoLayer) != 0)
	{
		material.CoatColor = sampledCoatColor.xyz;
		material.CoatStrength = sampledCoatColor.w;
		material.CoatRoughness = MultiLayerParallaxData.x;
		material.CoatF0 = MultiLayerParallaxData.y;

		float2 coatUv = uv;
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			coatUv = uvOriginal;
		}
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 sampledCoatProperties = TexBackLightSampler.Sample(SampBackLightSampler, coatUv);
			material.CoatRoughness *= sampledCoatProperties.w;
			[branch] if ((PBRFlags & PBR::Flags::CoatNormal) != 0)
			{
				coatWorldNormal = normalize(mul(tbn, TransformNormal(sampledCoatProperties.xyz)));
			}
		}
		material.CoatStrength = lerp(material.CoatStrength, 0, projectedMaterialWeight);
	}

	[branch] if ((PBRFlags & PBR::Flags::Fuzz) != 0)
	{
		material.FuzzColor = MultiLayerParallaxData.xyz;
		material.FuzzWeight = MultiLayerParallaxData.w;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 sampledFuzzProperties = TexBackLightSampler.Sample(SampBackLightSampler, uv);
			material.FuzzColor *= Color::Diffuse(sampledFuzzProperties.xyz);
			material.FuzzWeight *= sampledFuzzProperties.w;
		}
		material.FuzzWeight = lerp(material.FuzzWeight, 0, projectedMaterialWeight);
	}
#	endif
#else
	material.BaseColor = baseColor.xyz;
#	if defined(SPECULAR)
	material.Shininess = shininess;
	material.Glossiness = glossiness;
	material.SpecularColor = SpecularColor.xyz;
#	else
	material.Shininess = 0;
	material.Glossiness = 0;
	material.SpecularColor = 0;
#	endif
#	if (defined(RIM_LIGHTING) || defined(SOFT_LIGHTING) || defined(LOAD_SOFT_LIGHTING))
	material.rimSoftLightColor = rimSoftLightColor.xyz;
#	endif
#	if defined(BACK_LIGHTING)
	material.backLightColor = backLightColor.xyz;
#	endif
#endif  // TRUE_PBR

#if defined(SKIN) && defined(CS_SKIN)
	const float ExtraRoughness = BRDF::F_Schlick(0.04, saturate(dot(worldNormal.xyz, viewDirection))) * SharedData::skinData.fuzzParams.w;
	material.Roughness = SharedData::skinData.skinParams.x;
	material.Roughness = saturate(SharedData::skinData.skinParams.x - SharedData::skinData.skinParams.z * material.Glossiness);
	material.RoughnessSecondary = SharedData::skinData.skinParams.y;
	if (skinRoughnessSet) {
		material.Roughness = skinRoughness * SharedData::skinData.physicalParams.x;
		material.RoughnessSecondary = skinRoughness * SharedData::skinData.physicalParams.y;
	}
	material.Roughness = min(1.0, material.Roughness + ExtraRoughness);
	material.RoughnessSecondary = min(1.0, material.RoughnessSecondary + ExtraRoughness);
	material.SecondarySpecIntensity = SharedData::skinData.skinParams2.x;
	material.Thickness = 1 - skinsk.x;
	material.SubsurfaceColor = skinsk.xyz;
	material.F0 = SharedData::skinData.skinParams2.zzz;
	material.AO = skinAO;
	material.Curvature = Skin::CalculateCurvature(worldNormal.xyz);

	material.FuzzWeight = SharedData::skinData.fuzzParams.x;
	material.FuzzRoughness = SharedData::skinData.fuzzParams.y;
	material.FuzzColor = SharedData::skinData.fuzzParams.zzz;

	if (skinRoughnessSet) {
		material.F0 = 0.08f * skinSpecular * SharedData::skinData.physicalParams.z;
		material.FuzzWeight *= skinFuzzMask;
	}
#endif  // CS_SKIN

#if defined(SKIN)
	material.BaseColor = max(material.BaseColor, EPSILON_SKIN_ALBEDO);
#endif

#if defined(CS_HAIR) && defined(HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		material.Shininess = SharedData::hairSpecularSettings.HairGlossiness;
		material.F0 = Hair::HairF0();
		if (SharedData::hairSpecularSettings.HairMode == 1) {
			material.Roughness = 1;
		} else {
			material.Roughness = ShininessToRoughness(material.Shininess * 0.75);
		}
	}
#endif

	bool dynamicCubemap = false;

#if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float envMask = EnvmapData.x * MaterialData.x;

	float viewNormalAngle = dot(worldNormal.xyz, viewDirection);
	float3 envSamplingPoint = (viewNormalAngle * 2) * worldNormal.xyz - viewDirection;

	if (envMask > 0.0) {
		if (EnvmapData.y) {
			envMask *= envMaskBase;
		} else {
			envMask *= glossiness;
		}
	}

	float3 envColor = 0.0;

	if (envMask > 0.0) {
#	if defined(DYNAMIC_CUBEMAPS)
		uint2 envSize;
		TexEnvSampler.GetDimensions(envSize.x, envSize.y);

#		if defined(EMAT)
		if (envSize.x == 1 && envSize.y == 1 || complexMaterial) {
#		else
		if (envSize.x == 1 && envSize.y == 1) {
#		endif

			dynamicCubemap = true;

#		if defined(EMAT)
			if (!complexMaterial)
#		endif
			{
				// Dynamic Cubemap Creator sets this value to black, if it is anything but black it is wrong
				float3 envColorTest = TexEnvSampler.SampleLevel(SampEnvSampler, float3(0.0, 1.0, 0.0), 15).xyz;
				dynamicCubemap = all(envColorTest == 0.0);
			}

#		if defined(CREATOR)
			if (SharedData::cubemapCreatorSettings.Enabled) {
				dynamicCubemap = true;
			}
#		endif

			if (dynamicCubemap) {
				float4 envColorBase = TexEnvSampler.SampleLevel(SampEnvSampler, float3(1.0, 0.0, 0.0), 15);

				if (envColorBase.a < 1.0) {
					material.F0 = Color::SkyrimGammaToLinear(envColorBase.rgb);
					material.Roughness = envColorBase.a;
				} else {
					material.F0 = 1.0;
					material.Roughness = 1.0 / 7.0;
				}

#		if defined(CREATOR)
				if (SharedData::cubemapCreatorSettings.Enabled) {
					material.F0 = SharedData::cubemapCreatorSettings.CubemapColor.rgb;
					material.Roughness = SharedData::cubemapCreatorSettings.CubemapColor.a;
				}
#		endif

#		if defined(EMAT)
				float complexMaterialRoughness = 1.0 - complexMaterialColor.y;
				material.Roughness = lerp(material.Roughness, complexMaterialRoughness, complexMaterial);
				material.F0 = lerp(material.F0, complexSpecular, complexMaterial);
#		endif
			}
		}
#	endif

		if (!dynamicCubemap) {
			float3 envColorBase = Color::SkyrimGammaToLinear(TexEnvSampler.Sample(SampEnvSampler, envSamplingPoint).xyz);
			envColor = envColorBase.xyz * envMask;
		}
	}

#endif  // defined (ENVMAP) || defined (MULTI_LAYER_PARALLAX) || defined(EYE)

	float porosity = 1.0;

#if defined(SKYLIGHTING)
#	if defined(VR)
	float3 positionMSSkylight = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#	else
	float3 positionMSSkylight = input.WorldPosition.xyz;
#	endif
#	if defined(DEFERRED)
	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, worldNormal);
#	else
	sh2 skylightingSH = inWorld ? Skylighting::Sample(positionMSSkylight, worldNormal) : Skylighting::UNIT_SH;
#	endif

#endif

	float4 waterData = SharedData::GetWaterData(input.WorldPosition.xyz, eyeIndex);
	float waterHeight = waterData.w;

	float waterRoughnessSpecular = 1;

#if defined(WETNESS_EFFECTS)
	// Initialize wetness parameters
	float wetness = 0.0;
	float3 wetnessNormal = vertexNormal.xyz;

	// Calculate shore wetness factors
	float wetnessDistToWater = abs(input.WorldPosition.z - waterHeight);
	float shoreFactor = saturate(1.0 - (wetnessDistToWater / SharedData::wetnessEffectsSettings.ShoreRange));
	float shoreFactorAlbedo = (input.WorldPosition.z < waterHeight) ? 1.0 : shoreFactor;

	// Calculate wetness angle and occlusion
	float minWetnessValue = SharedData::wetnessEffectsSettings.MinRainWetness;
	float minWetnessAngle = saturate(max(minWetnessValue, vertexNormal.z));
#	if defined(SKYLIGHTING)
	float wetnessOcclusion = inWorld ? saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1))) : 0.0;
#	else
	float wetnessOcclusion = inWorld;
#	endif
	float flatnessAmount = smoothstep(SharedData::wetnessEffectsSettings.PuddleMaxAngle, 1.0, minWetnessAngle);
	// Calculate raindrop effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	bool shouldCalculateRaindrops = (worldNormal.z > 0.0) &&
	                                (SharedData::wetnessEffectsSettings.Raining > 0.0) &&
	                                (SharedData::wetnessEffectsSettings.EnableRaindropFx) &&
	                                (wetnessOcclusion > 0.5);

	if (shouldCalculateRaindrops) {
#	if defined(SKINNED)
		float3 ripplePosition = input.ModelPosition.xyz;
#	elif defined(DEFERRED)
		float3 ripplePosition = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
#	else
		float3 ripplePosition = !FrameBuffer::FrameParams.y ? input.ModelPosition.xyz : input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
#	endif
		raindropInfo = WetnessEffects::GetRainDrops(ripplePosition, SharedData::wetnessEffectsSettings.Time, wetnessNormal, flatnessAmount);
	}

	// Calculate different wetness types
	float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
	rainWetness = max(rainWetness, raindropInfo.w);

#	if defined(SKIN) || defined(HAIR)
	rainWetness = SharedData::wetnessEffectsSettings.SkinWetness * SharedData::wetnessEffectsSettings.Wetness;
#	endif

#	if defined(CS_SKIN) && !defined(SKIN)
	if (skinEnabled) {
		float2 dynamicWetness = Skin::GetWetness(input.WorldPosition.z + FrameBuffer::CameraPosAdjust[eyeIndex].z, worldNormal.xyz);
#		if defined(TRUE_PBR)
		dynamicWetness.x = lerp(dynamicWetness.x, 0.0f, material.Metallic);
#		endif
		float dynamicWetnessValue = clamp(dynamicWetness.x + dynamicWetness.y, 0.f, 2.f);
#		if defined(HAIR)
		dynamicWetnessValue = min(SharedData::skinData.skinParams2.y + dynamicWetnessValue, 2.0f);
#		endif
		rainWetness += min(dynamicWetnessValue, 1.f);
	}
#	endif
	float shoreWetness = shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness;
	wetness = max(shoreWetness, rainWetness);

	// Calculate puddle effects
	float puddleWetness = SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle;
	float puddle = wetness;

#	if !defined(SKINNED) && !(defined(SKIN) && defined(CS_SKIN))
	if (wetness > 0.0 || puddleWetness > 0.0) {
		float3 puddleCoords = ((input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz) * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
		puddle = Random::perlinNoise(puddleCoords) * 0.5 + 0.5;
		puddle = puddle * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
		puddle *= lerp(wetness, puddleWetness, saturate(puddle - 0.25));
	}
#	endif

	// Apply occlusion and distance factors
	puddle *= saturate(wetnessOcclusion * 2.0) * nearFactor;
	wetnessNormal = lerp(worldNormal.xyz, wetnessNormal, saturate(puddle));

	// Calculate wetness glossiness factors
	float wetnessGlossinessAlbedo = max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness);
	wetnessGlossinessAlbedo *= wetnessGlossinessAlbedo;

	float wetnessGlossinessSpecular = puddle;
	if (input.WorldPosition.z < waterHeight) {
		wetnessGlossinessSpecular *= shoreFactor;
	}

	// Update flatness and normal calculations
	flatnessAmount *= smoothstep(SharedData::wetnessEffectsSettings.PuddleMinWetness, 1.0, wetnessGlossinessSpecular);

	// Apply ripple normal effects
	float3 rippleNormal = normalize(lerp(float3(0, 0, 1), raindropInfo.xyz, lerp(flatnessAmount, 1.0, 0.5)));
	wetnessNormal = WetnessEffects::ReorientNormal(rippleNormal, wetnessNormal);

#	if defined(SKIN) && defined(CS_SKIN)
	if (skinEnabled && (skinWetness > 0.0f)) {
		wetnessNormal = skinWetNormal;
		wetnessGlossinessSpecular = saturate(max(wetnessGlossinessSpecular, skinWetness));
	}
#	endif

	// Minimum roughness prevents an extreme retroreflective peak (NdotH→1) for near-zero
	// roughness puddles. Real water has ripples and surface tension that keep it from being
	// optically perfect; the ripple normal map adds micro-variation but GGX still peaks
	// sharply without this floor.
	static const float wetnessMinPuddleRoughness = 0.05;
	waterRoughnessSpecular = max(saturate(1.0 - wetnessGlossinessSpecular), wetnessMinPuddleRoughness);
#endif

	float llDirLightMult = SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear && (inWorld || inReflection) && !SharedData::InInterior ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 dirLightColor = Color::DirectionalLight(DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;

#if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirLightColor *= ExponentialHeightFog::GetSunlightFogAttenuation(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	}
#endif

#if defined(WATER_EFFECTS)
	dirLightColor *= WaterEffects::ComputeCaustics(waterData, input.WorldPosition.xyz, eyeIndex);
#endif

	// Apply world shadow (terrain shadows, cloud shadows) directly to light color
	if (inWorld || inReflection)
		dirLightColor *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	float dirLightAngle = dot(worldNormal.xyz, DirLightDirection.xyz);

	float3 refractedDirLightDirection = DirLightDirection;
#if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
	{
		if (dot(DirLightDirection, coatWorldNormal) > 0)
			refractedDirLightDirection = -refract(-DirLightDirection, coatWorldNormal, eta);
	}
#endif

	float dirSoftShadow = 1.0;
	float dirVSMDetailedShadow = 1.0;

#if defined(VOLUMETRIC_SHADOWS)
	if (inWorld && !inReflection && ShadowSampling::HasDirectionalShadows())
		dirSoftShadow = ShadowSampling::GetLightingShadow(input.WorldPosition.xyz, eyeIndex, dirVSMDetailedShadow);
#endif

	float dirDetailedShadow = 1.0;

	float2 rotation;
	sincos(Math::TAU * screenNoise, rotation.y, rotation.x);
	float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
	float3 worldPositionWS = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	// Engine pre-renders the 4-cascade directional shadow into a screen-space
	// mask at t14. LLF samples only cascades 0/1; we pass the engine mask
	// through so LLF::GetDirectionalShadow can fall back to it past
	// EndSplitDistances.y instead of returning fully-lit.
	float4 shadowColor = (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) ? TexShadowMaskSampler.Load(int3(input.Position.xy, 0)) : 1.0;

	// Use HasDirectionalShadows() (= !IsInterior() || InteriorSun::IsActive) instead of
	// the bare !InInterior gate, so Interior Sun cells reach the LLF cascade + engine-mask
	// sampling path. Without this, interior scenes with active Interior Sun render with
	// zero directional contribution and no sun shadow.
	if (inWorld && !inReflection && ShadowSampling::HasDirectionalShadows()) {
#if !defined(LOD)
		// On non-deferred passes, use the cheaper VSM shadows if available
#	if defined(LIGHT_LIMIT_FIX) && (defined(DEFERRED) || !defined(VOLUMETRIC_SHADOWS))
		dirDetailedShadow = LightLimitFix::GetDirectionalShadow(input.WorldPosition.xyz, worldPositionWS, rotationMatrix, eyeIndex,
			(Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir) ? shadowColor.x : 1.0);
#	elif !defined(LIGHT_LIMIT_FIX)
		dirDetailedShadow = (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir) ? shadowColor.x : 1.0;
#	endif  // LIGHT_LIMIT_FIX

#	if defined(VOLUMETRIC_SHADOWS)
		float vsmDetailedShadow = 1.0;
		dirSoftShadow = VolumetricShadows::GetVSMShadow2D(input.WorldPosition.xyz, worldPositionWS, eyeIndex, vsmDetailedShadow);
		dirSoftShadow = max(dirSoftShadow, dirDetailedShadow);

#		if !defined(LIGHT_LIMIT_FIX)
		if (!(Permutation::PixelShaderDescriptor & Permutation::LightingFlags::ShadowDir))
			dirDetailedShadow = vsmDetailedShadow;
#		elif !(defined(DEFERRED))
		dirDetailedShadow = vsmDetailedShadow;
#		endif

#	else
		dirSoftShadow = dirDetailedShadow;
#	endif  // VOLUMETRIC_SHADOWS
#endif

#if defined(SCREEN_SPACE_SHADOWS) && defined(DEFERRED)
		if (!SharedData::InInterior && dirLightAngle >= 0.0)
			dirDetailedShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise, eyeIndex);
#endif  // SCREEN_SPACE_SHADOWS
	}

#if defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
	[branch] if (inWorld && SharedData::extendedMaterialSettings.EnableShadows)
	{
		float3 dirLightDirectionTS = mul(refractedDirLightDirection, tbn).xyz;
#	if defined(LANDSCAPE)
#		if defined(TRUE_PBR)
		if (SharedData::extendedMaterialSettings.EnableParallax) {
#		else
		if (SharedData::extendedMaterialSettings.EnableTerrainParallax || (SharedData::extendedMaterialSettings.EnableParallax && Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement)) {
#		endif
#		if defined(TERRAIN_VARIATION)
			float weights[6];
			// Initialize weights array
			weights[0] = weights[1] = weights[2] = weights[3] = weights[4] = weights[5] = 0.0;

			float sh0 = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, parallaxShadowQuality, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, dx, dy, weights);

			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, dirLightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams, sharedOffset, dx, dy);
#		else
			// Standard terrain parallax shadow without stochastic sampling
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, mipLevels, dirLightDirectionTS, sh0, parallaxShadowQuality, screenNoise, displacementParams);
#		endif
		}
#	elif defined(PARALLAX)
		[branch] if (SharedData::extendedMaterialSettings.EnableParallax)
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, lerp(parallaxShadowQuality, 1.0, SharedData::extendedMaterialSettings.ExtendShadows), screenNoise, displacementParams);
#	elif defined(EMAT_ENVMAP)
		[branch] if (complexMaterialParallax)
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexEnvMaskSampler, SampEnvMaskSampler, 3, lerp(parallaxShadowQuality, 1.0, SharedData::extendedMaterialSettings.ExtendShadows), screenNoise, displacementParams);
#	elif defined(TRUE_PBR) && !defined(LODLANDSCAPE) && !defined(FACEGEN)
		[branch] if (PBRParallax)
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, lerp(parallaxShadowQuality, 1.0, SharedData::extendedMaterialSettings.ExtendShadows), screenNoise, displacementParams);
#	endif  // LANDSCAPE
	}
#endif  // defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

#if defined(CS_HAIR) && defined(HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		vertexNormal.xyz = worldNormal.xyz;
		worldNormal.xyz = hairT;
	}
#endif

	float3 diffuseColor = 0.0.xxx;
	float3 specularColor = 0.0.xxx;
	float3 transmissionColor = 0.0.xxx;

	float3 lightsDiffuseColor = 0.0.xxx;
	float3 coatLightsDiffuseColor = 0.0.xxx;
	float3 lightsSpecularColor = 0.0.xxx;

	float3 lodLandDiffuseColor = 0;

	// Directiontal Lighting
	DirectContext dirLightContext;
	DirectLightingOutput dirLightOutput;
#if defined(TRUE_PBR)
	dirLightContext = CreateDirectLightingContext(worldNormal.xyz, coatWorldNormal, vertexNormal.xyz, refractedViewDirection, viewDirection, refractedDirLightDirection, DirLightDirection, dirLightColor, dirDetailedShadow, dirSoftShadow);
#else
	dirLightContext = CreateDirectLightingContext(worldNormal.xyz, vertexNormal.xyz, viewDirection, DirLightDirection, dirLightColor, dirDetailedShadow, dirSoftShadow);
#	if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, DirLightDirection, screenNoise, eyeIndex);
		dirLightContext.hairShadow = hairShadow;
	}
#	endif
#endif

	float2 uvOriginal_ddx = ddx(uvOriginal);
	float2 uvOriginal_ddy = ddy(uvOriginal);
	EvaluateLighting(dirLightContext, material, tbnTr, uvOriginal, uvOriginal_ddx, uvOriginal_ddy, dirLightOutput);
#if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		EvaluateWetnessLighting(wetnessNormal, dirLightContext, waterRoughnessSpecular, dirLightOutput);
#endif

	lightsDiffuseColor += dirLightOutput.diffuse;
	lightsSpecularColor += dirLightOutput.specular;
#if defined(TRUE_PBR)
	coatLightsDiffuseColor += dirLightOutput.coatDiffuse;
#	if defined(LOD_LAND_BLEND)
	lodLandDiffuseColor += dirLightColor / Math::PI * saturate(dirLightAngle) * dirDetailedShadow;
#	endif
#endif
	transmissionColor += dirLightOutput.transmission;

#include "LightingPS_LightLoop.hlsli"

	diffuseColor += lightsDiffuseColor;
	specularColor += lightsSpecularColor;

#if !defined(LANDSCAPE)
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::CharacterLight) {
		float charLightMul = saturate(dot(viewDirection, worldNormal.xyz)) * CharacterLightParams.x + CharacterLightParams.y * saturate(dot(float2(0.164398998, -0.986393988), worldNormal.yz));
		float charLightColor = min(CharacterLightParams.w, max(0, CharacterLightParams.z * TexCharacterLightProjNoiseSampler.Sample(SampCharacterLightProjNoiseSampler, screenUV).x));
		diffuseColor += (charLightMul * charLightColor).xxx;
	}
#endif

#if defined(EYE) && defined(VANILLA_EYE_NORMAL)
	worldNormal.xyz = input.EyeNormal;
#endif  // EYE

	// sRGB by default, linear if LL on
	float3 emitColor = Color::EmitColor(EmitColor);
#if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	bool hasEmissive = (0x3F & (Permutation::PixelShaderDescriptor >> 24)) == Permutation::LightingTechnique::Glowmap;
#	if defined(TRUE_PBR)
	hasEmissive = hasEmissive || (PBRFlags & PBR::Flags::HasEmissive != 0);
#	endif
	[branch] if (hasEmissive)
	{
		// Input TexGlowSampler = linear by default, but Color::Glowmap returns in sRGB if LL disabled
		float3 glowColor = Color::Glowmap(TexGlowSampler.Sample(SampGlowSampler, uv).xyz);

#	if defined(TRUE_PBR)
		float3 emitVertexColor = Color::SrgbToLinear(input.Color.xyz);
		float emitVertexAO = max(max(emitVertexColor.r, emitVertexColor.g), emitVertexColor.b);
		emitVertexColor = emitVertexAO == 0.0f ? 1.0f : emitVertexColor * lerp(1 / max(emitVertexAO, 1e-4), 1, SharedData::truePBRSettings.VertexAOStrength);

		if (!SharedData::linearLightingSettings.enableLinearLighting) {
			emitColor = Color::SrgbToLinear(emitColor);
			glowColor = Color::SrgbToLinear(glowColor);
			emitColor *= glowColor;
			emitColor *= emitVertexColor;
			emitColor = Color::LinearToSrgb(emitColor);
		} else {
			emitColor *= glowColor;
			emitColor *= emitVertexColor;
		}
#	else
		if (!SharedData::linearLightingSettings.enableLinearLighting) {
			emitColor = Color::LinearToSrgb(Color::SrgbToLinear(emitColor) * Color::SrgbToLinear(glowColor));
		} else {
			emitColor *= glowColor;
		}
#	endif  // TRUE_PBR
	}
#endif

#if !defined(TRUE_PBR)
	diffuseColor += emitColor.xyz;
#endif

	IndirectContext indirectContext = (IndirectContext)0;
	IndirectLobeWeights indirectLobeWeights;

	float3 ambientNormal = worldNormal.xyz;
#if defined(HAIR) && defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled) {
		if (SharedData::hairSpecularSettings.HairMode == 1)
			ambientNormal = normalize(viewDirection - hairT * dot(viewDirection, hairT));
		else
			ambientNormal = vertexNormal.xyz;
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(ambientNormal, false, eyeIndex));
	}
#endif

	float3 directionalAmbientColor = Color::Ambient(max(0, SharedData::GetAmbient(ambientNormal)));

#if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		if (SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection) {
			directionalAmbientColor = ImageBasedLighting::GetStaticDiffuseIBL(ambientNormal, SampColorSampler);
		}
	}
#endif

#if defined(SKYLIGHTING)
	float skylightingDiffuse = 1;
	float skylightingFadeOutFactor = 1.0;
	if (!SharedData::InInterior) {
		skylightingFadeOutFactor = Skylighting::GetFadeOutFactor(input.WorldPosition.xyz);
		skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, ambientNormal, skylightingFadeOutFactor);
	}
#endif

#if defined(LANDSCAPE)
	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		input.Color.xyz = 1;
	else
		input.Color.xyz /= max(max(max(input.Color.x, input.Color.y), input.Color.z), EPSILON_DIVISION);
#endif

#if defined(HAIR)
	float3 vertexColor = lerp(1, Color::ColorToLinear(TintColor.xyz), Color::ColorToLinear(input.Color.y));
	float vertexAO = 1;
#	if defined(CS_HAIR)
	if (SharedData::hairSpecularSettings.Enabled)
		vertexColor = 1;
#	endif
#elif defined(SKYLIGHTING)
	float3 vertexColor = input.Color.xyz;
	float vertexAO = Color::ColorToLinear(max(max(vertexColor.r, vertexColor.g), vertexColor.b).xxx).x;
#	if defined(TRUE_PBR)
	vertexAO = lerp(1, vertexAO, SharedData::truePBRSettings.VertexAOStrength);
	vertexColor = 1;
#	endif
	// Modify skylightingDiffuse such that skylightingDiffuse * vertexAO = min(skylightingDiffuse, vertexAO)
	skylightingDiffuse = saturate(skylightingDiffuse / max(vertexAO, 1e-5));
#else
#	if defined(TRUE_PBR)
	float3 vertexColor = 1;
#	else
	float3 vertexColor = input.Color.xyz;
#	endif
	float vertexAO = Color::ColorToLinear(max(max(vertexColor.r, vertexColor.g), vertexColor.b).xxx).x;
#endif  // defined (HAIR)

#if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		if (!(SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection)) {
#	if defined(SKYLIGHTING)
			directionalAmbientColor = ImageBasedLighting::GetDiffuseIBLOccluded(directionalAmbientColor, -ambientNormal, skylightingDiffuse);
#	else
			directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(directionalAmbientColor, -ambientNormal);
#	endif
		}
	}
#endif

	float3 reflectionDiffuseColor = diffuseColor + directionalAmbientColor;

#if defined(TRUE_PBR) && defined(LOD_LAND_BLEND) && !defined(DEFERRED)
	lodLandDiffuseColor += directionalAmbientColor;
#endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);

#if defined(WETNESS_EFFECTS)
#	if !(defined(FACEGEN) || defined(FACEGEN_RGB_TINT) || defined(EYE)) || defined(TREE_ANIM)
#		if defined(TRUE_PBR)
#			if !defined(LANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::TwoLayer) != 0)
	{
		porosity = 0;
	}
	else
#			endif
	{
		porosity = lerp(porosity, 0.0, saturate(sqrt(material.Metallic)));
	}
#		elif defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX)
	porosity = lerp(porosity, 0.0, saturate(sqrt(envMask)));
#		endif
	float wetnessDarkeningAmount = porosity * wetnessGlossinessAlbedo;
	material.BaseColor = lerp(material.BaseColor, pow(abs(material.BaseColor), 1.0 + wetnessDarkeningAmount), 0.5);
#	endif
#endif

	float4 color = 0;

	indirectContext = CreateIndirectLightingContext(ambientNormal, vertexNormal.xyz, viewDirection);

	GetIndirectLobeWeights(indirectLobeWeights, indirectContext, material, uvOriginal);

#if defined(WETNESS_EFFECTS)
#	if defined(DYNAMIC_CUBEMAPS)
	float3 wetnessReflectance = GetWetnessIndirectLobeWeights(indirectLobeWeights, wetnessNormal, waterRoughnessSpecular, indirectContext);
#	else
	float3 wetnessReflectance = 0.0;
#	endif
#endif
#if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	indirectLobeWeights.specular *= envMask;
#endif

#if defined(SPECULAR) && !defined(TRUE_PBR)
	indirectLobeWeights.specular *= MaterialData.yyy;
	specularColor *= MaterialData.yyy;
#endif

#if defined(TRUE_PBR)
	{
		float3 directLightsDiffuseInput = diffuseColor * material.BaseColor;
		[branch] if ((PBRFlags & PBR::Flags::ColoredCoat) != 0)
		{
			directLightsDiffuseInput = lerp(directLightsDiffuseInput, material.CoatColor * coatLightsDiffuseColor, material.CoatStrength);
		}

		color.xyz += directLightsDiffuseInput;
	}

	// Fixes white items in UI for VR
	[branch] if ((PBRFlags & PBR::Flags::HasEmissive) != 0)
	{
		color.xyz += emitColor.xyz;
	}
#else
	color.xyz += diffuseColor * material.BaseColor;
#endif

	color.xyz += indirectLobeWeights.diffuse * directionalAmbientColor;
	color.xyz += transmissionColor;

	color.xyz *= vertexColor;

#if defined(MULTI_LAYER_PARALLAX)
	float layerValue = MultiLayerParallaxData.x * TexLayerSampler.Sample(SampLayerSampler, uv).w;
	float3 tangentViewDirection = mul(viewDirection, tbn);
	float3 layerNormal = MultiLayerParallaxData.yyy * (normalColor.xyz * 2.0.xxx + float3(-1, -1, -2)) + float3(0, 0, 1);
	float layerViewAngle = dot(-tangentViewDirection.xyz, layerNormal.xyz) * 2;
	float3 layerViewProjection = -layerNormal.xyz * layerViewAngle.xxx - tangentViewDirection.xyz;
	float2 layerUv = uv * MultiLayerParallaxData.zw + (0.0009765625 * (layerValue / abs(layerViewProjection.z))).xx * layerViewProjection.xy;

	float3 layerColor = TexLayerSampler.Sample(SampLayerSampler, layerUv).xyz;

	float mlpBlendFactor = saturate(viewNormalAngle) * (1.0 - baseColor.w);

#	if defined(SKYLIGHTING)
	color.xyz = lerp(color.xyz, (diffuseColor + directionalAmbientColor * skylightingDiffuse) * vertexColor * layerColor, mlpBlendFactor);
#	else
	color.xyz = lerp(color.xyz, (diffuseColor + directionalAmbientColor) * vertexColor * layerColor, mlpBlendFactor);
#	endif

	indirectLobeWeights.diffuse *= 1.0 - mlpBlendFactor;
#endif  // MULTI_LAYER_PARALLAX

#if defined(SNOW)
	if (useSnowSpecular)
		specularColor = 0;
#endif

	diffuseColor = reflectionDiffuseColor;

#if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#	if defined(DYNAMIC_CUBEMAPS)
	if (!dynamicCubemap)
#	endif
		specularColor += envColor * Color::IrradianceToLinear(diffuseColor);
	indirectLobeWeights.diffuse += envColor;
#endif

#if defined(EMAT_ENVMAP)
	specularColor *= complexSpecular;
#endif  // defined (EMAT) && defined(ENVMAP)

#if defined(LOD_LAND_BLEND) && defined(TRUE_PBR)
	{
		lodLandDiffuseColor += directionalAmbientColor;
		float3 litLodLandColor = vertexColor * lodLandColor.xyz * lodLandFadeFactor * lodLandDiffuseColor;
		color.xyz = lerp(color.xyz * Color::PBRLightingScale, litLodLandColor, lodLandBlendFactor);

		specularColor = lerp(specularColor * Color::PBRLightingScale, 0, lodLandBlendFactor);
		indirectLobeWeights.diffuse = lerp(indirectLobeWeights.diffuse * Color::PBRLightingScale, vertexColor * lodLandColor.xyz * lodLandFadeFactor, lodLandBlendFactor);
		indirectLobeWeights.specular = lerp(indirectLobeWeights.specular, 0, lodLandBlendFactor);
		material.Roughness = lerp(material.Roughness, 1, lodLandBlendFactor);
	}
#elif defined(TRUE_PBR)
	color.xyz *= Color::PBRLightingScale;
	specularColor *= Color::PBRLightingScale;
	indirectLobeWeights.diffuse *= Color::PBRLightingScale;
#endif

	float3 outputAlbedo = indirectLobeWeights.diffuse * vertexColor.xyz;

	directionalAmbientColor *= outputAlbedo;

#if defined(SKYLIGHTING)
#	if defined(IBL)
	if (!SharedData::iblSettings.EnableIBL)
#	endif
	{
		Skylighting::ApplySkylighting(color.xyz, directionalAmbientColor, outputAlbedo, skylightingDiffuse);
	}
#endif

#if !defined(DEFERRED)
	color.xyz = Color::IrradianceToLinear(color.xyz);
	color.xyz += specularColor;

	if (any(indirectLobeWeights.specular > 0)
#	if defined(WETNESS_EFFECTS)
		|| any(wetnessReflectance > 0)
#	endif
	)
#	if defined(DYNAMIC_CUBEMAPS)
#		if defined(SKYLIGHTING)
		color.xyz += indirectLobeWeights.specular * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(worldNormal, viewDirection, material.Roughness, skylightingSH);
#			if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		color.xyz += wetnessReflectance * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(wetnessNormal, viewDirection, waterRoughnessSpecular, skylightingSH);
#			endif
#		else
		color.xyz += indirectLobeWeights.specular * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(worldNormal, viewDirection, material.Roughness);
#			if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		color.xyz += wetnessReflectance * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(wetnessNormal, viewDirection, waterRoughnessSpecular);
#			endif
#		endif
#	else
		color.xyz += indirectLobeWeights.specular * directionalAmbientColor;
#	endif

	color.xyz = Color::IrradianceToGamma(color.xyz);
	float3 fogColor = Color::Fog(input.FogParam.xyz);
	float fogFactor = Color::FogAlpha(input.FogParam.w);
#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#	endif
#	if defined(EXP_HEIGHT_FOG)
	float3 vanillaFogColor = fogColor;
	float vanillaFogFactor = fogFactor;
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog;
		if (inReflection) {
			exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFogNoVolumetric(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		} else {
			exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		}
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
	}
#	endif
	if ((FrameBuffer::FrameParams.y && FrameBuffer::FrameParams.z) || inReflection) {
#	if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled) {
			if (!ExponentialHeightFog::ShouldDisableVanillaFog()) {
				color.xyz = lerp(color.xyz, vanillaFogColor, vanillaFogFactor);
			}
			color.xyz = lerp(color.xyz, fogColor, fogFactor);
		} else {
			color.xyz = lerp(color.xyz, fogColor, fogFactor);
		}
#	else
		color.xyz = lerp(color.xyz, fogColor, fogFactor);
#	endif
	}
#endif

#if defined(TESTCUBEMAP) && defined(ENVMAP) && defined(DYNAMIC_CUBEMAPS)
	baseColor.xyz = 0.0;
	specularColor = 0.0;
	diffuseColor = 0.0;
	dynamicCubemap = true;
	envColor = 1.0;
	material.Roughness = 0.0;
	color.xyz = 0;
#endif

#if defined(LANDSCAPE) && !defined(LOD_LAND_BLEND)
	psout.Diffuse.w = 0;
#else
	float alpha = baseColor.w;
#	if defined(EMAT) && !defined(LANDSCAPE)
#		if defined(PARALLAX)
	alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
#		elif defined(TRUE_PBR)
	[branch] if (PBRParallax)
	{
		alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
	}
#		endif
#	endif
#	if defined(DO_ALPHA_TEST)
	[branch] if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::AdditionalAlphaMask) != 0)
	{
		uint2 alphaMask = input.Position.xy;
		alphaMask.x = ((alphaMask.x << 2) & 12);
		alphaMask.x = (alphaMask.y & 3) | (alphaMask.x & ~3);
		const float maskValues[16] = {
			0.003922,
			0.533333,
			0.133333,
			0.666667,
			0.800000,
			0.266667,
			0.933333,
			0.400000,
			0.200000,
			0.733333,
			0.066667,
			0.600000,
			0.996078,
			0.466667,
			0.866667,
			0.333333,
		};

		float testTmp = 0;
		if (MaterialData.z - maskValues[alphaMask.x] < 0) {
			discard;
		}
	}
	else
#	endif  // defined(DO_ALPHA_TEST)
	{
		alpha *= MaterialData.z;
	}
#	if !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
	alpha *= input.Color.w;
#	endif  // !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
#	if defined(DO_ALPHA_TEST)
#		if defined(DEPTH_WRITE_DECALS)
	if (alpha - 0.0156862754 < 0) {
		discard;
	}
	alpha = saturate(1.05 * alpha);
#		endif  // DEPTH_WRITE_DECALS
	if (alpha - AlphaTestRefRS < 0) {
		discard;
	}
#	endif      // DO_ALPHA_TEST

#	if defined(ANISOTROPIC_ALPHA)
	// Uniform alpha material settings
	uint AlphaMaterialModel = ExtendedTranslucency::GetMaterialModelFromDescriptor(Permutation::ExtraFeatureDescriptor);
	float AlphaMaterialReduction = 0.f;
	float AlphaMaterialSoftness = 0.f;
	float AlphaMaterialStrength = 0.f;
	[branch] if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::Default)
	{
		AlphaMaterialModel = SharedData::extendedTranslucencySettings.MaterialModel;
		AlphaMaterialReduction = SharedData::extendedTranslucencySettings.Reduction;
		AlphaMaterialSoftness = SharedData::extendedTranslucencySettings.Softness;
		AlphaMaterialStrength = SharedData::extendedTranslucencySettings.Strength;
	}

	[branch] if (ExtendedTranslucency::IsValidMaterial(AlphaMaterialModel))
	{
		if (alpha >= 0.0156862754 && alpha < 1.0) {
			float originalAlpha = alpha;
			alpha = alpha * (1.0 - AlphaMaterialReduction);
			[branch] if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::AnisotropicFabric)
			{
#		if defined(SKINNED) || !defined(MODELSPACENORMALS)
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric2D(alpha, viewDirection, tbnTr);
#		else
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric1D(alpha, viewDirection, worldNormal.xyz);
#		endif
			}
			else if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::IsotropicFabric)
			{
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric1D(alpha, viewDirection, worldNormal.xyz);
			}
			else
			{
				alpha = ExtendedTranslucency::GetViewDependentAlphaNaive(alpha, viewDirection, worldNormal.xyz);
			}
			alpha = saturate(ExtendedTranslucency::SoftClamp(alpha, 2.0f - AlphaMaterialSoftness));
			alpha = lerp(alpha, originalAlpha, AlphaMaterialStrength);
		}
	}
#	endif  // ANISOTROPIC_ALPHA

	psout.Diffuse.w = alpha;
#endif

#if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		psout.Diffuse.xyz = LightLimitFix::LLFDebugGetVizColor(
			llfDebug,
			Color::TurboColormap(LightLimitFix::NumStrictLights >= 7.0),
			Color::TurboColormap((float)LightLimitFix::NumStrictLights / 15.0),
			Color::TurboColormap((float)numClusteredLights / MAX_CLUSTER_LIGHTS),
			float3(dirSoftShadow, dirDetailedShadow, 0.0),
			color.xyz);
		baseColor.xyz = 0.0;
	} else {
		psout.Diffuse.xyz = color.xyz;
	}
#else
	psout.Diffuse.xyz = color.xyz;
#endif  // defined(LIGHT_LIMIT_FIX)

	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, psout.Diffuse.w);

#if defined(DEFERRED)

#	if defined(TERRAIN_BLENDING)
	[flatten] if (SharedData::terrainBlendingSettings.Enabled)
	{
		psout.Diffuse.w = blendFactorTerrain;
	}
#	endif

	psout.MotionVectors.zw = float2(0.0, psout.Diffuse.w);
	psout.Specular = float4(specularColor, psout.Diffuse.w);
	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

#	if defined(WETNESS_EFFECTS)
	indirectLobeWeights.specular += wetnessReflectance;
	if (waterRoughnessSpecular < 1) {
		// Reflection is from the water film surface; wetnessReflectance scales intensity by wetness amount.
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(wetnessNormal, false, eyeIndex));
		material.Roughness = waterRoughnessSpecular;
	}
#	endif

	psout.Reflectance = float4(indirectLobeWeights.specular, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), saturate(1.0 - material.Roughness), psout.Diffuse.w);

	float masksZ = Color::RGBToYCoCg(directionalAmbientColor).x;

#	if defined(SSS) && defined(SKIN)
	psout.Masks = float4(saturate(baseColor.a), !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsBeastRace), masksZ, psout.Diffuse.w);
#	else
	psout.Masks = float4(0, 0, masksZ, psout.Diffuse.w);
#	endif

	// Stored as 1 - vertexAO so the cleared default (0) means no occlusion
	// for pixels that do not write to this RT (sky, water, grass, effects).
	psout.Masks2 = float4(1.0 - vertexAO, 0, 0, 0);

	float stochasticBlend = (screenNoise * screenNoise) < psout.Diffuse.w ? 1.0 : 0.0;
	psout.NormalGlossiness.w = stochasticBlend;
#endif

#if !defined(HDR_OUTPUT)  // Do not apply gamma correction before we pass to ISHDR.
	if ((!inWorld && !inReflection) && SharedData::linearLightingSettings.enableLinearLighting) {
		psout.Diffuse.xyz = Color::LinearToSrgb(psout.Diffuse.xyz);
	}
#endif

	return psout;
}
