#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

Texture2D<float4> ProxyInput : register(t0);
Texture2D<float4> LowInput : register(t1);
Texture2D<float4> LowOutput : register(t2);
Texture2D<float> DepthInput : register(t3);
Texture2D<float2> MotionInput : register(t4);
SamplerState LinearClamp : register(s0);

RWTexture2D<float4> LowColor : register(u0);
RWTexture2D<float> LowDepth : register(u1);
RWTexture2D<float2> LowMotion : register(u2);
RWTexture2D<float4> ExpandedProxy : register(u3);

cbuffer LowRes : register(b1)
{
	uint LowWidth;
	uint LowHeight;
	uint GuideWidth;
	uint GuideHeight;
	float GuideSigma;
	uint Guided;
	uint2 LowResPad;
};

// The model sees an area-averaged proxy; depth and motion take the footprint's centre, since an average of either invents a surface.
[numthreads(8, 8, 1)] void Downsample(uint3 id : SV_DispatchThreadID) {
	if (id.x >= LowWidth || id.y >= LowHeight)
		return;
	const float2 uv = (float2(id.xy) + 0.5) / float2(LowWidth, LowHeight);
	const float2 quarter = 0.25 / float2(LowWidth, LowHeight);
	float4 sum = 0.0;
	[unroll] for (int i = 0; i < 4; ++i)
		sum += ProxyInput.SampleLevel(LinearClamp, uv + quarter * float2((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0), 0);
	LowColor[id.xy] = sum * 0.25;
	const int3 guide = int3(min(uint2(uv * float2(GuideWidth, GuideHeight)), uint2(GuideWidth - 1, GuideHeight - 1)), 0);
	LowDepth[id.xy] = DepthInput.Load(guide);
	LowMotion[id.xy] = MotionInput.Load(guide);
}

float LowLogLuma(float3 proxy)
{
	return log2(max(Color::RGBToLuminance(ProxySrgbToLinear(proxy), Luma), kLumaEpsilon));
}

// The composite takes only the model's luminance ratio, so the low-resolution gain is all that has to cross
// back: the full-resolution proxy scaled by it reads, to the composite, as a model frame of its own size.
[numthreads(8, 8, 1)] void ExpandGain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	const float3 currentLinear = ProxySrgbToLinear(ProxyInput[id.xy].rgb);
	const float currentLogLuma = log2(max(Color::RGBToLuminance(currentLinear, Luma), kLumaEpsilon));
	const float2 lowPosition = (float2(id.xy) + 0.5) / float2(Width, Height) * float2(LowWidth, LowHeight) - 0.5;
	const int2 base = int2(floor(lowPosition));
	const float2 fraction = lowPosition - float2(base);
	const int2 limit = int2(LowWidth - 1, LowHeight - 1);
	float gainSum = 0.0, weightSum = 0.0;
	[unroll] for (int i = 0; i < 4; ++i)
	{
		const int2 tap = clamp(base + int2(i & 1, i >> 1), int2(0, 0), limit);
		const float2 axis = float2((i & 1) ? fraction.x : 1.0 - fraction.x, (i & 2) ? fraction.y : 1.0 - fraction.y);
		const float inputLog = LowLogLuma(LowInput[tap].rgb);
		const float gain = LowLogLuma(LowOutput[tap].rgb) - inputLog;
		const float guide = Guided != 0 ? exp(-abs(currentLogLuma - inputLog) / max(GuideSigma, 1e-3)) : 1.0;
		const float weight = axis.x * axis.y * (guide + 1e-3);
		gainSum += gain * weight;
		weightSum += weight;
	}
	ExpandedProxy[id.xy] = float4(ProxyLinearToSrgb(currentLinear * exp2(gainSum / max(weightSum, 1e-6))), 1.0);
}
