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
	float4 Tuning0;  // x=centerScale, y=centerFeather, z=resetHistory, w=taaOuterScale
	float4 Tuning1;  // x=historyValid, y=centerHorizontalScale, z=tileDispatch, w=tileDispatchWidth
	float4 Tuning2;  // x=reactivityScale, y=instabilityScale, z=velocityScale, w=lockDecay
	row_major float4x4 CurrentViewProjInverse;
	row_major float4x4 PreviousViewProj;
	float4 CurrentCameraPosAdjust;
	float4 PreviousCameraPosAdjust;
};

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float2> CurrentMotionVectors : register(t2);
Texture2D<float> CurrentReactiveMask : register(t3);
Texture2D<float> CurrentTransparencyMask : register(t4);
Texture2D<float4> HistoryColor : register(t5);
Texture2D<float2> HistoryVelocity : register(t6);
Texture2D<float> HistoryLock : register(t7);
StructuredBuffer<uint2> DispatchTiles : register(t8);

SamplerState LinearSampler : register(s0);

RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float2> OutVelocity : register(u1);
RWTexture2D<float> OutLock : register(u2);
RWTexture2D<float4> OutHistoryColor : register(u3);

static const int2 kOffsets3x3[9] = {
	int2(-1, -1), int2(0, -1), int2(1, -1),
	int2(-1, 0),  int2(0, 0),  int2(1, 0),
	int2(-1, 1),  int2(0, 1),  int2(1, 1)
};
static const float kHmdVelocityDeltaRelaxThresholdPixels = 0.25;
static const float kHmdVelocityDeltaRelaxScale = 0.24;
static const float kHmdVelocityDeltaRelaxStrength = 1.50;
static const float kHmdDisocclusionSuppression = 0.95;
static const float kHmdDisocclusionConfidenceFloor = 0.80;
static const float kHmdDisocclusionCap = 0.22;
static const float kHmdMotionBlendSuppression = 0.65;
static const float kMotionInstabilitySuppression = 0.85;
static const float kCameraVelocityRelaxThresholdPixels = 0.50;
static const float kCameraVelocityRelaxScale = 0.10;
static const uint kCurrentCacheDim = 16;

groupshared float4 gCurrentColorCache[kCurrentCacheDim * kCurrentCacheDim];
groupshared float gCurrentDepthCache[kCurrentCacheDim * kCurrentCacheDim];
groupshared int2 gCurrentCacheBase;

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

uint FlattenCurrentCachePos(int2 cachePos)
{
	return (uint)cachePos.y * kCurrentCacheDim + (uint)cachePos.x;
}

bool TryGetCurrentCachePos(int2 pos, out int2 cachePos)
{
	cachePos = pos - gCurrentCacheBase;
	return all(cachePos >= int2(0, 0)) && all(cachePos < int2((int)kCurrentCacheDim, (int)kCurrentCacheDim));
}

float LoadCachedDepthClamped(int2 pos)
{
	int2 cachePos;
	if (TryGetCurrentCachePos(pos, cachePos))
		return gCurrentDepthCache[FlattenCurrentCachePos(cachePos)];
	return LoadDepthClamped(pos);
}

float3 LoadCachedCurrentColorClamped(int2 pos)
{
	int2 cachePos;
	if (TryGetCurrentCachePos(pos, cachePos))
		return gCurrentColorCache[FlattenCurrentCachePos(cachePos)].rgb;
	return LoadCurrentColorClamped(pos);
}

struct NeighborhoodStats
{
	float depth;
	float2 velocity;
	float2 uv;
	float3 meanColor;
	float3 meanSquare;
	float3 minColor;
	float3 maxColor;
};

NeighborhoodStats GatherNeighborhoodStats3x3(float2 inputUV)
{
	uint2 inputPos = ToInputPos(inputUV);
	float minDepth = 1.0;
	int2 minPos = int2(inputPos);
	float3 meanColor = 0.0.xxx;
	float3 meanSquare = 0.0.xxx;
	float3 minColor = 65504.0.xxx;
	float3 maxColor = -65504.0.xxx;

	[unroll]
	for (uint i = 0; i < 9; ++i) {
		int2 samplePos = int2(inputPos) + kOffsets3x3[i];
		float sampleDepth = LoadCachedDepthClamped(samplePos);
		if (sampleDepth < minDepth) {
			minDepth = sampleDepth;
			minPos = samplePos;
		}

		float3 sampleColor = LoadCachedCurrentColorClamped(samplePos);
		meanColor += sampleColor;
		meanSquare += sampleColor * sampleColor;
		minColor = min(minColor, sampleColor);
		maxColor = max(maxColor, sampleColor);
	}

	NeighborhoodStats result;
	result.depth = minDepth;
	result.velocity = LoadMotionClamped(minPos);
	result.uv = ClampInputUV((float2(minPos) + 0.5) * InvInputDim);
	result.meanColor = meanColor * (1.0 / 9.0);
	result.meanSquare = meanSquare * (1.0 / 9.0);
	result.minColor = minColor;
	result.maxColor = maxColor;
	return result;
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

float3 VarianceClipHistory3x3(NeighborhoodStats stats, float3 historyColor, float2 velocity)
{
	float3 sigma = sqrt(max(stats.meanSquare - stats.meanColor * stats.meanColor, 0.0.xxx));

	float velocityPixels = length(velocity * OutputDim);
	float varianceGamma = lerp(2.1, 1.15, saturate(velocityPixels * Tuning2.z));
	float3 clipMin = max(stats.minColor, stats.meanColor - sigma * varianceGamma);
	float3 clipMax = min(stats.maxColor, stats.meanColor + sigma * varianceGamma);
	return clamp(historyColor, clipMin, clipMax);
}

float ComputeVelocityDeltaPixels(float2 historyUV, float2 historyVelocity)
{
	if (any(historyUV < 0.0.xx) || any(historyUV > 1.0.xx))
		return 4096.0;

	uint2 historyPos = ToHistoryPos(historyUV);
	float2 previousVelocity = HistoryVelocity.Load(int3(historyPos, 0));
	return length((previousVelocity - historyVelocity) * OutputDim);
}

bool IsHistoryAuxValid(float2 historyUV, float centerScale, float centerFeather, float centerHorizontalScale, float taaOuterScale)
{
	if (any(historyUV < 0.0.xx) || any(historyUV > 1.0.xx))
		return false;

	float historyCenterWeight = FoveatedComputeCenterBlendWeight(historyUV, centerScale, centerFeather, centerHorizontalScale, CenterOffset);
	if (historyCenterWeight >= 1.0)
		return false;

	return FoveatedComputeMaskDistance(historyUV, taaOuterScale, centerHorizontalScale, CenterOffset) <= 1.0 + 1e-4;
}

float ComputeDisocclusion(float velocityDeltaPixels, float stabilityBias)
{
	float threshold = lerp(0.75, 1.15, stabilityBias);
	float scale = lerp(0.35, 0.26, stabilityBias);
	return saturate((velocityDeltaPixels - threshold) * scale);
}

float2 ComputeHmdHistoryDelta(float2 reprojectionUV, float depth)
{
	if (depth <= 0.0 || depth >= 0.99995)
		return 0.0.xx;

	float4 currentClip = float4(reprojectionUV.x * 2.0 - 1.0, 1.0 - reprojectionUV.y * 2.0, depth, 1.0);
	float4 worldPositionRelative = mul(CurrentViewProjInverse, currentClip);
	if (abs(worldPositionRelative.w) <= 1e-5)
		return 0.0.xx;
	worldPositionRelative /= worldPositionRelative.w;

	float3 worldPositionAbsolute = worldPositionRelative.xyz + CurrentCameraPosAdjust.xyz;
	float3 previousRelativeWorld = worldPositionAbsolute - PreviousCameraPosAdjust.xyz;

	float4 previousClip = mul(PreviousViewProj, float4(previousRelativeWorld, 1.0));
	if (previousClip.w <= 1e-5)
		return 0.0.xx;

	float2 previousNdc = previousClip.xy / previousClip.w;
	float2 previousUV = float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
	return previousUV - reprojectionUV;
}

float ComputeHmdRejectionRelaxation(float2 hmdDelta)
{
	float hmdMotionPixels = length(hmdDelta * OutputDim);
	return saturate((hmdMotionPixels - kHmdVelocityDeltaRelaxThresholdPixels) * kHmdVelocityDeltaRelaxScale);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
	const bool tileDispatch = Tuning1.z > 0.5;
	const uint tileDispatchWidth = max(1u, (uint)(Tuning1.w + 0.5));
	const uint tileIndex = groupID.x + groupID.y * tileDispatchWidth;
	if (tileDispatch && tileIndex >= (uint)(DispatchDim.x + 0.5))
		return;

	uint2 tileOrigin;
	uint2 localPos;
	uint2 outputPos;
	bool pixelValid;
	if (tileDispatch) {
		tileOrigin = DispatchTiles[tileIndex];
		localPos = groupThreadID.xy;
		outputPos = tileOrigin + groupThreadID.xy;
		pixelValid = !any(outputPos >= uint2(OutputDim));
	} else {
		tileOrigin = uint2(OutputOffset + 0.5) + groupID.xy * 8u;
		localPos = dispatchID.xy;
		outputPos = localPos + uint2(OutputOffset + 0.5);
		pixelValid = !any(localPos >= uint2(DispatchDim)) && !any(outputPos >= uint2(OutputDim));
	}

	if (groupThreadID.x == 0 && groupThreadID.y == 0) {
		float2 tileOutputUV = (float2(tileOrigin) + 0.5) * InvOutputDim;
		uint2 tileInputPos = ToInputPos(tileOutputUV - (Jitter * InvInputDim));
		gCurrentCacheBase = int2(tileInputPos) - int2(4, 4);
	}
	GroupMemoryBarrierWithGroupSync();

	const uint groupThreadIndex = groupThreadID.y * 8u + groupThreadID.x;
	for (uint cacheIndex = groupThreadIndex; cacheIndex < kCurrentCacheDim * kCurrentCacheDim; cacheIndex += 64u) {
		int2 cacheOffset = int2((int)(cacheIndex % kCurrentCacheDim), (int)(cacheIndex / kCurrentCacheDim));
		int2 samplePos = gCurrentCacheBase + cacheOffset;
		gCurrentColorCache[cacheIndex] = LoadCurrentSampleClamped(samplePos);
		gCurrentDepthCache[cacheIndex] = LoadDepthClamped(samplePos);
	}
	GroupMemoryBarrierWithGroupSync();

	if (!pixelValid)
		return;

	float2 outputUV = (float2(outputPos) + 0.5) * InvOutputDim;
	float2 inputUV = ClampInputUV(outputUV - (Jitter * InvInputDim));

	float centerScale = Tuning0.x;
	float centerFeather = Tuning0.y;
	float centerHorizontalScale = Tuning1.y;
	float normalizedFeather = FoveatedComputeNormalizedFeather(centerScale, centerFeather, centerHorizontalScale);
	float minOuterScale = centerScale * (1.0 + normalizedFeather);
	float taaOuterScale = min(max(Tuning0.w, minOuterScale), 1.0);
	bool resetHistory = Tuning0.z > 0.5;
	bool historyValid = Tuning1.x > 0.5 && !resetHistory;

	float centerWeight = FoveatedComputeCenterBlendWeight(outputUV, centerScale, centerFeather, centerHorizontalScale, CenterOffset);
	float peripheryWeight = saturate(1.0 - centerWeight);
	float4 currentSample = CurrentColor.SampleLevel(LinearSampler, inputUV, 0.0);
	if (peripheryWeight <= 0.0) {
		OutVelocity[outputPos] = 0.0.xx;
		OutLock[outputPos] = 0.0;
		OutHistoryColor[outputPos] = currentSample;
		return;
	}

	if (FoveatedComputeMaskDistance(outputUV, taaOuterScale, centerHorizontalScale, CenterOffset) > 1.0) {
		float2 passthroughVelocity = CurrentMotionVectors.SampleLevel(LinearSampler, inputUV, 0.0);
		OutVelocity[outputPos] = passthroughVelocity;
		OutLock[outputPos] = 0.0;
		OutColor[outputPos] = currentSample;
		OutHistoryColor[outputPos] = currentSample;
		return;
	}

	float3 currentColor = currentSample.rgb;
	float currentAlpha = currentSample.a;
	NeighborhoodStats neighborhood = GatherNeighborhoodStats3x3(inputUV);
	float currentDepth = neighborhood.depth;
	float2 currentVelocity = neighborhood.velocity;
	float2 hmdHistoryDeltaLookup = 0.0.xx;
	hmdHistoryDeltaLookup = ComputeHmdHistoryDelta(neighborhood.uv, currentDepth);
	float2 historyVelocity = currentVelocity + hmdHistoryDeltaLookup;
	float2 rejectionVelocity = currentVelocity;
	float velocityPixels = length(rejectionVelocity * OutputDim);
	float2 historyUV = outputUV + historyVelocity;
	float reactiveMask = CurrentReactiveMask.SampleLevel(LinearSampler, inputUV, 0.0);
	float transparencyMask = CurrentTransparencyMask.SampleLevel(LinearSampler, inputUV, 0.0);
	float reactivity = saturate(max(reactiveMask, transparencyMask) * Tuning2.x);

	float3 resolvedColor = currentColor;
	float currentBlend = 1.0;
	float newLock = 0.0;
	float disocclusion = 1.0;
	float instability = 0.0;
	float previousLock = 0.0;
	float velocityDeltaPixels = 0.0;
	float motionRejectionRelaxation = 0.0;

	if (historyValid) {
		bool historyAuxValid = IsHistoryAuxValid(historyUV, centerScale, centerFeather, centerHorizontalScale, taaOuterScale);
		if (historyAuxValid) {
			float3 rawHistoryColor = SampleHistoryCatmullRom(historyUV);
			float3 clippedHistory = VarianceClipHistory3x3(neighborhood, rawHistoryColor, rejectionVelocity);

			previousLock = HistoryLock.Load(int3(ToHistoryPos(historyUV), 0));
			float stabilityBias = 0.0;
			float lockBias = saturate(previousLock * 0.90 + 0.25);
			stabilityBias = lockBias;
			velocityDeltaPixels = ComputeVelocityDeltaPixels(historyUV, rejectionVelocity);
			float disocclusionVelocityDeltaPixels = velocityDeltaPixels;
			float hmdRejectionRelaxation = ComputeHmdRejectionRelaxation(hmdHistoryDeltaLookup);
			float cameraVelocityRelaxation = saturate((velocityPixels - kCameraVelocityRelaxThresholdPixels) * kCameraVelocityRelaxScale);
			motionRejectionRelaxation = max(hmdRejectionRelaxation, cameraVelocityRelaxation);
			float hmdMotionPixels = length(hmdHistoryDeltaLookup * OutputDim);
			disocclusionVelocityDeltaPixels = lerp(
				velocityDeltaPixels,
				max(0.0, velocityDeltaPixels - hmdMotionPixels * kHmdVelocityDeltaRelaxStrength),
				motionRejectionRelaxation);
			disocclusion = ComputeDisocclusion(disocclusionVelocityDeltaPixels, stabilityBias);
			float stableHistoryConfidence = max(kHmdDisocclusionConfidenceFloor, saturate(previousLock * 1.5 + 0.10));
			float hmdDisocclusionSuppression = motionRejectionRelaxation * stableHistoryConfidence;
			disocclusion *= 1.0 - hmdDisocclusionSuppression * kHmdDisocclusionSuppression;
			disocclusion = min(disocclusion, lerp(1.0, kHmdDisocclusionCap, hmdDisocclusionSuppression));

			float3 currentTM = Reinhard(currentColor);
			float3 historyTM = Reinhard(clippedHistory);
			float currentLuma = Luma(currentTM);
			float historyLuma = Luma(historyTM);

			float motionFactor = saturate(velocityPixels * Tuning2.z);
			motionFactor *= 1.0 - motionRejectionRelaxation * kHmdMotionBlendSuppression;

			float lumaDiff = abs(currentLuma - historyLuma) / max(max(currentLuma, historyLuma), 1e-3);
			instability = saturate(lumaDiff * Tuning2.y);
			instability *= (1.0 - motionFactor);
			instability *= 1.0 - motionRejectionRelaxation * kMotionInstabilitySuppression;

			float lockBoost = previousLock * (1.0 - reactivity) * (1.0 - disocclusion);
			currentBlend = 0.08;
			currentBlend += motionFactor * lerp(0.22, 0.16, stabilityBias);
			currentBlend += disocclusion * lerp(0.62, 0.50, stabilityBias);
			currentBlend += reactivity * lerp(0.35, 0.30, stabilityBias);
			currentBlend += instability * lerp(0.20, 0.14, stabilityBias);
			currentBlend -= lockBoost * lerp(0.18, 0.24, stabilityBias);
			currentBlend = clamp(currentBlend, 0.05, 1.0);

			float3 resolvedTM = lerp(historyTM, currentTM, currentBlend);
			resolvedColor = ReinhardInverse(resolvedTM);

			float trust = (1.0 - reactivity) * (1.0 - disocclusion) * (1.0 - instability);
			float accumulation = 1.0 - currentBlend;
			newLock = saturate(max(previousLock * Tuning2.w * trust, accumulation * trust));
		}
	}

	OutVelocity[outputPos] = rejectionVelocity;
	OutLock[outputPos] = newLock;
	OutColor[outputPos] = float4(resolvedColor, currentAlpha);
	OutHistoryColor[outputPos] = float4(resolvedColor, currentAlpha);
}
