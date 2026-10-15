#ifndef LOD_SHADOWS_HLSLI
#define LOD_SHADOWS_HLSLI

#include "Common/LodLandscape.hlsli"

namespace LODShadows
{
	static const uint MapCount = 3;
	static const float BorderWidth = 0.05;
	static const float MinReceiverPlaneCosine = 1e-4;
	static const float MinReceiverPlaneDepth = 1e-20;
	static const float MinMaskWeight = 1e-6;
	static const float MaskDepthSensitivity = 128.0;
	static const float MinMaskDepth = 1.0;
	static const uint RayFallback = MapCount;
	static const uint RayUnshadowed = MapCount + 1;
	struct RayProjection
	{
		float3 start;
		float3 end;
		float3 parameters;
		float3 historyStart;
		float3 historyEnd;
		float3 historyParameters;
		uint mode;
	};
	struct MapData
	{
		row_major float4x4 worldToClip;
		float4 parameters;
		float4 filter;
		row_major float4x4 historyWorldToClip;
		float4 history;
	};
	Texture2DArray<float> DepthMaps : register(t125);
	StructuredBuffer<MapData> Maps : register(t126);
	Texture2D<float4> FilteredMask : register(t124);
#if defined(PSHADER)
	SamplerComparisonState ComparisonSampler : register(s14);
#endif

	bool ReplacesTerrainShadows()
	{
		return Maps[1].parameters.w > 0.0;
	}

	float MaskDepthWeight(float depth, float sampleDepth)
	{
		return exp2(-abs(depth - sampleDepth) * MaskDepthSensitivity);
	}

	float CompareBilinear(float2 uv, float2 offset, float slice, float depth, float2 depthGradient, float texelSize, SamplerState linearSampler)
	{
		float2 sampleUV = uv + offset;
		float4 depths = DepthMaps.GatherRed(linearSampler, float3(sampleUV, slice));
		float2 fraction = frac(sampleUV / texelSize - 0.5);
		float2 baseOffset = offset - fraction * texelSize;
		float4 receiverDepths = depth + dot(depthGradient, baseOffset) +
		                        float4(depthGradient.y, depthGradient.x + depthGradient.y, depthGradient.x, 0.0) * texelSize;
		float4 lit = step(receiverDepths, depths);
		return lerp(lerp(lit.w, lit.z, fraction.x), lerp(lit.x, lit.y, fraction.x), fraction.y);
	}

	float SampleMap(float3 worldPosition, float3 positionDx, float3 positionDy, float4x4 worldToClip,
		float slice, float bias, float3 filter, SamplerState linearSampler, out float coverage, bool hardwareComparison)
	{
		float3 clipPosition = mul(float4(worldPosition, 1), worldToClip).xyz;
		float2 uv = clipPosition.xy * float2(0.5, -0.5) + 0.5;
		float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
		coverage = 0.0;
		float visibility = 1.0;
		[branch] if (edge > 0 && clipPosition.z > 0 && clipPosition.z < 1)
		{
			float texelSize = filter.x;
			float3 shadowDx = mul(float4(positionDx, 0), worldToClip).xyz * float3(0.5, -0.5, 1.0);
			float3 shadowDy = mul(float4(positionDy, 0), worldToClip).xyz * float3(0.5, -0.5, 1.0);
			float3 receiverPlane = cross(shadowDx, shadowDy);
			float2 depthGradient = 0.0;
			[branch] if (abs(receiverPlane.z) > MinReceiverPlaneCosine * length(receiverPlane))
				depthGradient = -receiverPlane.xy * sign(receiverPlane.z) / max(abs(receiverPlane.z), MinReceiverPlaneDepth);
			coverage = saturate(edge / BorderWidth);
#if defined(PSHADER)
			[branch] if (hardwareComparison)
				visibility = DepthMaps.SampleCmpLevelZero(ComparisonSampler, float3(uv, slice), clipPosition.z - bias);
			else
#endif
				visibility = CompareBilinear(uv, 0.0, slice, clipPosition.z - bias, depthGradient, texelSize, linearSampler);
		}
		return visibility;
	}

	float SampleMapHistory(float3 worldPosition, float3 positionDx, float3 positionDy, MapData map,
		SamplerState linearSampler, out float coverage, bool hardwareComparison)
	{
		float visibility = SampleMap(worldPosition, positionDx, positionDy, map.worldToClip,
			map.parameters.x, map.parameters.z, map.filter.xyz, linearSampler, coverage, hardwareComparison);
		[branch] if (map.history.z > 0.0 && map.history.y < 1.0)
		{
			float previousCoverage;
			float previousVisibility = SampleMap(worldPosition, positionDx, positionDy, map.historyWorldToClip,
				map.history.x, map.parameters.z, map.filter.xyz, linearSampler, previousCoverage, hardwareComparison);
			float occlusion = lerp((1.0 - previousVisibility) * previousCoverage, (1.0 - visibility) * coverage, map.history.y);
			coverage = lerp(previousCoverage, coverage, map.history.y);
			visibility = 1.0 - occlusion / max(coverage, MinMaskWeight);
		}
		return visibility;
	}

	float FilterShadow(float3 worldPosition, float3 positionDx, float3 positionDy,
		SamplerState linearSampler, out float coverage, bool volume = false)
	{
		float shadow = 1.0;
		float remaining = 1.0;
		[branch] if (Maps[0].parameters.w > 0)
		{
			[loop] for (uint i = 0; i < MapCount && remaining > 0.0; ++i)
			{
				float4 parameters = Maps[i].parameters;
				[branch] if (parameters.y == 0) continue;
				float mapCoverage;
				float visibility = SampleMapHistory(worldPosition, positionDx, positionDy, Maps[i], linearSampler, mapCoverage,
					volume && Maps[i].history.w > 0.0);
				float occlusion = (1.0 - visibility) * mapCoverage;
				shadow -= occlusion * remaining;
				remaining *= 1.0 - mapCoverage;
			}
		}
		coverage = 1.0 - remaining;
		return shadow;
	}

	RayProjection PrepareRay(float3 startPosition, float3 endPosition, bool enabled)
	{
		RayProjection ray = (RayProjection)0;
		ray.mode = RayUnshadowed;
		[branch] if (enabled)
		{
			[loop] for (uint i = 0; i < MapCount; ++i)
			{
				MapData map = Maps[i];
				if (map.parameters.y == 0.0)
					continue;
				float3 start = mul(float4(startPosition, 1), map.worldToClip).xyz;
				float3 end = mul(float4(endPosition, 1), map.worldToClip).xyz;
				start.xy = start.xy * float2(0.5, -0.5) + 0.5;
				end.xy = end.xy * float2(0.5, -0.5) + 0.5;
				float3 lower = min(start, end);
				float3 upper = max(start, end);
				if (any(upper <= 0.0) || any(lower >= 1.0)) {
					[branch] if (map.history.z > 0.0 && map.history.y < 1.0)
					{
						float3 historyStart = mul(float4(startPosition, 1), map.historyWorldToClip).xyz;
						float3 historyEnd = mul(float4(endPosition, 1), map.historyWorldToClip).xyz;
						historyStart.xy = historyStart.xy * float2(0.5, -0.5) + 0.5;
						historyEnd.xy = historyEnd.xy * float2(0.5, -0.5) + 0.5;
						if (all(max(historyStart, historyEnd) > 0.0) && all(min(historyStart, historyEnd) < 1.0)) {
							ray.mode = RayFallback;
							break;
						}
					}
					continue;
				}
				ray.mode = RayFallback;
				if (all(lower.xy >= BorderWidth) && all(upper.xy <= 1.0 - BorderWidth) && lower.z > 0.0 && upper.z < 1.0) {
					ray.mode = i;
					ray.start = start;
					ray.end = end;
					ray.parameters = float3(map.parameters.x, map.parameters.z, map.filter.x);
					ray.historyParameters = float3(map.history.x, 1.0, map.history.w);
					[branch] if (map.history.z > 0.0 && map.history.y < 1.0)
					{
						ray.historyStart = mul(float4(startPosition, 1), map.historyWorldToClip).xyz;
						ray.historyEnd = mul(float4(endPosition, 1), map.historyWorldToClip).xyz;
						ray.historyStart.xy = ray.historyStart.xy * float2(0.5, -0.5) + 0.5;
						ray.historyEnd.xy = ray.historyEnd.xy * float2(0.5, -0.5) + 0.5;
						float3 historyLower = min(ray.historyStart, ray.historyEnd);
						float3 historyUpper = max(ray.historyStart, ray.historyEnd);
						if (all(historyLower.xy >= BorderWidth) && all(historyUpper.xy <= 1.0 - BorderWidth) && historyLower.z > 0.0 && historyUpper.z < 1.0)
							ray.historyParameters.y = map.history.y;
						else
							ray.mode = RayFallback;
					}
				}
				break;
			}
		}
		return ray;
	}

	float CompareRayDepth(float3 position, float slice, float bias, float texelSize, bool hardware, SamplerState linearSampler)
	{
		float visibility;
#if defined(PSHADER)
		[branch] if (hardware)
			visibility = DepthMaps.SampleCmpLevelZero(ComparisonSampler, float3(position.xy, slice), position.z - bias);
		else
#endif
			visibility = CompareBilinear(position.xy, 0.0, slice, position.z - bias, 0.0, texelSize, linearSampler);
		return visibility;
	}

	float SampleRay(RayProjection ray, float t, float3 worldPosition, SamplerState linearSampler)
	{
		float visibility = 1.0;
		[branch] if (ray.mode < MapCount)
		{
			float3 position = lerp(ray.start, ray.end, t);
			visibility = CompareRayDepth(position, ray.parameters.x, ray.parameters.y, ray.parameters.z, ray.historyParameters.z > 0.0, linearSampler);
			[branch] if (ray.historyParameters.y < 1.0)
			{
				float3 previousPosition = lerp(ray.historyStart, ray.historyEnd, t);
				float previousVisibility = CompareRayDepth(previousPosition, ray.historyParameters.x, ray.parameters.y, ray.parameters.z, ray.historyParameters.z > 0.0, linearSampler);
				visibility = lerp(previousVisibility, visibility, ray.historyParameters.y);
			}
		}
		else[branch] if (ray.mode == RayFallback)
		{
			float coverage;
			visibility = FilterShadow(worldPosition, 0.0, 0.0, linearSampler, coverage, true);
		}
		return visibility;
	}

	float GetShadow(float3 worldPosition, SamplerState linearSampler, uint eyeIndex, bool surface, out float coverage)
	{
		coverage = 0.0;
		float visibility = 1.0;
		bool maskValid = false;
		float3 positionDx = 0.0;
		float3 positionDy = 0.0;
#if defined(PSHADER)
		if (surface) {
			positionDx = ddx(worldPosition);
			positionDy = ddy(worldPosition);
#	if defined(DEFERRED)
			float3 relativePosition = worldPosition - FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
			float4 positionCS = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(relativePosition, 1));
			float2 uv = positionCS.xy / positionCS.w * float2(0.5, -0.5) + 0.5;
			bool matchingDepth = true;
#		if defined(LODLANDSCAPE) || defined(LODLANDNOISE)
			// The distant terrain depth offset must not become a displaced shadow receiver.
			matchingDepth = FrameBuffer::ToStandardClipZ(positionCS, FrameBuffer::IsReverseProjection(eyeIndex)) <= LodLandscape::DepthBiasStart;
#		endif
			[branch] if (Maps[0].filter.w > 0.0 && matchingDepth)
			{
				float2 dimensions = float2(Maps[1].filter.w, Maps[2].filter.w);
				float2 pixel = Stereo::ConvertToStereoUV(uv, eyeIndex) * dimensions - 0.5;
				int2 base = int2(floor(pixel));
				float2 fraction = frac(pixel);
				float depth = log2(max(MinMaskDepth, mul(FrameBuffer::CameraView[eyeIndex], float4(relativePosition, 1)).z));
				int2 lower = 0;
				int2 upper = int2(dimensions) - 1;
#		if defined(VR)
				lower.x = eyeIndex * int(dimensions.x / 2);
				upper.x = lower.x + int(dimensions.x / 2) - 1;
#		endif
				float3 result = 0.0;
				[unroll] for (int y = 0; y < 2; ++y)
					[unroll] for (int x = 0; x < 2; ++x)
				{
					float4 sample = FilteredMask.Load(int3(clamp(base + int2(x, y), lower, upper), 0));
					float weight = (x ? fraction.x : 1.0 - fraction.x) * (y ? fraction.y : 1.0 - fraction.y) * MaskDepthWeight(depth, sample.z);
					result += float3(sample.xy, 1) * weight;
				}
				if (result.z > MinMaskWeight) {
					coverage = result.y / result.z;
					visibility = result.x / result.z;
					maskValid = true;
				}
			}
#	endif
		}
#endif
		[branch] if (!maskValid)
			visibility = FilterShadow(worldPosition, positionDx, positionDy, linearSampler, coverage, !surface);
		return visibility;
	}
}
#endif
