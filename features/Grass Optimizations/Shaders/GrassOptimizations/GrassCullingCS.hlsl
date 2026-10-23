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
	float OcclusionBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	float DensityDistanceRcp;
	float _pad0;
};

cbuffer CullBucket : register(b1)
{
	uint GroupCount;
	// SliceTable maps dispatched groups to visible source runs.
	uint SliceTableOffset;
	uint SliceCount;
	// Split large buckets across rows to stay within the 65,535-group dispatch limit.
	uint DispatchWidth;

	// Each LOD tier occupies TierStride records in Compacted.
	uint TierStride;
	uint OutputOffset;
	uint ArgsOffset;
	uint _bucketPad0;

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
};

ByteAddressBuffer Instances : register(t0);
// Source groups contain instance ranges and cell origins.
StructuredBuffer<GrassInstance::CullGroup> Groups : register(t1);
Texture2D<float> HiZ : register(t2);
// Each entry stores the run's first source group in .x and its first dispatch-group index in .y.
StructuredBuffer<uint2> SliceTable : register(t3);

// RunGrass resolves each output record against its source instance and group.
RWByteAddressBuffer Compacted : register(u0);
// Each tier has DrawIndexedInstanced arguments with its instance count in the second DWORD.
RWByteAddressBuffer Args : register(u1);

// Tiers select the full, middle and far meshes.
static const uint TierCount = 3;
static const uint ArgsStride = 20;
static const uint TierCulled = TierCount;

// Aggregate survivors within each group to issue one global atomic per occupied tier.
groupshared uint GroupTierCount[TierCount * 2];
groupshared uint GroupTierBase[TierCount * 2];

// Convert hashed bits to a uniform value in [0, 1).
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

// Return the LOD tier or TierCulled and populate the output record for surviving instances.
uint CullInstance(float4 data1, float4 data4, GrassInstance::CullGroup group, uint eyeIndex, out float fade, out uint flags)
{
	fade = 0.0;
	flags = 0;


	const float2 localXY = data1.xy;
	const float localZ = data1.z;

	const float3 world = float3(localXY, localZ) + group.Origin;

	const float3 dv = world - CameraPos[eyeIndex].xyz;
	const float distSq = dot(dv, dv);
	if (distSq > MaxDistSq)
		return TierCulled;

	const float dist = sqrt(distSq);

	// Bounds include the mesh's offset from its root, scale variation and deformation padding.
	const float sizeVariance = data4.y;
	const float boundsRadius = BoundExtent * max(1.0, abs(1.0 + sizeVariance)) + 128.0;
	[unroll] for (uint p = 0; p < 6; ++p)
	{
		if (dot(FrustumPlanes[eyeIndex * 6 + p].xyz, EyeCount > 1 ? dv : world) - FrustumPlanes[eyeIndex * 6 + p].w < -boundsRadius)
			return TierCulled;
	}

	// Conservatively approximate the vertex shader's ScaleMask-based size variation.
	const float instanceRadius = ModelRadius * (1.0 + max(sizeVariance, 0.0));

	// Use camera depth so pixel sizing stays consistent across the view.
	const float clipW = dot(CameraViewProj[eyeIndex * 4 + 3].xyz, dv) + CameraViewProj[eyeIndex * 4 + 3].w;
	const float projPx = (instanceRadius * ProjectionWLength[eyeIndex] / max(clipW, 1e-4)) * ProjScale;

	// Hash world positions so compaction preserves density and LOD choices.
	const uint2 rand = Random::pcg2d(asuint(world.xy) ^ asuint(world.z));

	// Choose the mesh before applying its distance and density penalties.
	const float h = RandFloat(rand.y);
	const float halfBand = MeshLODBandPx * 0.5;
	const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);

	uint tier = 0;
	if (MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 1;
	if (FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 2;

	const float meshCost = tier == 1 ? MidLODMeshCost : (tier == 2 ? FarLODMeshCost : MinPixelScale);
	const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));
	const float effCostBias = MeshCostBias * costRamp;
	const float dScale = lerp(1.0, tier == 0 ? DistScale : rcp(meshCost), effCostBias);
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
		const float distanceRamp = saturate(dist * DensityDistanceRcp);
		const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4)) * distanceRamp;
		const float keep = lerp(1.0, LODMinKeep, t);
		const float densityRandom = RandFloat(rand.x);
		if (densityRandom > keep + LODFadeBand)
			return TierCulled;
		lodFade = saturate((keep + LODFadeBand - densityRandom) / LODFadeBand);
	}

	const float maxDist = sqrt(effMaxDistSq);
	const float edgeStart = maxDist * EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

	const float4 clipOffset = float4(CameraViewProj[eyeIndex * 4 + 0].w, CameraViewProj[eyeIndex * 4 + 1].w, CameraViewProj[eyeIndex * 4 + 2].w, CameraViewProj[eyeIndex * 4 + 3].w);
	const float4 clip = ProjectDirection(dv, eyeIndex) + clipOffset;
	const float distFade = 1.0 - saturate((length(FrameBuffer::ToStandardClip(clip, ReverseProjection > 0.5)) - AlphaParam1) / AlphaParam2);
	const float spawnFade = saturate((FadeNow - group.FadeStart) * FadeInTimeRcp);

	fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= InvisibleFadeCull)
		return TierCulled;

	// Apply density and fade rejection before sampling depth.
	if (HiZEnabled > 0.5) {
		const float occRadius = boundsRadius;
		const float4 clipC = clip;
		const float minW = clipC.w - occRadius * ProjectionWLength[eyeIndex];

		// A sphere crossing the camera plane has no finite projected bounds.
		[branch] if (minW > 1e-4)
		{
			const float3 ndc = clipC.xyz / clipC.w;
			// Bound the NDC change by the sphere radius times the projection gradient, divided by the minimum positive W.
			const float gradientX = length(CameraViewProj[eyeIndex * 4 + 0].xyz - ndc.x * CameraViewProj[eyeIndex * 4 + 3].xyz);
			const float gradientY = length(CameraViewProj[eyeIndex * 4 + 1].xyz - ndc.y * CameraViewProj[eyeIndex * 4 + 3].xyz);
			const float gradientZ = length(CameraViewProj[eyeIndex * 4 + 2].xyz - ndc.z * CameraViewProj[eyeIndex * 4 + 3].xyz);

			const float3 extent = (occRadius / minW) * float3(gradientX, gradientY, gradientZ);
			const float3 ndcLo = ndc - extent - 1e-5;
			const float3 ndcHi = ndc + extent + 1e-5;
			float2 uvLo = saturate(float2(ndcLo.x, -ndcHi.y) * 0.5 + 0.5);
			float2 uvHi = saturate(float2(ndcHi.x, -ndcLo.y) * 0.5 + 0.5);
			if (EyeCount > 1) {
				uvLo.x = (uvLo.x + (float)eyeIndex) * 0.5;
				uvHi.x = (uvHi.x + (float)eyeIndex) * 0.5;
			}
			const float2 tcLo = uvLo * HiZSize;
			const float2 tcHi = uvHi * HiZSize;

			float wantLevel = ceil(log2(max(max(tcHi.x - tcLo.x, tcHi.y - tcLo.y), 1.0)));
			float scale = exp2(wantLevel);
			int2 t0 = int2(floor(tcLo / scale));
			int2 t1 = int2(floor(tcHi / scale));

			[branch] if (any(t1 - t0 > 1))
			{
				wantLevel += 1.0;
				scale *= 2.0;
				t0 = int2(floor(tcLo / scale));
				t1 = int2(floor(tcHi / scale));
			}

			// Near-plane intersections and footprints beyond the available pyramid remain visible.
			[branch] if (ndcLo.z > 0.0 && ndcHi.z < 1.0 && wantLevel <= HiZMipCount - 1.0)
			{
				const int level = (int)wantLevel;
				const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));
				const int eyeHalf = dimL.x >> 1;
				const int xMin = EyeCount > 1 && eyeHalf > 0 ? (int)eyeIndex * eyeHalf : 0;
				const int xMax = EyeCount > 1 && eyeHalf > 0 ? xMin + eyeHalf - 1 : dimL.x - 1;
				const int2 lo = clamp(t0, int2(xMin, 0), int2(xMax, dimL.y - 1));
				const int2 hi = clamp(t1, int2(xMin, 0), int2(xMax, dimL.y - 1));
				const float tileMax = FrameBuffer::FartherDepth(FrameBuffer::FartherDepth(HiZ.Load(int3(lo, level)), HiZ.Load(int3(hi.x, lo.y, level))),
					FrameBuffer::FartherDepth(HiZ.Load(int3(lo.x, hi.y, level)), HiZ.Load(int3(hi, level))));
				const float nearestDepth = ReverseProjection > 0.5 ? ndcHi.z : ndcLo.z;
				const float biasedOccluder = FrameBuffer::FartherDepth(tileMax + OcclusionBias, tileMax - OcclusionBias);
				if (FrameBuffer::IsNearerDepth(biasedOccluder, nearestDepth))
					return TierCulled;
			}
		}
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
	const uint groupIdx = groupId.y * DispatchWidth + groupId.x;
	if (groupIdx < GroupCount) {
		uint lo = 0;
		uint hi = SliceCount - 1;
		[loop] while (lo < hi)
		{
			const uint mid = (lo + hi + 1) >> 1;
			if (SliceTable[SliceTableOffset + mid].y <= groupIdx)
				lo = mid;
			else
				hi = mid - 1;
		}
		const uint2 run = SliceTable[SliceTableOffset + lo];
		groupIndex = run.x + (groupIdx - run.y);
		group = Groups[groupIndex];
		if (threadIdx < group.Count) {
			sourceIndex = group.SourceStart + threadIdx;
			GrassInstance::Decode(Instances.Load4(sourceIndex * GrassInstance::Stride), data1, data2, data3, data4);
			[unroll] for (uint cullEye = 0; cullEye < 2; ++cullEye)
			{
				if (cullEye < EyeCount)
					tier[cullEye] = CullInstance(data1, data4, group, cullEye, fade[cullEye], flags[cullEye]);
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
		Args.InterlockedAdd(ArgsOffset + threadIdx * ArgsStride + 4, GroupTierCount[threadIdx], base);
		GroupTierBase[threadIdx] = base;
	}
	GroupMemoryBarrierWithGroupSync();
	if (tier[0] == TierCulled && tier[1] == TierCulled)
		return;

	const float3 world = data1.xyz + group.Origin;
	float4 currentResponse, previousResponse;
	float2 flutter;
	GrassWindResponse::Sample(data1.xy, world.xy, world.xy, Math::IdentityMatrix, Math::IdentityMatrix,
		TimeBase * WavePeriod, PrevTimeBase * WavePeriod, currentResponse, previousResponse, flutter);
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
		const uint slot = OutputOffset + (tier[eye] * EyeCount + eye) * TierStride + GroupTierBase[eye * TierCount + tier[eye]] + localSlot[eye];
		const uint byteOffset = slot * GrassInstance::RecordStride;
		Compacted.Store4(byteOffset, uint4(sourceIndex, GrassInstance::PackRecord(groupIndex, fade[eye], flags[eye]), asuint(flutter)));
		Compacted.Store4(byteOffset + GrassInstance::CurrentWindOffset, asuint(currentResponse));
		Compacted.Store4(byteOffset + GrassInstance::PreviousWindOffset, asuint(previousResponse));
		Compacted.Store4(byteOffset + GrassInstance::CurrentCollisionOffset, asuint(currentCollision));
		Compacted.Store4(byteOffset + GrassInstance::PreviousCollisionOffset, asuint(previousCollision));
	}
}
