#ifndef WATER_SHALLOW_SURFACE_HLSLI
#define WATER_SHALLOW_SURFACE_HLSLI

#include "Common/WaterShallowSurfaceMath.hlsli"

#if defined(UNIFIED_WATER) && defined(DEPTH) && (!defined(VERTEX_ALPHA_DEPTH) || defined(VR)) && !defined(UNDERWATER) && !defined(LOD)
#	define UNIFIED_WATER_SHALLOW_FALLBACK

static const float UnifiedWaterDeepProbeMinRadiusPixels = 4.0;
static const float UnifiedWaterDeepProbeMaxRadiusPixels = 512.0;
static const float UnifiedWaterDeepProbeReliableRadiusRatio = 0.75;

int2 GetUnifiedWaterDepthRenderDimensions()
{
	return max(
		int2(1, 1),
		int2(round(
			SharedData::BufferDim.xy *
			FrameBuffer::DynamicResolutionParams1.xy)));
}

bool IsUnifiedWaterRawDepthValid(float rawDepth)
{
#	if defined(HORIZON_FIX)
	return WaterShallowSurface::IsSceneDepthValid(rawDepth, HorizonFix::EmptyDepthThreshold);
#	else
	return WaterShallowSurface::IsSceneDepthValid(rawDepth, FrameBuffer::FarPlaneDepth());
#	endif
}

bool TryGetUnifiedWaterScenePosition(
	float2 logicalUV,
	float rawDepth,
	uint eyeIndex,
	out float3 scenePosition)
{
	scenePosition = 0.0.xxx;
	return IsUnifiedWaterRawDepthValid(rawDepth) &&
	       WaterShallowSurface::TryReconstructScenePosition(logicalUV, rawDepth, eyeIndex,
			   FrameBuffer::CameraViewProjInverse[eyeIndex], scenePosition);
}

bool TryGetUnifiedWaterProbeColumn(
	int2 samplePixel,
	uint eyeIndex,
	int2 renderDimensions,
	float3 waterSurfacePosition,
	float3 waterPlaneNormal,
	float belowPlaneSign,
	out float probeColumn)
{
	probeColumn = -1.0;
	if (!WaterShallowSurface::IsDepthPixelInEyeBounds(
			samplePixel,
			eyeIndex,
			renderDimensions)) {
		return false;
	}

	float rawDepth = DepthTex.Load(int3(samplePixel, 0)).x;
	float2 logicalUV =
		FrameBuffer::DynamicResolutionParams2.xy *
			(float2(samplePixel) + 0.5) *
			VPOSOffset.xy +
		VPOSOffset.zw;
	float3 scenePosition;
	if (!TryGetUnifiedWaterScenePosition(
			logicalUV,
			rawDepth,
			eyeIndex,
			scenePosition))
		return false;

	probeColumn =
		belowPlaneSign *
		dot(scenePosition - waterSurfacePosition, waterPlaneNormal);
	return WaterFinite::IsFinite(probeColumn) && probeColumn > 0.0;
}

float GetUnifiedWaterDeepContextWeight(
	float2 screenPosition,
	uint eyeIndex,
	float2 primaryLogicalUV,
	float primaryRawDepth,
	float3 waterSurfacePosition,
	float centerColumn,
	float2 columnGradient,
	float3 waterSurfaceDx,
	float3 waterSurfaceDy)
{
	if (!WaterFinite::IsFinite(centerColumn) || centerColumn < 0.0)
		return 0.0;

	float deepDepth = max(
		SharedData::unifiedWaterSettings.DeepContextDepthUnits,
		1.0);
	float transition = max(
		SharedData::unifiedWaterSettings.DeepContextTransitionUnits,
		0.0);

	float maximumReach = max(
		SharedData::unifiedWaterSettings.DeepConnectionProbeReachUnits,
		0.0);
	if (
		maximumReach <= 0.0 ||
		!all(WaterFinite::IsFinite(columnGradient)) ||
		!all(WaterFinite::IsFinite(waterSurfaceDx)) ||
		!all(WaterFinite::IsFinite(waterSurfaceDy))) {
		return 0.0;
	}

	float gradientMagnitude = length(columnGradient);
	if (!WaterFinite::IsFinite(gradientMagnitude) || gradientMagnitude <= 1e-4)
		return 0.0;

	float2 probeDirection = columnGradient / gradientMagnitude;
	float3 directionalSurfaceStep =
		probeDirection.x * waterSurfaceDx +
		probeDirection.y * waterSurfaceDy;
	float surfaceUnitsPerPixel = length(directionalSurfaceStep);
	if (!WaterFinite::IsFinite(surfaceUnitsPerPixel) || surfaceUnitsPerPixel <= 1e-4)
		return 0.0;

	int2 renderDimensions = GetUnifiedWaterDepthRenderDimensions();
	float2 eyeRenderDimensions = float2(renderDimensions);
#	if defined(VR)
	eyeRenderDimensions.x *= 0.5;
#	endif
	float requestedRadiusPixels = maximumReach / surfaceUnitsPerPixel;
	float representableRadiusPixels = min(
		UnifiedWaterDeepProbeMaxRadiusPixels,
		length(eyeRenderDimensions));
	if (
		!WaterFinite::IsFinite(requestedRadiusPixels) ||
		requestedRadiusPixels < UnifiedWaterDeepProbeMinRadiusPixels ||
		representableRadiusPixels < UnifiedWaterDeepProbeMinRadiusPixels) {
		return 0.0;
	}

	float reliabilityFadeStartPixels =
		UnifiedWaterDeepProbeReliableRadiusRatio * representableRadiusPixels;
	float probeRepresentationConfidence =
		1.0 - smoothstep(
				  reliabilityFadeStartPixels,
				  representableRadiusPixels,
				  requestedRadiusPixels);

	float probeNearFieldConfidence = 1.0;
#	if defined(VR)
	float waterSurfaceViewDistance = length(waterSurfacePosition);
	if (!WaterFinite::IsFinite(waterSurfaceViewDistance))
		return 0.0;
	float nearFieldFadeStart = maximumReach;
	float nearFieldFadeEnd = max(
		2.0 * maximumReach,
		nearFieldFadeStart + 1.0);
	probeNearFieldConfidence = smoothstep(
		nearFieldFadeStart,
		nearFieldFadeEnd,
		waterSurfaceViewDistance);
#	endif

	float probeConfidence =
		probeRepresentationConfidence *
		probeNearFieldConfidence;
	float probeAuthority = saturate(probeConfidence);
	if (probeAuthority <= 1e-4)
		return 0.0;

	float radiusPixels = min(
		requestedRadiusPixels,
		representableRadiusPixels);

	int2 midpointOffset;
	int2 endpointOffset;
	if (!WaterShallowSurface::TryGetProbeOffsets(probeDirection, radiusPixels, midpointOffset, endpointOffset)) {
		return 0.0;
	}

	int2 centerPixel = int2(screenPosition);
	int2 midpointPixel = centerPixel + midpointOffset;
	int2 endpointPixel = centerPixel + endpointOffset;
	if (!WaterShallowSurface::IsDepthPixelInEyeBounds(
			centerPixel,
			eyeIndex,
			renderDimensions)) {
		return 0.0;
	}

	float3 centerScenePosition;
	if (!TryGetUnifiedWaterScenePosition(
			primaryLogicalUV,
			primaryRawDepth,
			eyeIndex,
			centerScenePosition)) {
		return 0.0;
	}

	float3 waterPlaneNormal = ReflectPlane[eyeIndex].xyz;
	float waterPlaneNormalLength = length(waterPlaneNormal);
	if (!WaterFinite::IsFinite(waterPlaneNormalLength) || waterPlaneNormalLength <= 1e-6)
		return 0.0;
	waterPlaneNormal /= waterPlaneNormalLength;

	float centerPlaneOffset = dot(
		centerScenePosition - waterSurfacePosition,
		waterPlaneNormal);
	if (!WaterFinite::IsFinite(centerPlaneOffset) || abs(centerPlaneOffset) <= 1e-4)
		return 0.0;
	float belowPlaneSign = centerPlaneOffset < 0.0 ? -1.0 : 1.0;

	float midpointColumn = -1.0;
	bool midpointValid = false;
	if (WaterShallowSurface::IsDepthPixelInEyeBounds(
			midpointPixel,
			eyeIndex,
			renderDimensions)) {
		midpointValid = TryGetUnifiedWaterProbeColumn(
			midpointPixel,
			eyeIndex,
			renderDimensions,
			waterSurfacePosition,
			waterPlaneNormal,
			belowPlaneSign,
			midpointColumn);
	}

	float endpointColumn = -1.0;
	bool endpointValid = false;
	if (WaterShallowSurface::IsDepthPixelInEyeBounds(
			endpointPixel,
			eyeIndex,
			renderDimensions)) {
		endpointValid = TryGetUnifiedWaterProbeColumn(
			endpointPixel,
			eyeIndex,
			renderDimensions,
			waterSurfacePosition,
			waterPlaneNormal,
			belowPlaneSign,
			endpointColumn);
	}

	return WaterShallowSurface::GetConnectedDeepWeight(midpointValid, midpointColumn,
		endpointValid, endpointColumn, deepDepth, transition, probeAuthority);
}

#endif

#endif
