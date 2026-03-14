cbuffer FoveatedPeripheryCB : register(b0)
{
	float2 OutputDim;
	float2 InvOutputDim;
	float2 InvSourceDim;
	float2 Jitter;
};

Texture2D<float4> InputColor : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutColor : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint2 outputPos = dispatchID.xy;
	if (any(outputPos >= uint2(OutputDim)))
		return;

	float2 uv = (float2(outputPos) + 0.5) * InvOutputDim;
	float2 sourceUV = uv - (Jitter * InvSourceDim);
	sourceUV = saturate(sourceUV);

	OutColor[outputPos] = InputColor.SampleLevel(LinearSampler, sourceUV, 0);
}
