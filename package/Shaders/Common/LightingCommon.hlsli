#ifndef LIGHTING_COMMON_HLSLI
#define LIGHTING_COMMON_HLSLI

struct DirectContext
{
	float3 worldNormal;
	float3 vertexNormal;
	float3 viewDir;
	float3 lightDir;
	float3 halfVector;
	float3 lightColor;
	float detailedShadow;
	float softShadow;
#if defined(TRUE_PBR)
	float3 coatWorldNormal;
	float3 coatViewDir;
	float3 coatLightDir;
	float3 coatHalfVector;
	float3 coatLightColor;
#elif defined(HAIR) && defined(CS_HAIR)
	float hairShadow;
#endif
};

struct IndirectContext
{
	float3 worldNormal;
	float3 vertexNormal;
	float3 viewDir;
};

struct DirectLightingOutput
{
	float3 diffuse;
	float3 specular;
	float3 transmission;
#if defined(TRUE_PBR)
	float3 coatDiffuse;
#endif
};

struct IndirectLobeWeights
{
	float3 diffuse;
	float3 specular;
};

#if defined(TRUE_PBR)
#	if defined(GLINT)
#		include "Common/Glints/Glints2023.hlsli"
#	else
namespace Glints
{
	typedef float GlintCachedVars;
}
#	endif
#endif

struct MaterialProperties
{
	float3 BaseColor;
#if !defined(TRUE_PBR)
	float Shininess;
	float Glossiness;
	float3 SpecularColor;
#	if (defined(RIM_LIGHTING) || defined(SOFT_LIGHTING))
	float3 rimSoftLightColor;
#	endif
#	if defined(BACK_LIGHTING)
	float3 backLightColor;
#	endif
	float Roughness;
	float3 F0;
#	if defined(CS_SKIN) && defined(SKIN)
	float RoughnessSecondary;
	float SecondarySpecIntensity;
	float Curvature;
	float Thickness;
	float3 SubsurfaceColor;
	float AO;
	float FuzzRoughness;
	float3 FuzzColor;
	float FuzzWeight;
#	endif
#else
	float Roughness;
	float Metallic;
	float AO;
	float3 F0;
	float3 SubsurfaceColor;
	float Thickness;
	float3 CoatColor;
	float CoatStrength;
	float CoatRoughness;
	float3 CoatF0;
	float3 FuzzColor;
	float FuzzWeight;
	float GlintScreenSpaceScale;
	float GlintLogMicrofacetDensity;
	float GlintMicrofacetRoughness;
	float GlintDensityRandomization;
	Glints::GlintCachedVars GlintCache;
	float Noise;
#endif
};

namespace Foliage
{
	namespace Constants
	{
		static const float Wrap = 0.5f;
		static const float ScatterRoughness = 0.6f;
		static const float Pi = 3.14159265f;
	}
}

// Cheap view-dependent foliage transmission shared by PBR and vanilla foliage paths.
float GetFoliageTransmission(float NdotL, float VdotL)
{
	const float wrapDenominator = (1.0f + Foliage::Constants::Wrap) * (1.0f + Foliage::Constants::Wrap);
	float wrappedBacklight = saturate((-NdotL + Foliage::Constants::Wrap) / wrapDenominator);

	float forwardScatter = saturate(-VdotL);
	const float scatterAlpha2 = Foliage::Constants::ScatterRoughness * Foliage::Constants::ScatterRoughness;
	float scatterDenominator = forwardScatter * forwardScatter * (scatterAlpha2 - 1.0f) + 1.0f;
	float scatter = scatterAlpha2 / (Foliage::Constants::Pi * scatterDenominator * scatterDenominator);

	return wrappedBacklight * scatter;
}

float ShininessToRoughness(float shininess)
{
	return pow(abs(2.0 / (shininess + 2.0)), 0.25);
}
#endif
