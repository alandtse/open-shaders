cbuffer FoveatedCenterBlendCB : register(b0)
{
	float2 InvOutputDim;
	float CenterScale;
	float CenterFeather;
	float2 OutputOffset;
	float2 DispatchDim;
	float2 InvDispatchDim;
	float2 pad0;
};

Texture2D<float4> CenterColor : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutputColor : register(u0);

float ComputeCenterBlendWeight(float2 uv)
{
	float halfSize = saturate(CenterScale) * 0.5;
	float2 outside = abs(uv - 0.5) - halfSize.xx;
	float distanceOutside = max(outside.x, outside.y);
	return 1.0 - smoothstep(0.0, max(CenterFeather, 1e-4), distanceOutside);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint2 localPos = dispatchID.xy;
	if (any(localPos >= uint2(DispatchDim)))
		return;

	uint2 outputPos = localPos + uint2(OutputOffset + 0.5);
	float2 outputUV = (float2(outputPos) + 0.5) * InvOutputDim;
	float blendWeight = ComputeCenterBlendWeight(outputUV);
	if (blendWeight <= 0.0)
		return;

	float4 baseColor = OutputColor[outputPos];
	float2 centerUV = (float2(localPos) + 0.5) * InvDispatchDim;
	float4 centerColor = CenterColor.SampleLevel(LinearSampler, centerUV, 0);

	OutputColor[outputPos] = blendWeight >= 1.0 ? centerColor : lerp(baseColor, centerColor, blendWeight);
}
