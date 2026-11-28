#include "Common/WindowShadow.hlsli"

cbuffer Openings : register(b0)
{
	uint ShadowSlice;
	float NormalizedDepthDrop;
	uint2 ShadowSize;
	uint MinimumRimCoverage;
	uint2 GridSize;
	float4 GridFromShadow[2];
	float2 ShadowDepthFromGrid;
};

Texture2DArray<float> ShadowDepth : register(t0);
Texture2D<float> DepthGrid : register(t1);
Texture2D<float> BoundaryDepth : register(t2);
struct Region
{
	uint parent;
	uint rimEdges;
	uint deepEdges;
};

static const uint NoRegion = 0xffffffff;

#ifdef OPENING_REGIONS
globallycoherent RWStructuredBuffer<Region> Regions : register(u0);
static const int2 Neighbors[] = { int2(-1, 0), int2(0, -1), int2(1, 0), int2(0, 1) };

uint Index(uint2 p)
{
	return p.y * GridSize.x + p.x;
}

bool InBasin(int2 p, float rim)
{
	float depth = DepthGrid.Load(int3(p, 0));
	return depth < 1 && depth > rim && BoundaryDepth.Load(int3(p, 0)) == rim;
}

[numthreads(8, 8, 1)] void SeedRegions(uint2 p : SV_DispatchThreadID) {
	if (any(p >= GridSize))
		return;
	float rim = BoundaryDepth.Load(int3(p, 0));
	uint parent = NoRegion;
	if (InBasin(p, rim)) {
		parent = Index(p);
		if (InBasin(int2(p) + Neighbors[1], rim))
			parent -= GridSize.x;
		else if (InBasin(int2(p) + Neighbors[0], rim))
			--parent;
	}
	Region region = { parent, 0, 0 };
	Regions[Index(p)] = region;
}

uint FindRoot(uint node)
{
	[allow_uav_condition] while (true)
	{
		uint parent;
		InterlockedAdd(Regions[node].parent, 0, parent);
		if (parent == node)
			return node;
		uint grandparent;
		InterlockedAdd(Regions[parent].parent, 0, grandparent);
		InterlockedMin(Regions[node].parent, grandparent);
		node = grandparent;
	}
	return node;
}

[numthreads(8, 8, 1)] void JoinRegions(uint2 p : SV_DispatchThreadID) {
	if (any(p >= GridSize))
		return;
	float rim = BoundaryDepth.Load(int3(p, 0));
	if (!InBasin(p, rim) || InBasin(int2(p) - 1, rim) || !InBasin(int2(p) + Neighbors[0], rim) || !InBasin(int2(p) + Neighbors[1], rim))
		return;
	uint a = Index(p) - 1, b = Index(p) - GridSize.x;
	[allow_uav_condition] while (true)
	{
		a = FindRoot(a);
		b = FindRoot(b);
		if (a == b)
			break;
		uint hi = max(a, b), lo = min(a, b), previous;
		InterlockedCompareExchange(Regions[hi].parent, hi, lo, previous);
		if (previous == hi)
			break;
		a = previous;
		b = lo;
	}
}

	[numthreads(8, 8, 1)] void MeasureRims(uint2 p : SV_DispatchThreadID)
{
	if (any(p >= GridSize))
		return;
	float rim = BoundaryDepth.Load(int3(p, 0));
	if (!InBasin(p, rim))
		return;
	uint root = FindRoot(Index(p));
	InterlockedMin(Regions[Index(p)].parent, root);
	float depth = DepthGrid.Load(int3(p, 0));
	uint edges = 0, deep = 0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		int2 neighbor = int2(p) + Neighbors[i];
		if (!InBasin(neighbor, rim)) {
			++edges;
			deep += depth - DepthGrid.Load(int3(neighbor, 0)) >= NormalizedDepthDrop;
		}
	}
	if (edges > 0) {
		InterlockedAdd(Regions[root].rimEdges, edges);
		InterlockedAdd(Regions[root].deepEdges, deep);
	}
}
#elif !defined(OPENING_COMPOSITE)
RWTexture2D<float> Result : register(u0);

static const uint Reduction = 4;
static const uint Tile = 16;
static const uint Interior = 8;
static const uint Halo = (Tile - Interior) / 2;
static const uint SweepLimit = 48;

[numthreads(8, 8, 1)] void ReduceDepth(uint2 p : SV_DispatchThreadID) {
	uint2 size, source;
	Result.GetDimensions(size.x, size.y);
	DepthGrid.GetDimensions(source.x, source.y);
	if (any(p >= size))
		return;
	float deepest = 0;
	[unroll] for (uint y = 0; y < Reduction; ++y)
		[unroll] for (uint x = 0; x < Reduction; ++x)
			deepest = max(deepest, DepthGrid.Load(int3(min(p * Reduction + uint2(x, y), source - 1), 0)));
	Result[p] = deepest;
}

groupshared float EscapeDepth[Tile * Tile];
groupshared uint Changed;

// A route to the map boundary keeps an unenclosed depth step from admitting sunlight.
[numthreads(Tile, Tile, 1)] void FindRims(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint idx : SV_GroupIndex) {
	uint2 size;
	DepthGrid.GetDimensions(size.x, size.y);
	int2 p = int2(group * Interior + local) - int(Halo);
	uint2 q = clamp(p, 0, int2(size) - 1);
	float depth = DepthGrid.Load(int3(q, 0));
	float upper = any(size > Interior) ? min(depth, BoundaryDepth.Load(int3(q / Reduction, 0))) : depth;
	bool outside = any(p <= 0) || any(p >= int2(size) - 1);
	bool edge = any(local == 0) || any(local == Tile - 1);
	EscapeDepth[idx] = outside ? depth : edge ? upper :
	                                            0;
	GroupMemoryBarrierWithGroupSync();
	bool settled = false;
	[loop] for (uint step = 0; step < SweepLimit; ++step)
	{
		float before = EscapeDepth[idx];
		float next = before;
		if (!outside && !edge) {
			float adjacent = max(max(EscapeDepth[idx - 1], EscapeDepth[idx + 1]), max(EscapeDepth[idx - Tile], EscapeDepth[idx + Tile]));
			next = min(upper, max(before, adjacent));
		}
		if (idx == 0)
			Changed = 0;
		GroupMemoryBarrierWithGroupSync();
		EscapeDepth[idx] = next;
		if (next > before)
			InterlockedOr(Changed, 1);
		GroupMemoryBarrierWithGroupSync();
		settled = Changed == 0;
		GroupMemoryBarrierWithGroupSync();
		if (settled)
			break;
	}
	if (all(local >= Halo) && all(local < Halo + Interior) && all(p >= 0) && all(p < int2(size)))
		Result[p] = settled ? EscapeDepth[idx] : upper;
}

StructuredBuffer<Region> MeasuredRegions : register(t3);

[numthreads(8, 8, 1)] void ClassifyOpenings(uint2 p : SV_DispatchThreadID) {
	if (any(p >= GridSize))
		return;
	float depth = DepthGrid.Load(int3(p, 0));
	float rim = BoundaryDepth.Load(int3(p, 0));
	float entry = WindowShadow::Encode(rim, true);
	uint root = MeasuredRegions[p.y * GridSize.x + p.x].parent;
	bool complete = false;
	if (root != NoRegion) {
		Region region = MeasuredRegions[root];
		complete = region.rimEdges > 0 && region.deepEdges * 100 >= region.rimEdges * MinimumRimCoverage;
	}
	// The sign carries component approval to the full-resolution depth check.
	Result[p] = complete ? -entry : (depth < 1 && depth - rim >= NormalizedDepthDrop ? entry : 0);
}

	[numthreads(8, 8, 1)] void FindWideCenters(uint2 p : SV_DispatchThreadID)
{
	uint2 size;
	Result.GetDimensions(size.x, size.y);
	if (any(p >= size))
		return;
	bool wide = all(p > 0) && all(p + 1 < size);
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
			wide = wide && DepthGrid.Load(int3(int2(p) + int2(x, y), 0)) != 0;
	Result[p] = wide;
}

Texture2D<float> WideCenters : register(t3);

[numthreads(8, 8, 1)] void OpeningEntries(uint2 p : SV_DispatchThreadID) {
	uint2 size;
	Result.GetDimensions(size.x, size.y);
	if (any(p >= size))
		return;
	bool wide = false;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
			wide = wide || WideCenters.Load(int3(int2(p) + int2(x, y), 0)) > 0;
	Result[p] = wide ? DepthGrid.Load(int3(p, 0)) : 0;
}

#else
float MergeEntries(float4 position : SV_Position) : SV_Target
{
	int2 pixel = int2(position.xy);
	float pane = DepthGrid.Load(int3(pixel, 0));
	if (pane != 0)
		return pane;
	float3 shadowUV = float3(position.xy / ShadowSize, 1);
	float2 cell = float2(dot(GridFromShadow[0].xyz, shadowUV), dot(GridFromShadow[1].xyz, shadowUV));
	float entry = BoundaryDepth.Load(int3(int2(floor(cell)), 0));
	if (entry == 0)
		return 0;
	float rim = asfloat(asuint(abs(entry)) & ~1u) - 1.0;
	float nativeRim = mad(rim, ShadowDepthFromGrid.x, ShadowDepthFromGrid.y);
	float depth = ShadowDepth.Load(int4(pixel, ShadowSlice, 0));
	float requiredDepth = nativeRim + (entry < 0 ? 0 : NormalizedDepthDrop * ShadowDepthFromGrid.x);
	return depth >= requiredDepth ? WindowShadow::Encode(nativeRim, true) : 0;
}

float CloseShadowDepth(float4 position : SV_Position) : SV_Depth
{
	if (WindowShadow::Open(DepthGrid.Load(int3(int2(position.xy), 0))))
		discard;
	return 0;
}
#endif
