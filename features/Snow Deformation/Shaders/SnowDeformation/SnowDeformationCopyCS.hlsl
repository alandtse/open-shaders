#include "SnowDeformation/UpdateParams.hlsli"

#define COPY_BLOCKS ((TILE_TEXELS + 2 + 7) / 8)

StructuredBuffer<int4> TileList : register(t3);
Texture2D<float> Source : register(t0);

RWTexture2D<float> Before : register(u0);

[numthreads(8, 8, 1)] void main(uint3 groupThreadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
	const int4 entry = TileList[groupID.y];
	const int2 slot = int2(entry.x & 0xFF, (entry.x >> 8) & 0xFF);
	const int2 local = int2(groupID.x % COPY_BLOCKS, groupID.x / COPY_BLOCKS) * 8 + int2(groupThreadID.xy) - 1;
	[branch] if (all(local <= TILE_TEXELS))
	{
		const int2 texel = (slot * TILE_TEXELS + local) & (FIELD_TEXELS - 1);
		Before[texel] = Source.Load(int3(texel, 0));
	}
}
