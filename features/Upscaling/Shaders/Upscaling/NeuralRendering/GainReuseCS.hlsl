#include "Common/NeuralRenderingReuse.hlsli"
#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

Texture2D<float4> ProxyInput : register(t0);
Texture2D<float4> NeuralOutput : register(t1);
Texture2D<float> GuideDepth : register(t2);
Texture2D<float2> MotionGuide : register(t3);
Texture2D<float2> GainHistory : register(t4);
Texture2D<float> DepthHistory : register(t5);
SamplerState LinearClamp : register(s0);

RWTexture2D<float2> GainHistoryOutput : register(u0);
RWTexture2D<float> DepthHistoryOutput : register(u1);
RWTexture2D<float4> ReuseOutput : register(u2);

Texture2D<float2> OtherGainHistory : register(t12);
Texture2D<float> OtherDepthHistory : register(t13);

cbuffer GainReuse : register(b1)
{
	float4 CameraData;
	uint DepthReversed;
	uint CrossEye;
	uint2 GainReusePad;
	row_major float4x4 StereoMap;
};

// Mirrors PrepareToneData tap for tap, so a reused delta is the one the composite would have computed.
[numthreads(8, 8, 1)] void StoreGain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	const float3 inputSample = ProxyInput[id.xy].rgb;
	const float3 outputSample = RegionStableNeuralSample(int2(id.xy), inputSample, NeuralOutput[id.xy].rgb);
	const float3 input = ProxySrgbToLinear(inputSample);
	const float3 output = ProxySrgbToLinear(outputSample);
	const float inputLuma = max(Color::RGBToLuminance(input, Luma), kLumaEpsilon);
	const float outputLuma = max(Color::RGBToLuminance(output, Luma), kLumaEpsilon);
	const float logInput = log2(inputLuma);
	GainHistoryOutput[id.xy] = float2(logInput, log2(outputLuma) - logInput);
	DepthHistoryOutput[id.xy] = GuideDepth[id.xy];
}

	[numthreads(8, 8, 1)] void ReuseGain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height)
		return;
	const float3 currentProxy = ProxyInput[id.xy].rgb;
	const float3 currentLinear = ProxySrgbToLinear(currentProxy);
	const float currentLogLuma = log2(max(Color::RGBToLuminance(currentLinear, Luma), kLumaEpsilon));
	// The guide holds the engine's per-eye normalized UV displacement, current to previous, so adding it
	// to the current UV lands on the pixel the gain was stored at.
	const float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
	const float2 previousUV = uv + MotionGuide[id.xy];
	float toneDelta = 0.0;
	bool found = false;
	if (all(previousUV > 0.0) && all(previousUV < 1.0)) {
		const int2 previousPixel = clamp(int2(previousUV * float2(Width, Height)), int2(0, 0), int2(int(Width) - 1, int(Height) - 1));
		const float2 history = GainHistory.Load(int3(previousPixel, 0));
		const float historyDepth = DepthHistory.Load(int3(previousPixel, 0));
		found = NeuralRenderingReuse::SampleAgrees(true, GuideDepth[id.xy], historyDepth, CameraData, DepthReversed != 0, currentLogLuma, history.x);
		if (found)
			toneDelta = history.y;
	}
#ifdef VR
	// A surface that just came into view has no earlier gain, but the other eye's model frame saw it this frame.
	if (!found && CrossEye != 0) {
		const float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), GuideDepth[id.xy], 1.0);
		float4 other = mul(StereoMap, clip);
		other /= other.w;
		const float2 otherUV = other.xy * float2(0.5, -0.5) + 0.5;
		if (all(otherUV > 0.0) && all(otherUV < 1.0)) {
			const int3 otherPixel = int3(clamp(int2(otherUV * float2(Width, Height)), int2(0, 0), int2(int(Width) - 1, int(Height) - 1)), 0);
			const float2 otherGain = OtherGainHistory.Load(otherPixel);
			if (NeuralRenderingReuse::SampleAgrees(true, other.z, OtherDepthHistory.Load(otherPixel), CameraData, DepthReversed != 0, currentLogLuma, otherGain.x))
				toneDelta = otherGain.y;
		}
	}
#endif
	ReuseOutput[id.xy] = float4(ProxyLinearToSrgb(NeuralRenderingReuse::ApplyGainLinear(currentLinear, toneDelta)), 1.0);
}
