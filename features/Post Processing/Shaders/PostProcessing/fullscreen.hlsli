struct FullscreenTriangleVSOutput
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD0;
};

FullscreenTriangleVSOutput FullscreenTriangleVS(uint vertexID : SV_VertexID)
{
	FullscreenTriangleVSOutput output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
	output.Position.y = -output.Position.y;
	return output;
}
