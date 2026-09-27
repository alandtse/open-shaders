#include "Common/Color.hlsli"
#include "Common/SceneExposure.hlsli"
#include "Upscaling/NeuralRendering/ColorContract.hlsli"
#include "Upscaling/NeuralRendering/ModeValues.hlsli"

cbuffer ColorTransfer : register(b0)
{
	uint Width;
	uint Height;
	uint EyeOffsetX;
	uint HasExposure;
	uint ConversionMode;
	uint ExposureMode;
	uint CompositeMode;
	uint MaskMode;
	uint VisualMode;
	float ExposureCompensation;
	float ExposureMin;
	float ExposureMax;
	float ManualExposure;
	float DifferenceStrength;
	float SplitPosition;
	float4 DynamicRangeProtect;
	float ToneLowStrength;
	float ToneRadius;
	float ToneHighStrength;
	uint HasToneData;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> NeuralInput : register(t1);
Texture2D<float4> NeuralOutput : register(t2);
StructuredBuffer<float> Adaptation : register(t3);
Texture2D<float2> ToneData : register(t4);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float2> ToneDataOutput : register(u2);

static const float3 Luma = float3(0.2126, 0.7152, 0.0722);
static const float kProxyEpsilon = 1e-8;
static const float kPeakEpsilon = 1e-6;
static const float kLumaEpsilon = 1e-5;
static const float kWeightEpsilon = 1e-5;
static const float kSpatialEpsilon = 1e-4;

float3 ProxyLinearToSrgb(float3 value)
{
	value = saturate(value);
	return lerp(value * 12.92, 1.055 * pow(max(value, kProxyEpsilon), 1.0 / 2.4) - 0.055, step(0.0031308, value));
}

float3 ProxySrgbToLinear(float3 value)
{
	value = saturate(value);
	return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4), step(0.04045, value));
}

float3 NeutwoEncode(float3 value)
{
	value = max(value, 0.0);
	float peak = max(value.r, max(value.g, value.b));
	if (peak <= kPeakEpsilon)
		return value;
	return value * ((peak * rsqrt(peak * peak + 1.0)) / peak);
}

float3 ProductionToLinear(float3 nativeColor)
{
	float3 linearColor = ENABLE_LL ? nativeColor : Color::SkyrimGammaToLinear(nativeColor);
	return (ENABLE_LL && ENABLE_ACEScg) ? AP1TosRGB(linearColor) : linearColor;
}

float3 ProductionFromLinear(float3 linearColor)
{
	float3 working = (ENABLE_LL && ENABLE_ACEScg) ? sRGBToAP1(linearColor) : linearColor;
	return ENABLE_LL ? working : Color::LinearToSkyrimGamma(max(working, 0.0));
}

float3 ToLinear(float3 value)
{
	switch (ConversionMode) {
	case NR::kConversionRaw:
		return value;
	case NR::kConversionLinearToSRGB:
		return value;
	case NR::kConversionSRGBToLinear:
		return ProxySrgbToLinear(value);
	case NR::kConversionLinearToGamma22:
		return pow(max(value, 0.0), 2.2);
	case NR::kConversionGamma22ToLinear:
		return pow(saturate(value), 1.0 / 2.2);
	case NR::kConversionSRGBToGamma22:
		return ProxySrgbToLinear(value);
	case NR::kConversionSkyrimGammaToGamma22:
		return Color::SkyrimGammaToLinear(value);
	case NR::kConversionLinearToSkyrimGamma:
		return value;
	case NR::kConversionLinearToLinear:
		return value;
	default:
		return ProductionToLinear(value);
	}
}

float3 FromLinear(float3 value)
{
	switch (ConversionMode) {
	case NR::kConversionRaw:
		return value;
	case NR::kConversionLinearToSRGB:
		return ProxyLinearToSrgb(max(value, 0.0));
	case NR::kConversionLinearToGamma22:
		return pow(max(value, 0.0), 1.0 / 2.2);
	case NR::kConversionGamma22ToLinear:
		return pow(saturate(value), 2.2);
	case NR::kConversionSRGBToGamma22:
		return ProxyLinearToSrgb(max(value, 0.0));
	case NR::kConversionSkyrimGammaToGamma22:
		return pow(max(value, 0.0), 1.0 / 2.2);
	case NR::kConversionLinearToSkyrimGamma:
		return Color::LinearToSkyrimGamma(max(value, 0.0));
	case NR::kConversionLinearToLinear:
		return value;
	default:
		return ProductionFromLinear(value);
	}
}

float3 ProxyToLinear(float3 value)
{
	switch (ConversionMode) {
	case NR::kConversionRaw:
		return value;
	case NR::kConversionLinearToSRGB:
		return value;
	case NR::kConversionSRGBToLinear:
		return ProxySrgbToLinear(value);
	case NR::kConversionLinearToGamma22:
		return pow(max(value, 0.0), 2.2);
	case NR::kConversionGamma22ToLinear:
		return pow(saturate(value), 1.0 / 2.2);
	case NR::kConversionSRGBToGamma22:
	case NR::kConversionSkyrimGammaToGamma22:
	case NR::kConversionLinearToSkyrimGamma:
	case NR::kConversionLinearToLinear:
		return ProxySrgbToLinear(value);
	default:
		return ProxySrgbToLinear(value);
	}
}

float3 MakeDisplayProxy(float3 linearColor)
{
	return ProxyLinearToSrgb(NeutwoEncode(linearColor));
}

[numthreads(8, 8, 1)] void PrepareToneData(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 input = ProxySrgbToLinear(NeuralInput[id.xy].rgb);
	float3 output = ProxySrgbToLinear(NeuralOutput[id.xy].rgb);
	float inputLuma = max(Color::RGBToLuminance(input, Luma), kLumaEpsilon);
	float outputLuma = max(Color::RGBToLuminance(output, Luma), kLumaEpsilon);
	float logInput = log2(inputLuma);
	ToneDataOutput[id.xy] = float2(logInput, log2(outputLuma) - logInput);
}

float ToneLowAt(int2 pixel, float centerDelta)
{
	float radius = ToneRadius;
	if (radius <= 0.01)
		return centerDelta;
	int2 limit = int2(max(Width, 1u) - 1, max(Height, 1u) - 1);
	float centerLogLuma = ToneData[pixel].x;
	float weighted = 0.0;
	float weightSum = 0.0;
	for (int y = -2; y <= 2; ++y) {
		for (int x = -2; x <= 2; ++x) {
			float distance = float(x * x + y * y);
			float spatial = exp(-distance / max(2.0 * radius * radius, kSpatialEpsilon));
			int2 samplePixel = clamp(pixel + int2(x, y), int2(0, 0), limit);
			float2 sample = ToneData[samplePixel];
			float edge = exp(-abs(sample.x - centerLogLuma) * 2.0);
			float weight = spatial * edge;
			weighted += sample.y * weight;
			weightSum += weight;
		}
	}
	return weightSum > kWeightEpsilon ? weighted / weightSum : centerDelta;
}

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 source = Original[id.xy + uint2(EyeOffsetX, 0)].rgb;
	float exposure = ManualExposure;
	if (ExposureMode == NR::kExposureIgnore || ExposureMode == NR::kExposureForceOne || ExposureMode == NR::kExposureDoNotPass)
		exposure = 1.0;
	if (HasExposure != 0 && (ExposureMode == NR::kExposureProduction || ExposureMode == NR::kExposureGame || ExposureMode == NR::kExposureDeExposeReExpose || ExposureMode == NR::kExposurePassOnly)) {
		float average = Adaptation[0];
		if (isfinite(average) && average > 0.0)
			exposure *= SceneExposure::Evaluate(average, float2(ExposureMin, ExposureMax), ExposureCompensation);
	}
	if (ExposureMode == NR::kExposureDeExposeReExpose)
		exposure = 1.0 / max(exposure, 1.0 / 65536.0);
	else if (ExposureMode == NR::kExposureManual)
		exposure = ManualExposure;
	exposure = isfinite(exposure) && exposure > 0.0 ? exposure : 1.0;
	float3 proxy = MakeDisplayProxy(max(ToLinear(source), 0.0) * exposure);
	Output[id.xy] = float4(all(isfinite(proxy)) ? proxy : 0.0, 1.0);
}

	[numthreads(8, 8, 1)] void Composite(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height)
		return;
	uint2 sourcePixel = id.xy + uint2(EyeOffsetX, 0);
	float4 original = Original[sourcePixel];
	float3 rawNeural = NeuralOutput[id.xy].rgb;
	if (!all(isfinite(original))) {
		Output[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	Output[id.xy] = original;
	if (!all(isfinite(rawNeural)))
		return;
	float3 inputProxy = ProxyToLinear(NeuralInput[id.xy].rgb);
	float3 neuralProxy = ProxyToLinear(rawNeural);
	float exposure = ManualExposure;
	if (ExposureMode == NR::kExposureProduction || ExposureMode == NR::kExposureGame || ExposureMode == NR::kExposureDeExposeReExpose || ExposureMode == NR::kExposurePassOnly)
		exposure = max(exposure, 1.0 / 65536.0);
	if (ExposureMode == NR::kExposureIgnore || ExposureMode == NR::kExposureForceOne || ExposureMode == NR::kExposureDoNotPass)
		exposure = 1.0;
	const float ratioFloor = 1.0 / 512.0;
	float inputLuminance = Color::RGBToLuminance(inputProxy, Luma);
	float neuralLuminance = Color::RGBToLuminance(neuralProxy, Luma);
	float ratio = (neuralLuminance + ratioFloor) / (inputLuminance + ratioFloor);
	const float ratioLimit = 2.0;
	float lift = lerp(1.0, ratioLimit, smoothstep(0.0, 8.0 * ratioFloor, inputLuminance));
	float drop = lerp(1.0 / ratioLimit, 1.0, smoothstep(0.6, 1.0, inputLuminance));
	ratio = clamp(ratio, drop, lift);
	float mask = MaskMode == NR::kMaskForceZero ? 0.0 : (MaskMode == NR::kMaskForceOne ? 1.0 : saturate(4.0 * abs(ratio - 1.0)));
	float3 originalLinear = max(ToLinear(original.rgb), 0.0);
	float3 neuralLinear = max(ProxyToLinear(rawNeural), 0.0);
	float toneDelta = log2(max(neuralLuminance, ratioFloor)) - log2(max(inputLuminance, ratioFloor));
	float toneLow = toneDelta;
	[branch] if (HasToneData != 0)
		toneLow = ToneLowAt(int2(id.xy), toneDelta);
	float toneHigh = toneDelta - toneLow;
	float tone = ToneLowStrength == ToneHighStrength ? toneDelta * ToneHighStrength :
	                                                   toneLow * ToneLowStrength + toneHigh * ToneHighStrength;
	float toneGain = exp2(tone);
	float sceneLuminance = Color::RGBToLuminance(originalLinear, Luma);
	float logSceneLuminance = log2(max(sceneLuminance, ratioFloor));
	float shadowWeight = smoothstep(-8.0, -3.0, logSceneLuminance);
	float highlightWeight = 1.0 - smoothstep(0.0, 2.0, logSceneLuminance);
	float protectionWeight = (1.0 - DynamicRangeProtect.x * (1.0 - shadowWeight)) *
	                         (1.0 - DynamicRangeProtect.y * (1.0 - highlightWeight));
	float3 result = originalLinear * ratio;
	const bool boundedGain = CompositeMode == NR::kCompositeProduction;
	if (boundedGain)
		result = original.rgb * NR::CompositeGain(tone * saturate(protectionWeight), NR::kMaxToneStops);
	if (CompositeMode == NR::kCompositeReplacement)
		result = neuralLinear;
	else if (CompositeMode == NR::kCompositeMaskedLerp)
		result = lerp(originalLinear, neuralLinear, mask);
	else if (CompositeMode == NR::kCompositeHalfMaskedLerp)
		result = lerp(originalLinear, neuralLinear, mask * 0.5);
	else if (CompositeMode == NR::kCompositePreserveLuminance)
		result = neuralLinear * (Color::RGBToLuminance(originalLinear, Luma) / max(Color::RGBToLuminance(neuralLinear, Luma), ratioFloor));
	else if (CompositeMode == NR::kCompositePreserveRatio)
		result = originalLinear * ratio;
	else if (CompositeMode == NR::kCompositeResidual)
		result = originalLinear + (neuralLinear - inputProxy) * mask;
	else if (CompositeMode == NR::kCompositeRatio)
		result = originalLinear * ratio;
	if (VisualMode == NR::kVisualInput)
		result = inputProxy;
	else if (VisualMode == NR::kVisualOutput)
		result = neuralLinear;
	else if (VisualMode == NR::kVisualDifference)
		result = abs(neuralLinear - inputProxy) * DifferenceStrength;
	else if (VisualMode == NR::kVisualRatio)
		result = ratio.xxx;
	else if (VisualMode == NR::kVisualOriginal)
		result = originalLinear;
	else if (VisualMode == NR::kVisualPostComposite)
		result = result;
	else if (VisualMode == NR::kVisualLuminanceDifference)
		result = abs(Color::RGBToLuminance(neuralLinear - inputProxy, Luma)).xxx * DifferenceStrength;
	else if (VisualMode == NR::kVisualChromaDifference)
		result = abs(neuralLinear - inputProxy).xxx * DifferenceStrength;
	else if (VisualMode == NR::kVisualMask)
		result = mask.xxx;
	else if (VisualMode == NR::kVisualExposure)
		result = exposure.xxx;
	else if (VisualMode == NR::kVisualLogRatio)
		result = (log2(max(neuralLuminance, ratioFloor)) - log2(max(inputLuminance, ratioFloor))).xxx * DifferenceStrength;
	else if (VisualMode == NR::kVisualToneDelta)
		result = toneDelta.xxx * DifferenceStrength;
	else if (VisualMode == NR::kVisualToneLow)
		result = toneLow.xxx * DifferenceStrength;
	else if (VisualMode == NR::kVisualToneHigh)
		result = toneHigh.xxx * DifferenceStrength;
	else if (VisualMode == NR::kVisualToneLowGain)
		result = toneGain.xxx;
	else if (VisualMode == NR::kVisualFinalLuminanceRatio)
		result = (Color::RGBToLuminance(result, Luma) / max(sceneLuminance, ratioFloor)).xxx;
	else if (VisualMode >= NR::kVisualSplitOriginalOutput) {
		const bool left = (float(id.x) / max(1.0, float(Width))) < SplitPosition;
		if (VisualMode == NR::kVisualSplitOriginalOutput)
			result = left ? originalLinear : neuralLinear;
		else if (VisualMode == NR::kVisualSplitOriginalComposite)
			result = left ? originalLinear : result;
		else if (VisualMode == NR::kVisualSplitInputOutput)
			result = left ? inputProxy : neuralLinear;
		else
			result = left ? originalLinear : result;
	}
	if (!all(isfinite(result)))
		return;
	if (VisualMode == NR::kVisualNone && !boundedGain)
		result = FromLinear(result);
	Output[id.xy] = float4(result, original.a);
}
