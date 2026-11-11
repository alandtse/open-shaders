#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace SnowDeformationMath
{
	constexpr float Pi = 3.14159265f;

	/** @brief Ground footprint of a sphere or round print of a_radius. */
	inline float CircleFootprint(float a_radius)
	{
		return Pi * a_radius * a_radius;
	}

	/** @brief Ground footprint of a box whose half extents along the ground axes are a_halfX and a_halfY. */
	inline float BoxFootprint(float a_halfX, float a_halfY)
	{
		return 4.0f * std::max(a_halfX, 0.0f) * std::max(a_halfY, 0.0f);
	}

	/** @brief Ground footprint of a capsule of a_radius whose axis covers a_groundLength on the ground. */
	inline float CapsuleFootprint(float a_groundLength, float a_radius)
	{
		return 2.0f * a_radius * (std::max(a_groundLength, 0.0f) + 2.0f * a_radius);
	}

	/** @brief Weight per footprint area for a body of a_weight resting on a_area; 0 when either is unknown. */
	inline float ContactPressure(float a_weight, float a_area)
	{
		return a_weight > 0.0f && a_area > 1e-3f ? a_weight / a_area : 0.0f;
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

	/** @brief Fraction of an object of a_height whose bottom at a_bottom sits under the snow surface at a_surface, from 0 (clear) to 1 (buried). */
	inline float SubmergedFraction(float a_bottom, float a_surface, float a_height)
	{
		return std::clamp((a_surface - a_bottom) / std::max(a_height, 1e-3f), 0.0f, 1.0f);
	}

	/** @brief Velocity multiplier over a_dt seconds for drag of a_strength per second at full burial, scaled by the buried fraction. */
	inline float DragFactor(float a_strength, float a_submerged, float a_dt)
	{
		return std::exp(-std::max(a_strength, 0.0f) * std::clamp(a_submerged, 0.0f, 1.0f) * std::max(a_dt, 0.0f));
	}

	/**
	 * @brief Height above the land where an object with a_weightScale (0 light, 1 heavy) comes to rest in a blanket of a_blanket
	 *        whose surface a_depression has been dug down: light objects float near the surface, heavy ones sink by their weight share of the blanket.
	 */
	inline float RestHeight(float a_blanket, float a_depression, float a_weightScale)
	{
		const float blanket = std::max(a_blanket, 0.0f);
		const float surface = blanket - std::clamp(a_depression, 0.0f, blanket);
		return std::max(surface - blanket * std::clamp(a_weightScale, 0.0f, 1.0f), 0.0f);
	}

	/** @brief Upward speed that lifts an object whose bottom is a_depth under its floor, growing by a_spring per second and capped at a_maxRise. */
	inline float FloorRiseSpeed(float a_depth, float a_spring, float a_maxRise)
	{
		return std::clamp(a_depth * a_spring, 0.0f, a_maxRise);
	}

	/** @brief Fraction of walking speed lost wading a_buried deep with legs a_legHeight long, reaching a_strength at full burial. */
	inline float WadeSlowdown(float a_buried, float a_legHeight, float a_strength)
	{
		return std::clamp(a_strength, 0.0f, 1.0f) * std::clamp(a_buried / std::max(a_legHeight, 1e-3f), 0.0f, 1.0f);
	}

	/** @brief Whether a race name has a word starting with frost, ice, icy, snow or ghost, so the creature is at home in the cold. */
	inline bool IsColdAdaptedName(std::string_view a_name)
	{
		constexpr std::string_view prefixes[] = { "frost", "ice", "icy", "snow", "ghost" };
		size_t start = 0;
		while (start < a_name.size()) {
			while (start < a_name.size() && !std::isalpha(static_cast<unsigned char>(a_name[start])))
				++start;
			size_t end = start;
			while (end < a_name.size() && std::isalpha(static_cast<unsigned char>(a_name[end])))
				++end;
			const std::string_view word = a_name.substr(start, end - start);
			for (const auto prefix : prefixes) {
				if (word.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), word.begin(), [](char a_prefix, char a_char) { return a_prefix == std::tolower(static_cast<unsigned char>(a_char)); }))
					return true;
			}
			start = end;
		}
		return false;
	}
}
