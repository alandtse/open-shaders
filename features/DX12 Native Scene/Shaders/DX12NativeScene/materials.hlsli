#include "Common/PBRMath.hlsli"
#include "Common/LightingCommon.hlsli"

struct SurfaceRecord
{
	uint4 geometry;
	uint4 material;
};

struct MaterialRecord
{
	uint4 colorNormal;
	uint4 packedEmission;
	float4 uv;
	float4 emission;
	float4 specular;
	float4 scalar;
	uint4 flags;
	float4 reserved;
};

StructuredBuffer<SurfaceRecord> Surfaces : register(t4);
StructuredBuffer<MaterialRecord> Materials : register(t5);
ByteAddressBuffer Attributes[8192] : register(t0, space2);
Texture2DArray<float4> TexturePools[256] : register(t0, space3);
SamplerState ClampClampSampler : register(s0);
SamplerState ClampWrapSampler : register(s1);
SamplerState WrapClampSampler : register(s2);
SamplerState WrapWrapSampler : register(s3);
static const uint MissingTexture = 0xffffffff;

struct TriangleSurface
{
	float2 uv;
	float4 color;
	float3 normal, tangent, bitangent;
	float3 position0, position1, position2;
	float2 uv0, uv1, uv2;
	uint stride;
};

struct EvaluatedSourceMaterial
{
	float3 baseColor, normal, emission, f0;
	float roughness, metallic, ao, opacity;
};

bool LoadTriangle(uint instance, uint primitive, float2 barycentric, out TriangleSurface surface)
{
	surface = (TriangleSurface)0;
	if (instance >= InstanceCount) return false;
	SurfaceRecord binding = Surfaces[instance];
	if (primitive >= binding.material.y || (binding.geometry.w != 36 && binding.geometry.w != 60)) return false;
	uint indexOffset = binding.geometry.x + (binding.geometry.y + primitive * 3) * 4;
	uint3 indices = GeometryBuffers[NonUniformResourceIndex(instance)].Load3(indexOffset);
	if (any(indices >= binding.geometry.z)) return false;
	float3 weights = float3(1 - barycentric.x - barycentric.y, barycentric);
	surface.stride = binding.geometry.w;
	float2 uvs[3];
	float3 positions[3];
	for (uint i = 0; i < 3; ++i) {
		uint offset = indices[i] * binding.geometry.w;
		float3 normal = asfloat(Attributes[NonUniformResourceIndex(instance)].Load3(offset));
		uvs[i] = asfloat(Attributes[NonUniformResourceIndex(instance)].Load2(offset + 12));
		positions[i] = asfloat(GeometryBuffers[NonUniformResourceIndex(instance)].Load3(indices[i] * 12));
		surface.normal += normal * weights[i];
		surface.uv += uvs[i] * weights[i];
		surface.color += asfloat(Attributes[NonUniformResourceIndex(instance)].Load4(offset + 20)) * weights[i];
		if (binding.geometry.w == 60) {
			surface.tangent += asfloat(Attributes[NonUniformResourceIndex(instance)].Load3(offset + 36)) * weights[i];
			surface.bitangent += asfloat(Attributes[NonUniformResourceIndex(instance)].Load3(offset + 48)) * weights[i];
		}
	}
	surface.position0 = positions[0]; surface.position1 = positions[1]; surface.position2 = positions[2];
	surface.uv0 = uvs[0]; surface.uv1 = uvs[1]; surface.uv2 = uvs[2];
	return all(isfinite(surface.normal)) && dot(surface.normal, surface.normal) > 0 && all(isfinite(surface.uv));
}

float4 SampleMap(uint2 location, uint samplerIndex, float2 uv, float lod, float4 missing)
{
	if (location.x == MissingTexture || location.x >= 256 || samplerIndex >= 4) return missing;
	uint width, height, slices, mips;
	TexturePools[NonUniformResourceIndex(location.x)].GetDimensions(0, width, height, slices, mips);
	if (width == 0 || height == 0 || location.y >= slices || mips == 0) return missing;
	float mip = clamp(lod + log2(float(max(width, height))), 0.0, float(mips - 1));
	float3 coordinates = float3(uv, location.y);
	if (samplerIndex == 0) return TexturePools[NonUniformResourceIndex(location.x)].SampleLevel(ClampClampSampler, coordinates, mip);
	if (samplerIndex == 1) return TexturePools[NonUniformResourceIndex(location.x)].SampleLevel(ClampWrapSampler, coordinates, mip);
	if (samplerIndex == 2) return TexturePools[NonUniformResourceIndex(location.x)].SampleLevel(WrapClampSampler, coordinates, mip);
	return TexturePools[NonUniformResourceIndex(location.x)].SampleLevel(WrapWrapSampler, coordinates, mip);
}

bool AlphaPass(float value, float threshold, uint function)
{
	if (function == 0) return true;
	if (function == 1) return value < threshold;
	if (function == 2) return value == threshold;
	if (function == 3) return value <= threshold;
	if (function == 4) return value > threshold;
	if (function == 5) return value != threshold;
	if (function == 6) return value >= threshold;
	return false;
}

bool AdmitCandidate(uint instance, uint primitive, float2 barycentric)
{
	SurfaceRecord binding = Surfaces[instance];
	if (binding.material.z != 0) return true;
	MaterialRecord material = Materials[binding.material.x];
	if ((material.flags.x & 256) == 0 || material.flags.w != 0 || (material.flags.x & 16) == 0) return false;
	TriangleSurface surface;
	if (!LoadTriangle(instance, primitive, barycentric, surface)) return false;
	float2 uv = surface.uv * material.uv.xy + material.uv.zw;
	float alpha = SampleMap(material.colorNormal.xy, material.flags.z, uv, -32, 0).a * material.scalar.x;
	if (material.flags.x & 32) alpha *= surface.color.a;
	return AlphaPass(alpha, material.scalar.y, (material.flags.y >> 10) & 7);
}

float3 SrgbToLinear(float3 value)
{
	return lerp(value / 12.92, pow(max((value + 0.055) / 1.055, 0), 2.4), value > 0.04045);
}

float3 LinearToSrgb(float3 value)
{
	value = max(value, 0);
	return lerp(12.92 * value, 1.055 * pow(value, 1.0 / 2.4) - 0.055, value > 0.0031308);
}

float MaterialLod(MaterialRecord material, TriangleSurface surface, float3x4 objectToWorld, float footprint)
{
	float3 edge0 = mul((float3x3)objectToWorld, surface.position1 - surface.position0);
	float3 edge1 = mul((float3x3)objectToWorld, surface.position2 - surface.position0);
	float worldArea = length(cross(edge0, edge1));
	float2 uv0 = (surface.uv1 - surface.uv0) * material.uv.xy;
	float2 uv1 = (surface.uv2 - surface.uv0) * material.uv.xy;
	float uvArea = abs(uv0.x * uv1.y - uv0.y * uv1.x);
	float density = sqrt(uvArea / max(worldArea, 1e-12));
	return log2(max(footprint * density, 1e-10));
}

bool LoadSourceMaterial(uint instance, uint primitive, float2 barycentric, float3x4 objectToWorld, float3x4 worldToObject, float footprint, out EvaluatedSourceMaterial evaluated)
{
	evaluated = (EvaluatedSourceMaterial)0;
	TriangleSurface surface;
	if (!LoadTriangle(instance, primitive, barycentric, surface)) return false;
	MaterialRecord material = Materials[Surfaces[instance].material.x];
	if ((material.flags.x & 1) == 0) return false;
	float2 uv = surface.uv * material.uv.xy + material.uv.zw;
	float lod = MaterialLod(material, surface, objectToWorld, footprint);
	float4 base = SampleMap(material.colorNormal.xy, material.flags.z, uv, lod, 0);
	float4 normalMap = SampleMap(material.colorNormal.zw, material.flags.z, uv, lod, float4(0.5, 0.5, 1, 0));
	float3 baseColor = SrgbToLinear(base.rgb);
	if (material.flags.x & 8) baseColor *= SrgbToLinear(max(surface.color.rgb, 0));
	float3 n = normalize(mul(surface.normal, (float3x3)worldToObject));
	if (material.flags.x & 4) n = normalize(mul(normalMap.xzy * 2 - 1, (float3x3)worldToObject));
	else {
		float3 t = mul((float3x3)objectToWorld, surface.tangent);
		t = normalize(t - n * dot(n, t));
		float3 b = cross(n, t);
		b *= dot(b, mul((float3x3)objectToWorld, surface.bitangent)) < 0 ? -1 : 1;
		float3 mapped = normalMap.xyz * 2 - 1;
		n = normalize(t * mapped.x + b * mapped.y + n * mapped.z);
	}
	if (!all(isfinite(n))) return false;
	float4 packed = SampleMap(material.packedEmission.xy, material.flags.z, uv, lod, float4(1, 0, 1, 0));
	evaluated.baseColor = baseColor;
	evaluated.normal = n;
	evaluated.roughness = (material.flags.x & 2) ? clamp(packed.r * material.scalar.w, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness) : ShininessToRoughness(max(material.scalar.z, 0));
	evaluated.metallic = (material.flags.x & 2) ? saturate(packed.g) : 0;
	evaluated.ao = (material.flags.x & 2) ? saturate(packed.b) : 1;
	evaluated.f0 = (material.flags.x & 2) ? lerp(saturate(packed.a * material.scalar.z).xxx, baseColor, evaluated.metallic) : 0;
	evaluated.opacity = base.a * material.scalar.x * ((material.flags.x & 32) ? surface.color.a : 1);
	evaluated.emission = 0;
	if (material.flags.x & 64) evaluated.emission = SrgbToLinear(SampleMap(material.packedEmission.zw, material.flags.z, uv, lod, float4(1, 1, 1, 1)).rgb) * material.emission.rgb * material.emission.w;
	return all(isfinite(evaluated.baseColor)) && all(isfinite(evaluated.emission)) && all(isfinite(evaluated.f0)) &&
		isfinite(evaluated.roughness) && isfinite(evaluated.metallic) && isfinite(evaluated.ao) && isfinite(evaluated.opacity);
}

float4 EvaluateMaterial(uint instance, uint primitive, float2 barycentric, float3x4 objectToWorld, float3x4 worldToObject, float footprint, uint mode)
{
	MaterialRecord material = Materials[Surfaces[instance].material.x];
	if (mode == 6) return float4((material.flags.x & 1) == 0 ? float3(1, 0, 1) : material.flags.w != 0 ? float3(1, 0.5, 0) : float3(0, 1, 0), 3);
	if ((material.flags.x & 1) == 0) return float4(1, 0, 1, 3);
	if (material.flags.w != 0 && mode >= 3) return float4(1, 0.5, 0, 3);
	EvaluatedSourceMaterial evaluated;
	if (!LoadSourceMaterial(instance, primitive, barycentric, objectToWorld, worldToObject, footprint, evaluated)) return float4(1, 0, 1, 3);
	if (mode == 8 && (material.flags.x & 2) == 0) return float4(1, 0.5, 0, 3);
	float3 display = mode == 1 ? LinearToSrgb(evaluated.baseColor) : mode == 2 ? evaluated.normal * 0.5 + 0.5 :
		mode == 3 ? evaluated.roughness.xxx : mode == 4 ? evaluated.metallic.xxx : mode == 5 ? LinearToSrgb(evaluated.emission) :
		mode == 7 ? evaluated.ao.xxx : mode == 8 ? LinearToSrgb(evaluated.f0) : saturate(evaluated.opacity).xxx;
	return float4(display, 3);
}
