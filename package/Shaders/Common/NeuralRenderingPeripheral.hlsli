#ifndef NR_PERIPHERAL_COMPRESSION_HLSLI
#define NR_PERIPHERAL_COMPRESSION_HLSLI

#include "NeuralRenderingFinite.hlsli"

// Analytic curve adapted from Yuri Grib (BeliyG3), optimizer-fps-dlss5, MIT.
// See SKSE/Plugins/CommunityShaders/Licenses/NeuralRendering/PeripheralCompression.txt.
namespace NeuralRenderingPeripheral
{
	struct Mapping
	{
		float protectedFraction, boundaryScale, edgeSlope;
		uint padding;
	};

	float Distance(float2 delta, Mapping map)
	{
		return 2.0 * max(abs(delta.x), abs(delta.y));
	}

	float ForwardRadius(float radius, Mapping map)
	{
		if (radius <= map.protectedFraction)
			return radius;
		if (radius >= 1.0)
			return map.boundaryScale + (radius - 1.0) * map.edgeSlope;
		float phase = (radius - map.protectedFraction) / (1.0 - map.protectedFraction);
		return map.protectedFraction + (map.boundaryScale - map.protectedFraction) * (2.0 * phase / (1.0 + phase));
	}

	float InverseScale(float radius, Mapping map)
	{
		if (radius <= map.protectedFraction)
			return 1.0;
		if (radius >= map.boundaryScale)
			return (1.0 + (radius - map.boundaryScale) / map.edgeSlope) / radius;
		float phase = (radius - map.protectedFraction) / (map.boundaryScale - map.protectedFraction);
		return (map.protectedFraction + (1.0 - map.protectedFraction) * phase / (2.0 - phase)) / radius;
	}

	float2 Forward(float2 source, Mapping map)
	{
		float2 delta = source - 0.5;
		float radius = Distance(delta, map);
		float scale = radius > map.protectedFraction ? ForwardRadius(radius, map) / radius : 1.0;
		return 0.5 + delta * scale / map.boundaryScale;
	}

	float2 Inverse(float2 target, Mapping map)
	{
		float2 delta = (target - 0.5) * map.boundaryScale;
		return 0.5 + delta * InverseScale(Distance(delta, map), map);
	}

	void SourceBounds(float2 lower, float2 upper, Mapping map, out float2 sourceLower, out float2 sourceUpper)
	{
		float2 low = (lower - 0.5) * map.boundaryScale;
		float2 high = (upper - 0.5) * map.boundaryScale;
		float2 nearest = float2(low.x <= 0.0 && high.x >= 0.0 ? 0.0 : min(abs(low.x), abs(high.x)),
			low.y <= 0.0 && high.y >= 0.0 ? 0.0 : min(abs(low.y), abs(high.y)));
		float2 farthest = max(abs(low), abs(high));
		float nearScale = InverseScale(Distance(nearest, map), map);
		float farScale = InverseScale(Distance(farthest, map), map);
		sourceLower = 0.5 + min(low * nearScale, low * farScale);
		sourceUpper = 0.5 + max(high * nearScale, high * farScale);
	}

	float2 Motion(float2 anchor, float2 velocity, Mapping map)
	{
		const float2 transformed = Forward(anchor + velocity, map) - Forward(anchor, map);
		return all(NeuralRenderingFinite::IsFinite(transformed)) ? transformed : 0.0;
	}
}

#endif
