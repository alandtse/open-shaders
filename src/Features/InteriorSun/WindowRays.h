#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace InteriorSunRays
{
	/** @brief Nearest hit of a ray against triangles [first, end) of a triangle list (Moller-Trumbore); FLT_MAX for none within reach. */
	inline float CastRay(const float3& origin, const float3& direction, float reach, const std::vector<float3>& triangles, uint32_t first, uint32_t end)
	{
		float nearest = FLT_MAX;
		for (uint32_t i = first; i < end; i += 3) {
			const float3 edge1 = triangles[i + 1] - triangles[i];
			const float3 edge2 = triangles[i + 2] - triangles[i];
			const float3 p = direction.Cross(edge2);
			const float det = edge1.Dot(p);
			if (std::abs(det) < 1e-6f)
				continue;
			const float3 s = origin - triangles[i];
			const float u = s.Dot(p) / det;
			if (u < 0.0f || u > 1.0f)
				continue;
			const float3 q = s.Cross(edge1);
			const float v = direction.Dot(q) / det;
			if (v < 0.0f || u + v > 1.0f)
				continue;
			const float t = edge2.Dot(q) / det;
			if (t > 0.0f && t < reach && t < nearest)
				nearest = t;
		}
		return nearest;
	}

	/** @brief Whether a ray enters an axis-aligned box within reach. */
	inline bool RayHitsBox(const float3& origin, const float3& direction, float reach, const float3& boxMin, const float3& boxMax)
	{
		float enter = 0.0f, exit = reach;
		for (int axis = 0; axis < 3; ++axis) {
			const float o = (&origin.x)[axis], d = (&direction.x)[axis];
			const float lo = (&boxMin.x)[axis], hi = (&boxMax.x)[axis];
			if (std::abs(d) < 1e-8f) {
				if (o < lo || o > hi)
					return false;
				continue;
			}
			float t0 = (lo - o) / d, t1 = (hi - o) / d;
			if (t0 > t1)
				std::swap(t0, t1);
			enter = std::max(enter, t0);
			exit = std::min(exit, t1);
			if (enter > exit)
				return false;
		}
		return true;
	}
}
