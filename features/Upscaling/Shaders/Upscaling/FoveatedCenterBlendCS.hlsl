#include "Common/FoveatedMask.hlsli"

cbuffer FoveatedCenterBlendCB : register(b0)
{
	float2 InvOutputDim;
	float CenterScale;
	float CenterFeather;
	float2 CenterOffset;
	float2 OutputOffset;
	float2 DispatchDim;
	float2 SourceOffset;
	float2 InvSourceDim;
	float2 Pad0;  // x=centerHorizontalScale, y reserved
};

Texture2D<float4> CenterColor : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint2 localPos = dispatchID.xy;
	if (any(localPos >= uint2(DispatchDim)))
		return;

	uint2 outputPos = localPos + uint2(OutputOffset + 0.5);
	float2 outputUV = (float2(outputPos) + 0.5) * InvOutputDim;
	float blendWeight = FoveatedComputeCenterBlendWeight(outputUV, CenterScale, CenterFeather, Pad0.x, CenterOffset);
	if (blendWeight <= 0.0)
		return;

	float2 centerUV = (float2(localPos) + SourceOffset + 0.5) * InvSourceDim;
	float4 centerColor = CenterColor.SampleLevel(LinearSampler, centerUV, 0);

	if (blendWeight >= 1.0) {
		OutputColor[outputPos] = centerColor;
	} else {
		float4 baseColor = OutputColor[outputPos];
		OutputColor[outputPos] = lerp(baseColor, centerColor, blendWeight);
	}
}
