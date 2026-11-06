#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SnowDeformationMath
{
	constexpr float Pi = 3.14159265f;

	/** @brief Mass per footprint area for a body of a_mass resting on a circle of a_radius; 0 when either is unknown. */
	inline float ContactPressure(float a_mass, float a_radius)
	{
		return a_mass > 0.0f && a_radius > 1e-3f ? a_mass / (Pi * a_radius * a_radius) : 0.0f;
	}

	/**
	 * @brief How much of a full print a body leaves, from 0 for something that settles on top of the snow to 1 for something that sinks fully.
	 * @details Smoothly interpolated on a log scale between a_light and a_heavy pressure. A pressure of 0 means the weight is unknown and leaves a full print.
	 */
	inline float WeightScale(float a_pressure, float a_light, float a_heavy)
	{
		if (!(a_pressure > 0.0f))
			return 1.0f;
		const float span = std::log(std::max(a_heavy, a_light * 1.001f) / std::max(a_light, 1e-9f));
		const float t = std::clamp(std::log(a_pressure / std::max(a_light, 1e-9f)) / span, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

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

	/** @brief Whether a hand moved at least a_minMotion across the ground this frame, so it brushes the snow instead of resting in it. */
	inline bool IsBrushing(float a_moveX, float a_moveY, float a_minMotion)
	{
		return a_moveX * a_moveX + a_moveY * a_moveY >= a_minMotion * a_minMotion;
	}
}
