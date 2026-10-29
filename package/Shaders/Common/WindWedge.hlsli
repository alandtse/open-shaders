#ifndef WIND_WEDGE_HLSLI
#define WIND_WEDGE_HLSLI

#include "Common/Math.hlsli"

namespace WindWedge
{
	static const float kMinimumEdgeSoftness = 0.01f;
	static const float kMaximumEdgeSoftness = 0.49f;
	static const float kWidestFraction = 0.35f;
	static const float kTailFraction = 1.0f - kWidestFraction;

	void GetAxes(float3 forward, float roll, out float3 right, out float3 up)
	{
		float horizontalLength = length(forward.xy);
		float3 referenceRight = horizontalLength > EPSILON_WIND_GEOMETRY ?
		                            float3(forward.y, -forward.x, 0.0f) / horizontalLength :
		                            float3(1.0f, 0.0f, 0.0f);
		float3 referenceUp = cross(referenceRight, forward);
		float sine, cosine;
		sincos(roll, sine, cosine);
		right = referenceRight * cosine + referenceUp * sine;
		up = cross(right, forward);
	}

	bool ClipHeight(float3 normal, float limit, float2 offset, inout float low, inout float high)
	{
		float remaining = limit - dot(normal.xy, offset);
		if (abs(normal.z) < EPSILON_WIND_GEOMETRY)
			return remaining >= 0.0f;
		float boundary = remaining / normal.z;
		if (normal.z > 0.0f)
			high = min(high, boundary);
		else
			low = max(low, boundary);
		return low < high;
	}

	bool SampleHeight(float2 position, float3 origin, float3 forward, float roll,
		float extent, float halfWidth, float above, float below, float bandLow, float bandHigh,
		out float height)
	{
		height = 0.0f;
		if (extent <= 0.0f || halfWidth <= 0.0f || above <= 0.0f)
			return false;
		float3 right, up;
		GetAxes(forward, roll, right, up);
		float low = bandLow - origin.z;
		float high = bandHigh - origin.z;
		float2 offset = position - origin.xy;
		if (!ClipHeight(forward, 0.0f, offset, low, high) ||
			!ClipHeight(-forward, extent, offset, low, high))
			return false;
		float3 start = float3(offset, low);
		float3 delta = float3(0.0f, 0.0f, high - low);
		float along = -dot(start, forward) / max(extent, EPSILON_WIND_GEOMETRY);
		float alongDelta = -dot(delta, forward) / max(extent, EPSILON_WIND_GEOMETRY);
		float3 localStart = float3(dot(start, right) / max(halfWidth, EPSILON_WIND_GEOMETRY),
			(dot(start, up) + (below - above) * along) / max(above, EPSILON_WIND_GEOMETRY),
			(along - kWidestFraction) / kTailFraction);
		float3 localDelta = float3(dot(delta, right) / max(halfWidth, EPSILON_WIND_GEOMETRY),
			(dot(delta, up) + (below - above) * alongDelta) / max(above, EPSILON_WIND_GEOMETRY),
			alongDelta / kTailFraction);
		float a = dot(localDelta, localDelta);
		float b = dot(localStart, localDelta);
		float c = dot(localStart, localStart) - 1.0f;
		if (a <= EPSILON_WIND_GEOMETRY * EPSILON_WIND_GEOMETRY) {
			height = origin.z + (low + high) * 0.5f;
			return c < 0.0f;
		}
		float discriminant = b * b - a * c;
		if (discriminant <= 0.0f)
			return false;
		float root = sqrt(discriminant);
		float first = max((-b - root) / a, 0.0f);
		float last = min((-b + root) / a, 1.0f);
		if (first >= last)
			return false;
		height = origin.z + lerp(low, high, (first + last) * 0.5f);
		return true;
	}

	float3 Sample(float3 offset, float3 forward, float roll, float extent, float halfWidth,
		float above, float below, float softness, float outwardFlow, float downwash,
		out float intensity)
	{
		intensity = 0.0f;
		if (extent <= 0.0f || halfWidth <= 0.0f || above <= 0.0f)
			return 0.0f.xxx;
		float3 right, up;
		GetAxes(forward, roll, right, up);
		float along = -dot(offset, forward) / max(extent, EPSILON_WIND_GEOMETRY);
		float longitudinal = (along - kWidestFraction) / kTailFraction;
		float sectionScale = sqrt(max(1.0f - longitudinal * longitudinal, 0.0f));
		float width = halfWidth * sectionScale;
		float side = dot(offset, right);
		float height = dot(offset, up) + along * (below - above);
		if (along <= 0.0f || along >= 1.0f || width <= 0.0f)
			return 0.0f.xxx;
		float edge = clamp(softness, kMinimumEdgeSoftness, kMaximumEdgeSoftness);
		float sideRatio = side / max(width, EPSILON_WIND_GEOMETRY);
		float heightRatio = height / max(above * sectionScale, EPSILON_WIND_GEOMETRY);
		float radialRatio = length(float2(sideRatio, heightRatio));
		intensity = smoothstep(0.0f, edge, along) * (1.0f - smoothstep(0.0f, 1.0f, along)) *
		            (1.0f - smoothstep(1.0f - edge, 1.0f, radialRatio));
		return normalize(forward + right * (sideRatio * outwardFlow) - up * downwash) * intensity;
	}
}

#endif
