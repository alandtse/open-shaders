#pragma once

#include "RE/Skyrim.h"

#include <SimpleMath.h>

#include <algorithm>
#include <cmath>

namespace WindMath
{
	inline float ClampFiniteOrDefault(float a_value, float a_minimum, float a_maximum,
		float a_default) noexcept
	{
		return std::isfinite(a_value) ? std::clamp(a_value, a_minimum, a_maximum) : a_default;
	}

	template <class Range>
	inline float ClampFiniteOrDefault(float a_value, const Range& a_range, float a_default) noexcept
	{
		return ClampFiniteOrDefault(a_value, a_range.minimum, a_range.maximum, a_default);
	}

	inline float SquaredDistance(const RE::NiPoint3& a_left, const RE::NiPoint3& a_right) noexcept
	{
		const auto difference = a_left - a_right;
		return difference.x * difference.x + difference.y * difference.y + difference.z * difference.z;
	}

	[[nodiscard]] inline DirectX::SimpleMath::Vector3 GetHorizontalVelocityDirection(
		const RE::NiPoint3& a_velocity) noexcept
	{
		const float length = std::sqrt(a_velocity.x * a_velocity.x + a_velocity.y * a_velocity.y);
		if (!std::isfinite(length) || length <= 1e-4f)
			return { 1.0f, 0.0f, 0.0f };
		return { a_velocity.x / length, a_velocity.y / length, 0.0f };
	}
}
