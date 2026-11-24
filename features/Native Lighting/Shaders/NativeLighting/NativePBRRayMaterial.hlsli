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

// Native ray-hit POM uses the same resident DDS texture pages as the other
// material inputs. It must use an explicit mip: pixel derivatives do not exist
// in RayQuery's compute shader. The shallow, bounded march follows the
// single-layer Extended Materials height convention without FXC dependencies.
bool NativePBRParallaxUV(NativePBR::MaterialRecord material, TriangleSurface hitSurface,
	float3x4 objectToWorld, float3x4 worldToObject, float3 viewDirectionWS,
	uint2 heightLocation, uint heightChannel, float lod, inout float2 uv)
{
	if (heightLocation.x == NativePBR::MissingTexture || heightChannel > 3u)
		return false;
	float3 normalWS = mul(hitSurface.normal, (float3x3)worldToObject);
	if (!all(isfinite(normalWS)) || dot(normalWS, normalWS) <= 1e-8f)
		return false;
	normalWS = normalize(normalWS);
	float3 edge0 = mul((float3x3)objectToWorld, hitSurface.position1 - hitSurface.position0);
	float3 edge1 = mul((float3x3)objectToWorld, hitSurface.position2 - hitSurface.position0);
	float2 uvEdge0 = (hitSurface.uv1 - hitSurface.uv0) * material.uv[0].zw;
	float2 uvEdge1 = (hitSurface.uv2 - hitSurface.uv0) * material.uv[0].zw;
	float determinant = uvEdge0.x * uvEdge1.y - uvEdge1.x * uvEdge0.y;
	float3 tangentWS = mul((float3x3)objectToWorld, hitSurface.tangent);
	float3 bitangentRawWS = mul((float3x3)objectToWorld, hitSurface.bitangent);
	if (isfinite(determinant) && abs(determinant) > 1e-8f) {
		float3 uvTangent = (edge0 * uvEdge1.y - edge1 * uvEdge0.y) / determinant;
		float3 uvBitangent = (edge1 * uvEdge0.x - edge0 * uvEdge1.x) / determinant;
		if (all(isfinite(uvTangent)) && all(isfinite(uvBitangent)) &&
			dot(uvTangent, uvTangent) > 1e-8f && dot(uvBitangent, uvBitangent) > 1e-8f) {
			tangentWS = uvTangent;
			bitangentRawWS = uvBitangent;
		}
	}
	tangentWS -= normalWS * dot(normalWS, tangentWS);
	if (!all(isfinite(tangentWS)) || dot(tangentWS, tangentWS) <= 1e-8f)
		return false;
	tangentWS = normalize(tangentWS);
	if (!all(isfinite(bitangentRawWS)) || dot(bitangentRawWS, bitangentRawWS) <= 1e-8f)
		return false;
	float3 bitangentWS = cross(normalWS, tangentWS) *
		(dot(cross(normalWS, tangentWS), bitangentRawWS) < 0.0f ? -1.0f : 1.0f);
	float3 viewWS = viewDirectionWS;
	if (!all(isfinite(viewWS)) || dot(viewWS, viewWS) <= 1e-8f)
		return false;
	viewWS = normalize(viewWS);
	float3 viewTS = float3(dot(viewWS, tangentWS), dot(viewWS, bitangentWS), dot(viewWS, normalWS));
	float scale = clamp(material.base.z, 0.0f, 4.0f) * 0.1f;
	float2 span = viewTS.xy * (scale / max(0.3f, viewTS.z * 0.7f + 0.3f));
	if (!all(isfinite(span)))
		return false;
	const uint steps = 16u;
	float2 previousUV = uv + span * 0.5f;
	float previousDifference = -1.0f;
	[loop] for (uint step = 1u; step <= steps; ++step) {
		float bound = 1.0f - float(step) / float(steps);
		float2 candidate = previousUV - span / float(steps);
		float4 heightSample;
		if (!SampleNativePBR(heightLocation, material.properties.z, candidate, lod + 1.0f, heightSample))
			return false;
		float height = heightSample[heightChannel];
		float difference = height - bound;
		if (difference >= 0.0f) {
			float t = saturate((-previousDifference) / max(difference - previousDifference, 1e-5f));
			uv = lerp(previousUV, candidate, t);
			return all(isfinite(uv));
		}
		previousUV = candidate;
		previousDifference = difference;
	}
	uv = previousUV;
	return all(isfinite(uv));
}

bool LoadNativePBRMaterial(uint instance, uint primitive, float2 barycentric,
	float3x4 objectToWorld, float3x4 worldToObject, float footprint, float3 viewDirectionWS, out EvaluatedSourceMaterial evaluated,
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
	const float2 flatSnowUV = uv;
	float lod = NativePBRLod(material, hitSurface, objectToWorld, footprint);
	if (!all(isfinite(uv)) || !isfinite(lod))
		return false;
	const bool enableParallax = (NativePBRMaterialOptions & NativePBR::MaterialOptionParallax) != 0u;
	const bool enableComplex = (NativePBRMaterialOptions & NativePBR::MaterialOptionComplex) != 0u;
	if (!enableComplex)
		material.controls.y &= ~NativePBR::LegacyComplexMaskFlag;
	// Standard displacement takes precedence. Vanilla complex materials also
	// encode height in the alpha channel of their environment mask.
	const bool legacyComplex = (material.controls.y & NativePBR::LegacyComplexMaskFlag) != 0u;
	bool complexMaskDetected = false;
	float4 coarseMask = 0.0f;
	if (legacyComplex) {
		if (!SampleNativePBR(material.textures[1].xy, material.properties.z, uv, 32.0f, coarseMask))
			return false;
		const float epsilon = 4.0f / 255.0f;
		const bool grayscale = abs(coarseMask.r - coarseMask.g) < epsilon &&
			abs(coarseMask.g - coarseMask.b) < epsilon && abs(coarseMask.r - coarseMask.b) < epsilon;
		const bool blackHeight = all(coarseMask.rgb < epsilon) &&
			coarseMask.a > epsilon && coarseMask.a < 1.0f - epsilon;
		complexMaskDetected = coarseMask.a < 1.0f - epsilon && (!grayscale || blackHeight);
	}
	if (enableParallax && material.textures[2].x != NativePBR::MissingTexture) {
		if (!NativePBRParallaxUV(material, hitSurface, objectToWorld, worldToObject, viewDirectionWS,
			material.textures[2].xy, 0u, lod, uv))
			return false;
	} else if (enableParallax && legacyComplex && complexMaskDetected) {
		float4 originalMask;
		if (!SampleNativePBR(material.textures[1].xy, material.properties.z, uv, lod, originalMask))
			return false;
		const float epsilon = 4.0f / 255.0f;
		if (originalMask.a > epsilon && originalMask.a < 1.0f - epsilon &&
			!NativePBRParallaxUV(material, hitSurface, objectToWorld, worldToObject, viewDirectionWS,
				material.textures[1].xy, 3u, lod, uv))
			return false;
	}
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
	surface.snowUV = (NativePBRMaterialOptions & NativePBR::MaterialOptionSnowParallaxUV) != 0u ? uv : flatSnowUV;
	surface.uvDx = 0.0f;
	surface.uvDy = 0.0f;
	surface.geometricNormal = normalize(mul(hitSurface.normal, (float3x3)worldToObject));
	surface.tangent = mul((float3x3)objectToWorld, hitSurface.tangent);
	surface.bitangent = mul((float3x3)objectToWorld, hitSurface.bitangent);
	surface.vertexColor = hitSurface.color.rgb;
	surface.facing = 1.0f;
	surface.snowHitSeed = NativePBR::SnowHash(binding.identity ^ uint2(instance, primitive));
	surface.projectedSnowIncidence = 0.0f;
	surface.snowUpwardAlignment = 0.0f;
	if ((binding.flags & NativePBR::DrawProjectedSnowCandidateFlag) != 0u) {
		float3 edge0 = mul((float3x3)objectToWorld, hitSurface.position1 - hitSurface.position0);
		float3 edge1 = mul((float3x3)objectToWorld, hitSurface.position2 - hitSurface.position0);
		float3 face = cross(edge0, edge1);
		float lengthSq = dot(face, face);
		if (all(isfinite(face)) && lengthSq > 1e-12f) {
			const float upward = saturate(face.z * rsqrt(lengthSq));
			surface.snowUpwardAlignment = upward;
			float gravity = upward * upward * (3.0f - 2.0f * upward);
			if (SnowDetailControls.y > 0.0f && gravity > 0.0f && gravity < 1.0f) {
				const float2 waveUV = clamp(flatSnowUV, -4096.0f, 4096.0f) * max(SnowTuning.y, 1.0f) * 0.25f;
				const uint waveSeed = NativePBR::SnowHash(binding.identity ^ uint2(instance, 0u)) ^ 0x27d4eb2fu;
				const float wave = NativePBR::SnowNoise(waveUV, waveSeed) - 0.5f;
				const float boundary = 4.0f * gravity * (1.0f - gravity);
				gravity = saturate(gravity + wave * saturate(SnowDetailControls.y) * boundary);
			}
			gravity = smoothstep(0.1f, 0.9f, gravity);
			surface.projectedSnowIncidence = lerp(1.0f, gravity, saturate(SnowLumps.w));
		}
	}
	if (!all(isfinite(surface.geometricNormal)) ||
		!all(isfinite(surface.tangent)) || !all(isfinite(surface.bitangent)))
		return false;
	NativePBR::SurfaceMaterial result = NativePBR::EvaluateOpaque(material, surface, base, normal, packed, emission,
		coat, complexMaskDetected, binding.flags, SnowTuning, SnowRoughness, SnowUltravioletEmission,
		SnowColorAffection, SnowSpecularAmount, SnowLumps, SnowDetailControls);
	evaluated.baseColor = result.baseColor;
	evaluated.normal = result.normal;
	evaluated.f0 = result.f0;
	evaluated.emission = result.emission;
	evaluated.roughness = result.roughness;
	evaluated.metallic = result.metallic;
	evaluated.ao = result.ao;
	evaluated.opacity = result.opacity;
	evaluated.projectedSnowCoverageCandidate = result.projectedSnowCoverageCandidate;
	evaluated.snowCoverage = result.snowCoverage;
	evaluated.snowSparkle = result.snowSparkle;
	clearcoat = float3(result.coatStrength, result.coatRoughness, result.coatF0);
	return all(isfinite(evaluated.baseColor)) && all(isfinite(evaluated.normal)) &&
		all(isfinite(evaluated.f0)) && all(isfinite(evaluated.emission)) &&
		isfinite(evaluated.roughness) && isfinite(evaluated.metallic) &&
		isfinite(evaluated.ao) && isfinite(evaluated.opacity) &&
		isfinite(evaluated.snowSparkle) && all(isfinite(clearcoat));
}

#endif
