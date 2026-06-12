#if defined(LANDSCAPE)
// Layer 1 (LandBlendWeights1.x)
if (input.LandBlendWeights1.x > 0.01) {
	float weight = input.LandBlendWeights1.x;

	// Sample diffuse texture for layer 1
#	if defined(TERRAIN_VARIATION)
	float4 landColor1;
	[branch] if (useTerrainVariation)
	{
		landColor1 = StochasticEffect(TexColorSampler, SampColorSampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landColor1 = TexColorSampler.SampleBias(SampColorSampler, uv, SharedData::MipBias);
	}
#	else
	float4 landColor1 = TexColorSampler.SampleBias(SampColorSampler, uv, SharedData::MipBias);
#	endif
	float3 landColorRGB1 = landColor1.rgb;
#	if defined(TRUE_PBR)
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile0PBR) == 0)
	{
		landColorRGB1 = Color::SrgbToLinear(landColorRGB1 / Color::PBRLightingScale);
	}
#	endif
	float landAlpha1 = landColor1.a;
	float landSnowMask1 = GetLandSnowMaskValue(landColor1.w);

	// Sample normal texture for layer 1
#	if defined(TERRAIN_VARIATION)
	float4 landNormal1;
	[branch] if (useTerrainVariation)
	{
		landNormal1 = StochasticEffect(TexNormalSampler, SampNormalSampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landNormal1 = TexNormalSampler.SampleBias(SampNormalSampler, uv, SharedData::MipBias);
	}
#	else
	float4 landNormal1 = TexNormalSampler.SampleBias(SampNormalSampler, uv, SharedData::MipBias);
#	endif
	float3 landNormalRGB1 = landNormal1.rgb;
	float landNormalAlpha1 = landNormal1.a;
#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask += LandscapeTexture1to4IsSnow.x * input.LandBlendWeights1.x * landSnowMask1;
#	endif  // SNOW

	// Sample RMAOS texture for layer 1
#	if defined(TRUE_PBR)
	float4 landRMAOS1;
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile0PBR) != 0)
	{
#		if defined(TERRAIN_VARIATION)
		[branch] if (useTerrainVariation)
		{
			landRMAOS1 = StochasticEffect(TexRMAOSSampler, SampRMAOSSampler, uv, sharedOffset, dx, dy) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
		}
		else
		{
			landRMAOS1 = TexRMAOSSampler.SampleBias(SampRMAOSSampler, uv, SharedData::MipBias) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
		}
#		else
		landRMAOS1 = TexRMAOSSampler.SampleBias(SampRMAOSSampler, uv, SharedData::MipBias) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
#		endif
		if ((PBRFlags & PBR::TerrainFlags::LandTile0HasGlint) != 0) {
			glintParameters += weight * LandscapeTexture1GlintParameters;
		}
	}
	else
	{
		landRMAOS1 = input.LandBlendWeights1.x * float4(1 - glossiness.x, 0, 1, 0);
	}
	blendedRMAOS += landRMAOS1 * weight;
#	endif
	blendedRGB += landColorRGB1 * weight;
	blendedAlpha += landAlpha1 * weight;
	blendedNormalRGB += landNormalRGB1 * weight;
	blendedNormalAlpha += landNormalAlpha1 * weight;
}

// Layer 2 (LandBlendWeights1.y)
if (input.LandBlendWeights1.y > 0.01) {
	float weight = input.LandBlendWeights1.y;

	// Sample diffuse texture for layer 2
#	if defined(TERRAIN_VARIATION)
	float4 landColor2;
	[branch] if (useTerrainVariation)
	{
		landColor2 = StochasticEffect(TexLandColor2Sampler, SampLandColor2Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landColor2 = TexLandColor2Sampler.SampleBias(SampLandColor2Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landColor2 = TexLandColor2Sampler.SampleBias(SampLandColor2Sampler, uv, SharedData::MipBias);
#	endif
	float3 landColorRGB2 = landColor2.rgb;
#	if defined(TRUE_PBR)
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile1PBR) == 0)
	{
		landColorRGB2 = Color::SrgbToLinear(landColorRGB2 / Color::PBRLightingScale);
	}
#	endif
	float landAlpha2 = landColor2.a;
	float landSnowMask2 = GetLandSnowMaskValue(landColor2.w);

	// Sample normal texture for layer 2
#	if defined(TERRAIN_VARIATION)
	float4 landNormal2;
	[branch] if (useTerrainVariation)
	{
		landNormal2 = StochasticEffect(TexLandNormal2Sampler, SampLandNormal2Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landNormal2 = TexLandNormal2Sampler.SampleBias(SampLandNormal2Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landNormal2 = TexLandNormal2Sampler.SampleBias(SampLandNormal2Sampler, uv, SharedData::MipBias);
#	endif
	float3 landNormalRGB2 = landNormal2.rgb;
	float landNormalAlpha2 = landNormal2.a;
#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask += LandscapeTexture1to4IsSnow.y * input.LandBlendWeights1.y * landSnowMask2;
#	endif  // SNOW

	// Sample RMAOS texture for layer 2
#	if defined(TRUE_PBR)
	float4 landRMAOS2;
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile1PBR) != 0)
	{
#		if defined(TERRAIN_VARIATION)
		[branch] if (useTerrainVariation)
		{
			landRMAOS2 = StochasticEffect(TexLandRMAOS2Sampler, SampLandRMAOS2Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture2PBRParams.x, 1, 1, LandscapeTexture2PBRParams.z);
		}
		else
		{
			landRMAOS2 = TexLandRMAOS2Sampler.SampleBias(SampLandRMAOS2Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture2PBRParams.x, 1, 1, LandscapeTexture2PBRParams.z);
		}
#		else
		landRMAOS2 = TexLandRMAOS2Sampler.SampleBias(SampLandRMAOS2Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture2PBRParams.x, 1, 1, LandscapeTexture2PBRParams.z);
#		endif
		if ((PBRFlags & PBR::TerrainFlags::LandTile1HasGlint) != 0) {
			glintParameters += weight * LandscapeTexture2GlintParameters;
		}
	}
	else
	{
		landRMAOS2 = input.LandBlendWeights1.y * float4(1 - glossiness.x, 0, 1, 0);
	}
	blendedRMAOS += landRMAOS2 * weight;
#	endif
	blendedRGB += landColorRGB2 * weight;
	blendedAlpha += landAlpha2 * weight;
	blendedNormalRGB += landNormalRGB2 * weight;
	blendedNormalAlpha += landNormalAlpha2 * weight;
}

// Layer 3 (LandBlendWeights1.z)
if (input.LandBlendWeights1.z > 0.01) {
	float weight = input.LandBlendWeights1.z;
	// Sample diffuse texture for layer 3
#	if defined(TERRAIN_VARIATION)
	float4 landColor3;
	[branch] if (useTerrainVariation)
	{
		landColor3 = StochasticEffect(TexLandColor3Sampler, SampLandColor3Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landColor3 = TexLandColor3Sampler.SampleBias(SampLandColor3Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landColor3 = TexLandColor3Sampler.SampleBias(SampLandColor3Sampler, uv, SharedData::MipBias);
#	endif
	float3 landColorRGB3 = landColor3.rgb;
#	if defined(TRUE_PBR)
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile2PBR) == 0)
	{
		landColorRGB3 = Color::SrgbToLinear(landColorRGB3 / Color::PBRLightingScale);
	}
#	endif
	float landAlpha3 = landColor3.a;
	float landSnowMask3 = GetLandSnowMaskValue(landColor3.w);

	// Sample normal texture for layer 3
#	if defined(TERRAIN_VARIATION)
	float4 landNormal3;
	[branch] if (useTerrainVariation)
	{
		landNormal3 = StochasticEffect(TexLandNormal3Sampler, SampLandNormal3Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landNormal3 = TexLandNormal3Sampler.SampleBias(SampLandNormal3Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landNormal3 = TexLandNormal3Sampler.SampleBias(SampLandNormal3Sampler, uv, SharedData::MipBias);
#	endif
	float3 landNormalRGB3 = landNormal3.rgb;
	float landNormalAlpha3 = landNormal3.a;
#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask += LandscapeTexture1to4IsSnow.z * input.LandBlendWeights1.z * landSnowMask3;
#	endif  // SNOW

	// Sample RMAOS texture for layer 3
#	if defined(TRUE_PBR)
	float4 landRMAOS3;
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile2PBR) != 0)
	{
#		if defined(TERRAIN_VARIATION)
		[branch] if (useTerrainVariation)
		{
			landRMAOS3 = StochasticEffect(TexLandRMAOS3Sampler, SampLandRMAOS3Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture3PBRParams.x, 1, 1, LandscapeTexture3PBRParams.z);
		}
		else
		{
			landRMAOS3 = TexLandRMAOS3Sampler.SampleBias(SampLandRMAOS3Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture3PBRParams.x, 1, 1, LandscapeTexture3PBRParams.z);
		}
#		else
		landRMAOS3 = TexLandRMAOS3Sampler.SampleBias(SampLandRMAOS3Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture3PBRParams.x, 1, 1, LandscapeTexture3PBRParams.z);
#		endif
		if ((PBRFlags & PBR::TerrainFlags::LandTile2HasGlint) != 0) {
			glintParameters += weight * LandscapeTexture3GlintParameters;
		}
	}
	else
	{
		landRMAOS3 = input.LandBlendWeights1.z * float4(1 - glossiness.x, 0, 1, 0);
	}
	blendedRMAOS += landRMAOS3 * weight;
#	endif
	blendedRGB += landColorRGB3 * weight;
	blendedAlpha += landAlpha3 * weight;
	blendedNormalRGB += landNormalRGB3 * weight;
	blendedNormalAlpha += landNormalAlpha3 * weight;
}
// Layer 4 (LandBlendWeights1.w)
if (input.LandBlendWeights1.w > 0.01) {
	float weight = input.LandBlendWeights1.w;

	// Sample diffuse texture for layer 4
#	if defined(TERRAIN_VARIATION)
	float4 landColor4;
	[branch] if (useTerrainVariation)
	{
		landColor4 = StochasticEffect(TexLandColor4Sampler, SampLandColor4Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landColor4 = TexLandColor4Sampler.SampleBias(SampLandColor4Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landColor4 = TexLandColor4Sampler.SampleBias(SampLandColor4Sampler, uv, SharedData::MipBias);
#	endif
	float3 landColorRGB4 = landColor4.rgb;
#	if defined(TRUE_PBR)
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile3PBR) == 0)
	{
		landColorRGB4 = Color::SrgbToLinear(landColorRGB4 / Color::PBRLightingScale);
	}
#	endif
	float landAlpha4 = landColor4.a;
	float landSnowMask4 = GetLandSnowMaskValue(landColor4.w);

	// Sample normal texture for layer 4
#	if defined(TERRAIN_VARIATION)
	float4 landNormal4;
	[branch] if (useTerrainVariation)
	{
		landNormal4 = StochasticEffect(TexLandNormal4Sampler, SampLandNormal4Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landNormal4 = TexLandNormal4Sampler.SampleBias(SampLandNormal4Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landNormal4 = TexLandNormal4Sampler.SampleBias(SampLandNormal4Sampler, uv, SharedData::MipBias);
#	endif
	float3 landNormalRGB4 = landNormal4.rgb;
	float landNormalAlpha4 = landNormal4.a;
#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask += LandscapeTexture1to4IsSnow.w * input.LandBlendWeights1.w * landSnowMask4;
#	endif  // SNOW

	// Sample RMAOS texture for layer 4
#	if defined(TRUE_PBR)
	float4 landRMAOS4;
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile3PBR) != 0)
	{
#		if defined(TERRAIN_VARIATION)
		[branch] if (useTerrainVariation)
		{
			landRMAOS4 = StochasticEffect(TexLandRMAOS4Sampler, SampLandRMAOS4Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture4PBRParams.x, 1, 1, LandscapeTexture4PBRParams.z);
		}
		else
		{
			landRMAOS4 = TexLandRMAOS4Sampler.SampleBias(SampLandRMAOS4Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture4PBRParams.x, 1, 1, LandscapeTexture4PBRParams.z);
		}
#		else
		landRMAOS4 = TexLandRMAOS4Sampler.SampleBias(SampLandRMAOS4Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture4PBRParams.x, 1, 1, LandscapeTexture4PBRParams.z);
#		endif
		if ((PBRFlags & PBR::TerrainFlags::LandTile3HasGlint) != 0) {
			glintParameters += weight * LandscapeTexture4GlintParameters;
		}
	}
	else
	{
		landRMAOS4 = input.LandBlendWeights1.w * float4(1 - glossiness.x, 0, 1, 0);
	}
	blendedRMAOS += landRMAOS4 * weight;
#	endif
	blendedRGB += landColorRGB4 * weight;
	blendedAlpha += landAlpha4 * weight;
	blendedNormalRGB += landNormalRGB4 * weight;
	blendedNormalAlpha += landNormalAlpha4 * weight;
}

// Layer 5 (LandBlendWeights2.x)
if (input.LandBlendWeights2.x > 0.01) {
	float weight = input.LandBlendWeights2.x;
	// Sample diffuse texture for layer 5
#	if defined(TERRAIN_VARIATION)
	float4 landColor5;
	[branch] if (useTerrainVariation)
	{
		landColor5 = StochasticEffect(TexLandColor5Sampler, SampLandColor5Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landColor5 = TexLandColor5Sampler.SampleBias(SampLandColor5Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landColor5 = TexLandColor5Sampler.SampleBias(SampLandColor5Sampler, uv, SharedData::MipBias);
#	endif
	float3 landColorRGB5 = landColor5.rgb;
#	if defined(TRUE_PBR)
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile4PBR) == 0)
	{
		landColorRGB5 = Color::SrgbToLinear(landColorRGB5 / Color::PBRLightingScale);
	}
#	endif
	float landAlpha5 = landColor5.a;
	float landSnowMask5 = GetLandSnowMaskValue(landColor5.w);

	// Sample normal texture for layer 5
#	if defined(TERRAIN_VARIATION)
	float4 landNormal5;
	[branch] if (useTerrainVariation)
	{
		landNormal5 = StochasticEffect(TexLandNormal5Sampler, SampLandNormal5Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landNormal5 = TexLandNormal5Sampler.SampleBias(SampLandNormal5Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landNormal5 = TexLandNormal5Sampler.SampleBias(SampLandNormal5Sampler, uv, SharedData::MipBias);
#	endif
	float3 landNormalRGB5 = landNormal5.rgb;
	float landNormalAlpha5 = landNormal5.a;

#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask += LandscapeTexture5to6IsSnow.x * input.LandBlendWeights2.x * landSnowMask5;
#	endif  // SNOW

	// Sample RMAOS texture for layer 5
#	if defined(TRUE_PBR)
	float4 landRMAOS5;
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile4PBR) != 0)
	{
#		if defined(TERRAIN_VARIATION)
		[branch] if (useTerrainVariation)
		{
			landRMAOS5 = StochasticEffect(TexLandRMAOS5Sampler, SampLandRMAOS5Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture5PBRParams.x, 1, 1, LandscapeTexture5PBRParams.z);
		}
		else
		{
			landRMAOS5 = TexLandRMAOS5Sampler.SampleBias(SampLandRMAOS5Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture5PBRParams.x, 1, 1, LandscapeTexture5PBRParams.z);
		}
#		else
		landRMAOS5 = TexLandRMAOS5Sampler.SampleBias(SampLandRMAOS5Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture5PBRParams.x, 1, 1, LandscapeTexture5PBRParams.z);
#		endif
		if ((PBRFlags & PBR::TerrainFlags::LandTile4HasGlint) != 0) {
			glintParameters += weight * LandscapeTexture5GlintParameters;
		}
	}
	else
	{
		landRMAOS5 = input.LandBlendWeights2.x * float4(1 - glossiness.x, 0, 1, 0);
	}
	blendedRMAOS += landRMAOS5 * weight;
#	endif
	blendedRGB += landColorRGB5 * weight;
	blendedAlpha += landAlpha5 * weight;
	blendedNormalRGB += landNormalRGB5 * weight;
	blendedNormalAlpha += landNormalAlpha5 * weight;
}
// Layer 6 (LandBlendWeights2.y)
if (input.LandBlendWeights2.y > 0.01) {
	float weight = input.LandBlendWeights2.y;

	// Sample layer 6 textures
#	if defined(TERRAIN_VARIATION)
	float4 landColor6;
	[branch] if (useTerrainVariation)
	{
		landColor6 = StochasticEffect(TexLandColor6Sampler, SampLandColor6Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landColor6 = TexLandColor6Sampler.SampleBias(SampLandColor6Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landColor6 = TexLandColor6Sampler.SampleBias(SampLandColor6Sampler, uv, SharedData::MipBias);
#	endif
	float3 landColorRGB6 = landColor6.rgb;
#	if defined(TRUE_PBR)
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile5PBR) == 0)
	{
		landColorRGB6 = Color::SrgbToLinear(landColorRGB6 / Color::PBRLightingScale);
	}
#	endif
	float landAlpha6 = landColor6.a;
	float landSnowMask6 = GetLandSnowMaskValue(landColor6.w);

	// Sample normal texture for layer 6
#	if defined(TERRAIN_VARIATION)
	float4 landNormal6;
	[branch] if (useTerrainVariation)
	{
		landNormal6 = StochasticEffect(TexLandNormal6Sampler, SampLandNormal6Sampler, uv, sharedOffset, dx, dy);
	}
	else
	{
		landNormal6 = TexLandNormal6Sampler.SampleBias(SampLandNormal6Sampler, uv, SharedData::MipBias);
	}
#	else
	float4 landNormal6 = TexLandNormal6Sampler.SampleBias(SampLandNormal6Sampler, uv, SharedData::MipBias);
#	endif
	float3 landNormalRGB6 = landNormal6.rgb;
	float landNormalAlpha6 = landNormal6.a;
#	if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask += LandscapeTexture5to6IsSnow.y * input.LandBlendWeights2.y * landSnowMask6;
#	endif  // SNOW

	// Sample RMAOS texture for layer 6
#	if defined(TRUE_PBR)
	float4 landRMAOS6;
	[branch] if ((PBRFlags & PBR::TerrainFlags::LandTile5PBR) != 0)
	{
#		if defined(TERRAIN_VARIATION)
		[branch] if (useTerrainVariation)
		{
			landRMAOS6 = StochasticEffect(TexLandRMAOS6Sampler, SampLandRMAOS6Sampler, uv, sharedOffset, dx, dy) * float4(LandscapeTexture6PBRParams.x, 1, 1, LandscapeTexture6PBRParams.z);
		}
		else
		{
			landRMAOS6 = TexLandRMAOS6Sampler.SampleBias(SampLandRMAOS6Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture6PBRParams.x, 1, 1, LandscapeTexture6PBRParams.z);
		}
#		else
		landRMAOS6 = TexLandRMAOS6Sampler.SampleBias(SampLandRMAOS6Sampler, uv, SharedData::MipBias) * float4(LandscapeTexture6PBRParams.x, 1, 1, LandscapeTexture6PBRParams.z);
#		endif
		if ((PBRFlags & PBR::TerrainFlags::LandTile5HasGlint) != 0) {
			glintParameters += weight * LandscapeTexture6GlintParameters;
		}
	}
	else
	{
		landRMAOS6 = input.LandBlendWeights2.y * float4(1 - glossiness.x, 0, 1, 0);
	}
	blendedRMAOS += landRMAOS6 * weight;
#	endif
	blendedRGB += landColorRGB6 * weight;
	blendedAlpha += landAlpha6 * weight;
	blendedNormalRGB += landNormalRGB6 * weight;
	blendedNormalAlpha += landNormalAlpha6 * weight;
}

float4 rawBaseColor = float4(blendedRGB, blendedAlpha);
baseColor = float4(Color::Diffuse(blendedRGB), blendedAlpha);
normal = float4(blendedNormalRGB, blendedNormalAlpha);
#	if defined(TRUE_PBR)
rawRMAOS = blendedRMAOS;
#	endif
#else  // Non-landscape code
float4 rawBaseColor = TexColorSampler.SampleBias(SampColorSampler, diffuseUv, SharedData::MipBias);
baseColor = float4(Color::Diffuse(rawBaseColor.rgb), rawBaseColor.a);
float4 normalColor = TexNormalSampler.SampleBias(SampNormalSampler, uv, SharedData::MipBias);
normal = normalColor;
#	if defined(TRUE_PBR)
rawRMAOS = TexRMAOSSampler.SampleBias(SampRMAOSSampler, diffuseUv, SharedData::MipBias) * float4(PBRParams1.x, 1, 1, PBRParams1.z);
if ((PBRFlags & PBR::Flags::Glint) != 0) {
	glintParameters = MultiLayerParallaxData;
}
#	endif
#endif
