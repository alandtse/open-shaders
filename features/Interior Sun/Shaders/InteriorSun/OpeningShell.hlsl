cbuffer Shape : register(b0)
{
	row_major float4x4 WorldToClip;
	float4 UVTransform;
	float AlphaThreshold;
};

struct Vertex
{
	float4 position: SV_Position;
	float2 uv: TEXCOORD0;
};

Vertex ShellVS(float3 position : POSITION, float2 uv : TEXCOORD0)
{
	Vertex output;
	output.position = mul(float4(position, 1), WorldToClip);
	output.uv = uv * UVTransform.xy + UVTransform.zw;
	return output;
}

float ShellDepth(Vertex input) : SV_Target
{
	return input.position.z;
}

Texture2D<float4> Diffuse : register(t0);
SamplerState MaterialSampler : register(s0);

float ShellCutoutDepth(Vertex input) : SV_Target
{
	clip(Diffuse.Sample(MaterialSampler, input.uv).a - AlphaThreshold);
	return input.position.z;
}
