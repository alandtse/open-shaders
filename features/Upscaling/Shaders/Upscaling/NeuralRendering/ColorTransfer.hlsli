#ifndef __NR_COLOR_TRANSFER_HLSLI__
#define __NR_COLOR_TRANSFER_HLSLI__

#include "Common/Color.hlsli"
#include "Common/RegionFeather.hlsli"

// Shared by ColorTransferCS.hlsl and GainReuseCS.hlsl: the stagger's gain store has to produce exactly
// the tone delta the composite would have computed, so both read one definition of it.
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
	uint RegionBaseX;
	uint RegionBaseY;
	uint RegionWidth;
	uint RegionHeight;
	uint RegionOverlayEnabled;
	float RegionOutlineThickness;
	uint RegionActorBaseX;
	uint RegionActorBaseY;
	uint RegionActorWidth;
	uint RegionActorHeight;
	// Tone multiplier per category, Skin..Landscape in .x; 16-byte rows mirror the C++ struct.
	float4 CategoryStrength[5];
	uint MaterialMapEnabled;
	uint MaterialMapMode;
	uint MaterialMapFilter;
	uint MaterialMapStrengthBound;
	float4 MaterialStrengthsA;  // None, Skin, Hair, Eyes
	float4 MaterialStrengthsB;  // Foliage, Landscape, Cloth, Metal
};

static const float3 Luma = Color::kRec709LuminanceWeights;
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

static const float kRegionFeatherDefault = 32.0;
static const float kRegionFeatherMin = 16.0;
static const float kRegionFeatherMax = 96.0;

float RegionWeight(int2 pixel)
{
	float weight = 1.0;
	if (RegionWidth != 0) {
		const float4 crop = float4(RegionBaseX, RegionBaseY, RegionBaseX + RegionWidth, RegionBaseY + RegionHeight);
		const float4 subject = float4(RegionActorBaseX, RegionActorBaseY, RegionActorBaseX + RegionActorWidth, RegionActorBaseY + RegionActorHeight);
		weight = RegionFeather::Weight(float2(pixel) + 0.5, crop, subject, float2(Width, Height),
			kRegionFeatherDefault, kRegionFeatherMin, kRegionFeatherMax);
	}
	return weight;
}

// Weight 0 must return the input exactly: lerp propagates a NaN neural sample even at t = 0.
float3 RegionStableNeuralSample(int2 pixel, float3 inputSample, float3 neuralSample)
{
	float3 result = neuralSample;
	if (RegionWidth != 0) {
		const float weight = RegionWeight(pixel);
		result = weight <= 0.0 ? inputSample : lerp(inputSample, neuralSample, weight);
	}
	return result;
}

#endif  // __NR_COLOR_TRANSFER_HLSLI__
