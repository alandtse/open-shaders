#include "Common/FrameBuffer.hlsli"
#include "Common/GrassWindResponse.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

#ifdef GRASS_COLLISION
#	include "GrassCollision/GrassCollisionField.hlsli"
#endif

cbuffer CullParams : register(b0)
{
	// [0..5] = eye 0's planes; [6..11] = eye 1's. VR planes use camera-relative coordinates.
	float4 FrustumPlanes[12];

	uint EyeCount;
	float3 _padEye;

	float MinPixelSize;
	float FullDetailPixelSize;
	float LODMinKeep;
	float LODFadeBand;

	float MeshCostBias;
	float ProjScale;
	float MaxDistSq;
	float EdgeFadeStart;

	float AlphaParam1;
	float AlphaParam2;
	float FadeNow;
	float FadeInTimeRcp;

	float InvisibleFadeCull;
	float SimpleShadingPixelSize;
	float HiZProjScale;
	float MidLODPixelSize;

	float MeshLODBandPx;
	float HiZEnabled;
	float2 HiZSize;

	float HiZTexelPixels;
	float HiZMipCount;
	float OcclusionBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	float3 _pad0;
};

cbuffer CullBucket : register(b1)
{
	uint InstanceCount;
	float WavePeriod;
	float TimeBase;
	float PrevTimeBase;
	float3 BoundCenter;
	float ModelRadius;
	float DistScale;
	float MinPixelScale;
	float IsComplex;
	float MidLODEnabled;
	// The dispatch covers only these slices' combined instance count.
	uint SliceTableOffset;
	uint SliceCount;
	float FarLODEnabled;
	// Per-eye slot capacity of the output buffers below; eye 1's survivors land at this offset.
	uint OutputCapacityPerEye;
};

ByteAddressBuffer Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);
// Max-depth reduction of the scene depth copy (see GrassHiZCS.hlsl).
Texture2D<float> HiZ : register(t2);
// Maps a compacted thread index back to a real instance: .x = the slice's first instance, .y = the instance total of every visible slice before it.
StructuredBuffer<uint2> SliceTable : register(t3);

RWByteAddressBuffer Compacted : register(u0);
RWStructuredBuffer<float4> Extras : register(u1);
RWByteAddressBuffer Counter : register(u2);

RWByteAddressBuffer MidLODCompacted : register(u3);
RWStructuredBuffer<float4> MidLODExtras : register(u4);

RWByteAddressBuffer FarLODCompacted : register(u5);
RWStructuredBuffer<float4> FarLODExtras : register(u6);
// Mid and Far LOD survivor counts share one UAV so the shader stays within D3D11's eight-UAV limit.
// Packed as 4 bytes per (eye, tier): eye 1's slots sit kLODCounterEyeStride bytes past eye 0's.
RWByteAddressBuffer LODCounters : register(u7);

static const uint kLODCounterEyeStride = 8;
static const uint MiddleLODCountOffset = 0;
static const uint FarLODCountOffset = 4;

// Uses a uint hash to generate a random float between [0, 1)
float RandFloat(uint bits)
{
	const uint mantissaMask = 0x007FFFFFu;
	const uint one = 0x3F800000u;

	bits &= mantissaMask;
	bits |= one;

	return asfloat(bits) - 1.0;
}

bool CullEye(uint eyeIndex, float3 world, float4 og, uint4 raw0, uint4 raw1, uint2 rand,
	float sizeVariance, out float fade, out float flags, out uint tier)
{
	fade = 0.0;
	flags = 0.0;
	tier = 0u;
	const float3 dv = world - FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	const float distSq = dot(dv, dv);

	const float dist = sqrt(distSq);

	// Mesh complexity only biases culling past CostBiasStartDist, ramping to full over the distance past that
	const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));
	const float effCostBias = MeshCostBias * costRamp;

	const float dScale = lerp(1.0, DistScale, effCostBias);
	const float effMaxDistSq = MaxDistSq * dScale * dScale;
	if (distSq > effMaxDistSq)
		return false;

	const float instanceRadius = ModelRadius * (1.0 + max(sizeVariance, 0.0));
#if defined(VR)
	// The VR planes are camera-relative and conservatively test the full instance bound.
	const float frustumRadius = instanceRadius + length(BoundCenter) * (1.0 + max(sizeVariance, 0.0));
#endif
	[unroll] for (uint p = 0; p < 6; ++p)
	{
		const float4 plane = FrustumPlanes[eyeIndex * 6 + p];
#if defined(VR)
		if (dot(plane.xyz, dv) - plane.w < -frustumRadius)
			return false;
#else
		if (dot(plane.xyz, world) - plane.w < 0.0)
			return false;
#endif
	}

	const float projPx = (instanceRadius / dist) * ProjScale;
	const float pxScale = lerp(1.0, MinPixelScale, effCostBias);
	const float effMinPx = MinPixelSize * pxScale;
	if (projPx < effMinPx)
		return false;

	if (HiZEnabled > 0.5) {
		// ModelRadius bounds about BoundCenter, not the instance root, so the sphere has to be there to be accurately occluded.
		const float3 rot0 = float3(f16tof32(raw0.z & 0xFFFF), f16tof32(raw0.z >> 16), f16tof32(raw0.w & 0xFFFF));
		const float3 rot1 = float3(f16tof32(raw1.x & 0xFFFF), f16tof32(raw1.x >> 16), f16tof32(raw1.y & 0xFFFF));
		const float3 rot2 = float3(f16tof32(raw1.z & 0xFFFF), f16tof32(raw0.w >> 16), f16tof32(raw1.y >> 16));

		const float3 msCentre = BoundCenter * (1.0 + max(sizeVariance, 0.0));
		const float3 dvC = dv + float3(dot(rot0, msCentre), dot(rot1, msCentre), dot(rot2, msCentre));

		const float occRadius = instanceRadius + length(BoundCenter) * abs(sizeVariance);
		const float distC = max(length(dvC), 1e-4);
		const float projPxOcc = (occRadius / distC) * HiZProjScale;

		const float4 clipC = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(dvC, 1.0));
		if (clipC.w > 0.0) {
			float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
			// Same UV split as Stereo::ConvertToStereoUV -- keep in sync if that changes.
			if (EyeCount > 1)
				uv.x = (uv.x + (float)eyeIndex) * 0.5;
			const float2 tc = uv * HiZSize;
			const float rT = projPxOcc / HiZTexelPixels;  // occlusion radius expressed in level-0 texels

			// The level where the instance spans ~2 texels, so the 3x3 below covers it exactly.
			const float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));

			// A level too fine to cover the instance would underestimate the max and cull visible grass.
			[branch] if (wantLevel <= HiZMipCount - 1.0)
			{
				const int level = (int)wantLevel;
				const float scale = exp2((float)level);
				const float2 tcL = tc / scale;
				const float rTL = rT / scale;
				const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));
				// Clamp the footprint to the active eye's half so taps near the stereo seam don't sample the other eye.
				const int eyeHalf = dimL.x / 2;
				const int xMin = (EyeCount > 1 && eyeHalf > 0) ? (int(eyeIndex) * eyeHalf) : 0;
				const int xMax = (EyeCount > 1 && eyeHalf > 0) ? (xMin + eyeHalf - 1) : (dimL.x - 1);

				const int2 t0 = int2(floor(tcL - rTL));
				const int2 t1 = int2(floor(tcL + rTL));
				// An instance hides only once even its nearest point is behind the occluder. A camera inside
				// the sphere collapses dvC, putting nearZ below any tile depth so the test never fires.
				const float3 dvNear = dvC * (max(distC - occRadius, 0.0) / distC);
				const float4 clipN = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(dvNear, 1.0));
				const float nearZ = clipN.z / max(clipN.w, 1e-4);

				float tileMax = 0.0;
				[unroll] for (int y = 0; y < 3; ++y)
				{
					[unroll] for (int x = 0; x < 3; ++x)
					{
						if (t0.x + x <= t1.x && t0.y + y <= t1.y) {
							const int2 t = clamp(t0 + int2(x, y), int2(xMin, 0), int2(xMax, dimL.y - 1));
							tileMax = max(tileMax, HiZ.Load(int3(t, level)));
						}
					}
				}

				// Behind the farthest occluder of every covering tile means hidden. The tolerance absorbs
				// projection and depth error that would otherwise drop instances only marginally behind it.
				if (nearZ > tileMax + OcclusionBias)
					return false;
			}
		}
	}

	float lodFade = 1.0;
	const float effFullPx = FullDetailPixelSize * pxScale;
	if (projPx < effFullPx) {
		const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
		const float keep = lerp(1.0, LODMinKeep, t);
		const float h = RandFloat(rand.x);
		if (h > keep + LODFadeBand)
			return false;
		lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
	}

	const float maxDist = sqrt(effMaxDistSq);
	const float edgeStart = maxDist * EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

	const float4 clip = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(dv, 1.0));
	const float distFade = 1.0 - saturate((length(clip.xyz) - AlphaParam1) / AlphaParam2);
	const float spawnFade = saturate((FadeNow - og.w) * FadeInTimeRcp);

	fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= InvisibleFadeCull)
		return false;

	flags = (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize) ? 2.0 : 0.0;

	// Both thresholds use the same hash so an instance crosses middle then far in order.
	const float h = RandFloat(rand.y);
	const float halfBand = MeshLODBandPx * 0.5;
	const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);
	if (MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 1u;
	if (FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 2u;

	return true;
}

void StoreSurvivor(uint eyeIndex, uint tier, uint4 raw0, uint4 raw1, float4 e0, float4 e1,
	float4 currentResponse, float4 previousResponse, float4 currentCollision, float4 previousCollision)
{
	// Scaled by 4 so it clears the far-shading flag already packed into e1.w.
	const float4 e1Tier = float4(e1.xyz, e1.w + 4.0 * (float)tier);

	// eyeSlotBase must match the StartInstanceLocation baked into eye 1's args block: SV_InstanceID
	// excludes it, so the SRV index needs it added explicitly.
	static const uint kArgsBlockStride = 32;
	const uint eyeByteOffset = eyeIndex * kArgsBlockStride;
	const uint eyeSlotBase = eyeIndex * OutputCapacityPerEye;
	const uint lodCounterEyeOffset = eyeIndex * kLODCounterEyeStride;

	uint slot;
	if (tier == 2) {
		LODCounters.InterlockedAdd(lodCounterEyeOffset + FarLODCountOffset, 1, slot);
		slot += eyeSlotBase;
		FarLODCompacted.Store4(slot * 32, raw0);
		FarLODCompacted.Store4(slot * 32 + 16, raw1);
		FarLODExtras[slot * 6 + 0] = e0;
		FarLODExtras[slot * 6 + 1] = e1Tier;
		FarLODExtras[slot * 6 + 2] = currentResponse;
		FarLODExtras[slot * 6 + 3] = previousResponse;
		FarLODExtras[slot * 6 + 4] = currentCollision;
		FarLODExtras[slot * 6 + 5] = previousCollision;
	} else if (tier == 1) {
		LODCounters.InterlockedAdd(lodCounterEyeOffset + MiddleLODCountOffset, 1, slot);
		slot += eyeSlotBase;
		MidLODCompacted.Store4(slot * 32, raw0);
		MidLODCompacted.Store4(slot * 32 + 16, raw1);
		MidLODExtras[slot * 6 + 0] = e0;
		MidLODExtras[slot * 6 + 1] = e1Tier;
		MidLODExtras[slot * 6 + 2] = currentResponse;
		MidLODExtras[slot * 6 + 3] = previousResponse;
		MidLODExtras[slot * 6 + 4] = currentCollision;
		MidLODExtras[slot * 6 + 5] = previousCollision;
	} else {
		Counter.InterlockedAdd(eyeByteOffset, 1, slot);
		slot += eyeSlotBase;
		Compacted.Store4(slot * 32, raw0);
		Compacted.Store4(slot * 32 + 16, raw1);
		Extras[slot * 6 + 0] = e0;
		Extras[slot * 6 + 1] = e1;
		Extras[slot * 6 + 2] = currentResponse;
		Extras[slot * 6 + 3] = previousResponse;
		Extras[slot * 6 + 4] = currentCollision;
		Extras[slot * 6 + 5] = previousCollision;
	}
}

[numthreads(64, 1, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	const uint compactIdx = tid.x;
	if (compactIdx >= InstanceCount || SliceCount == 0)
		return;

	// Find the source slice containing this compacted index.
	uint lo = 0;
	uint hi = SliceCount - 1;
	[loop] while (lo < hi)
	{
		const uint mid = (lo + hi + 1) >> 1;
		if (SliceTable[SliceTableOffset + mid].y <= compactIdx)
			lo = mid;
		else
			hi = mid - 1;
	}
	const uint2 slice = SliceTable[SliceTableOffset + lo];

	// Use the source index to keep dither decisions stable across slice changes.
	const uint idx = slice.x + (compactIdx - slice.y);

	// Generate independent density and LOD dither values with one hash.
	const uint2 rand = Random::pcg2d(uint2(idx, 0u));

	const uint base = idx * 32;
	const uint4 raw0 = Instances.Load4(base);
	const uint4 raw1 = Instances.Load4(base + 16);

	const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
	const float localZ = f16tof32(raw0.y & 0xFFFF);

	const float4 og = Origins[idx];
	const float3 world = float3(localXY, localZ) + og.xyz;

	const float sizeVariance = f16tof32(raw1.z >> 16);
	const float4 e0 = float4(og.xyz, IsComplex);

	float fade0, flags0;
	uint tier0;
	const bool visible0 = CullEye(0u, world, og, raw0, raw1, rand, sizeVariance, fade0, flags0, tier0);
#if defined(VR)
	float fade1 = 0.0, flags1 = 0.0;
	uint tier1 = 0u;
	const bool visible1 = CullEye(1u, world, og, raw0, raw1, rand, sizeVariance, fade1, flags1, tier1);

	if (!visible0 && !visible1)
		return;
#else
	if (!visible0)
		return;
#endif

	// Both eyes share the wind and collision response while retaining independent visibility and draw records.
	float4 currentResponse, previousResponse;
	float2 flutter;
	GrassWindResponse::Sample(localXY, world.xy, world.xy, Math::IdentityMatrix, Math::IdentityMatrix,
		TimeBase * WavePeriod, PrevTimeBase * WavePeriod, currentResponse, previousResponse, flutter);
	float4 currentCollision = 0.0;
	float4 previousCollision = 0.0;
#ifdef GRASS_COLLISION
	const float3 currentCollisionRoot = world - FrameBuffer::CameraPosAdjust[0].xyz;
	const float3 previousRoot = world - FrameBuffer::CameraPreviousPosAdjust[0].xyz;
	currentCollision = GrassCollision::SampleCurrentDeformation(currentCollisionRoot.xy);
	previousCollision = GrassCollision::SamplePreviousDeformation(previousRoot.xy);
	currentCollision.w = smoothstep(GrassCollision::FADE_DISTANCE, 0.0, length(currentCollisionRoot));
	previousCollision.w = smoothstep(GrassCollision::FADE_DISTANCE, 0.0, length(previousRoot));
#endif

	if (visible0)
		StoreSurvivor(0u, tier0, raw0, raw1, e0, float4(flutter, fade0, flags0),
			currentResponse, previousResponse, currentCollision, previousCollision);
#if defined(VR)
	if (visible1)
		StoreSurvivor(1u, tier1, raw0, raw1, e0, float4(flutter, fade1, flags1),
			currentResponse, previousResponse, currentCollision, previousCollision);
#endif
}
