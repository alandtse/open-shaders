#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>

namespace Util
{
	/** @brief Axis-aligned box grown one point at a time. */
	struct PointBox
	{
		float3 min{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
		float3 max{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };

		/** @brief Grows the box to contain a_point. */
		void Include(const float3& a_point)
		{
			min = float3(std::min(min.x, a_point.x), std::min(min.y, a_point.y), std::min(min.z, a_point.z));
			max = float3(std::max(max.x, a_point.x), std::max(max.y, a_point.y), std::max(max.z, a_point.z));
		}

		/** @brief True once at least one point has been included. */
		[[nodiscard]] bool Valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

		/** @brief Grows every face outwards by a_margin. */
		void Expand(float a_margin)
		{
			const float3 margin{ a_margin, a_margin, a_margin };
			min = min - margin;
			max = max + margin;
		}
	};

	/**
	 * @brief Fixed-capacity set of world-space points whose projected extent bounds a target.
	 *        Holds no heap allocation, so it can be built for every candidate every frame.
	 */
	struct BoundPoints
	{
		/** @brief Most points a set holds: an authored box and a skeleton box. */
		static constexpr size_t kCapacity = 16;
		std::array<float3, kCapacity> points{};
		size_t count = 0;

		/** @brief Appends a point; a full set drops it. */
		void Add(const float3& a_point)
		{
			if (count < kCapacity)
				points[count++] = a_point;
		}

		/** @brief Appends the eight corners of an axis-aligned box. */
		void AddBox(const float3& a_min, const float3& a_max)
		{
			for (const float x : { a_min.x, a_max.x })
				for (const float y : { a_min.y, a_max.y })
					for (const float z : { a_min.z, a_max.z })
						Add({ x, y, z });
		}

		/** @brief The points added so far. */
		[[nodiscard]] std::span<const float3> View() const { return { points.data(), count }; }
	};
}
