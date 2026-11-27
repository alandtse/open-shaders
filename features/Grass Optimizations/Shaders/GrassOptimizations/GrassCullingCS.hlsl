#include "Common/GrassInstance.hlsli"
#include "Common/GrassWindResponse.hlsli"
#include "Common/ReverseZ.hlsli"

#ifdef GRASS_COLLISION
#	include "GrassCollision/GrassCollisionField.hlsli"
#endif
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

cbuffer CullParams : register(b0)
{
	float4 FrustumPlanes[12];

	uint EyeCount;
	float3 _padEye;

	// PerFrame uses the light's camera during shadow passes, so retain the main camera's projection here.
	float4 CameraViewProj[8];
	float4 CameraPos[2];

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
	float ReverseProjection;
	float MidLODPixelSize;

	float MeshLODBandPx;
	float HiZEnabled;
	float2 HiZSize;

	float2 ProjectionWLength;
	float HiZMipCount;
	// Separation behind an occluder as a fraction of its view depth.
	float OcclusionDepthBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	// Precomputed sqrt(MaxDistSq) for edge fading.
	float MaxDist;
	// Values below 1 shrink the occlusion sphere for more aggressive culling.
	float OcclusionTightness;
	// Perspective depth is NDCDepthAtInfinity + NDCDepthNumerator / w.
	float NDCDepthAtInfinity;

	float NDCDepthNumerator;
	float2 _pad0;
};

// CullBucketCB fields followed by the dispatch range.
struct CullBucket
{
	uint GroupCount;
	uint SliceTableOffset;
	uint SliceCount;
	uint GroupBase;
	uint TierStride;
	uint OutputOffset;
	uint ArgsOffset;
	float OcclusionRadius;
	float BoundExtent;
	float ModelRadius;
	float DistScale;
	float MinPixelScale;
	float MidLODEnabled;
	float FarLODEnabled;
	float ComplexThreshold;
	float MidLODMeshCost;
	float WavePeriod;
	float TimeBase;
	float PrevTimeBase;
	float FarLODMeshCost;
	float3 CanopyHeights;
	// Lowest alpha threshold among depth-writing draws.
	float AlphaTestRef;
	uint2 _entryPad0;
	// Buckets are sorted by DispatchStart for lookup.
	uint DispatchStart;
	uint _entryPad1;
};
StructuredBuffer<CullBucket> Buckets : register(t4);

cbuffer DispatchParams : register(b1)
{
	uint BucketCount;
	uint DispatchGroupCount;
	uint DispatchRowWidth;
	uint _dispatchPad;
};

// All threads in a group share one bucket.
static CullBucket Bucket;

ByteAddressBuffer Instances : register(t0);
StructuredBuffer<GrassInstance::CullGroup> Groups : register(t1);
Texture2D<float> HiZ : register(t2);
// x and y are the run's first source and dispatch group indices.
StructuredBuffer<uint2> SliceTable : register(t3);

static const uint TierCount = 3;
static const uint ArgsStride = 20;
static const uint TierCulled = TierCount;

// Each tier's DrawIndexedInstanced arguments store the instance count at byte offset 4.
RWByteAddressBuffer Args : register(u0);
// RunGrass resolves each output record against its source instance and group.
RWStructuredBuffer<GrassInstance::CullRecord> Compacted[TierCount] : register(u1);

// Aggregate survivors within each group to issue one global atomic per occupied tier.
groupshared uint GroupTierCount[TierCount * 2];
groupshared uint GroupTierBase[TierCount * 2];

float RandFloat(uint bits)
{
	const uint mantissaMask = 0x007FFFFFu;
	const uint one = 0x3F800000u;

	bits &= mantissaMask;
	bits |= one;

	return asfloat(bits) - 1.0;
}

float4 ProjectDirection(float3 v, uint eyeIndex)
{
	return float4(dot(CameraViewProj[eyeIndex * 4 + 0].xyz, v), dot(CameraViewProj[eyeIndex * 4 + 1].xyz, v), dot(CameraViewProj[eyeIndex * 4 + 2].xyz, v), dot(CameraViewProj[eyeIndex * 4 + 3].xyz, v));
}

// Invalid clip W must not occlude grass.
float HiZViewDepth(float ndcDepth)
{
	const float w = NDCDepthNumerator / (ndcDepth - NDCDepthAtInfinity);
	return w > 0.0 ? w : 3.402823466e+38;
}

// Eyes share one side-by-side pyramid, so each eye samples only its half.
void HiZEyeRange(int2 dim, uint eyeIndex, out int xMin, out int xMax)
{
	const int eyeHalf = dim.x >> 1;
	xMin = EyeCount > 1 && eyeHalf > 0 ? (int)eyeIndex * eyeHalf : 0;
	xMax = EyeCount > 1 && eyeHalf > 0 ? xMin + eyeHalf - 1 : dim.x - 1;
}

bool OccludedSphere(float3 centerDV, float radius, uint eyeIndex)
{
	const float4 clipOffset = float4(CameraViewProj[eyeIndex * 4 + 0].w, CameraViewProj[eyeIndex * 4 + 1].w, CameraViewProj[eyeIndex * 4 + 2].w, CameraViewProj[eyeIndex * 4 + 3].w);
	const float4 clipC = ProjectDirection(centerDV, eyeIndex) + clipOffset;
	const float minW = clipC.w - radius * ProjectionWLength[eyeIndex];

	// Keep camera-plane intersections visible. [branch] causes fxc X4000 here.
	if (minW <= 1e-4)
		return false;

	const float3 ndc = clipC.xyz / clipC.w;
	// Bound projected extents using the projection gradient and nearest positive W.
	const float gradientX = length(CameraViewProj[eyeIndex * 4 + 0].xyz - ndc.x * CameraViewProj[eyeIndex * 4 + 3].xyz);
	const float gradientY = length(CameraViewProj[eyeIndex * 4 + 1].xyz - ndc.y * CameraViewProj[eyeIndex * 4 + 3].xyz);
	const float gradientZ = length(CameraViewProj[eyeIndex * 4 + 2].xyz - ndc.z * CameraViewProj[eyeIndex * 4 + 3].xyz);

	const float3 extent = (radius / minW) * float3(gradientX, gradientY, gradientZ);
	const float3 ndcLo = ndc - extent - 1e-5;
	const float3 ndcHi = ndc + extent + 1e-5;

	// The near plane is NDC 0 in standard depth and 1 in reverse depth.
	[branch] if (ReverseProjection > 0.5 ? ndcHi.z >= 1.0 : ndcLo.z <= 0.0) return false;

	float2 uvLo = saturate(float2(ndcLo.x, -ndcHi.y) * 0.5 + 0.5);
	float2 uvHi = saturate(float2(ndcHi.x, -ndcLo.y) * 0.5 + 0.5);
	if (EyeCount > 1) {
		uvLo.x = (uvLo.x + (float)eyeIndex) * 0.5;
		uvHi.x = (uvHi.x + (float)eyeIndex) * 0.5;
	}
	const float2 tcLo = uvLo * HiZSize;
	const float2 tcHi = uvHi * HiZSize;

	// This mip covers the footprint with at most 2x2 texels.
	const float wantLevel = ceil(log2(max(max(tcHi.x - tcLo.x, tcHi.y - tcLo.y), 1.0)));

	[branch] if (wantLevel > HiZMipCount - 1.0) return false;

	const float scale = exp2(wantLevel);
	const int level = (int)wantLevel;
	const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));
	int xMin, xMax;
	HiZEyeRange(dimL, eyeIndex, xMin, xMax);
	const int2 lo = clamp(int2(floor(tcLo / scale)), int2(xMin, 0), int2(xMax, dimL.y - 1));
	const int2 hi = clamp(int2(floor(tcHi / scale)), int2(xMin, 0), int2(xMax, dimL.y - 1));
	const float tileFarthest = FrameBuffer::FartherDepth(FrameBuffer::FartherDepth(HiZ.Load(int3(lo, level)), HiZ.Load(int3(hi.x, lo.y, level))),
		FrameBuffer::FartherDepth(HiZ.Load(int3(lo.x, hi.y, level)), HiZ.Load(int3(hi, level))));

	// View-space bias stays proportional to occluder depth.
	if (minW > HiZViewDepth(tileFarthest) * (1.0 + OcclusionDepthBias))
		return true;

	// Refining by two mips covers at most 5x5 texels and requires the depth gradient to increase with distance.
	[branch] if (wantLevel < 2.0 || (ReverseProjection > 0.5 ? NDCDepthNumerator <= 0.0 : NDCDepthNumerator >= 0.0)) return false;

	// Compare in NDC to avoid a division for each texel.
	const float ndcLimit = NDCDepthAtInfinity + NDCDepthNumerator * (1.0 + OcclusionDepthBias) / minW;
	const int fineLevel = level - 2;
	const float fineScale = scale * 0.25;

	const int2 dimF = max(int2(ceil(HiZSize / fineScale)), int2(1, 1));
	int fineXMin, fineXMax;
	HiZEyeRange(dimF, eyeIndex, fineXMin, fineXMax);
	const int2 fineLo = clamp(int2(floor(tcLo / fineScale)), int2(fineXMin, 0), int2(fineXMax, dimF.y - 1));
	const int2 fineHi = clamp(int2(floor(tcHi / fineScale)), int2(fineXMin, 0), int2(fineXMax, dimF.y - 1));

	[loop] for (int y = fineLo.y; y <= fineHi.y; ++y)
	{
		[loop] for (int x = fineLo.x; x <= fineHi.x; ++x)
		{
			if (!FrameBuffer::IsNearerDepth(HiZ.Load(int3(x, y, fineLevel)), ndcLimit))
				return false;
		}
	}
	return true;
}

// Return the LOD tier or TierCulled and populate the output record for surviving instances.
uint CullInstance(float4 data1, float4 data2, float4 data3, float4 data4, GrassInstance::CullGroup group, uint eyeIndex, out float fade, out uint flags)
{
	fade = 0.0;
	flags = 0;

	const float3 world = data1.xyz + group.Origin;

	const float3 dv = world - CameraPos[eyeIndex].xyz;
	const float distSq = dot(dv, dv);
	if (distSq > MaxDistSq)
		return TierCulled;

	// Frustum bounds include the mesh offset, scaling and deformation.
	const float sizeVariance = data4.y;
	const float boundsRadius = Bucket.BoundExtent * max(1.0, abs(1.0 + sizeVariance)) + 128.0;

	[unroll] for (uint p = 0; p < 6; ++p)
	{
		if (dot(FrustumPlanes[eyeIndex * 6 + p].xyz, EyeCount > 1 ? dv : world) - FrustumPlanes[eyeIndex * 6 + p].w < -boundsRadius)
			return TierCulled;
	}

	const float dist = sqrt(distSq);

	// Conservatively bound the vertex shader's size variation.
	const float instanceRadius = Bucket.ModelRadius * (1.0 + max(sizeVariance, 0.0));

	// Use camera depth so pixel sizing stays consistent across the view.
	const float clipW = dot(CameraViewProj[eyeIndex * 4 + 3].xyz, dv) + CameraViewProj[eyeIndex * 4 + 3].w;
	const float projPx = (instanceRadius * ProjectionWLength[eyeIndex] / max(clipW, 1e-4)) * ProjScale;

	// Seed from world position so compaction cannot change density or LOD choices.
	const uint2 rand = Random::pcg2d(asuint(world.xy) ^ asuint(world.z));

	const float h = RandFloat(rand.y);
	const float halfBand = MeshLODBandPx * 0.5;
	const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);

	uint tier = 0;
	if (Bucket.MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 1;
	if (Bucket.FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 2;

	const float meshCost = tier == 1 ? Bucket.MidLODMeshCost : (tier == 2 ? Bucket.FarLODMeshCost : Bucket.MinPixelScale);
	const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));

	const float effCostBias = MeshCostBias * costRamp;
	const float dScale = lerp(1.0, tier == 0 ? Bucket.DistScale : rcp(meshCost), effCostBias);
	const float effMaxDistSq = MaxDistSq * dScale * dScale;

	if (distSq > effMaxDistSq)
		return TierCulled;

	const float pxScale = lerp(1.0, meshCost, effCostBias);
	const float effMinPx = MinPixelSize * pxScale;

	if (projPx < effMinPx)
		return TierCulled;

	float lodFade = 1.0;
	const float effFullPx = FullDetailPixelSize * pxScale;

	if (projPx < effFullPx) {
		const float ratio = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
		const float keep = lerp(1.0, LODMinKeep, ratio);
		const float densityRandom = RandFloat(rand.x);

		// Scale the fade band with density to avoid retaining too many fading instances.
		const float cutoff = keep * (1.0 + LODFadeBand);
		if (densityRandom > cutoff)
			return TierCulled;

		lodFade = saturate((cutoff - densityRandom) / max(keep * LODFadeBand, 1e-4));
	}

	const float maxDist = MaxDist * abs(dScale);
	const float edgeStart = maxDist * EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

	const float4 clipOffset = float4(CameraViewProj[eyeIndex * 4 + 0].w, CameraViewProj[eyeIndex * 4 + 1].w, CameraViewProj[eyeIndex * 4 + 2].w, CameraViewProj[eyeIndex * 4 + 3].w);
	const float4 clip = ProjectDirection(dv, eyeIndex) + clipOffset;
	const float distFade = 1.0 - saturate((length(FrameBuffer::ToStandardClip(clip, ReverseProjection > 0.5)) - AlphaParam1) / AlphaParam2);
	const float spawnFade = saturate((FadeNow - group.FadeStart) * FadeInTimeRcp);

	fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= InvisibleFadeCull)
		return TierCulled;

	// Match the vertex shader's 9-bit fade and allow for interpolation rounding.
	const float storedFade = round(saturate(fade) * GrassInstance::RecordFadeMask) / GrassInstance::RecordFadeMask;
	if (storedFade < Bucket.AlphaTestRef - 1e-3)
		return TierCulled;

	// Occlusion uses the canopy sphere without frustum deformation padding.
	[branch] if (HiZEnabled > 0.5)
	{
		const float canopyMid = 0.5 * (tier == 0 ? Bucket.CanopyHeights.x : (tier == 1 ? Bucket.CanopyHeights.y : Bucket.CanopyHeights.z));
		const float3 canopyOffset = canopyMid * (1.0 + sizeVariance) * float3(data2.z, data3.z, data3.w);

		const float3 occCenter = dv + canopyOffset;
		const float occRadius = Bucket.OcclusionRadius * max(1.0, abs(1.0 + sizeVariance));

		if (OccludedSphere(occCenter, occRadius * OcclusionTightness, eyeIndex))
			return TierCulled;
	}

	flags = tier << GrassInstance::FlagTierShift;
	if (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize)
		flags |= GrassInstance::FlagSimpleShading;

	return tier;
}

[numthreads(64, 1, 1)] void main(uint3 groupId : SV_GroupID, uint threadIdx : SV_GroupIndex) {
	if (threadIdx < TierCount * EyeCount)
		GroupTierCount[threadIdx] = 0;
	GroupMemoryBarrierWithGroupSync();

	uint tier[2] = { TierCulled, TierCulled };
	float fade[2] = { 0.0, 0.0 };
	uint flags[2] = { 0, 0 };
	uint groupIndex = 0;
	uint sourceIndex = 0;
	float4 data1 = 0.0, data2, data3, data4;
	GrassInstance::CullGroup group = (GrassInstance::CullGroup)0;
	const uint dispatchIdx = groupId.y * DispatchRowWidth + groupId.x;
	bool activeGroup = dispatchIdx < DispatchGroupCount;
	uint groupIdx = 0;

	if (activeGroup) {
		uint first = 0;
		uint last = BucketCount - 1;
		[loop] while (first < last)
		{
			const uint mid = (first + last + 1) >> 1;
			if (Buckets[mid].DispatchStart <= dispatchIdx)
				first = mid;
			else
				last = mid - 1;
		}

		Bucket = Buckets[first];
		groupIdx = dispatchIdx - Bucket.DispatchStart;
		activeGroup = groupIdx < Bucket.GroupCount;
	}

	if (activeGroup) {
		uint lo = 0;
		uint hi = Bucket.SliceCount - 1;
		[loop] while (lo < hi)
		{
			const uint mid = (lo + hi + 1) >> 1;
			if (SliceTable[Bucket.SliceTableOffset + mid].y <= groupIdx)
				lo = mid;
			else
				hi = mid - 1;
		}

		const uint2 run = SliceTable[Bucket.SliceTableOffset + lo];
		groupIndex = run.x + (groupIdx - run.y);
		group = Groups[Bucket.GroupBase + groupIndex];
		if (threadIdx < group.Count) {
			sourceIndex = group.SourceStart + threadIdx;
			GrassInstance::Decode(Instances.Load4(sourceIndex * GrassInstance::Stride), data1, data2, data3, data4);
			[unroll] for (uint cullEye = 0; cullEye < 2; ++cullEye)
			{
				if (cullEye < EyeCount)
					tier[cullEye] = CullInstance(data1, data2, data3, data4, group, cullEye, fade[cullEye], flags[cullEye]);
			}
		}
	}

	uint localSlot[2] = { 0, 0 };
	[unroll] for (uint countEye = 0; countEye < 2; ++countEye)
	{
		if (countEye < EyeCount && tier[countEye] != TierCulled)
			InterlockedAdd(GroupTierCount[countEye * TierCount + tier[countEye]], 1, localSlot[countEye]);
	}

	GroupMemoryBarrierWithGroupSync();
	if (threadIdx < TierCount * EyeCount && GroupTierCount[threadIdx] != 0) {
		uint base;
		Args.InterlockedAdd(Bucket.ArgsOffset + threadIdx * ArgsStride + 4, GroupTierCount[threadIdx], base);
		GroupTierBase[threadIdx] = base;
	}
	GroupMemoryBarrierWithGroupSync();
	if (tier[0] == TierCulled && tier[1] == TierCulled)
		return;

	const float3 world = data1.xyz + group.Origin;
	float4 currentResponse, previousResponse;
	float2 flutter;
	GrassWindResponse::Sample(data1.xy, world.xy, world.xy, Math::IdentityMatrix, Math::IdentityMatrix,
		Bucket.TimeBase * Bucket.WavePeriod, Bucket.PrevTimeBase * Bucket.WavePeriod, currentResponse, previousResponse, flutter);
	float4 currentCollision = 0.0;
	float4 previousCollision = 0.0;
#ifdef GRASS_COLLISION
	const float3 currentRoot = world - FrameBuffer::CameraPosAdjust[0].xyz;
	const float3 previousRoot = world - FrameBuffer::CameraPreviousPosAdjust[0].xyz;
	currentCollision = GrassCollision::SampleCurrentDeformation(currentRoot.xy);
	previousCollision = GrassCollision::SamplePreviousDeformation(previousRoot.xy);
	currentCollision.w = smoothstep(GrassCollision::FADE_DISTANCE, 0.0, length(currentRoot));
	previousCollision.w = smoothstep(GrassCollision::FADE_DISTANCE, 0.0, length(previousRoot));
#endif
	[unroll] for (uint eye = 0; eye < 2; ++eye)
	{
		if (eye >= EyeCount || tier[eye] == TierCulled)
			continue;
		const uint slot = Bucket.OutputOffset + eye * Bucket.TierStride + GroupTierBase[eye * TierCount + tier[eye]] + localSlot[eye];
		GrassInstance::CullRecord record = { sourceIndex, GrassInstance::PackRecord(groupIndex, fade[eye], flags[eye]), flutter,
			currentResponse, previousResponse, currentCollision, previousCollision };
		[unroll] for (uint outTier = 0; outTier < TierCount; ++outTier)
		{
			if (tier[eye] == outTier)
				Compacted[outTier][slot] = record;
		}
	}
}
