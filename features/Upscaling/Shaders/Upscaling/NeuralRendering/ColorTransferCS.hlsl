#include "Common/Color.hlsli"

cbuffer ColorTransfer : register(b0)
{
	uint Width;
	uint Height;
	uint EyeOffsetX;
	uint HasExposure;
	float ExposureCompensation;
	float ExposureMin;
	float ExposureMax;
	float ManualExposure;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> NeuralInput : register(t1);
Texture2D<float4> NeuralOutput : register(t2);
StructuredBuffer<float> Adaptation : register(t3);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> NeuralReactive : register(u1);

static const float3 Luma = float3(0.2126, 0.7152, 0.0722);

float3 ToLinear(float3 nativeColor)
{
	float3 linearColor = ENABLE_LL ? nativeColor : Color::SkyrimGammaToLinear(nativeColor);
	return (ENABLE_LL && ENABLE_ACEScg) ? AP1TosRGB(linearColor) : linearColor;
}

float3 FromLinear(float3 linearColor)
{
	float3 working = (ENABLE_LL && ENABLE_ACEScg) ? sRGBToAP1(linearColor) : linearColor;
	return ENABLE_LL ? working : Color::LinearToSkyrimGamma(max(working, 0.0));
}

float3 MakeDisplayProxy(float3 linearColor)
{
	float luminance = dot(linearColor, Luma);
	if (luminance > 0.75) {
		float rolled = 0.75 + 0.25 * (1.0 - exp(-(luminance - 0.75) / 0.25));
		linearColor *= rolled / luminance;
	}
	float peak = max(linearColor.r, max(linearColor.g, linearColor.b));
	if (peak > 1.0)
		linearColor /= peak;
	return saturate(Color::LinearToSrgb(linearColor));
}

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 source = Original[id.xy + uint2(EyeOffsetX, 0)].rgb;
	float exposure = ManualExposure;
	if (HasExposure != 0) {
		float average = Adaptation[0];
		if (isfinite(average) && average > 0.0)
			exposure *= 0.18 * ExposureCompensation / clamp(average, ExposureMin, ExposureMax);
	}
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
	float3 inputProxy = Color::SrgbToLinear(saturate(NeuralInput[id.xy].rgb));
	float3 neuralProxy = Color::SrgbToLinear(saturate(rawNeural));
	const float ratioFloor = 1.0 / 512.0;
	float inputLuminance = dot(inputProxy, Luma);
	float neuralLuminance = dot(neuralProxy, Luma);
	float ratio = (neuralLuminance + ratioFloor) / (inputLuminance + ratioFloor);
	const float ratioLimit = 2.0;
	float lift = lerp(1.0, ratioLimit, smoothstep(0.0, 8.0 * ratioFloor, inputLuminance));
	float drop = lerp(1.0 / ratioLimit, 1.0, smoothstep(0.6, 1.0, inputLuminance));
	ratio = clamp(ratio, drop, lift);
	float3 result = FromLinear(max(ToLinear(original.rgb), 0.0) * ratio);
	if (!all(isfinite(result)))
		return;
	Output[id.xy] = float4(result, original.a);
	NeuralReactive[sourcePixel] = saturate(4.0 * abs(ratio - 1.0));
}
