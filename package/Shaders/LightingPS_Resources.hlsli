
SamplerState SampTerrainParallaxSampler : register(s1);

#if defined(LANDSCAPE)

SamplerState SampColorSampler : register(s0);

#	define SampLandColor2Sampler SampColorSampler
#	define SampLandColor3Sampler SampColorSampler
#	define SampLandColor4Sampler SampColorSampler
#	define SampLandColor5Sampler SampColorSampler
#	define SampLandColor6Sampler SampColorSampler
#	define SampNormalSampler SampColorSampler
#	define SampLandNormal2Sampler SampColorSampler
#	define SampLandNormal3Sampler SampColorSampler
#	define SampLandNormal4Sampler SampColorSampler
#	define SampLandNormal5Sampler SampColorSampler
#	define SampLandNormal6Sampler SampColorSampler
#	define SampRMAOSSampler SampColorSampler
#	define SampLandRMAOS2Sampler SampColorSampler
#	define SampLandRMAOS3Sampler SampColorSampler
#	define SampLandRMAOS4Sampler SampColorSampler
#	define SampLandRMAOS5Sampler SampColorSampler
#	define SampLandRMAOS6Sampler SampColorSampler

#else

SamplerState SampColorSampler : register(s0);

#	define SampNormalSampler SampColorSampler

#	if defined(MODELSPACENORMALS) && !defined(LODLANDNOISE)
SamplerState SampSpecularSampler : register(s2);
#	endif
#	if defined(FACEGEN)
SamplerState SampTintSampler : register(s3);
SamplerState SampDetailSampler : register(s4);
#	elif defined(PARALLAX)
SamplerState SampParallaxSampler : register(s3);
#	elif defined(PROJECTED_UV) && !defined(SPARKLE)
SamplerState SampProjDiffuseSampler : register(s3);
#	endif

#	if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(SNOW_FLAG) || defined(EYE)) && !defined(FACEGEN)
SamplerState SampEnvSampler : register(s4);
SamplerState SampEnvMaskSampler : register(s5);
#	endif

#	if defined(TRUE_PBR) && !defined(FACEGEN)
SamplerState SampParallaxSampler : register(s4);
#	endif
#	if defined(TRUE_PBR)
SamplerState SampRMAOSSampler : register(s5);
#	endif

SamplerState SampGlowSampler : register(s6);

#	if defined(MULTI_LAYER_PARALLAX)
SamplerState SampLayerSampler : register(s8);
#	elif defined(PROJECTED_UV) && !defined(SPARKLE)
SamplerState SampProjNormalSampler : register(s8);
#	endif

SamplerState SampBackLightSampler : register(s9);

#	if defined(PROJECTED_UV)
SamplerState SampProjDetailSampler : register(s10);
#	endif

SamplerState SampCharacterLightProjNoiseSampler : register(s11);
SamplerState SampRimSoftLightWorldMapOverlaySampler : register(s12);

#	if defined(WORLD_MAP) && (defined(LODLANDSCAPE) || defined(LODLANDNOISE))
SamplerState SampWorldMapOverlaySnowSampler : register(s13);
#	endif

#endif

#if defined(LOD_LAND_BLEND)
SamplerState SampLandLodBlend1Sampler : register(s13);
SamplerState SampLandLodBlend2Sampler : register(s15);
#elif defined(LODLANDNOISE)
SamplerState SampLandLodNoiseSampler : register(s15);
#endif

#if defined(LANDSCAPE)

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexLandColor2Sampler : register(t1);
Texture2D<float4> TexLandColor3Sampler : register(t2);
Texture2D<float4> TexLandColor4Sampler : register(t3);
Texture2D<float4> TexLandColor5Sampler : register(t4);
Texture2D<float4> TexLandColor6Sampler : register(t5);
Texture2D<float4> TexNormalSampler : register(t7);
Texture2D<float4> TexLandNormal2Sampler : register(t8);
Texture2D<float4> TexLandNormal3Sampler : register(t9);
Texture2D<float4> TexLandNormal4Sampler : register(t10);
Texture2D<float4> TexLandNormal5Sampler : register(t11);
Texture2D<float4> TexLandNormal6Sampler : register(t12);

Texture2D<float4> TexLandTHDisp0Sampler : register(t92);
Texture2D<float4> TexLandTHDisp1Sampler : register(t93);
Texture2D<float4> TexLandTHDisp2Sampler : register(t94);
Texture2D<float4> TexLandTHDisp3Sampler : register(t95);
Texture2D<float4> TexLandTHDisp4Sampler : register(t96);
Texture2D<float4> TexLandTHDisp5Sampler : register(t97);

#	if defined(TRUE_PBR)

Texture2D<float4> TexLandDisplacement0Sampler : register(t80);
Texture2D<float4> TexLandDisplacement1Sampler : register(t81);
Texture2D<float4> TexLandDisplacement2Sampler : register(t82);
Texture2D<float4> TexLandDisplacement3Sampler : register(t83);
Texture2D<float4> TexLandDisplacement4Sampler : register(t84);
Texture2D<float4> TexLandDisplacement5Sampler : register(t85);

Texture2D<float4> TexRMAOSSampler : register(t86);
Texture2D<float4> TexLandRMAOS2Sampler : register(t87);
Texture2D<float4> TexLandRMAOS3Sampler : register(t88);
Texture2D<float4> TexLandRMAOS4Sampler : register(t89);
Texture2D<float4> TexLandRMAOS5Sampler : register(t90);
Texture2D<float4> TexLandRMAOS6Sampler : register(t91);

#	endif

#else

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);  // normal in xyz, glossiness in w if not modelspacenormal

#	if defined(MODELSPACENORMALS) && !defined(LODLANDNOISE)
Texture2D<float4> TexSpecularSampler : register(t2);
#	endif
#	if defined(FACEGEN)
Texture2D<float4> TexTintSampler : register(t3);
Texture2D<float4> TexDetailSampler : register(t4);
#	elif defined(PARALLAX)
Texture2D<float4> TexParallaxSampler : register(t3);
#	elif defined(PROJECTED_UV) && !defined(SPARKLE)
Texture2D<float4> TexProjDiffuseSampler : register(t3);
#	endif

#	if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(SNOW_FLAG) || defined(EYE)) && !defined(FACEGEN)
TextureCube<float4> TexEnvSampler : register(t4);
Texture2D<float4> TexEnvMaskSampler : register(t5);
#	endif

#	if defined(TRUE_PBR) && !defined(FACEGEN)
Texture2D<float4> TexParallaxSampler : register(t4);
#	endif
#	if defined(TRUE_PBR)
Texture2D<float4> TexRMAOSSampler : register(t5);
#	endif

Texture2D<float4> TexGlowSampler : register(t6);

#	if defined(MULTI_LAYER_PARALLAX)
Texture2D<float4> TexLayerSampler : register(t8);
#	elif defined(PROJECTED_UV) && !defined(SPARKLE)
Texture2D<float4> TexProjNormalSampler : register(t8);
#	endif

Texture2D<float4> TexBackLightSampler : register(t9);

#	if defined(PROJECTED_UV)
Texture2D<float4> TexProjDetail : register(t10);
#	endif

Texture2D<float4> TexCharacterLightProjNoiseSampler : register(t11);
Texture2D<float4> TexRimSoftLightWorldMapOverlaySampler : register(t12);

#	if defined(WORLD_MAP) && (defined(LODLANDSCAPE) || defined(LODLANDNOISE))
Texture2D<float4> TexWorldMapOverlaySnowSampler : register(t13);
#	endif

#endif

#if defined(LOD_LAND_BLEND)
Texture2D<float4> TexLandLodBlend1Sampler : register(t13);
Texture2D<float4> TexLandLodBlend2Sampler : register(t15);
#elif defined(LODLANDNOISE)
Texture2D<float4> TexLandLodNoiseSampler : register(t15);
#endif

Texture2D<float4> TexShadowMaskSampler : register(t14);

#if defined(SKIN) && defined(CS_SKIN)
Texture2D<float4> TexSkinExtraSampler : register(t71);
Texture2D<float4> TexSkinWetnessSampler : register(t74);
Texture2D<float4> TexSkinWetnessNormalSampler : register(t75);
#endif

cbuffer PerTechnique : register(b0)
{
	float4 FogColor : packoffset(c0);           // Color in xyz, invFrameBufferRange in w
	float4 ColourOutputClamp : packoffset(c1);  // fLightingOutputColourClampPostLit in x, fLightingOutputColourClampPostEnv in y, fLightingOutputColourClampPostSpec in z
	float4 VPOSOffset : packoffset(c2);         // ???
};

cbuffer PerMaterial : register(b1)
{
	float4 LODTexParams : packoffset(c0);  // TerrainTexOffset in xy, LodBlendingEnabled in z
#if !(defined(LANDSCAPE) && defined(TRUE_PBR))
	float4 TintColor : packoffset(c1);
	float4 EnvmapData : packoffset(c2);  // fEnvmapScale in x, 1 or 0 in y depending of if has envmask
	float4 ParallaxOccData : packoffset(c3);
	float4 SpecularColor : packoffset(c4);  // Shininess in w, color in xyz
	float4 SparkleParams : packoffset(c5);
	float4 MultiLayerParallaxData : packoffset(c6);  // Layer thickness in x, refraction scale in y, uv scale in zw
#else
	float4 LandscapeTexture1GlintParameters : packoffset(c1);
	float4 LandscapeTexture2GlintParameters : packoffset(c2);
	float4 LandscapeTexture3GlintParameters : packoffset(c3);
	float4 LandscapeTexture4GlintParameters : packoffset(c4);
	float4 LandscapeTexture5GlintParameters : packoffset(c5);
	float4 LandscapeTexture6GlintParameters : packoffset(c6);
#endif
	float4 LightingEffectParams : packoffset(c7);  // fSubSurfaceLightRolloff in x, fRimLightPower in y
	float4 IBLParams : packoffset(c8);

#if !defined(TRUE_PBR)
	float4 LandscapeTexture1to4IsSnow : packoffset(c9);
	float4 LandscapeTexture5to6IsSnow : packoffset(c10);  // bEnableSnowMask in z, inverse iLandscapeMultiNormalTilingFactor in w
	float4 LandscapeTexture1to4IsSpecPower : packoffset(c11);
	float4 LandscapeTexture5to6IsSpecPower : packoffset(c12);
	float4 SnowRimLightParameters : packoffset(c13);  // fSnowRimLightIntensity in x, fSnowGeometrySpecPower in y, fSnowNormalSpecPower in z, bEnableSnowRimLighting in w
#endif

#if defined(TRUE_PBR) && defined(LANDSCAPE)
	float3 LandscapeTexture2PBRParams : packoffset(c9);
	float3 LandscapeTexture3PBRParams : packoffset(c10);
	float3 LandscapeTexture4PBRParams : packoffset(c11);
	float3 LandscapeTexture5PBRParams : packoffset(c12);
	float3 LandscapeTexture6PBRParams : packoffset(c13);
#endif

	float4 CharacterLightParams : packoffset(c14);
	// VR is [9] instead of [15]

	uint PBRFlags : packoffset(c15.x);
	float3 PBRParams1 : packoffset(c15.y);  // roughness scale, displacement scale, specular level
	float4 PBRParams2 : packoffset(c16);    // subsurface color, subsurface opacity

	float3 MaterialObjectRGBScale : packoffset(c17);  // RGB multipliers for material objects
};

cbuffer PerGeometry : register(b2)
{
#if !defined(VR)
	float3 DirLightDirection : packoffset(c0);
	float3 DirLightColor : packoffset(c1);
	float4 ShadowLightMaskSelect : packoffset(c2);
	float4 MaterialData : packoffset(c3);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c4);
	float3 EmitColor : packoffset(c4.y);
	float4 ProjectedUVParams : packoffset(c6);
	float4 SSRParams : packoffset(c7);
	float4 WorldMapOverlayParametersPS : packoffset(c8);
	float4 ProjectedUVParams2 : packoffset(c9);
	float4 ProjectedUVParams3 : packoffset(c10);  // fProjectedUVDiffuseNormalTilingScale in x, fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c11);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c14);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[7] : packoffset(c15);               // point light radius in w
	float4 PointLightColor[7] : packoffset(c22);
	float2 NumLightNumShadowLight : packoffset(c29);
#else
	// VR is [49] instead of [30]
	float3 DirLightDirection : packoffset(c0);
	float4 UnknownPerGeometry[12] : packoffset(c1);
	float3 DirLightColor : packoffset(c13);
	float4 ShadowLightMaskSelect : packoffset(c14);
	float4 MaterialData : packoffset(c15);  // envmapLODFade in x, specularLODFade in y, alpha in z
	float AlphaTestRef : packoffset(c16);
	float3 EmitColor : packoffset(c16.y);
	float4 ProjectedUVParams : packoffset(c18);
	float4 SSRParams : packoffset(c19);
	float4 WorldMapOverlayParametersPS : packoffset(c20);
	float4 ProjectedUVParams2 : packoffset(c21);
	float4 ProjectedUVParams3 : packoffset(c22);  // fProjectedUVDiffuseNormalTilingScale in x,	fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
	row_major float3x4 DirectionalAmbient : packoffset(c23);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c26);  // Fresnel power in z, color in xyz
	float4 PointLightPosition[14] : packoffset(c27);              // point light radius in w
	float4 PointLightColor[7] : packoffset(c41);
	float2 NumLightNumShadowLight : packoffset(c48);
#endif  // VR
};

#if !defined(VR)
cbuffer AlphaTestRefBuffer : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#endif
