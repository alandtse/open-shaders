#ifndef WATER_SHALLOW_SURFACE_MATH_HLSLI
#define WATER_SHALLOW_SURFACE_MATH_HLSLI

#include "Common/ReverseZ.hlsli"
#include "Common/VR.hlsli"
#include "Common/WaterFinite.hlsli"

namespace WaterShallowSurface
{
	static const float NearDepthMargin = 1e-5;
	static const float CullFadeRange = 2048.0;
	static const float MinimumRange = 1e-4;

	bool IsSceneDepthValid(float rawDepth, float emptyDepth)
	{
		return WaterFinite::IsFinite(rawDepth) && rawDepth >= 0.0 && rawDepth <= 1.0 &&
		       FrameBuffer::IsNearerDepth(rawDepth, emptyDepth) &&
		       FrameBuffer::IsNearerDepth(FrameBuffer::ToNativeDepth(NearDepthMargin), rawDepth);
	}

	bool IsDepthPixelInEyeBounds(int2 pixelPosition, uint eyeIndex, int2 renderDimensions)
	{
		return all(renderDimensions > 0) &&
		       all(pixelPosition == Stereo::ClampToEyeBounds(pixelPosition, eyeIndex, renderDimensions));
	}

	bool TryReconstructScenePosition(float2 logicalUV, float rawDepth, uint eyeIndex,
		float4x4 inverseViewProjection, out float3 scenePosition)
	{
		scenePosition = 0.0.xxx;
		if (!all(WaterFinite::IsFinite(logicalUV)) || !WaterFinite::IsFinite(rawDepth))
			return false;
#if defined(VR)
		float2 clipPosition = Stereo::ConvertFromStereoUV(logicalUV, eyeIndex, 1) * 2.0 - 1.0;
#else
		float2 clipPosition = (logicalUV * 2.0 - 1.0) * float2(1.0, -1.0);
#endif
		float4 positionH = mul(inverseViewProjection, float4(clipPosition, rawDepth, 1.0));
		if (!all(WaterFinite::IsFinite(positionH)) || abs(positionH.w) <= 1e-6)
			return false;
		scenePosition = positionH.xyz / positionH.w;
		return all(WaterFinite::IsFinite(scenePosition));
	}

	float GetAvailability(float viewDistance, float maximumDistance)
	{
		if (!WaterFinite::IsFinite(viewDistance) || !WaterFinite::IsFinite(maximumDistance) || maximumDistance <= 0.0)
			return 0.0;
		float fadeRange = min(CullFadeRange, 0.25 * maximumDistance);
		return 1.0 - smoothstep(maximumDistance - fadeRange, maximumDistance, viewDistance);
	}

	float GetShoreContactWeight(float columnDepth, float blendRange, float minimumFadePixels, float depthFootprint)
	{
		if (!WaterFinite::IsFinite(columnDepth) || columnDepth < 0.0 ||
			!WaterFinite::IsFinite(blendRange) || !WaterFinite::IsFinite(minimumFadePixels))
			return 0.0;
		float adaptiveRange = WaterFinite::IsFinite(depthFootprint) ?
		                          max(minimumFadePixels, 0.0) * max(depthFootprint, 0.0) :
		                          0.0;
		float effectiveRange = max(max(blendRange, 0.0), adaptiveRange);
		return effectiveRange <= MinimumRange ? 1.0 : smoothstep(0.0, effectiveRange, columnDepth);
	}

	float GetDepthWeight(float columnDepth, float depthRange)
	{
		if (!WaterFinite::IsFinite(columnDepth) || columnDepth < 0.0 || !WaterFinite::IsFinite(depthRange))
			return 0.0;
		float depth = saturate(columnDepth / max(depthRange, 1.0));
		return 1.0 - depth * depth * depth * (depth * (depth * 6.0 - 15.0) + 10.0);
	}

	float GetCandidateWeight(float shoreWeight, float depthWeight, float strength, float nativeRefractionMul, float availability)
	{
		if (!WaterFinite::IsFinite(nativeRefractionMul))
			return 0.0;
		return 0.5 * saturate(shoreWeight) * saturate(depthWeight) * saturate(strength) *
		       (1.0 - saturate(nativeRefractionMul)) * saturate(availability);
	}

	bool TryGetProbeOffsets(float2 direction, float radius, out int2 midpoint, out int2 endpoint)
	{
		midpoint = 0;
		endpoint = 0;
		if (!all(WaterFinite::IsFinite(direction)) || !WaterFinite::IsFinite(radius) || radius <= 0.0)
			return false;
		endpoint = int2(round(direction * radius));
		midpoint = int2(round(0.5 * float2(endpoint)));
		return any(endpoint != 0) && any(midpoint != 0) && any(midpoint != endpoint);
	}

	float GetConnectedDeepWeight(bool midpointValid, float midpointColumn, bool endpointValid, float endpointColumn,
		float deepDepth, float transition, float authority)
	{
		if (!midpointValid || !WaterFinite::IsFinite(midpointColumn) || midpointColumn <= 0.0)
			return 0.0;
		float deepestColumn = midpointColumn;
		if (endpointValid && WaterFinite::IsFinite(endpointColumn))
			deepestColumn = max(deepestColumn, endpointColumn);
		float deepEvidence = transition > MinimumRange ?
		                         smoothstep(max(deepDepth - transition, 0.0), deepDepth, deepestColumn) :
		                         step(deepDepth, deepestColumn);
		return saturate(deepEvidence * authority);
	}
}

#endif
