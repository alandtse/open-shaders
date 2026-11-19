// Reprojects a baked paraboloid shadow tile from its bake origin to the light's current
// origin, so a cached static shadow follows flame flicker without a rebake.
//
// Pass 1 (CSHADER): scatter each baked texel to its new texel, keeping the nearest depth.
// Pass 2 (PSHADER): resolve the scratch texture into the live atlas tile, filling cracks.
//
// Tile layout, depth encoding and matrix convention mirror GetOmnidirectionalShadow in
// LightLimitFix.hlsli: depth = |lightSpace| / radius, paraboloid UV from the half vector,
// omni tiles stack the upper (y < 0.5) and lower (y >= 0.5) paraboloid.

#ifdef CSHADER

Texture2D<float> StaticDepth : register(t0);
RWTexture2D<uint> Scratch : register(u0);

cbuffer WarpParams : register(b0)
{
	row_major float4x4 InvBake;   // bake light space -> world (row-vector convention)
	row_major float4x4 LiveProj;  // world -> live light space
	uint2 SrcOrigin;              // tile origin in the static atlas
	uint TileSize;
	uint IsOmni;  // 2 = dual paraboloid, 1 = single paraboloid
	float BakeRadius;
	float LiveRadius;
	float2 _pad;
};

static const float HemisphereOverlapDepth = 1.0;

void StoreWarpedDepth(float3 direction, bool upper, bool omni, float depth)
{
	const float2 q = upper ? direction.xy / (1.0 + direction.z) : direction.xy / (direction.z - 1.0);
	const float2 faceUV = q * 0.5 + 0.5;
	if (any(faceUV < 0.0) || any(faceUV >= 1.0))
		return;

	const float2 uv = float2(faceUV.x, omni ? (upper ? 0.5 * faceUV.y : 1.0 - 0.5 * faceUV.y) : faceUV.y);
	const uint2 texel = min(uint2(uv * TileSize), TileSize - 1);
	uint original;
	InterlockedMin(Scratch[texel], asuint(depth), original);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= TileSize))
		return;

	const float depth = StaticDepth.Load(int3(SrcOrigin + id.xy, 0));
	if (depth >= 0.999)
		return;

	const bool omni = IsOmni == 2;
	const float2 uv = (float2(id.xy) + 0.5) / TileSize;
	const bool upper = omni ? (uv.y < 0.5) : true;
	float2 q;
	q.x = 2.0 * uv.x - 1.0;
	q.y = omni ? (upper ? 4.0 * uv.y - 1.0 : 4.0 * (1.0 - uv.y) - 1.0) : (2.0 * uv.y - 1.0);
	const float q2 = dot(q, q);

	const float inv = 1.0 / (1.0 + q2);
	const float sgn = upper ? 1.0 : -1.0;
	const float3 dir = float3(sgn * 2.0 * q * inv, sgn * (1.0 - q2) * inv);

	const float3 world = mul(float4(dir * (depth * BakeRadius), 1.0), InvBake).xyz;
	const float3 pLS = mul(float4(world, 1.0), LiveProj).xyz;
	const float len = length(pLS);
	if (len < 1e-3)
		return;
	const float newDepth = len / LiveRadius;
	if (newDepth >= 1.0)
		return;

	const float3 d1 = pLS / len;
	const bool front = pLS.z >= 0.0;
	if (front || omni)
		StoreWarpedDepth(d1, front, omni, newDepth);
	// The shadow caster rasterizes one light-space unit past the hemisphere boundary.
	if (abs(pLS.z) <= HemisphereOverlapDepth && (omni || !front))
		StoreWarpedDepth(d1, !front, omni, newDepth);
}

#endif

#ifdef PSHADER

Texture2D<uint> Warped : register(t0);

cbuffer ResolveParams : register(b0)
{
	uint2 DstOrigin;  // tile origin in the live atlas
	uint TileSize;
	uint _pad;
};

float main(float4 position : SV_Position) : SV_Depth
{
	const int2 p = int2(position.xy) - int2(DstOrigin);
	const uint written = Warped.Load(int3(p, 0));
	if (written != 0xFFFFFFFFu)
		return saturate(asfloat(written));

	// Fill one-texel cracks left by stretching: take the nearest neighbour when most
	// of the neighbourhood was written, otherwise leave the texel at far depth.
	float nearest = 1e9;
	uint neighbours = 0;
	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			if (dx == 0 && dy == 0)
				continue;
			const int2 s = clamp(p + int2(dx, dy), 0, int(TileSize) - 1);
			const uint v = Warped.Load(int3(s, 0));
			if (v != 0xFFFFFFFFu) {
				nearest = min(nearest, asfloat(v));
				neighbours++;
			}
		}
	}
	return neighbours >= 5 ? saturate(nearest) : 1.0;
}

#endif
