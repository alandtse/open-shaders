#include "Common/DummyVSTexCoord.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float Depth : SV_Depth;
};

SamplerState LinearSampler : register(s0);

Texture2D<float4> DepthTex : register(t0);

cbuffer PerFrame : register(b0)
{
	float4 ResolutionScale;
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	float depth = DepthTex.SampleLevel(LinearSampler, input.TexCoord.xy * ResolutionScale.x, 0);
	psout.Depth = depth;
	return psout;
}
