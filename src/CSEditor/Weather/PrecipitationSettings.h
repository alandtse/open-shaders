#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace PrecipitationSerialization
{
	/** @brief Decodes integer box sizes and legacy serialized float views of their bits. */
	inline std::optional<std::uint32_t> ReadBoxSize(double value)
	{
		if (!std::isfinite(value))
			return std::nullopt;
		if (value >= 1.0 && value <= (std::numeric_limits<std::uint32_t>::max)())
			return static_cast<std::uint32_t>(value);
		if (value > 0.0 && value < (std::numeric_limits<float>::min)()) {
			constexpr int kFloatSubnormalExponent = 149;
			const auto decoded = std::round(std::ldexp(value, kFloatSubnormalExponent));
			if (decoded >= 1.0 && decoded <= (std::numeric_limits<std::uint32_t>::max)())
				return static_cast<std::uint32_t>(decoded);
		}
		return std::nullopt;
	}
}
