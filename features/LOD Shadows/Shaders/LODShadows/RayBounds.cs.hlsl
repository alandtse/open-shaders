static const uint GroupSize = 8;
static const uint GroupThreads = GroupSize * GroupSize;
static const uint SamplesPerThread = 2;

cbuffer BoundsData : register(b0)
{
	uint Dimension;
	uint Slice;
	uint TileSize;
	uint Padding;
};

RWTexture2DArray<float2> OutputBounds : register(u0);

#if defined(REDUCE)
Texture2DArray<float2> InputBounds : register(t0);

[numthreads(GroupSize, GroupSize, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= Dimension))
		return;
	float2 a = InputBounds.Load(int4(id * 2, Slice, 0));
	float2 b = InputBounds.Load(int4(id * 2 + uint2(1, 0), Slice, 0));
	float2 c = InputBounds.Load(int4(id * 2 + uint2(0, 1), Slice, 0));
	float2 d = InputBounds.Load(int4(id * 2 + 1, Slice, 0));
	OutputBounds[uint3(id, 0)] = float2(min(min(a.x, b.x), min(c.x, d.x)), max(max(a.y, b.y), max(c.y, d.y)));
}
#else
Texture2DArray<float> Depth : register(t0);
groupshared float2 Bounds[GroupThreads];

[numthreads(GroupSize, GroupSize, 1)] void main(uint2 group : SV_GroupID, uint2 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
	uint2 origin = group * TileSize + thread * SamplesPerThread;
	float a = Depth.Load(int4(origin, Slice, 0));
	float b = Depth.Load(int4(origin + uint2(1, 0), Slice, 0));
	float c = Depth.Load(int4(origin + uint2(0, 1), Slice, 0));
	float d = Depth.Load(int4(origin + 1, Slice, 0));
	Bounds[index] = float2(min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = GroupThreads / 2; stride > 0; stride >>= 1)
	{
		if (index < stride)
			Bounds[index] = float2(min(Bounds[index].x, Bounds[index + stride].x), max(Bounds[index].y, Bounds[index + stride].y));
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0)
		OutputBounds[uint3(group, 0)] = Bounds[0];
}
#endif
