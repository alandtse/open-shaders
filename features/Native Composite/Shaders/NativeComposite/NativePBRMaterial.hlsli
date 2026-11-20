#ifndef NATIVE_PBR_MATERIAL_HLSLI
#define NATIVE_PBR_MATERIAL_HLSLI

#include "Common/PBRMath.hlsli"

namespace NativePBR
{
	static const uint MissingTexture = 0xffffffffu;
	static const uint DrawVertexColorFlag = 1u;
	static const uint DrawProjectedSnowCandidateFlag = 1u << 3u;
	static const uint SnowCoverageShift = 16u;
	static const uint LegacyComplexMaskFlag = 1u << 8u;
	static const uint MaterialOptionParallax = 1u << 0u;
	static const uint MaterialOptionComplex = 1u << 1u;

	struct MaterialRecord
	{
		uint4 textures[4];
		float4 uv[2];
		float4 base;
		float4 coat;
		float4 surface;
		float4 projected;
		float4 coatColor;
		float4 subsurfaceColor;
		float4 fuzzColor;
		float4 thinSurface;
		float4 glint;
		float4 projectedGlint;
		uint4 properties;
		uint4 identity;
		uint4 controls;
	};

	struct SurfaceInput
	{
		float2 uv;
		float2 uvDx;
		float2 uvDy;
		float3 tangent;
		float3 bitangent;
		float3 geometricNormal;
		float3 vertexColor;
		float facing;
		float projectedSnowIncidence;
	};

	struct SurfaceMaterial
	{
		float3 baseColor;
		float3 normal;
		float3 f0;
		float3 emission;
		float roughness;
		float metallic;
		float ao;
		float opacity;
		float coatStrength;
		float coatRoughness;
		float coatF0;
		float projectedSnowCoverageCandidate;
		float snowCoverage;
	};

	float3 SrgbToLinear(float3 value)
	{
		value = max(value, 0.0f);
		return lerp(value / 12.92f, pow((value + 0.055f) / 1.055f, 2.4f), value > 0.04045f);
	}

	float3 DecodeNormal(SurfaceInput surface, float3 sampled)
	{
		float normalLengthSq = dot(surface.geometricNormal, surface.geometricNormal);
		float3 n = normalLengthSq > 1e-8f ? surface.geometricNormal * rsqrt(normalLengthSq) : float3(0.0f, 0.0f, 1.0f);
		n *= surface.facing < 0.0f ? -1.0f : 1.0f;
		float3 t = surface.tangent - n * dot(n, surface.tangent);
		float tangentLengthSq = dot(t, t);
		if (tangentLengthSq <= 1e-8f)
			return n;
		t *= rsqrt(tangentLengthSq);
		float handedness = dot(cross(n, t), surface.bitangent) < 0.0f ? -1.0f : 1.0f;
		float3 b = cross(n, t) * handedness;
		float3 tangentNormal = sampled * 2.0f - 1.0f;
		return normalize(t * tangentNormal.x + b * tangentNormal.y + n * tangentNormal.z);
	}

	SurfaceMaterial EvaluateOpaque(MaterialRecord material, SurfaceInput surface, float4 baseTexel,
		float4 normalTexel, float4 packedTexel, float4 emissionTexel, float4 coatTexel,
		bool complexMaskDetected, uint drawFlags)
	{
		SurfaceMaterial result;
		result.baseColor = baseTexel.rgb;
		if ((drawFlags & DrawVertexColorFlag) != 0u)
			result.baseColor *= NativePBR::SrgbToLinear(surface.vertexColor);
		result.normal = DecodeNormal(surface, normalTexel.xyz);
		if ((material.controls.y & LegacyComplexMaskFlag) != 0u) {
			// Vanilla environment masks are not RMAOS. Alpha identifies complex masks;
			// Y is their gloss multiplier and Z controls conductive/specular color.
			// Preserve normal-alpha gloss when the mask has no complex payload.
			const bool complex = complexMaskDetected && packedTexel.g > (4.0f / 255.0f);
			result.roughness = clamp(1.0f - normalTexel.a * (complex ? packedTexel.g : 1.0f),
				PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
			result.metallic = complex ? saturate(packedTexel.b) : 0.0f;
			result.ao = 1.0f;
			result.f0 = lerp(saturate(material.base.w * packedTexel.r).xxx, result.baseColor,
				result.metallic);
		} else {
			result.roughness = clamp(packedTexel.r * material.base.y, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
			result.metallic = saturate(packedTexel.g);
			result.ao = saturate(packedTexel.b);
			result.f0 = lerp(saturate(packedTexel.a * material.base.w).xxx, result.baseColor, result.metallic);
		}
		result.opacity = saturate(baseTexel.a * material.base.x);
		result.emission = material.textures[1].z != MissingTexture ? emissionTexel.rgb : 0.0f;
		const bool simpleCoat = material.controls.w == (1u << 1u);
		result.coatStrength = simpleCoat ? saturate(material.coat.x * coatTexel.a) : 0.0f;
		result.coatRoughness = clamp(material.coat.y, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
		result.coatF0 = saturate(material.coat.z);
		result.projectedSnowCoverageCandidate = (drawFlags & DrawProjectedSnowCandidateFlag) != 0u ?
			saturate(surface.projectedSnowIncidence) : 0.0f;
		result.snowCoverage = result.projectedSnowCoverageCandidate * float((drawFlags >> SnowCoverageShift) & 255u) / 255.0f;
		result.baseColor = lerp(result.baseColor, float3(0.78f, 0.84f, 0.89f), result.snowCoverage);
		result.roughness = lerp(result.roughness, 0.82f, result.snowCoverage);
		result.metallic *= 1.0f - result.snowCoverage;
		result.f0 = lerp(result.f0, float3(0.04f, 0.04f, 0.04f), result.snowCoverage);
		result.coatStrength *= 1.0f - result.snowCoverage;
		return result;
	}
}

#endif
