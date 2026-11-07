#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SnowDeformationMath
{
	/** @brief Sample points along a capsule axis: one per capsule diameter of length, at least 1 and at most a_max. */
	inline size_t CapsuleSampleCount(float a_length, float a_radius, size_t a_max)
	{
		const float count = std::ceil(a_length / (2.0f * std::max(a_radius, 1e-3f)));
		return std::clamp(static_cast<size_t>(std::max(count, 1.0f)), size_t{ 1 }, std::max(a_max, size_t{ 1 }));
	}

	/** @brief Position of sample a_index of a_count along the axis, 0 at point A and 1 at point B; a single sample sits at the middle. */
	inline float CapsuleSampleParameter(size_t a_index, size_t a_count)
	{
		return a_count <= 1 ? 0.5f : static_cast<float>(a_index) / static_cast<float>(a_count - 1);
	}

	/** @brief Whether a capsule is a held item rather than a hand or finger: its axis is longer than a_handLength. */
	inline bool IsHeldItemCapsule(float a_length, float a_handLength)
	{
		return a_length > a_handLength;
	}

	/** @brief Radius of one round print covering a hand whose capsules span a_extentX by a_extentY on the ground, never below a_minRadius. */
	inline float BlobRadius(float a_extentX, float a_extentY, float a_minRadius)
	{
		return std::max(0.5f * std::max(a_extentX, a_extentY), a_minRadius);
	}

	/**
	 * @brief Whether a sphere of a_radius at a_centreZ reaches the snow over land at a_landZ.
	 * @param a_reach How far above the land the snow surface is, never below the ground contact tolerance.
	 * @param a_belowLimit How far under the land a sphere may sit before it counts as underground and is ignored.
	 */
	inline bool TouchesSnow(float a_centreZ, float a_radius, float a_landZ, float a_reach, float a_belowLimit)
	{
		const float clearance = a_centreZ - a_radius - a_landZ;
		return clearance <= a_reach && clearance >= -a_belowLimit;
	}
}
