#pragma once

#include "RE/Skyrim.h"

#include "Utils/MathUtils.h"

#include <SimpleMath.h>

#include <algorithm>
#include <cmath>

namespace WindMath
{
	template <class Range>
	inline float ClampToRange(float a_value, const Range& a_range, float a_default) noexcept
	{
		return Util::ClampFinite(a_value, a_range.minimum, a_range.maximum, a_default);
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
