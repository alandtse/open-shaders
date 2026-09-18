#include "Common/Color.hlsli"

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
	float TonePadding;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> NeuralInput : register(t1);
Texture2D<float4> NeuralOutput : register(t2);
StructuredBuffer<float> Adaptation : register(t3);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> NeuralReactive : register(u1);

static const float3 Luma = float3(0.2126, 0.7152, 0.0722);

float3 ProxyLinearToSrgb(float3 value)
{
	value = saturate(value);
	return lerp(value * 12.92, 1.055 * pow(max(value, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, value));
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
	if (peak <= 1e-6)
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
	case 0:
		return value;
	case 1:
		return value;
	case 2:
		return ProxySrgbToLinear(value);
	case 3:
		return pow(max(value, 0.0), 2.2);
	case 4:
		return pow(saturate(value), 1.0 / 2.2);
	case 5:
		return ProxySrgbToLinear(value);
	case 6:
		return Color::SkyrimGammaToLinear(value);
	case 7:
		return value;
	case 8:
		return value;
	default:
		return ProductionToLinear(value);
	}
}

float3 FromLinear(float3 value)
{
	switch (ConversionMode) {
	case 0:
		return value;
	case 1:
		return ProxyLinearToSrgb(max(value, 0.0));
	case 3:
		return pow(max(value, 0.0), 1.0 / 2.2);
	case 4:
		return pow(saturate(value), 2.2);
	case 5:
		return ProxyLinearToSrgb(max(value, 0.0));
	case 6:
		return pow(max(value, 0.0), 1.0 / 2.2);
	case 7:
		return Color::LinearToSkyrimGamma(max(value, 0.0));
	case 8:
		return value;
	default:
		return ProductionFromLinear(value);
	}
}

float3 ProxyToLinear(float3 value)
{
	switch (ConversionMode) {
	case 0:
		return value;
	case 1:
		return value;
	case 2:
		return ProxySrgbToLinear(value);
	case 3:
		return pow(max(value, 0.0), 2.2);
	case 4:
		return pow(saturate(value), 1.0 / 2.2);
	case 5:
	case 6:
	case 7:
	case 8:
		return ProxySrgbToLinear(value);
	default:
		return ProxySrgbToLinear(value);
	}
}

float3 MakeDisplayProxy(float3 linearColor)
{
	return ProxyLinearToSrgb(NeutwoEncode(linearColor));
}

float ToneDeltaAt(int2 pixel)
{
	int2 limit = int2(max(Width, 1u) - 1, max(Height, 1u) - 1);
	pixel = clamp(pixel, int2(0, 0), limit);
	float3 input = ProxySrgbToLinear(NeuralInput[pixel].rgb);
	float3 output = ProxySrgbToLinear(NeuralOutput[pixel].rgb);
	float inputLuma = max(dot(input, Luma), 1e-5);
	float outputLuma = max(dot(output, Luma), 1e-5);
	return log2(outputLuma) - log2(inputLuma);
}

float ToneLowAt(int2 pixel, float centerDelta)
{
	float radius = ToneRadius;
	if (radius <= 0.01)
		return centerDelta;
	float3 center = ProxySrgbToLinear(NeuralInput[clamp(pixel, int2(0, 0), int2(max(Width, 1u) - 1, max(Height, 1u) - 1))].rgb);
	float centerLuma = max(dot(center, Luma), 1e-5);
	float weighted = 0.0;
	float weightSum = 0.0;
	for (int y = -2; y <= 2; ++y) {
		for (int x = -2; x <= 2; ++x) {
			float distance = float(x * x + y * y);
			float spatial = exp(-distance / max(2.0 * radius * radius, 1e-4));
			int2 samplePixel = pixel + int2(x, y);
			int2 limit = int2(max(Width, 1u) - 1, max(Height, 1u) - 1);
			samplePixel = clamp(samplePixel, int2(0, 0), limit);
			float3 sample = ProxySrgbToLinear(NeuralInput[samplePixel].rgb);
			float sampleLuma = max(dot(sample, Luma), 1e-5);
			float edge = exp(-abs(log2(sampleLuma) - log2(centerLuma)) * 2.0);
			float weight = spatial * edge;
			weighted += ToneDeltaAt(samplePixel) * weight;
			weightSum += weight;
		}
	}
	return weightSum > 1e-5 ? weighted / weightSum : centerDelta;
}

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 source = Original[id.xy + uint2(EyeOffsetX, 0)].rgb;
	float exposure = ManualExposure;
	if (ExposureMode == 1 || ExposureMode == 2 || ExposureMode == 7)
		exposure = 1.0;
	if (HasExposure != 0 && (ExposureMode == 0 || ExposureMode == 3 || ExposureMode == 5 || ExposureMode == 6)) {
		float average = Adaptation[0];
		if (isfinite(average) && average > 0.0)
			exposure *= 0.18 * ExposureCompensation / clamp(average, ExposureMin, ExposureMax);
	}
	if (ExposureMode == 5)
		exposure = 1.0 / max(exposure, 1.0 / 65536.0);
	else if (ExposureMode == 4)
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
	NeuralReactive[sourcePixel] = 0.0;
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
	if (ExposureMode == 0 || ExposureMode == 3 || ExposureMode == 5 || ExposureMode == 6)
		exposure = max(exposure, 1.0 / 65536.0);
	if (ExposureMode == 1 || ExposureMode == 2 || ExposureMode == 7)
		exposure = 1.0;
	if (MaskMode == 1)
		NeuralReactive[sourcePixel] = 0.0;
	else if (MaskMode == 2)
		NeuralReactive[sourcePixel] = 1.0;
	const float ratioFloor = 1.0 / 512.0;
	float inputLuminance = dot(inputProxy, Luma);
	float neuralLuminance = dot(neuralProxy, Luma);
	float ratio = (neuralLuminance + ratioFloor) / (inputLuminance + ratioFloor);
	const float ratioLimit = 2.0;
	float lift = lerp(1.0, ratioLimit, smoothstep(0.0, 8.0 * ratioFloor, inputLuminance));
	float drop = lerp(1.0 / ratioLimit, 1.0, smoothstep(0.6, 1.0, inputLuminance));
	ratio = clamp(ratio, drop, lift);
	float mask = MaskMode == 1 ? 0.0 : (MaskMode == 2 ? 1.0 : saturate(4.0 * abs(ratio - 1.0)));
	float3 originalLinear = max(ToLinear(original.rgb), 0.0);
	float3 neuralLinear = max(ProxyToLinear(rawNeural), 0.0);
	float toneDelta = log2(max(neuralLuminance, ratioFloor)) - log2(max(inputLuminance, ratioFloor));
	float toneLow = ToneLowAt(int2(id.xy), toneDelta);
	float toneHigh = toneDelta - toneLow;
	float tone = toneLow * ToneLowStrength + toneHigh * ToneHighStrength;
	float toneGain = exp2(tone);
	float sceneLuminance = dot(originalLinear, Luma);
	float logSceneLuminance = log2(max(sceneLuminance, ratioFloor));
	float shadowWeight = smoothstep(-8.0, -3.0, logSceneLuminance);
	float highlightWeight = 1.0 - smoothstep(0.0, 2.0, logSceneLuminance);
	float protectionWeight = (1.0 - DynamicRangeProtect.x * (1.0 - shadowWeight)) *
	                         (1.0 - DynamicRangeProtect.y * (1.0 - highlightWeight));
	float3 result = originalLinear * ratio;
	if (CompositeMode == 0 || CompositeMode == 5 || CompositeMode == 7)
		result = originalLinear * exp2(tone * saturate(protectionWeight));
	if (CompositeMode == 1)
		result = neuralLinear;
	else if (CompositeMode == 2)
		result = lerp(originalLinear, neuralLinear, mask);
	else if (CompositeMode == 3)
		result = lerp(originalLinear, neuralLinear, mask * 0.5);
	else if (CompositeMode == 4)
		result = neuralLinear * (dot(originalLinear, Luma) / max(dot(neuralLinear, Luma), ratioFloor));
	else if (CompositeMode == 5)
		result = originalLinear * ratio;
	else if (CompositeMode == 6)
		result = originalLinear + (neuralLinear - inputProxy) * mask;
	else if (CompositeMode == 7)
		result = originalLinear * ratio;
	if (VisualMode == 1)
		result = inputProxy;
	else if (VisualMode == 2)
		result = neuralLinear;
	else if (VisualMode == 3)
		result = abs(neuralLinear - inputProxy) * DifferenceStrength;
	else if (VisualMode == 4)
		result = ratio.xxx;
	else if (VisualMode == 5)
		result = originalLinear;
	else if (VisualMode == 6)
		result = result;
	else if (VisualMode == 7)
		result = abs(dot(neuralLinear - inputProxy, Luma)).xxx * DifferenceStrength;
	else if (VisualMode == 8)
		result = abs(neuralLinear - inputProxy).xxx * DifferenceStrength;
	else if (VisualMode == 9)
		result = mask.xxx;
	else if (VisualMode == 10)
		result = exposure.xxx;
	else if (VisualMode == 15)
		result = (log2(max(neuralLuminance, ratioFloor)) - log2(max(inputLuminance, ratioFloor))).xxx * DifferenceStrength;
	else if (VisualMode == 16)
		result = toneDelta.xxx * DifferenceStrength;
	else if (VisualMode == 17)
		result = toneLow.xxx * DifferenceStrength;
	else if (VisualMode == 18)
		result = toneHigh.xxx * DifferenceStrength;
	else if (VisualMode == 19)
		result = toneGain.xxx;
	else if (VisualMode == 20)
		result = (dot(result, Luma) / max(sceneLuminance, ratioFloor)).xxx;
	else if (VisualMode >= 11) {
		const bool left = (float(id.x) / max(1.0, float(Width))) < SplitPosition;
		if (VisualMode == 11)
			result = left ? originalLinear : neuralLinear;
		else if (VisualMode == 12)
			result = left ? originalLinear : result;
		else if (VisualMode == 13)
			result = left ? inputProxy : neuralLinear;
		else
			result = left ? originalLinear : result;
	}
	if (!all(isfinite(result)))
		return;
	if (VisualMode == 0)
		result = FromLinear(result);
	Output[id.xy] = float4(result, original.a);
	NeuralReactive[sourcePixel] = mask;
}
