RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> RasterDepth : register(t1);
StructuredBuffer<float4> Lights : register(t2);
#ifdef NATIVE_SHADOW_OFFLOAD
RWTexture2D<float2> RawMask : register(u0);
cbuffer ShadowInputs : register(b0)
{
	row_major float4x4 InverseProjection;
	row_major float4x4 InverseView;
	float4 PositionAdjust;
	uint2 Size;
	uint LightCount;
	uint Padding;
	uint2 PhysicalSize;
};
static const float MinDistance = 0.5f;
static const float MaxDistance = 60000.0f;
static const uint CheckerboardOrigin = 0u;
#define LogicalSize PhysicalSize
static const uint LightStride = 2u;
#else
RWTexture2D<float4> RawMask : register(u0);
static const uint LightStride = 3u;
cbuffer ShadowInputs : register(b0)
{
	row_major float4x4 InverseProjection;
	row_major float4x4 InverseView;
	float4 PositionAdjust;
	uint2 Size;
	uint FrameIndex;
	uint QualityModeUnused;
	uint LightCount;
	float MinDistance;
	float MaxDistance;
	uint Unused;
	float ShadowReachMultiplier;
	float ShadowFadeRange;
	float ShadowFalloffCurve;
	float NearLightRadius;
	uint NearLightExclusion;
	uint StochasticSampling;
	float EmitterRadiusMultiplier;
	uint CheckerboardOrigin;
	uint2 LogicalSize;
	uint2 PhysicalSize;
};
#endif
#include "Common/NativeShadowQuality.hlsli"
#ifdef NATIVE_SHADOW_OFFLOAD
static const uint QualityMode = kShadowOffloadingQuality;
#else
#ifndef NATIVE_SHADOW_QUALITY
#define NATIVE_SHADOW_QUALITY 1
#endif
static const uint QualityMode = NATIVE_SHADOW_QUALITY;
#endif
#ifndef NATIVE_SHADOW_OFFLOAD
#include "NativeShadowSampling.hlsli"

float3 SampleEmitterOffset(float3 lightDirection, float emitterRadius, uint2 pixel, uint lightID)
{
	const uint seed = NativeShadowEmitterSeed(pixel.x, pixel.y, FrameIndex, lightID);
	const float radius = emitterRadius * sqrt(NativeShadowEmitterRandom(seed, ShadowEmitterRadialSalt));
	const float angle = NativeShadowEmitterRandom(seed, ShadowEmitterAngularSalt) * 6.28318530718f;
	const float3 tangent = normalize(cross(abs(lightDirection.z) < 0.9f ? float3(0, 0, 1) : float3(0, 1, 0), lightDirection));
	const float3 bitangent = cross(lightDirection, tangent);
	float sine, cosine;
	sincos(angle, sine, cosine);
	return radius * (cosine * tangent + sine * bitangent);
}

#endif
static const uint kTileThreads = 64u;
static const uint kTileLightWordCount = 128u;
static const uint kTileCullThreshold = 32u;
#ifdef NATIVE_SHADOW_OFFLOAD
groupshared uint TileMinimumOrdered[3];
groupshared uint TileMaximumOrdered[3];
#else
groupshared float3 TileMinimum[kTileThreads];
groupshared float3 TileMaximum[kTileThreads];
#endif
groupshared uint TileLightMask[kTileLightWordCount];

bool ReadReceiver(uint2 packed, out uint2 p, out float3 world, out float receiverDistance)
{
	p = 0;
	world = 0;
	receiverDistance = 0;
	if (any(packed >= PhysicalSize))
		return false;
	const uint2 logical = uint2(NativeShadowLogicalX(packed.x, packed.y, QualityMode, CheckerboardOrigin), packed.y);
	if (any(logical >= LogicalSize))
		return false;
	p = uint2(NativeShadowRenderCoordinate(logical.x, Size.x, QualityMode),
		NativeShadowRenderCoordinate(logical.y, Size.y, QualityMode));
	float z = RasterDepth.Load(int3(p, 0));
	if (!isfinite(z) || z <= 0 || z >= 1)
		return false;
	float2 clip = 2.0f * (float2(p) + 0.5f) / float2(Size) - 1.0f;
	clip.y = -clip.y;
	float4 v = mul(InverseProjection, float4(clip, z, 1));
	if (!all(isfinite(v)) || abs(v.w) < 1e-8f)
		return false;
	float4 w = mul(InverseView, float4(v.xyz / v.w, 1));
	if (!all(isfinite(w)) || abs(w.w) < 1e-8f)
		return false;
	world = w.xyz / w.w + PositionAdjust.xyz;
	receiverDistance = length(mul((float3x3)InverseView, v.xyz / v.w));
	return isfinite(receiverDistance) && receiverDistance > 0.0f;
}

#ifdef NATIVE_SHADOW_OFFLOAD
uint OrderedFloat(float value)
{
	const uint bits = asuint(value);
	return bits ^ ((bits & 0x80000000u) != 0u ? 0xffffffffu : 0x80000000u);
}

float DecodeOrderedFloat(uint value)
{
	return asfloat(value ^ ((value & 0x80000000u) != 0u ? 0x80000000u : 0xffffffffu));
}

void BuildTileLightMask(uint lane, bool valid, float3 world)
{
	const float extreme = 3.402823466e+38f;
	const bool finiteReceiver = valid && all(isfinite(world));
	const uint wordCount = (LightCount + 31u) / 32u;
	if (lane < 3u) {
		TileMinimumOrdered[lane] = OrderedFloat(extreme);
		TileMaximumOrdered[lane] = OrderedFloat(-extreme);
	}
	for (uint word = lane; word < wordCount; word += kTileThreads)
		TileLightMask[word] = 0u;
	GroupMemoryBarrierWithGroupSync();

	const float3 waveLower = WaveActiveMin(finiteReceiver ? world : float3(extreme, extreme, extreme));
	const float3 waveUpper = WaveActiveMax(finiteReceiver ? world : float3(-extreme, -extreme, -extreme));
	if (WaveIsFirstLane()) {
		[unroll] for (uint axis = 0u; axis < 3u; ++axis) {
			InterlockedMin(TileMinimumOrdered[axis], OrderedFloat(waveLower[axis]));
			InterlockedMax(TileMaximumOrdered[axis], OrderedFloat(waveUpper[axis]));
		}
	}
	GroupMemoryBarrierWithGroupSync();

	const float3 lower = float3(DecodeOrderedFloat(TileMinimumOrdered[0]), DecodeOrderedFloat(TileMinimumOrdered[1]), DecodeOrderedFloat(TileMinimumOrdered[2]));
	const float3 upper = float3(DecodeOrderedFloat(TileMaximumOrdered[0]), DecodeOrderedFloat(TileMaximumOrdered[1]), DecodeOrderedFloat(TileMaximumOrdered[2]));
	if (all(lower <= upper)) {
		[loop] for (uint lightIndex = lane; lightIndex < LightCount; lightIndex += kTileThreads) {
			const float4 light = Lights[lightIndex * LightStride];
			const float radius = max(light.w, MinDistance) * 1.00001f;
			const float3 delta = max(max(lower - light.xyz, light.xyz - upper), 0.0f);
			const float distanceSquared = dot(delta, delta);
			if (!isfinite(distanceSquared) || !isfinite(radius) || distanceSquared <= radius * radius)
				InterlockedOr(TileLightMask[lightIndex / 32u], 1u << (lightIndex & 31u));
		}
	}
	GroupMemoryBarrierWithGroupSync();
}
#else
void BuildTileLightMask(uint lane, bool valid, float3 world)
{
	const float extreme = 3.402823466e+38f;
	const bool finiteReceiver = valid && all(isfinite(world));
	TileMinimum[lane] = finiteReceiver ? world : float3(extreme, extreme, extreme);
	TileMaximum[lane] = finiteReceiver ? world : float3(-extreme, -extreme, -extreme);
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = kTileThreads / 2u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride) {
			TileMinimum[lane] = min(TileMinimum[lane], TileMinimum[lane + stride]);
			TileMaximum[lane] = max(TileMaximum[lane], TileMaximum[lane + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	const float3 lower = TileMinimum[0];
	const float3 upper = TileMaximum[0];
	const uint wordCount = (LightCount + 31u) / 32u;
	[loop] for (uint word = lane; word < wordCount; word += kTileThreads)
	{
		uint mask = 0u;
		const uint first = word * 32u;
		const uint last = any(lower > upper) ? first : min(first + 32u, LightCount);
		[loop] for (uint lightIndex = first; lightIndex < last; ++lightIndex)
		{
			const float4 light = Lights[lightIndex * LightStride];
			const float radius = max(light.w * ShadowReachMultiplier, MinDistance);
			const float3 delta = max(max(lower - light.xyz, light.xyz - upper), 0.0f);
			const float distanceSquared = dot(delta, delta);
			const float conservativeRadius = radius * 1.00001f;
			if (!isfinite(distanceSquared) || !isfinite(conservativeRadius) ||
				distanceSquared <= conservativeRadius * conservativeRadius)
				mask |= 1u << (lightIndex - first);
		}
		TileLightMask[word] = mask;
	}
	GroupMemoryBarrierWithGroupSync();
}

#endif
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
	const uint2 packed = id.xy;
	uint2 p;
	float3 world;
	float receiverDistance;
	const bool valid = ReadReceiver(packed, p, world, receiverDistance);
	// All lanes participate before rejecting invalid receivers; large inventories
	// fall back to the complete ordered light loop rather than dropping lights.
	const bool tiled = LightCount >= kTileCullThreshold && LightCount <= kTileLightWordCount * 32u;
	if (tiled)
		BuildTileLightMask(lane, valid, world);
	if (any(packed >= PhysicalSize))
		return;
	if (!valid) {
#ifdef NATIVE_SHADOW_OFFLOAD
		RawMask[packed] = float2(1, 0);
#else
		RawMask[packed] = float4(1, 0, 0, 0);
#endif
		return;
	}
	// Valid receiver with no applicable light: neutral, not an unsampled pixel.
#ifndef NATIVE_SHADOW_OFFLOAD
	RawMask[packed] = float4(1, 0, receiverDistance, 0);
#endif
	float total = 0, visible = 0;
	[loop] for (uint i = 0; i < LightCount; )
	{
		if (tiled) {
			const uint word = i / 32u;
			const uint remaining = TileLightMask[word] & (0xffffffffu << (i & 31u));
			if (remaining == 0u) {
				i = (word + 1u) * 32u;
				continue;
			}
			i = word * 32u + uint(firstbitlow(remaining));
		}
		const uint lightIndex = i++;
		float4 light = Lights[lightIndex * LightStride];     // xyz vanilla position + vanilla radius
		float3 delta = light.xyz - world;
		float d2 = dot(delta, delta);
		if (d2 <= MinDistance * MinDistance || !isfinite(d2))
			continue;
		// Native reach is shadow-only; multiplier 1.0 preserves the light's vanilla radius.
#ifdef NATIVE_SHADOW_OFFLOAD
		const float radius = max(light.w, MinDistance);
#else
		const float radius = max(light.w * ShadowReachMultiplier, MinDistance);
#endif
		if (d2 > radius * radius)
			continue;
		float d = sqrt(d2);
		if (d >= radius)
			continue;
#ifdef NATIVE_SHADOW_OFFLOAD
		const float2 emission = Lights[lightIndex * LightStride + 1u].xy;
#else
		float4 radiance = Lights[lightIndex * LightStride + 1u];
		float4 params = Lights[lightIndex * LightStride + 2u];
#endif
#ifdef NATIVE_SHADOW_OFFLOAD
		const float fraction = kOffloadingFadeRange;
#else
		const float fraction = saturate(ShadowFadeRange);
#endif
		float fade = fraction <= 0.0f ? 1.0f :
		                                saturate((radius - d) / max(radius * fraction, 1e-4f));
		fade = fade * fade * (3.0f - 2.0f * fade);
#ifdef NATIVE_SHADOW_OFFLOAD
		const float gradualFalloff = fade;
#else
		const float gradualFalloff = fade <= 0.0f ? 0.0f :
		                       ShadowFalloffCurve == 1.0f ? fade : pow(fade, max(0.0f, ShadowFalloffCurve));
#endif
#ifdef NATIVE_SHADOW_OFFLOAD
		float weight = emission.x * gradualFalloff / max(d2 + emission.y, 1.0f);
#else
		float strength = dot(max(radiance.rgb, 0), float3(.2126, .7152, .0722));
		float weight = strength * max(0, radiance.a) * gradualFalloff /
		               max(d2 + params.x * params.x, 1);
#endif
		if (weight <= 0 || !isfinite(weight))
			continue;

		// One visibility ray per contributing light; only its endpoint changes.
		float3 rayDirection = delta / d;
		float rayDistance = d;
		float projection = d;
		float perpendicularSquared = 0.0f;
#ifndef NATIVE_SHADOW_OFFLOAD
		if (StochasticSampling != 0u && EmitterRadiusMultiplier > 0.0f) {
			const float emitterRadius = NativeShadowEmitterRadius(light.w, params.x, EmitterRadiusMultiplier, d);
			if (emitterRadius > 0.0f) {
				const float3 sampledDelta = delta + SampleEmitterOffset(rayDirection, emitterRadius, p, asuint(params.w));
				rayDistance = length(sampledDelta);
				rayDirection = sampledDelta / rayDistance;
				projection = dot(delta, rayDirection);
				const float3 perpendicular = cross(delta, rayDirection);
				perpendicularSquared = dot(perpendicular, perpendicular);
			}
		}

#endif
		float tMax = min(rayDistance - MinDistance, MaxDistance);
#ifdef NATIVE_SHADOW_OFFLOAD
		tMax = min(tMax, d - kOffloadingNearRadius - MinDistance);
#else
		if (NearLightExclusion != 0u && NearLightRadius > 0.0f) {
			const float discriminant = NearLightRadius * NearLightRadius - perpendicularSquared;
			if (discriminant >= 0.0f)
				tMax = min(tMax, projection - sqrt(discriminant) - MinDistance);
		}
#endif
		float visibility = 1.0f;
		if (tMax > MinDistance) {
			RayDesc ray;
			ray.Origin = world + rayDirection * MinDistance;
			ray.Direction = rayDirection;
			ray.TMin = MinDistance;
			ray.TMax = tMax;
			RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
			query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
			while (query.Proceed()) {}
			// Fade the occlusion itself, not just its light weight.
			if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
				visibility = 1.0f - gradualFalloff;
		}
		total += weight;
		visible += weight * visibility;
	}
#ifdef NATIVE_SHADOW_OFFLOAD
	RawMask[packed] = float2(total > 1e-8f ? saturate(visible / total) : 1.0f, receiverDistance);
#else
	if (total > 1e-8f)
		RawMask[packed] = float4(saturate(visible / total), 1, receiverDistance, total);
#endif
}
