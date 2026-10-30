#include "SnowDeformation/UpdateParams.hlsli"
#include "SnowDeformation/Coverage.hlsli"

Texture2D<float4> LandGrid : register(t4);
RWTexture2D<float> Blanket : register(u0);

float4 LandAt(int2 world)
{
	return LandGrid.Load(int3(world & (BLANKET_TEXELS - 1), 0));
}

float NeighbourHeight(int2 world, float fallback)
{
	float4 land = LandAt(world);
	return land.y < 0.0 ? fallback : land.x;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const int2 slot = int2(dispatchID.xy);
	const int2 world = BlanketCell.xy + ((slot - BlanketCell.xy) & (BLANKET_TEXELS - 1));
	const float4 land = LandAt(world);

	float raise = 0.0;
	[branch] if (land.y >= 0.0)
	{
		const float slopeX = (NeighbourHeight(world + int2(1, 0), land.x) - NeighbourHeight(world - int2(1, 0), land.x)) / (2.0 * BLANKET_CELL);
		const float slopeY = (NeighbourHeight(world + int2(0, 1), land.x) - NeighbourHeight(world - int2(0, 1), land.x)) / (2.0 * BLANKET_CELL);
		const float normalZ = rsqrt(1.0 + slopeX * slopeX + slopeY * slopeY);

		const float2 windowPosition = float2(world * BLANKET_CELL) - float2(OriginCell.xy) * Control.z;
		const float2 worldPosition = float2(world) * BLANKET_CELL;
		const float snow = max(land.y, SnowCoverAt(worldPosition, windowPosition, land.x, normalZ));

		const float2 fromCentre = abs(windowPosition - BlanketFade.xy);
		const float fade = 1.0 - saturate((max(fromCentre.x, fromCentre.y) - BlanketFade.z) * BlanketFade.w);
		raise = min(BlanketShape.x * snow, BlanketShape.x * land.z) * fade;
	}
	Blanket[slot] = raise;
}
