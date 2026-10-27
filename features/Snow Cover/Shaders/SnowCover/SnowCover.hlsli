#ifndef SNOW_COVER_HLSLI
#define SNOW_COVER_HLSLI

#define COLOR_HSV
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#if defined(PSHADER)

namespace SnowCover
{

	static const uint MaxFireMeltSources = 8;
	static const uint WorldMaterialFlags = Permutation::ExtraFlags::InWorld | Permutation::ExtraFlags::InReflection;
	static const float WeatherDensityScale = 500.0;
	static const float WeatherFadeWobble = 1000.0;
	static const float WeatherNoiseScale = 0.001;
	static const float FoliageTransitionWidth = 5000.0;
	static const float TextureWorldScale = 100.0;
	static const float WaterFadeDepth = 64.0;
	static const float WaterFadeHeight = 8.0;
	static const float DisplacementInfluence = 5.0;
	static const float MaterialNoiseScale = 0.01;
	static const float MaterialNoisePhase = 0.025;
	static const float MaterialNoiseStrength = 0.05;
	static const float MinFoliageCoverage = 0.01;
	static const float LegacySpecularScale = 12500.0;
	static const float DisabledGlintDensity = 1.0;
	static const float GlintEnableThreshold = 1.1;
	static const float SkyVisibilityThreshold = 0.75;
	static const float GrassBaseVisibility = 0.55;
	static const float TreeBaseVisibility = 0.15;
	static const float FoliageNormalBias = 0.5;
	static const float NormalBlendStrength = 0.75;
	static const float CoolFoliageHue = 0.55;
	static const float AutumnHue = 0.1;
	static const float MinSeasonalSaturation = 0.25;
	static const float RooftopHeightFadeStart = 0.5;

	Texture2D<float4> SnowAlbedo : register(t38);
	Texture2D<float3> SnowNormal : register(t39);
	Texture2D<float4> SnowRmaos : register(t40);
	Texture2D<float4> IceAlbedo : register(t41);
	Texture2D<float3> IceNormal : register(t42);
	Texture2D<float4> IceRmaos : register(t43);
	Texture2D<float> SnowMap : register(t44);

	bool IsActive()
	{
		return SharedData::snowCoverSettings.EnableSnowCover && (Permutation::ExtraShaderDescriptor & WorldMaterialFlags) != 0;
	}

	float GetWeatherRange(float3 p, float viewDist)
	{
		float fadeStart = SharedData::snowCoverSettings.WeatherFadeStart;
		float fadeEnd = max(fadeStart + 1, SharedData::snowCoverSettings.WeatherFadeEnd);
		float wobble = WeatherFadeWobble * sin(p.z * WeatherNoiseScale + cos(p.x * p.y * WeatherNoiseScale));
		return 1 - smoothstep(fadeStart, max(fadeStart + 1, fadeEnd + wobble), viewDist);
	}
	float GetObjectFade(float viewDist)
	{
		float amount = SharedData::snowCoverSettings.ObjectFadeAmount;
		if (amount <= 0)
			return 1;
		float fadeStart = SharedData::snowCoverSettings.ObjectFadeStart;
		float fadeEnd = max(fadeStart + 1, SharedData::snowCoverSettings.ObjectFadeEnd);
		return 1 - smoothstep(fadeStart, fadeEnd, viewDist) * amount;
	}

	float GetFireMelt(float3 p)
	{
		float melt = 0;
		uint fireCount = min(SharedData::snowCoverSettings.FireMeltCount, MaxFireMeltSources);
		[loop] for (uint i = 0; i < fireCount; i++)
		{
			float4 fire = SharedData::snowCoverSettings.FireMeltSpheres[i];
			float radius = fire.w * SharedData::snowCoverSettings.FireMeltRadiusScale;
			float3 delta = p - fire.xyz;
			melt = max(melt, 1 - smoothstep(radius * radius * 0.25, radius * radius, dot(delta, delta)));
		}
		return melt * SharedData::snowCoverSettings.FireMeltStrength;
	}

	float GetHeightMult(float3 p)
	{
		float2 scale = SharedData::snowCoverSettings.mapScale;
		float2 offset = SharedData::snowCoverSettings.mapOffset;
		float2 uv = p.xy * scale + offset;
		float height_tresh = p.z - SharedData::snowCoverSettings.SnowHeightOffset + (SnowMap.SampleLevel(SampColorSampler, uv, 0) - 0.5) * SharedData::snowCoverSettings.mapZscale;
		return height_tresh;
	}

	float GetEnvironmentalMultiplier(float3 p)
	{
		return (GetHeightMult(p) + SharedData::snowCoverSettings.SeasonalAltitude) / SharedData::snowCoverSettings.BlendSmoothness;
	}

	float GetRooftopLift(float modelHeight, float skyVisibility)
	{
		float height = SharedData::snowCoverSettings.RooftopHeight;
		float exposure = smoothstep(SharedData::snowCoverSettings.RooftopExposure, 1.0, skyVisibility);
		return SharedData::snowCoverSettings.RooftopSnow * exposure * smoothstep(RooftopHeightFadeStart * height, height, modelHeight);
	}

	void ApplyFoliageColor(inout float3 color, float env_mult)
	{
		float gmult = saturate(env_mult - SharedData::snowCoverSettings.FoliageHeightOffset / FoliageTransitionWidth);
		if (gmult <= 0)
			return;
		float3 hsv = Color::RGBtoHSV(color);
		if (hsv.x > CoolFoliageHue)
			hsv.x = frac(lerp(hsv.x, 1 + AutumnHue, gmult));
		else
			hsv.x = lerp(hsv.x, AutumnHue, gmult);
		hsv.y *= lerp(1, MinSeasonalSaturation, 4.0 * gmult * (1.0 - gmult));
		color = Color::HSVtoRGB(hsv);
	}

	float GetFoliageSnowBase(float3 worldNormal, float3 p, float viewDist, float distMult, out float env_mult)
	{
		env_mult = GetEnvironmentalMultiplier(p);
		float weatherMult = GetWeatherRange(p, viewDist) * SharedData::snowCoverSettings.TimeSnowing * max(WeatherDensityScale, SharedData::snowCoverSettings.SnowingDensity) / WeatherDensityScale;
		float mult = SharedData::snowCoverSettings.MainTint.a * saturate(env_mult);
		return distMult * saturate(mult + weatherMult) * smoothstep(SharedData::snowCoverSettings.minAngle, SharedData::snowCoverSettings.maxAngle, worldNormal.z);
	}

	void ApplySnowFoliageBase(inout float3 color, float3 worldNormal, float3 p, float env_mult, float mult)
	{
		[branch] if (mult > 0)
			mult *= 1 - GetFireMelt(p);
#	if defined(GRASS)
		if (SharedData::snowCoverSettings.AffectGrassTint) {
#	else
		if (SharedData::snowCoverSettings.AffectTreeTint) {
#	endif
			ApplyFoliageColor(color, env_mult);
		}
		if (mult < MinFoliageCoverage)
			return;
		float2 uv = frac(SharedData::snowCoverSettings.UVScale * (p.xy + worldNormal.xy) / TextureWorldScale);
		float3 snowSample = SnowAlbedo.Sample(SampColorSampler, uv).rgb;
#	if defined(TRUE_PBR)
		float3 diffuse = Color::Diffuse(snowSample) * SharedData::snowCoverSettings.MainTint.rgb * Color::PBRLightingScale;
#	else
		float3 diffuse = Color::Diffuse(Color::LinearToSrgb(snowSample)) * SharedData::snowCoverSettings.MainTint.rgb * Color::PBRLightingScale;
#	endif

		color = lerp(color, diffuse, mult);
	}

	void ApplySnowFoliage(inout float3 color, float3 worldNormal, float3 p, float skylight, float viewDist, float distMult)
	{
		float environment;
		float coverage = GetFoliageSnowBase(worldNormal, p, viewDist, distMult, environment);
		ApplySnowFoliageBase(color, worldNormal, p, environment, coverage * skylight);
	}

#	if defined(BASIC_SNOW_COVER)
	void ApplyDistantTreeSnow(inout float3 color, float3 normal, float3 relativeWorldPos, float2 uv, uint eyeIndex, Texture2D<float4> diffuseTexture)
	{
		if (IsActive()) {
			float skylight = TreeBaseVisibility;
			if (SharedData::snowCoverSettings.EnableExpensiveFoliage) {
				float rx;
				float ry;
				diffuseTexture.GetDimensions(rx, ry);
				skylight = 1 - diffuseTexture.Sample(SampColorSampler, uv - float2(0, 2. / ry)).a;
			}
			skylight *= saturate(relativeWorldPos.z - SharedData::GetWaterData(relativeWorldPos, eyeIndex).w);
			// A LOD billboard is a vertical quad, so the raw geometric normal has z == 0 and the angle mask rejects it outright.
			float3 snowNormal = normalize(normal + float3(0, 0, FoliageNormalBias));
			float snowViewDist = length(mul(FrameBuffer::CameraView[eyeIndex], float4(relativeWorldPos, 1)).xyz);
			ApplySnowFoliage(color, snowNormal, relativeWorldPos + FrameBuffer::CameraPosAdjust[eyeIndex].xyz, skylight, snowViewDist, GetObjectFade(snowViewDist));
		}
	}

	void ApplyGrassSnow(inout float3 color, float3 normal, float3 relativeWorldPos, float2 uv, float3 viewPosition, float skyVisibility, bool complex, uint eyeIndex, Texture2D<float4> diffuseTexture)
	{
		float3 snowPosition = relativeWorldPos + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		float3 snowNormal = float3(uv, normal.z * (1 - FoliageNormalBias) + FoliageNormalBias);
		float snowEnvironment;
		float snowCoverage = GetFoliageSnowBase(snowNormal, snowPosition, length(viewPosition), 1.0, snowEnvironment);
		float snowOcclusion = smoothstep(0, SkyVisibilityThreshold, skyVisibility) * GrassBaseVisibility;
		[branch] if (snowCoverage > 0 && SharedData::snowCoverSettings.EnableExpensiveFoliage)
		{
			float rx;
			float ry;
			diffuseTexture.GetDimensions(rx, ry);
#		if !defined(TRUE_PBR)
			if (complex) {
				snowOcclusion = max(snowOcclusion, 1 - diffuseTexture.SampleBias(SampColorSampler, float2(uv.x, uv.y * 0.5 - 0.5 / ry), SharedData::MipBias).a);
			} else
#		endif  // !TRUE_PBR
			{
				snowOcclusion = max(snowOcclusion, 1 - diffuseTexture.SampleBias(SampColorSampler, uv - float2(0, 1. / ry), SharedData::MipBias).a);
			}
		}
		snowOcclusion *= saturate(relativeWorldPos.z - SharedData::GetWaterData(relativeWorldPos, eyeIndex).w);
		ApplySnowFoliageBase(color, snowNormal, snowPosition, snowEnvironment, snowCoverage * snowOcclusion);
	}
#	endif

#	if !defined(BASIC_SNOW_COVER)
	float ApplySnowBase(float3 worldNormal, inout float2 uv, out bool alt, float disp, float3 p, float skylight, float waterDist, float viewDist, float lift)
	{
		waterDist = smoothstep(-WaterFadeDepth, WaterFadeHeight, -waterDist - disp);
		float weatherMult = GetWeatherRange(p, viewDist) * (SharedData::snowCoverSettings.TimeSnowing * SharedData::snowCoverSettings.TimeSnowing * SharedData::snowCoverSettings.TimeSnowing) * max(WeatherDensityScale, SharedData::snowCoverSettings.SnowingDensity) / WeatherDensityScale;
		weatherMult = clamp((weatherMult)*max(SharedData::snowCoverSettings.minAngle, worldNormal.z), -1, 1);
		float env_mult = saturate(max((GetEnvironmentalMultiplier(p) + lift / SharedData::snowCoverSettings.BlendSmoothness + disp * DisplacementInfluence), weatherMult)) - waterDist;
#		if !defined(LANDSCAPE) && !defined(LOD) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE)
		float distMult = GetObjectFade(viewDist);
#		else
		float distMult = 1;
#		endif
		float mult = distMult * skylight * env_mult * smoothstep(SharedData::snowCoverSettings.minAngle, SharedData::snowCoverSettings.maxAngle, worldNormal.z);
		[branch] if (mult > 0)
			mult *= 1 - GetFireMelt(p);
		if (mult <= 0) {
			alt = false;
			return 0;
		}
		float main_mult = (1 - abs(worldNormal.z - SharedData::snowCoverSettings.peakMainAngle)) + min(0, weatherMult) * SharedData::snowCoverSettings.minAngle;
		float alt_mult = (1 - abs(worldNormal.z - SharedData::snowCoverSettings.peakAltAngle)) + sin(p.z * MaterialNoiseScale + cos(p.x * p.y * MaterialNoiseScale) * MaterialNoisePhase) * MaterialNoiseStrength;
		alt = alt_mult > main_mult;
		uv = frac(SharedData::snowCoverSettings.UVScale * (p.xy / TextureWorldScale + worldNormal.xy * disp));
		return min(1, mult);
	}

#		if defined(TRUE_PBR)
	float ApplySnowPBR(inout MaterialProperties material, inout float3 worldNormal, float disp, float3 p, float skylight, float waterDist, float viewDist, float2 uv, float lift)
	{
		bool alt;
		float mult = ApplySnowBase(worldNormal, uv, alt, disp, p, skylight, waterDist, viewDist, lift);
		if (mult <= 0)
			return mult;
		float4 rmaos;
		if (alt) {
			rmaos = IceRmaos.Sample(SampColorSampler, uv);
			float3 albedo = IceAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::Diffuse(albedo) * SharedData::snowCoverSettings.AltTint.rgb;
			material.BaseColor = lerp(material.BaseColor, albedo, mult * SharedData::snowCoverSettings.AltTint.w);
			worldNormal = TransformNormal(IceNormal.Sample(SampNormalSampler, uv).rgb);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.altSpec, mult);

		} else {
			rmaos = SnowRmaos.Sample(SampColorSampler, uv);
			float3 albedo = SnowAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::Diffuse(albedo) * SharedData::snowCoverSettings.MainTint.rgb;
			material.BaseColor = lerp(material.BaseColor, albedo, mult * SharedData::snowCoverSettings.MainTint.w);
			worldNormal = TransformNormal(SnowNormal.Sample(SampNormalSampler, uv).rgb);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.mainSpec, mult);
		}
		material.Roughness = lerp(material.Roughness, rmaos.x, mult);
		material.Metallic = lerp(material.Metallic, rmaos.y, mult);
		material.AO = lerp(material.AO, rmaos.z, mult * 0.5);
		if (material.GlintLogMicrofacetDensity > GlintEnableThreshold) {
			material.GlintScreenSpaceScale = lerp(material.GlintScreenSpaceScale, SharedData::snowCoverSettings.Glint.x, mult);
			material.GlintLogMicrofacetDensity = lerp(material.GlintLogMicrofacetDensity, SharedData::snowCoverSettings.Glint.y, mult);
			material.GlintMicrofacetRoughness = lerp(material.GlintMicrofacetRoughness, SharedData::snowCoverSettings.Glint.z, mult);
			material.GlintDensityRandomization = lerp(material.GlintDensityRandomization, SharedData::snowCoverSettings.Glint.w, mult);
		} else {
			material.GlintScreenSpaceScale = SharedData::snowCoverSettings.Glint.x;
			material.GlintLogMicrofacetDensity = lerp(PBR::Constants::MaxGlintDensity, SharedData::snowCoverSettings.Glint.y, mult);
			material.GlintMicrofacetRoughness = SharedData::snowCoverSettings.Glint.z;
			material.GlintDensityRandomization = SharedData::snowCoverSettings.Glint.w;
		}
		return mult;
	}
#		else

	float ApplySnow(inout MaterialProperties material, inout float3 worldNormal, float disp, float3 p, float skylight, float waterDist, float viewDist, float2 uv, float lift)
	{
		bool alt;
		float mult = ApplySnowBase(worldNormal, uv, alt, disp, p, skylight, waterDist, viewDist, lift);
		if (mult <= 0.0)
			return 0;
		float4 rmaos;
		if (alt) {
			float3 albedo = IceAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::Diffuse(Color::LinearToSrgb(albedo)) * SharedData::snowCoverSettings.AltTint.rgb * Color::PBRLightingScale;
			rmaos = IceRmaos.Sample(SampColorSampler, uv);
			material.Roughness = lerp(material.Roughness, rmaos.x, mult);
			material.Shininess = lerp(material.Shininess, LegacySpecularScale * SharedData::snowCoverSettings.altSpec * rmaos.w, mult);
			worldNormal = TransformNormal(IceNormal.Sample(SampNormalSampler, uv).rgb);
			material.BaseColor = lerp(material.BaseColor, rmaos.z * albedo, mult * SharedData::snowCoverSettings.AltTint.w);
			material.SpecularColor = lerp(material.SpecularColor, rmaos.y * albedo + float3(1, 1, 1) * (1 - rmaos.y), mult);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.altSpec, mult);
		} else {
			float3 albedo = SnowAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::Diffuse(Color::LinearToSrgb(albedo)) * SharedData::snowCoverSettings.MainTint.rgb * Color::PBRLightingScale;
			rmaos = SnowRmaos.Sample(SampColorSampler, uv);
			material.Roughness = lerp(material.Roughness, rmaos.x, mult);
			material.Shininess = lerp(material.Shininess, LegacySpecularScale * SharedData::snowCoverSettings.mainSpec * rmaos.w, mult);
			worldNormal = TransformNormal(SnowNormal.Sample(SampNormalSampler, uv).rgb);
			material.BaseColor = lerp(material.BaseColor, rmaos.z * albedo, mult * SharedData::snowCoverSettings.MainTint.w);
			material.SpecularColor = lerp(material.SpecularColor, rmaos.y * albedo + float3(1, 1, 1) * (1 - rmaos.y), mult);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.mainSpec, mult);
		}
		return mult;
	}
#		endif
#	endif

}
#endif

#endif
