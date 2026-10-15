#include "Common/LodLandscape.hlsli"

cbuffer CasterData : register(b0)
{
	row_major float4x4 WorldToClip;
	float4 UVTransform;
	float4 Alpha;
	row_major float4x4 World;
	float4 LandBounds;
};

#if defined(VSHADER)
struct Input
{
	float4 position: POSITION;
	float2 uv: TEXCOORD0;
#	if defined(INSTANCED_TREE)
	float4 instancePosition: TEXCOORD4;
	float4 instanceRotation: TEXCOORD5;
#	endif
};

struct Output
{
	float4 position: SV_POSITION;
	float2 uv: TEXCOORD0;
	float2 visibility: TEXCOORD1;
};

Output main(Input input)
{
	Output output;
	float3 position = input.position.xyz;
	output.visibility = float2(1.0, 0.0);
#	if defined(INSTANCED_TREE)
	position *= input.instancePosition.w;
	position.xy = float2(dot(float2(1, -1) * input.instanceRotation.xy, position.xy), dot(input.instanceRotation.yx, position.xy));
	position += input.instancePosition.xyz;
	output.visibility = input.instanceRotation.zw;
#	endif
	float4 positionMS = float4(position, 1);
	if (Alpha.z > 0)
		positionMS = LodLandscape::AdjustLodLandscapeVertexPositionMS(positionMS, World, LandBounds);
	output.position = mul(positionMS, WorldToClip);
	output.uv = input.uv * UVTransform.xy + UVTransform.zw;
	return output;
}
#else
Texture2D<float4> Diffuse : register(t0);
SamplerState DiffuseSampler : register(s0);

static const float VisibilityThreshold = 0.5;
static const float DepthOffsets[16] = {
	0.003921568, 0.533333361, 0.133333340, 0.666666687,
	0.800000000, 0.266666681, 0.933333337, 0.400000000,
	0.200000000, 0.733333349, 0.066666670, 0.600000000,
	0.996078432, 0.466666669, 0.866666675, 0.333333343
};

void main(float4 position : SV_POSITION, float2 uv : TEXCOORD0, float2 visibility : TEXCOORD1)
{
	uint2 pixel = uint2(position.xy);
	uint index = ((pixel.x << 2) & 12) | (pixel.y & 3);
	clip(visibility.x + visibility.y * (VisibilityThreshold - DepthOffsets[index]) - VisibilityThreshold);
	clip(Diffuse.Sample(DiffuseSampler, uv).a * Alpha.y - Alpha.x);
}
#endif
