// Occlusion-culling depth downscale for the VR engine. Keeps the farthest depth of each 4x4 block so
// the occluder buffer stays conservative under either depth convention.

#include "Common/ReverseZ.hlsli"

Texture2D<float4> DepthTex : register(t0);
SamplerState DepthSampler : register(s0);

cbuffer PerGeometry : register(b2)
{
	float2 GatherSpacing : packoffset(c3);  // UV distance between the four 2x2 gathers: two source texels
	float2 GatherBias : packoffset(c3.z);   // UV offset of the first gather: -1.5 source texels
};

float main(float4 position : SV_POSITION, float2 texCoord : TEXCOORD0) : SV_Depth
{
	float2 base = texCoord + GatherBias;
	float4 topLeft = DepthTex.GatherRed(DepthSampler, base);
	float4 topRight = DepthTex.GatherRed(DepthSampler, base + float2(GatherSpacing.x, 0.0));
	float4 bottomLeft = DepthTex.GatherRed(DepthSampler, base + float2(0.0, GatherSpacing.y));
	float4 bottomRight = DepthTex.GatherRed(DepthSampler, base + GatherSpacing);

	float4 farthestPerGather = float4(
		FrameBuffer::FarthestDepth(topLeft),
		FrameBuffer::FarthestDepth(topRight),
		FrameBuffer::FarthestDepth(bottomLeft),
		FrameBuffer::FarthestDepth(bottomRight));
	return FrameBuffer::FarthestDepth(farthestPerGather);
}
