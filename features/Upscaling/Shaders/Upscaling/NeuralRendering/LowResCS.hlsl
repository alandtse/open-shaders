#include "Common/NeuralRenderingFinite.hlsli"
#include "Common/NeuralRenderingReconstruction.hlsli"
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

bool ReadModelGain(int2 coordinate, out float gain, out float inputLog, out float3 inputLinear)
{
	const int2 tap = clamp(coordinate, int2(0, 0), int2(LowWidth - 1, LowHeight - 1));
	const float3 input = LowInput[tap].rgb;
	const float3 output = LowOutput[tap].rgb;
	inputLinear = ProxySrgbToLinear(input);
	inputLog = LowLogLuma(input);
	gain = LowLogLuma(output) - inputLog;
	return all(NeuralRenderingFinite::IsFinite(input)) && all(NeuralRenderingFinite::IsFinite(output)) && all(NeuralRenderingFinite::IsFinite(inputLinear)) && NeuralRenderingFinite::IsFinite(gain);
}

bool BilinearModelGain(float2 position, float3 reference, out float gain)
{
	const int2 base = int2(floor(position));
	const float2 phase = frac(position);
	const float4 spatial = float4((1.0 - phase.x) * (1.0 - phase.y), phase.x * (1.0 - phase.y), (1.0 - phase.x) * phase.y, phase.x * phase.y);
	float4 gains, inputLogs;
	float3 p00, p10, p01, p11;
	bool valid = ReadModelGain(base, gains.x, inputLogs.x, p00);
	valid = ReadModelGain(base + int2(1, 0), gains.y, inputLogs.y, p10) && valid;
	valid = ReadModelGain(base + int2(0, 1), gains.z, inputLogs.z, p01) && valid;
	valid = ReadModelGain(base + int2(1, 1), gains.w, inputLogs.w, p11) && valid;
	float4 weights = spatial;
#ifdef NR_RGB_GUIDED_RECONSTRUCTION
	NeuralRenderingReconstruction::RgbWeights(reference, p00, p10, p01, p11, spatial, Luma, weights);
#else
	if (Guided != 0) {
		const float referenceLog = log2(max(Color::RGBToLuminance(reference, Luma), kLumaEpsilon));
		weights *= exp(-abs(referenceLog - inputLogs) / max(GuideSigma, 1e-3)) + 1e-3;
	}
#endif
	gain = dot(gains, weights) / max(dot(weights, 1.0), NeuralRenderingReconstruction::kMinimumNormalization);
	return valid && NeuralRenderingFinite::IsFinite(gain);
}

// The composite takes only the model's luminance ratio, so the low-resolution gain is all that has to cross
// back: the full-resolution proxy scaled by it reads, to the composite, as a model frame of its own size.
[numthreads(8, 8, 1)] void ExpandGain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	const float4 source = ProxyInput[id.xy];
	const float3 currentLinear = ProxySrgbToLinear(source.rgb);
	const float2 lowPosition = (float2(id.xy) + 0.5) / float2(Width, Height) * float2(LowWidth, LowHeight) - 0.5;
	float gain;
	const bool valid = BilinearModelGain(lowPosition, currentLinear, gain);
	const float3 candidate = ProxyLinearToSrgb(currentLinear * exp2(gain));
	ExpandedProxy[id.xy] = NeuralRenderingFinite::ResolveHalfOutput(source, candidate, valid);
}
