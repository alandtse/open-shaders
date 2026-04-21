#include "Common/FoveatedMask.hlsli"

cbuffer FoveatedPeripheryCB : register(b0)
{
	float2 OutputDim;
	float2 InvOutputDim;
	float2 InvSourceDim;
	float2 SourceScale;
	float2 SourceOffset;
	float2 DispatchDim;
	float2 OutputOffset;
	float2 Jitter;
	float2 CenterOffset;
	float2 Pad0;
	float4 Tuning0;  // x=centerScale, y=centerFeather, z=centerHorizontalScale, w reserved
	float4 Tuning1;  // x=visualizeMask, y=showThreeZoneMask, z=taaOuterScale, w reserved
};

Texture2D<float4> InputColor : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutColor : register(u0);

float2 ClampToSourceRegion(float2 uv, float2 regionMin, float2 regionMax)
{
	return clamp(uv, regionMin, regionMax);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint2 localPos = dispatchID.xy;
	if (any(localPos >= uint2(DispatchDim)))
		return;

	uint2 outputPos = localPos + uint2(OutputOffset + 0.5);
	if (any(outputPos >= uint2(OutputDim)))
		return;

	float2 uv = (float2(outputPos) + 0.5) * InvOutputDim;

	const float centerScale = Tuning0.x;
	const float centerFeather = Tuning0.y;
	const float centerHorizontalScale = Tuning0.z;
	const float visualizeMask = Tuning1.x;
	const float showThreeZoneMask = Tuning1.y;
	const float taaOuterScale = max(Tuning1.z, centerScale);

	if (visualizeMask > 0.5) {
		const float normalizedFeather = FoveatedComputeNormalizedFeather(centerScale, centerFeather, centerHorizontalScale);
		const float centerDistance = FoveatedComputeMaskDistance(uv, centerScale, centerHorizontalScale, CenterOffset);

		static const float3 kCenterZoneColor = float3(0.22, 0.68, 0.53);
		static const float3 kTaaZoneColor = float3(0.95, 0.76, 0.33);
		static const float3 kOuterZoneColor = float3(0.36, 0.50, 0.86);
		static const float3 kBaseOutsideColor = float3(0.17, 0.22, 0.31);

		float3 maskColor;
		if (showThreeZoneMask > 0.5) {
			const bool inCenterZone = centerDistance <= (1.0 + normalizedFeather);
			const float outerDistance = FoveatedComputeMaskDistance(uv, taaOuterScale, centerHorizontalScale, CenterOffset);
			const bool inTaaZone = !inCenterZone && outerDistance <= 1.0;
			maskColor = inCenterZone ? kCenterZoneColor : (inTaaZone ? kTaaZoneColor : kOuterZoneColor);
		} else {
			const bool inBaseCenterMask = centerDistance <= (1.0 + normalizedFeather);
			maskColor = inBaseCenterMask ? kCenterZoneColor : kBaseOutsideColor;
		}

		OutColor[outputPos] = float4(maskColor, 1.0);
		return;
	}

	float2 sourceRegionMin = SourceOffset;
	float2 sourceRegionMax = SourceOffset + SourceScale;
	float2 halfTexel = InvSourceDim * 0.5;
	sourceRegionMin = min(sourceRegionMin + halfTexel, sourceRegionMax);
	sourceRegionMax = max(sourceRegionMax - halfTexel, sourceRegionMin);

	float2 sourceUV = (uv * SourceScale + SourceOffset) - (Jitter * InvSourceDim);
	sourceUV = ClampToSourceRegion(sourceUV, sourceRegionMin, sourceRegionMax);

	OutColor[outputPos] = InputColor.SampleLevel(LinearSampler, sourceUV, 0.0);
}
