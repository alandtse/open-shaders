#pragma once

#include <cmath>
#include <cstdint>

namespace NR::Reconstruction
{
	/** @brief Reconstruction used for both eyes of one model evaluation. */
	enum class Mode : uint32_t
	{
		Bilinear,
		RgbGuided,
		Native
	};

	/** @brief Resolves the effective model scale and validated per-eye extents before either eye dispatches. */
	[[nodiscard]] inline Mode Select(bool guided, float scale,
		uint32_t width, uint32_t height, uint32_t modelWidth, uint32_t modelHeight)
	{
		if (!std::isfinite(scale) || scale <= 0.0f || scale > 1.0f || !width || !height || !modelWidth || !modelHeight ||
			modelWidth > width || modelHeight > height || (modelWidth == width && modelHeight == height))
			return Mode::Native;
		if (!guided)
			return Mode::Bilinear;
		return Mode::RgbGuided;
	}

	/** @brief Stable names for the effective reconstruction reported by devbench. */
	[[nodiscard]] constexpr const char* Name(Mode mode)
	{
		switch (mode) {
		case Mode::Bilinear:
			return "bilinear";
		case Mode::RgbGuided:
			return "rgb_guided";
		default:
			return "native";
		}
	}
}
