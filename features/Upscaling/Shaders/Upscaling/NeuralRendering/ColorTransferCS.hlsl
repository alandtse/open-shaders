#include "Common/SceneExposure.hlsli"
#include "Common/SharedData.hlsli"
#include "Upscaling/NeuralRendering/ColorContract.hlsli"

// Production colour contract of the NR pass: Prepare builds the bounded display proxy the model
// consumes, StabilizeResidual temporally stabilizes the model's log2 tone residual, Composite
// applies the stabilized residual as a scalar gain in kMAIN's own domain.
cbuffer NRColor : register(b0)
{
	uint Width;
	uint Height;
	uint EyeOffsetX;
	uint HistoryValid;
	// 0 = ExposureScalar, 1 = the scene exposure in ExposureInput, 2 = the local estimate in it.
	uint ExposureMode;
	float ExposureScalar;
	float ExposureCompensation;
	float ExposureRangeMin;
	float ExposureRangeMax;
	float ToneMaxStops;
	float TemporalAlpha;
	float TemporalSigmaClip;
	float TemporalClamp;
	float DepthRejectThreshold;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> NeuralInput : register(t1);
Texture2D<float4> NeuralOutput : register(t2);
// Adaptation buffer in scene-exposure mode, the pass's own two-float state in local mode.
StructuredBuffer<float> ExposureInput : register(t3);
Texture2D<float> TemporalResidual : register(t4);
Texture2D<float2> NeuralMotion : register(t5);
Texture2D<float> NeuralDepth : register(t6);
Texture2D<float> NeuralDepthHistory : register(t7);
SamplerState TemporalSampler : register(s0);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> TemporalResidualOutput : register(u2);

static const uint kExposureScalar = 0;
static const uint kExposureScene = 1;
static const uint kExposureLocal = 2;

float ResolveExposure()
{
	float exposure = ExposureScalar;
	if (ExposureMode == kExposureScene)
		exposure = SceneExposure::Evaluate(ExposureInput[0], float2(ExposureRangeMin, ExposureRangeMax), ExposureCompensation);
	else if (ExposureMode == kExposureLocal)
		exposure = ExposureInput[1];
	return (isfinite(exposure) && exposure > 0.0) ? exposure : 1.0;
}

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 source = Original[id.xy + uint2(EyeOffsetX, 0)].rgb;
	float exposure = ResolveExposure();
	// The proxy is bounded to [0,1] by construction, so no finiteness test is needed on it.
	float3 proxy = NR::MakeDisplayProxy(max(NR::ToLinearNR(source), 0.0) * exposure);
	Output[id.xy] = float4(proxy, 1.0);
}

float ToneDeltaAt(int2 pixel)
{
	int2 limit = int2(max(Width, 1u) - 1, max(Height, 1u) - 1);
	pixel = clamp(pixel, int2(0, 0), limit);
	float3 input = NR::ProxySrgbToLinear(NeuralInput[pixel].rgb);
	float3 output = NR::ProxySrgbToLinear(NeuralOutput[pixel].rgb);
	float inputLuma = max(dot(input, NR::Luma), NR::kLumaFloor);
	float outputLuma = max(dot(output, NR::Luma), NR::kLumaFloor);
	return log2(outputLuma) - log2(inputLuma);
}

[numthreads(8, 8, 1)] void StabilizeResidual(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	int2 pixel = int2(id.xy);
	int2 limit = int2(max(Width, 1u) - 1, max(Height, 1u) - 1);
	float current = ToneDeltaAt(pixel);
	float neighborhoodSum = 0.0;
	float neighborhoodSquareSum = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			float neighbor = ToneDeltaAt(clamp(pixel + int2(x, y), int2(0, 0), limit));
			neighborhoodSum += neighbor;
			neighborhoodSquareSum += neighbor * neighbor;
		}
	}
	float neighborhoodMean = neighborhoodSum / 9.0;
	float neighborhoodVariance = max(neighborhoodSquareSum / 9.0 - neighborhoodMean * neighborhoodMean, 0.0);
	float clipRadius = max(TemporalSigmaClip * sqrt(neighborhoodVariance), TemporalClamp);

	float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
	float2 previousUV = uv + NeuralMotion[id.xy];
	bool valid = HistoryValid != 0 && all(previousUV > 0.0) && all(previousUV < 1.0);
	float history = TemporalResidual.SampleLevel(TemporalSampler, saturate(previousUV), 0);
	float currentDepth = NeuralDepth[id.xy];
	float previousDepth = NeuralDepthHistory.SampleLevel(TemporalSampler, saturate(previousUV), 0);
	float currentScreenDepth = SharedData::GetScreenDepth(currentDepth);
	float previousScreenDepth = SharedData::GetScreenDepth(previousDepth);
	float relativeDepthDelta = abs(currentScreenDepth - previousScreenDepth) /
	                           max(max(abs(currentScreenDepth), abs(previousScreenDepth)), NR::kDepthEpsilon);
	valid = valid && isfinite(history) && isfinite(currentDepth) && isfinite(previousDepth) &&
	        isfinite(relativeDepthDelta) && relativeDepthDelta <= DepthRejectThreshold;
	history = clamp(history, neighborhoodMean - clipRadius, neighborhoodMean + clipRadius);
	float stabilized = lerp(history, current, valid ? TemporalAlpha : 1.0);
	TemporalResidualOutput[id.xy] = isfinite(stabilized) ? stabilized : current;
}

	[numthreads(8, 8, 1)] void Composite(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height)
		return;
	uint2 sourcePixel = id.xy + uint2(EyeOffsetX, 0);
	float4 original = Original[sourcePixel];
	Output[id.xy] = original;
	if (!all(isfinite(original)) || !all(isfinite(NeuralOutput[id.xy].rgb)))
		return;
	float gain = NR::CompositeGain(TemporalResidual[id.xy], ToneMaxStops);
	Output[id.xy] = float4(original.rgb * gain, original.a);
}
