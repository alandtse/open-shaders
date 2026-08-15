#include "Common/FrameBuffer.hlsli"
#include "LightLimitFix/Common.hlsli"

// VR: Depth is the double-wide stereo copy (left eye x in [0, dim.x/2), right
// in [dim.x/2, dim.x)); ClusterSize.xy is already the per-eye tile width, so
// only the physical depth texel and the eye-indexed matrices need the eye index.

// Independent of the live installed-slot count; an index beyond this fixed
// cap falls into gOverflowLDS below rather than corrupting a neighbor.
#define MAX_SHADOW_DEMAND_SLOTS 128

cbuffer PerFrame : register(b0)
{
	float LightsNear;
	float LightsFar;
	float InvLogFarOverNear;  // 1 / log(LightsFar / LightsNear), precomputed CPU-side
	uint FrameIndex;          // 0 disables the jitter, keeping the tap at the tile centre
	uint4 ClusterSize;
	uint TapCount;  // Spatial samples per tile per frame; forced to 1 when FrameIndex == 0.
	uint3 pad;
}

Texture2D<float> Depth : register(t0);
StructuredBuffer<LightGrid> lightGridIn : register(t1);
StructuredBuffer<uint> lightIndexListIn : register(t2);
StructuredBuffer<Light> lights : register(t3);
// (nearRaw, farRaw) hardware depth extremes per (tile, eye), from
// ShadowDepthPyramidCS's exhaustive per-tile reduction. Used below for a
// sphere-vs-visible-depth-slab occlusion test.
StructuredBuffer<float2> TileDepthRange : register(t4);

RWStructuredBuffer<uint> demand : register(u0);
RWStructuredBuffer<uint> overflowCount : register(u1);
// [MAX_SHADOW_DEMAND_SLOTS] per-slot maxima plus a trailing cluster-saturation
// flag at [MAX_SHADOW_DEMAND_SLOTS]; the flag rides the same buffer as the
// samples it invalidates so it can never arrive a readback later than they do.
RWStructuredBuffer<uint> demandMax : register(u2);

groupshared uint gDemandLDS[MAX_SHADOW_DEMAND_SLOTS];
groupshared uint gMaxLDS[MAX_SHADOW_DEMAND_SLOTS];
groupshared uint gOverflowLDS;
groupshared uint gSaturatedLDS;

// demandWeight is scaled by this before the atomic add so the uint32 accumulator
// keeps useful fractional precision; matched on readback (C++ side) when turning
// the raw counts back into a diagnostic float.
static const float kDemandScale = 1024.0;

// Per-sample ceiling: InterlockedAdd on uint32 wraps silently, and wrapping to
// exactly 0 reads as "light absent", freezing a bright light. Divided by the
// worst-case kMaxTapCount, not the live TapCount, so an override can't
// silently overflow the accumulator.
static const uint kMaxTapCount = 8;
#if defined(VR)
static const uint kDemandEyes = 2;
#else
static const uint kDemandEyes = 1;
#endif
static const uint kDemandCeiling = (1u << 18) / (kMaxTapCount * kDemandEyes);

// Stratified 8-rook base offsets over the 64x64 tile. A fixed centre tap probes
// the same pixel forever, so any lit region between taps is a permanent blind
// spot; rotating a stratified set keeps instantaneous coverage even while the
// per-tile hash below breaks the cross-cycle aliasing.
static const uint2 kJitterOffsets[8] = {
	uint2(4, 4), uint2(12, 36), uint2(20, 20), uint2(28, 52),
	uint2(36, 12), uint2(44, 44), uint2(52, 28), uint2(60, 60)
};

uint DemandTileHash(uint2 tile, uint cycle)
{
	uint h = tile.x * 73856093u ^ tile.y * 19349663u ^ cycle * 83492791u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h;
}

// Reprojects one eye's depth tap to view/world space, looks up its cluster's
// light list and accumulates demand for every shadow light found. eyeIndex is
// a compile-time-constant 0 on the non-VR call site, so this inlines down to
// the exact same instructions as the pre-VR single-eye body there.
void AccumulateEyeSample(int2 texel, float2 texcoord, uint eyeIndex, uint2 tileXY)
{
	float depth = Depth.Load(int3(texel, 0));

	float4 clip;
	clip.xy = texcoord * 2.0 - 1.0;
	clip.y *= -1;
	clip.z = depth;
	clip.w = 1.0;

	float4 posVS4 = mul(FrameBuffer::CameraProjInverse[eyeIndex], clip);
	float3 posVS = posVS4.xyz / posVS4.w;
	float4 posWS4 = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], clip);
	float3 posWS = posWS4.xyz / posWS4.w;

	// Same log-space Z slicing ClusterBuildingCS derives its AABBs with;
	// clamp so a sky texel or near-plane degenerate can't produce a
	// NaN/negative log() -> an out-of-range uint cast.
	float viewZ = clamp(posVS.z, LightsNear, LightsFar);
	float zSlice = floor(ClusterSize.z * log(viewZ / LightsNear) * InvLogFarOverNear);
	uint zIndex = (uint)clamp(zSlice, 0.0, float(ClusterSize.z - 1));

	// tileXY is eye-invariant (ClusterBuildingCS unions both eyes' frustum
	// bounds into one shared AABB), but zIndex is not -- the two eyes may
	// legitimately read different light lists at the same tile.
	uint clusterIndex = tileXY.x + tileXY.y * ClusterSize.x + zIndex * (ClusterSize.x * ClusterSize.y);
	LightGrid grid = lightGridIn[clusterIndex];

	// Tile's FARTHEST visible-surface view-space Z (unproject both raw extremes
	// -- robust to either depth convention). Needs the far bound: a light past
	// only the nearest surface can still be lighting something farther back.
	uint tileIndex = eyeIndex * (ClusterSize.x * ClusterSize.y) + tileXY.y * ClusterSize.x + tileXY.x;
	float2 tileDepthRaw = TileDepthRange[tileIndex];
	float4 clipTileNear = clip;
	clipTileNear.z = tileDepthRaw.x;
	float4 clipTileFar = clip;
	clipTileFar.z = tileDepthRaw.y;
	float4 posVSTileNear4 = mul(FrameBuffer::CameraProjInverse[eyeIndex], clipTileNear);
	float4 posVSTileFar4 = mul(FrameBuffer::CameraProjInverse[eyeIndex], clipTileFar);
	float tileFarViewZ = max(posVSTileNear4.z / posVSTileNear4.w, posVSTileFar4.z / posVSTileFar4.w);

	// ClusterCullingCS drops every light past MAX_CLUSTER_LIGHTS, so a light
	// dropped from every cluster it touches reads as zero demand. Flag the
	// frame so the consumer discards the sample instead of trusting it.
	if (grid.lightCount >= MAX_CLUSTER_LIGHTS)
		InterlockedOr(gSaturatedLDS, 1);

	for (uint i = 0; i < grid.lightCount; i++) {
		uint lightIndex = lightIndexListIn[grid.offset + i];
		Light light = lights[lightIndex];
		if (!(light.lightFlags & LightFlags::Shadow))
			continue;

		// Windowed inverse-square falloff (Karis/Lagarde) tracks how strongly
		// the light reaches this tile, not just cluster-AABB overlap; a sphere
		// whose near point along view-Z is behind the tile's visible surface
		// is fully occluded here.
		float lightViewZ = mul(FrameBuffer::CameraView[eyeIndex], float4(light.positionWS[eyeIndex].xyz, 1)).z;
		if (lightViewZ - light.radius > tileFarViewZ)
			continue;

		float3 toLight = light.positionWS[eyeIndex].xyz - posWS;
		float distSq = dot(toLight, toLight);
		float t = saturate(1.0 - distSq * light.invRadius * light.invRadius);
		float atten = t * t;

		float luminance = dot(light.color, float3(0.2126, 0.7152, 0.0722));
		// NOT multiplied by any shadow-sample result: GetShadowLightShadow
		// returns 0 for occluded, so weighting by it would score an
		// actively-shadowing light as "unused" and freeze its shadow stale.
		float demandWeight = luminance * light.fade * atten;

		if (demandWeight <= 0.0)
			continue;

		if (light.shadowMapIndex < MAX_SHADOW_DEMAND_SLOTS) {
			// Same scale across taps/eyes/channels: rescaling here would
			// truncate low-demand lights to zero before the CPU sees them.
			// SUM normalizes once on CPU readback; MAX needs none, since
			// InterlockedMax already yields "visible to any tap, either eye".
			uint scaled = min((uint)(demandWeight * kDemandScale), kDemandCeiling);
			if (scaled > 0) {
				InterlockedAdd(gDemandLDS[light.shadowMapIndex], scaled);
				// Per-tile MAX, not sum: a sum would dilute a light strongly
				// present in one tile and absent in the other 509.
				InterlockedMax(gMaxLDS[light.shadowMapIndex], scaled);
			}
		} else {
			InterlockedAdd(gOverflowLDS, 1);
		}
	}
}

[numthreads(16, 16, 1)] void main(
	uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	// The cooperative zero/flush below must run on every thread, including
	// out-of-bounds tiles -- do not early-return before the barriers, or a
	// partial-group barrier is undefined behavior on SM5.
	bool inBounds = all(dispatchThreadId.xy < ClusterSize.xy);

	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS) {
		gDemandLDS[groupIndex] = 0;
		gMaxLDS[groupIndex] = 0;
	}
	if (groupIndex == 0) {
		gOverflowLDS = 0;
		gSaturatedLDS = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	if (inBounds) {
		// ClusterSize covers a padded 64-multiple, so on a non-aligned
		// resolution an unclamped tap can leave the depth texture; an
		// out-of-range Load reads 0 and fabricates a near-plane hit. Clamped
		// on both the jittered and unjittered (FrameIndex 0) paths.
		uint2 dim;
		Depth.GetDimensions(dim.x, dim.y);
#if defined(VR)
		// Clamp the tile-local tap against the PER-EYE width, not the full
		// double-wide buffer -- else a tile near an eye's right edge could
		// sample a texel whose texcoord points past the eye boundary.
		uint2 localDim = uint2(dim.x / 2, dim.y);
#else
		uint2 localDim = dim;
#endif
		// FrameIndex 0 is the CPU's "jitter off" sentinel and must reproduce
		// the unjittered centre tap exactly; the CPU forces TapCount to 1
		// whenever FrameIndex == 0.
		if (FrameIndex == 0) {
			uint2 texelLocal = min(dispatchThreadId.xy * 64 + 32, localDim - 1);
			float2 texcoord = (float2(texelLocal) + 0.5) / (float2(ClusterSize.xy) * 64.0);
#if defined(VR)
			[unroll] for (uint eyeIndex = 0; eyeIndex < 2; eyeIndex++)
			{
				int2 texel = int2(texelLocal.x + eyeIndex * localDim.x, texelLocal.y);
				AccumulateEyeSample(texel, texcoord, eyeIndex, dispatchThreadId.xy);
			}
#else
			AccumulateEyeSample(int2(texelLocal), texcoord, 0, dispatchThreadId.xy);
#endif
		} else {
			// base folds TapCount into the jitter/cycle index so K taps this
			// frame consume K *different* rook positions, not the same one K times.
			for (uint k = 0; k < TapCount; k++) {
				uint base = FrameIndex * TapCount + k;
				uint h = DemandTileHash(dispatchThreadId.xy, base / 8);
				uint2 jitter = (kJitterOffsets[base % 8] + uint2(h & 63, (h >> 6) & 63)) % 64;
				uint2 texelLocal = min(dispatchThreadId.xy * 64 + jitter, localDim - 1);
				float2 texcoord = (float2(texelLocal) + 0.5) / (float2(ClusterSize.xy) * 64.0);
#if defined(VR)
				[unroll] for (uint eyeIndex = 0; eyeIndex < 2; eyeIndex++)
				{
					int2 texel = int2(texelLocal.x + eyeIndex * localDim.x, texelLocal.y);
					AccumulateEyeSample(texel, texcoord, eyeIndex, dispatchThreadId.xy);
				}
#else
				AccumulateEyeSample(int2(texelLocal), texcoord, 0, dispatchThreadId.xy);
#endif
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS && gDemandLDS[groupIndex] > 0)
		InterlockedAdd(demand[groupIndex], gDemandLDS[groupIndex]);
	if (groupIndex < MAX_SHADOW_DEMAND_SLOTS && gMaxLDS[groupIndex] > 0)
		InterlockedMax(demandMax[groupIndex], gMaxLDS[groupIndex]);
	if (groupIndex == 0 && gOverflowLDS > 0)
		InterlockedAdd(overflowCount[0], gOverflowLDS);
	if (groupIndex == 0 && gSaturatedLDS > 0)
		InterlockedOr(demandMax[MAX_SHADOW_DEMAND_SLOTS], 1);
}
