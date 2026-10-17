#ifdef VSHADER
float4 main(uint vertexID : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#endif

#ifdef PSHADER
Texture2D<float> StaticDepth : register(t0);

cbuffer CopyParams : register(b0)
{
	float DepthScale;
	float3 _pad;
};

float main(float4 position : SV_Position) : SV_Depth
{
	const float depth = StaticDepth.Load(int3(int2(position.xy), 0));
	// Far depth marks unwritten texels and must stay far under any scale.
	return depth >= 1.0 ? 1.0 : saturate(depth * DepthScale);
}
#endif
