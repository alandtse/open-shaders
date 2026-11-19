// Gap closing, second half: shadows shrink back by Radius texels (the farthest of the grown depths around),
// so openings wider than the gaps keep their size. Written straight into the cascade's depth.
cbuffer ShadowGapsCB : register(b0)
{
	uint2 Size;
	uint Slice;
	int Radius;
};

Texture2D<float> Grown : register(t0);

float4 vsmain(uint vertexId : SV_VertexID) : SV_POSITION
{
	// One triangle covering the viewport
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float main(float4 position : SV_POSITION) : SV_Depth
{
	const int2 texel = int2(position.xy);
	float farthest = 0.0;
	for (int y = -Radius; y <= Radius; ++y)
		for (int x = -Radius; x <= Radius; ++x)
			farthest = max(farthest, Grown.Load(int3(clamp(texel + int2(x, y), 0, int2(Size) - 1), 0)));
	return farthest;
}
