#include "Common/FoveatedMask.hlsli"

// Custom Community Shaders VR periphery-only TAA.
// Source lineage and required attribution for adapted MIT-licensed ideas:
// - Godot Engine / Spartan Engine TAA lineage:
//   godotengine/godot :: servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl
//   (Godot documents this path as based on the old Spartan Engine TAA implementation.)
// - AMD FidelityFX FSR2 / FSR3 temporal heuristics:
//   GPUOpen-Effects/FidelityFX-FSR2 and GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
//   for lock, reactivity, and luminance-instability style history management.
// - General temporal AA background:
//   Lei Yang, Shiqiu Liu, Marco Salvi, "A Survey of Temporal Antialiasing Techniques", Computer Graphics Forum, 2020.
// This shader is an original periphery-only implementation for Community Shaders VR and is not a verbatim copy.

cbuffer PeripheryTAACB : register(b0)
{
	float2 OutputDim;
	float2 InvOutputDim;
	float2 InputDim;
	float2 InvInputDim;
	float2 DispatchDim;
	float2 OutputOffset;
	float2 Jitter;
	float2 CenterOffset;
	float4 Tuning0;  // x=centerScale, y=centerFeather, z=resetHistory, w=showDebug
	float4 Tuning1;  // x=disableLocks, y=disableReactivity, z=disableInstability, w=historyValid
	float4 Tuning2;  // x=reactivityScale, y=instabilityScale, z=velocityScale, w=lockDecay
};

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float2> CurrentMotionVectors : register(t2);
Texture2D<float> CurrentReactiveMask : register(t3);
Texture2D<float> CurrentTransparencyMask : register(t4);
Texture2D<float4> HistoryColor : register(t5);
Texture2D<float2> HistoryVelocity : register(t6);
Texture2D<float> HistoryLock : register(t7);

SamplerState LinearSampler : register(s0);

RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float2> OutVelocity : register(u1);
RWTexture2D<float> OutLock : register(u2);

static const int2 kOffsets3x3[9] = {
	int2(-1, -1), int2(0, -1), int2(1, -1),
	int2(-1, 0),  int2(0, 0),  int2(1, 0),
	int2(-1, 1),  int2(0, 1),  int2(1, 1)
};

float3 Reinhard(float3 color)
{
	return color / (1.0 + color);
}

float3 ReinhardInverse(float3 color)
{
	return color / max(1.0 - color, 1e-4);
}

float Luma(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float2 ClampInputUV(float2 uv)
{
	float2 halfTexel = InvInputDim * 0.5;
	return clamp(uv, halfTexel, 1.0 - halfTexel);
}

float2 ClampHistoryUV(float2 uv)
{
	float2 halfTexel = InvOutputDim * 0.5;
	return clamp(uv, halfTexel, 1.0 - halfTexel);
}

uint2 ToInputPos(float2 uv)
{
	float2 clamped = ClampInputUV(uv);
	return min((uint2)floor(clamped * InputDim), uint2(InputDim) - 1);
}

uint2 ToHistoryPos(float2 uv)
{
	float2 clamped = ClampHistoryUV(uv);
	return min((uint2)floor(clamped * OutputDim), uint2(OutputDim) - 1);
}

float LoadDepthClamped(int2 pos)
{
	int2 clamped = clamp(pos, int2(0, 0), int2(InputDim) - 1);
	return CurrentDepth.Load(int3(clamped, 0));
}

float2 LoadMotionClamped(int2 pos)
{
	int2 clamped = clamp(pos, int2(0, 0), int2(InputDim) - 1);
	return CurrentMotionVectors.Load(int3(clamped, 0));
}

float4 LoadCurrentSampleClamped(int2 pos)
{
	int2 clamped = clamp(pos, int2(0, 0), int2(InputDim) - 1);
	return CurrentColor.Load(int3(clamped, 0));
}

float3 LoadCurrentColorClamped(int2 pos)
{
	return LoadCurrentSampleClamped(pos).rgb;
}

float2 GetClosestDepthVelocity3x3(float2 inputUV)
{
	uint2 inputPos = ToInputPos(inputUV);
	float minDepth = 1.0;
	int2 minPos = int2(inputPos);

	[unroll]
	for (uint i = 0; i < 9; ++i) {
		int2 samplePos = int2(inputPos) + kOffsets3x3[i];
		float sampleDepth = LoadDepthClamped(samplePos);
		if (sampleDepth < minDepth) {
			minDepth = sampleDepth;
			minPos = samplePos;
		}
	}

	return LoadMotionClamped(minPos);
}

float3 SampleHistoryCatmullRom(float2 historyUV)
{
	// Compact 9-tap Catmull-Rom reconstruction adapted for this periphery-only resolve.
	float2 samplePos = historyUV * OutputDim;
	float2 texPos1 = floor(samplePos - 0.5) + 0.5;
	float2 f = samplePos - texPos1;

	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);

	float2 w12 = w1 + w2;
	float2 offset12 = w2 / max(w12, 1e-4);

	float2 uv0 = ClampHistoryUV((texPos1 - 1.0) * InvOutputDim);
	float2 uv3 = ClampHistoryUV((texPos1 + 2.0) * InvOutputDim);
	float2 uv12 = ClampHistoryUV((texPos1 + offset12) * InvOutputDim);

	float3 result = 0.0.xxx;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv0.x, uv0.y), 0.0).rgb * w0.x * w0.y;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv12.x, uv0.y), 0.0).rgb * w12.x * w0.y;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv3.x, uv0.y), 0.0).rgb * w3.x * w0.y;

	result += HistoryColor.SampleLevel(LinearSampler, float2(uv0.x, uv12.y), 0.0).rgb * w0.x * w12.y;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv12.x, uv12.y), 0.0).rgb * w12.x * w12.y;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv3.x, uv12.y), 0.0).rgb * w3.x * w12.y;

	result += HistoryColor.SampleLevel(LinearSampler, float2(uv0.x, uv3.y), 0.0).rgb * w0.x * w3.y;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv12.x, uv3.y), 0.0).rgb * w12.x * w3.y;
	result += HistoryColor.SampleLevel(LinearSampler, float2(uv3.x, uv3.y), 0.0).rgb * w3.x * w3.y;
	return result;
}

float3 VarianceClipHistory3x3(float2 inputUV, float3 historyColor, float2 velocity)
{
	uint2 inputPos = ToInputPos(inputUV);
	float3 meanColor = 0.0.xxx;
	float3 meanSquare = 0.0.xxx;
	float3 minColor = 65504.0.xxx;
	float3 maxColor = -65504.0.xxx;

	[unroll]
	for (uint i = 0; i < 9; ++i) {
		float3 sampleColor = LoadCurrentColorClamped(int2(inputPos) + kOffsets3x3[i]);
		meanColor += sampleColor;
		meanSquare += sampleColor * sampleColor;
		minColor = min(minColor, sampleColor);
		maxColor = max(maxColor, sampleColor);
	}

	meanColor *= (1.0 / 9.0);
	meanSquare *= (1.0 / 9.0);
	float3 sigma = sqrt(max(meanSquare - meanColor * meanColor, 0.0.xxx));

	float velocityPixels = length(velocity * OutputDim);
	float varianceGamma = lerp(2.1, 1.15, saturate(velocityPixels * Tuning2.z));
	float3 clipMin = max(minColor, meanColor - sigma * varianceGamma);
	float3 clipMax = min(maxColor, meanColor + sigma * varianceGamma);
	return clamp(historyColor, clipMin, clipMax);
}

float ComputeDisocclusion(float2 historyUV, float2 currentVelocity)
{
	if (any(historyUV < 0.0.xx) || any(historyUV > 1.0.xx))
		return 1.0;

	uint2 historyPos = ToHistoryPos(historyUV);
	float2 previousVelocity = HistoryVelocity.Load(int3(historyPos, 0));
	float velocityDeltaPixels = length((previousVelocity - currentVelocity) * OutputDim);
	return saturate((velocityDeltaPixels - 0.75) * 0.35);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint2 localPos = dispatchID.xy;
	if (any(localPos >= uint2(DispatchDim)))
		return;

	uint2 outputPos = localPos + uint2(OutputOffset + 0.5);
	if (any(outputPos >= uint2(OutputDim)))
		return;

	float2 outputUV = (float2(outputPos) + 0.5) * InvOutputDim;
	float2 inputUV = ClampInputUV(outputUV - (Jitter * InvInputDim));

	float centerScale = Tuning0.x;
	float centerFeather = Tuning0.y;
	bool resetHistory = Tuning0.z > 0.5;
	bool showDebug = Tuning0.w > 0.5;
	bool disableLocks = Tuning1.x > 0.5;
	bool disableReactivity = Tuning1.y > 0.5;
	bool disableInstability = Tuning1.z > 0.5;
	bool historyValid = Tuning1.w > 0.5 && !resetHistory;

	float centerWeight = FoveatedComputeCenterBlendWeight(outputUV, centerScale, centerFeather, CenterOffset);
	float peripheryWeight = saturate(1.0 - centerWeight);

	float4 currentSample = CurrentColor.SampleLevel(LinearSampler, inputUV, 0.0);
	float3 currentColor = currentSample.rgb;
	float currentAlpha = currentSample.a;
	float2 currentVelocity = GetClosestDepthVelocity3x3(inputUV);
	float reactiveMask = CurrentReactiveMask.SampleLevel(LinearSampler, inputUV, 0.0);
	float transparencyMask = CurrentTransparencyMask.SampleLevel(LinearSampler, inputUV, 0.0);
	float reactivity = disableReactivity ? 0.0 : saturate(max(reactiveMask, transparencyMask) * Tuning2.x);

	float3 resolvedColor = currentColor;
	float newLock = 0.0;
	float disocclusion = 1.0;
	float instability = 0.0;
	float previousLock = 0.0;

	if (historyValid) {
		float2 historyUV = outputUV + currentVelocity;
		float3 historyColor = SampleHistoryCatmullRom(historyUV);
		float3 clippedHistory = VarianceClipHistory3x3(inputUV, historyColor, currentVelocity);

		disocclusion = ComputeDisocclusion(historyUV, currentVelocity);
		previousLock = disableLocks ? 0.0 : HistoryLock.Load(int3(ToHistoryPos(historyUV), 0));

		float3 currentTM = Reinhard(currentColor);
		float3 historyTM = Reinhard(clippedHistory);
		float currentLuma = Luma(currentTM);
		float historyLuma = Luma(historyTM);

		float velocityPixels = length(currentVelocity * OutputDim);
		float motionFactor = saturate(velocityPixels * Tuning2.z);

		if (!disableInstability) {
			float lumaDiff = abs(currentLuma - historyLuma) / max(max(currentLuma, historyLuma), 1e-3);
			instability = saturate(lumaDiff * Tuning2.y);
			instability *= (1.0 - motionFactor);
		}

		float lockBoost = disableLocks ? 0.0 : previousLock * (1.0 - reactivity) * (1.0 - disocclusion);
		float currentBlend = 0.08;
		currentBlend += motionFactor * 0.22;
		currentBlend += disocclusion * 0.62;
		currentBlend += reactivity * 0.35;
		currentBlend += instability * 0.20;
		currentBlend -= lockBoost * 0.18;
		currentBlend = clamp(currentBlend, 0.05, 1.0);
		currentBlend = lerp(1.0, currentBlend, peripheryWeight);

		float3 resolvedTM = lerp(historyTM, currentTM, currentBlend);
		resolvedColor = ReinhardInverse(resolvedTM);

		if (!disableLocks) {
			float trust = (1.0 - reactivity) * (1.0 - disocclusion) * (1.0 - instability);
			float accumulation = 1.0 - currentBlend;
			newLock = saturate(max(previousLock * Tuning2.w * trust, accumulation * trust));
		}
	}

	OutVelocity[outputPos] = currentVelocity;
	OutLock[outputPos] = newLock;

	if (showDebug) {
		float3 debugColor = float3(reactivity, disableLocks ? 0.0 : newLock, max(disocclusion, instability));
		OutColor[outputPos] = float4(debugColor, 1.0);
	} else {
		OutColor[outputPos] = float4(resolvedColor, currentAlpha);
	}
}
