#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/PBRMath.hlsli"

namespace GrassLighting
{
	static const float LegacyTransmissionSaturation = 1.15;
	static const float LegacyGreenTransmissionBlend = 0.17;
	static const float LegacyTransmissionScale = 0.5;
	static const float LegacyMinimumForwardScatter = 0.25;

	float3 GetLegacyTransmissionTint(float3 albedo)
	{
		albedo = max(albedo, 0.0);
		float grey = dot(albedo, 0.333333.xxx);
		float3 tint = max(lerp(grey.xxx, albedo, LegacyTransmissionSaturation), 0.0);
		float greenDominance = saturate((albedo.g - max(albedo.r, albedo.b)) / max(albedo.g, EPSILON_DIVISION));
		tint.r = lerp(tint.r, tint.g, LegacyGreenTransmissionBlend * greenDominance);
		return tint;
	}

	float GetLegacyTransmissionFactor(float NdotL, float VdotL, float amount)
	{
		float backLight = saturate(-NdotL);
		backLight = backLight * backLight * (3.0 - 2.0 * backLight);
		float forwardScatter = saturate(-VdotL);
		forwardScatter *= forwardScatter;
		return LegacyTransmissionScale * amount * backLight * lerp(LegacyMinimumForwardScatter, 1.0, forwardScatter);
	}

	void GetDirectLighting(float3 albedo, float3 reflectionAlbedo, float3 frontTransmissionAlbedo, float3 backTransmissionAlbedo,
		float3 shineThroughAlbedo, float opacity, float3 irradiance, float NdotL, float VdotL, float wrap,
		float reflectionShadow, float2 transmissionShadows, out float3 reflection, out float3 transmission)
	{
		[branch] if (SharedData::grassLightingSettings.EnableNewLightingModel)
			PBR::GetThinSurfaceDirectScattering(reflectionAlbedo, frontTransmissionAlbedo, backTransmissionAlbedo, shineThroughAlbedo,
				opacity, irradiance, NdotL, wrap, reflectionShadow, transmissionShadows, reflection, transmission);
		else
		{
			reflection = albedo * irradiance * reflectionShadow * (saturate(NdotL + wrap) / (1.0 + wrap));
			transmission = GetLegacyTransmissionTint(albedo) * irradiance * min(transmissionShadows.x, transmissionShadows.y) *
			               GetLegacyTransmissionFactor(NdotL, VdotL, SharedData::grassLightingSettings.SubsurfaceScatteringAmount);
		}
	}

	float GetRainWetness()
	{
#if defined(WETNESS_EFFECTS)
		return saturate(SharedData::wetnessEffectsSettings.Wetness * SharedData::wetnessEffectsSettings.MaxRainWetness);
#else
		return 0.0;
#endif
	}

	void GetSubsurfaceAlbedos(float3 albedo, float amount, float balance,
		out float3 reflectionAlbedo, out float3 frontTransmissionAlbedo, out float3 backTransmissionAlbedo)
	{
		[branch] if (SharedData::grassLightingSettings.EnableNewLightingModel)
		{
			float reflectionWeight;
			PBR::GetThinSurfaceSubsurfaceAlbedos(albedo, albedo, 1.0 - 0.5 * saturate(amount), balance,
				reflectionWeight, frontTransmissionAlbedo, backTransmissionAlbedo);
			reflectionAlbedo = albedo * reflectionWeight;
		}
		else
		{
			reflectionAlbedo = albedo;
			frontTransmissionAlbedo = 0;
			backTransmissionAlbedo = 0;
		}
	}

	float3 GetLightSpecularInput(float3 L, float3 V, float3 N, float3 lightColor, float roughness, float3 F0)
	{
		float3 H = normalize(V + L);
#if defined(VANILLA_FRESNEL)
		if (SharedData::vanillaFresnelSettings.Enable && SharedData::vanillaFresnelSettings.EnableGGXOnGrass) {
			float NdotL = saturate(dot(N, L));
			float NdotV = saturate(dot(N, V));
			float NdotH = saturate(dot(N, H));
			float VdotH = saturate(dot(V, H));

			float D = BRDF::D_GGX(roughness, NdotH);
			float G = BRDF::Vis_SmithJointApprox(roughness, NdotL, NdotV);
			float3 F = BRDF::F_Schlick(F0, VdotH);
			float3 specular = D * G * F;
			return specular * lightColor * NdotL * Color::PBRLightingCompensation;
		}
#endif
		float shininess = (1.0 - roughness) * 100.f;
		// log2(0) times a zero shininess is NaN
		float HdotN = max(saturate(dot(H, N)), 1e-6);
		float lightColorMultiplier = exp2(shininess * log2(HdotN));
		return lightColor * lightColorMultiplier.xxx;
	}

	float GetCanopyVisibility(float canopyHeight)
	{
		if (!SharedData::grassLightingSettings.EnableNewLightingModel)
			return 1.0;
		return 1.0 - SharedData::grassLightingSettings.CanopyOcclusion * (1.0 - saturate(canopyHeight));
	}

	// Bend within the normal's tangent plane to avoid hemisphere flips and cancellation.
	float3 GetClumpBlendedNormal(float3 bladeNormal, float3 clumpNormal)
	{
		if (!SharedData::grassLightingSettings.EnableNewLightingModel)
			return bladeNormal;
		float3 bend = clumpNormal - bladeNormal * dot(bladeNormal, clumpNormal);
		return normalize(bladeNormal + bend * saturate(SharedData::grassLightingSettings.ClumpShading));
	}

	// Clump occlusion must not depend on which side of a blade faces the light.
	float GetClumpLightVisibility(float3 clumpNormal, float3 L)
	{
		if (!SharedData::grassLightingSettings.EnableNewLightingModel)
			return 1.0;
		return lerp(1.0, saturate(dot(clumpNormal, L) * 0.5 + 0.5), SharedData::grassLightingSettings.ClumpShading);
	}

#if defined(TRUE_PBR)
	static const float LegacyPBRDiffuseWrap = 0.5;
	static const float LegacyPBRSubsurfacePower = 12.234;

	void GetLegacyPBRDirectLighting(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material, bool doSpecular)
	{
		lightingOutput = (DirectLightingOutput)0;
		const float3 detailedLightColor = context.lightColor * context.detailedShadow;
		const float3 softLightColor = context.lightColor * context.softShadow;

		const float3 N = context.worldNormal;
		const float3 V = context.viewDir;
		const float3 L = context.lightDir;

		float NdotL = dot(N, L);
		float VdotL = dot(V, L);

		float satNdotL = clamp(NdotL, EPSILON_DOT_CLAMP, 1);

		float3 F = 0;
		float3 Fr = 0;
		[branch] if (doSpecular)
		{
			const float3 H = context.halfVector;
			float satNdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
			float satNdotH = saturate(dot(N, H));
			float satVdotH = saturate(dot(V, H));
			Fr = PBR::SpecularMicrofacet(material.Roughness, material.F0, satNdotL, satNdotV, satNdotH, satVdotH, F);
		}
		float3 kD = 1 - F;

		float wrappedNdotL = saturate((abs(NdotL) + LegacyPBRDiffuseWrap) / (1.0 + LegacyPBRDiffuseWrap));
		lightingOutput.diffuse += material.BaseColor * detailedLightColor * wrappedNdotL * BRDF::Diffuse_Lambert() * kD;
		lightingOutput.specular += Fr * detailedLightColor * satNdotL;

		[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
		{
			float forwardScatter = exp2(saturate(-VdotL) * LegacyPBRSubsurfacePower - LegacyPBRSubsurfacePower);
			float backScatter = saturate(satNdotL * material.Thickness + (1.0 - material.Thickness)) * 0.5;
			float subsurface = lerp(backScatter, 1, forwardScatter) * (1.0 - material.Thickness);
			lightingOutput.transmission += material.SubsurfaceColor * subsurface * softLightColor * BRDF::Diffuse_Lambert() * kD;
		}
	}

	void GetLegacyPBRIndirectLighting(out IndirectLobeWeights lobeWeights, IndirectContext context, MaterialProperties material, bool doSpecular)
	{
		lobeWeights = (IndirectLobeWeights)0;

		lobeWeights.diffuse = material.BaseColor;

		[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
		{
			lobeWeights.diffuse += material.SubsurfaceColor * (1 - material.Thickness) / Math::PI;
		}

		[branch] if (doSpecular)
		{
			float NdotV = saturate(dot(context.worldNormal, context.viewDir));
			float2 specularBRDF = BRDF::EnvBRDF(material.Roughness, NdotV);
			lobeWeights.specular = material.F0 * specularBRDF.x + specularBRDF.y;
			lobeWeights.diffuse *= 1 - lobeWeights.specular;

			float alpha = material.Roughness * material.Roughness;
			lobeWeights.specular *= SpecularOcclusion(NdotV, alpha, material.AO);
		}

		lobeWeights.diffuse *= MultiBounceAO(material.BaseColor, material.AO);
	}
#endif

	float3 TransformNormal(float3 normal)
	{
		return normal * 2 + -1.0.xxx;
	}

	// http://www.thetenthplanet.de/archives/1180
	float3x3 CalculateTBN(float3 N, float3 p, float2 uv)
	{
		// get edge vectors of the pixel triangle
		float3 dp1 = ddx_coarse(p);
		float3 dp2 = ddy_coarse(p);
		float2 duv1 = ddx_coarse(uv);
		float2 duv2 = ddy_coarse(uv);

		// solve the linear system
		float3 dp2perp = cross(dp2, N);
		float3 dp1perp = cross(N, dp1);
		float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
		float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

		// construct a scale-invariant frame
		float invmax = rsqrt(max(dot(T, T), dot(B, B)));
		return float3x3(T * invmax, B * invmax, N);
	}
}
