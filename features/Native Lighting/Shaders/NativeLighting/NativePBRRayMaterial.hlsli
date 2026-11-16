#ifndef NATIVE_PBR_RAY_MATERIAL_HLSLI
#define NATIVE_PBR_RAY_MATERIAL_HLSLI

#include "NativeComposite/NativePBRMaterial.hlsli"

struct NativePBRRayBinding
{
	uint2 identity;
	uint2 revision;
	uint materialIndex;
	uint flags;
};

StructuredBuffer<NativePBRRayBinding> NativePBRInstances : register(t8);
StructuredBuffer<NativePBR::MaterialRecord> NativePBRRecords : register(t9);
Texture2DArray<float4> NativePBRPages[512] : register(t0, space4);

bool SampleNativePBR(uint2 location, uint clampMode, float2 uv, float lod, out float4 value)
{
	value = 0.0f;
	if (location.x == NativePBR::MissingTexture || location.x >= 512u || clampMode > 3u)
		return false;
	uint width, height, slices, mips;
	NativePBRPages[NonUniformResourceIndex(location.x)].GetDimensions(0, width, height, slices, mips);
	if (width == 0u || height == 0u || mips == 0u || location.y >= slices)
		return false;
	float mip = clamp(lod + log2(float(max(width, height))), 0.0f, float(mips - 1u));
	float3 coords = float3(uv, location.y);
	if (clampMode == 0u)
		value = NativePBRPages[NonUniformResourceIndex(location.x)].SampleLevel(ClampClampSampler, coords, mip);
	else if (clampMode == 1u)
		value = NativePBRPages[NonUniformResourceIndex(location.x)].SampleLevel(ClampWrapSampler, coords, mip);
	else if (clampMode == 2u)
		value = NativePBRPages[NonUniformResourceIndex(location.x)].SampleLevel(WrapClampSampler, coords, mip);
	else
		value = NativePBRPages[NonUniformResourceIndex(location.x)].SampleLevel(WrapWrapSampler, coords, mip);
	return all(isfinite(value));
}

float NativePBRLod(NativePBR::MaterialRecord material, TriangleSurface hitSurface,
	float3x4 objectToWorld, float footprint)
{
	float3 edge0 = mul((float3x3)objectToWorld, hitSurface.position1 - hitSurface.position0);
	float3 edge1 = mul((float3x3)objectToWorld, hitSurface.position2 - hitSurface.position0);
	float worldArea = length(cross(edge0, edge1));
	float2 uvEdge0 = (hitSurface.uv1 - hitSurface.uv0) * material.uv[0].zw;
	float2 uvEdge1 = (hitSurface.uv2 - hitSurface.uv0) * material.uv[0].zw;
	float uvArea = abs(uvEdge0.x * uvEdge1.y - uvEdge0.y * uvEdge1.x);
	return log2(max(footprint * sqrt(uvArea / max(worldArea, 1e-12f)), 1e-10f));
}

bool LoadNativePBRMaterial(uint instance, uint primitive, float2 barycentric,
	float3x4 objectToWorld, float3x4 worldToObject, float footprint, out EvaluatedSourceMaterial evaluated,
	out bool usedFallback, out float3 clearcoat)
{
	evaluated = (EvaluatedSourceMaterial)0;
	usedFallback = false;
	clearcoat = 0.0f;
	if (NativePBRReady == 0u || instance >= InstanceCount)
		return false;
	NativePBRRayBinding binding = NativePBRInstances[instance];
	if (binding.materialIndex == 0xffffffffu)
		return false;
	NativePBR::MaterialRecord material = NativePBRRecords[binding.materialIndex];
	if (any(material.identity.xy != binding.identity) || any(material.identity.zw != binding.revision))
		return false;
	TriangleSurface hitSurface;
	if (!LoadTriangle(instance, primitive, barycentric, hitSurface))
		return false;
	float2 uv = hitSurface.uv * material.uv[0].zw + material.uv[0].xy;
	float lod = NativePBRLod(material, hitSurface, objectToWorld, footprint);
	if (!all(isfinite(uv)) || !isfinite(lod))
		return false;
	float4 base, normal, packed, emission = 0.0f;
	const bool baseReady = SampleNativePBR(material.textures[0].xy, material.properties.z, uv, lod, base);
	if (!baseReady && NativePBRPadding.y == 0u) return false;
	const bool normalReady = SampleNativePBR(material.textures[0].zw, material.properties.z, uv, lod, normal);
	if (!normalReady && NativePBRPadding.y == 0u) return false;
	const bool packedReady = SampleNativePBR(material.textures[1].xy, material.properties.z, uv, lod, packed);
	if (!packedReady && NativePBRPadding.y == 0u) return false;
	if (!baseReady) base = float4(1, 1, 1, 1);
	if (!normalReady) normal = float4(0.5f, 0.5f, 1, 1);
	if (!packedReady) packed = float4(0.7f, 0, 1, 0.04f);
	usedFallback = !baseReady || !normalReady || !packedReady;
	if (material.textures[1].z != NativePBR::MissingTexture &&
		!SampleNativePBR(material.textures[1].zw, material.properties.z, uv, lod, emission)) {
		if (NativePBRPadding.y == 0u) return false;
		emission = 0.0f;
		usedFallback = true;
	}
	float4 coat = 1.0f;
	if (material.controls.w == (1u << 1u) && material.textures[2].z != NativePBR::MissingTexture &&
		!SampleNativePBR(material.textures[2].zw, material.properties.z, uv, lod, coat)) {
		if (NativePBRPadding.y == 0u) return false;
		coat = 1.0f;
		usedFallback = true;
	}
	NativePBR::SurfaceInput surface;
	surface.uv = uv;
	surface.uvDx = 0.0f;
	surface.uvDy = 0.0f;
	surface.geometricNormal = normalize(mul(hitSurface.normal, (float3x3)worldToObject));
	surface.tangent = mul((float3x3)objectToWorld, hitSurface.tangent);
	surface.bitangent = mul((float3x3)objectToWorld, hitSurface.bitangent);
	surface.vertexColor = hitSurface.color.rgb;
	surface.facing = 1.0f;
	if (!all(isfinite(surface.geometricNormal)) ||
		!all(isfinite(surface.tangent)) || !all(isfinite(surface.bitangent)))
		return false;
	NativePBR::SurfaceMaterial result = NativePBR::EvaluateOpaque(material, surface, base, normal, packed, emission, coat, binding.flags);
	evaluated.baseColor = result.baseColor;
	evaluated.normal = result.normal;
	evaluated.f0 = result.f0;
	evaluated.emission = result.emission;
	evaluated.roughness = result.roughness;
	evaluated.metallic = result.metallic;
	evaluated.ao = result.ao;
	evaluated.opacity = result.opacity;
	clearcoat = float3(result.coatStrength, result.coatRoughness, result.coatF0);
	return all(isfinite(evaluated.baseColor)) && all(isfinite(evaluated.normal)) &&
		all(isfinite(evaluated.f0)) && all(isfinite(evaluated.emission)) &&
		isfinite(evaluated.roughness) && isfinite(evaluated.metallic) &&
		isfinite(evaluated.ao) && isfinite(evaluated.opacity) && all(isfinite(clearcoat));
}

#endif
