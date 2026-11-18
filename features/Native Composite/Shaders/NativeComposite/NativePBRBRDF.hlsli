#ifndef NATIVE_PBR_BRDF_HLSLI
#define NATIVE_PBR_BRDF_HLSLI

#include "NativePBRMaterial.hlsli"

namespace NativePBR
{
	struct DirectResponse
	{
		float3 diffuse;
		float3 specular;
	};

	DirectResponse EvaluateDirect(SurfaceMaterial material, float3 viewDirection, float3 lightDirection, float3 incidentRadiance)
	{
		DirectResponse response = (DirectResponse)0;
		float3 n = material.normal;
		float3 v = normalize(viewDirection);
		float3 l = normalize(lightDirection);
		float nDotL = saturate(dot(n, l));
		float nDotV = saturate(dot(n, v));
		if (nDotL <= 0.0f || nDotV <= 0.0f)
			return response;
		float3 h = normalize(v + l);
		float nDotH = saturate(dot(n, h));
		float vDotH = saturate(dot(v, h));
		float3 fresnel;
		float3 specularBRDF = PBR::SpecularMicrofacet(material.roughness, material.f0, nDotL, nDotV, nDotH, vDotH, fresnel);
		float3 diffuseWeight = (1.0f - fresnel) * (1.0f - material.metallic);
		response.diffuse = material.baseColor * diffuseWeight * BRDF::Diffuse_Burley(material.roughness, nDotV, nDotL, vDotH) * nDotL * incidentRadiance;
		response.specular = specularBRDF * nDotL * incidentRadiance;
		return response;
	}

	float3 EvaluateIndirectDiffuse(SurfaceMaterial material, float3 irradiance)
	{
		float3 weight = (1.0f - material.metallic) * (1.0f - material.f0);
		return material.baseColor * weight * material.ao * max(irradiance, 0.0f) * BRDF::Diffuse_Lambert();
	}
}

#endif
