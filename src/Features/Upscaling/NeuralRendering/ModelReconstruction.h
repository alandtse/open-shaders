#pragma once

#include <cmath>
#include <cstdint>

namespace NR::Reconstruction
{
	inline constexpr float kMaximumRgbGuidedScale = 0.5f;
	inline constexpr uint32_t kBilinearFilter = 0, kLanczos3Filter = 1;

	/** @brief Reconstruction used for both eyes of one model evaluation. */
	enum class Mode : uint32_t
	{
		Bilinear,
		Lanczos3,
		BrightnessGuided,
		RgbGuided,
		Native
	};

	/** @brief Unknown saved filters use the inexpensive baseline. */
	[[nodiscard]] constexpr uint32_t SanitizeFilter(uint32_t filter)
	{
		return filter == kLanczos3Filter ? kLanczos3Filter : kBilinearFilter;
	}

	/** @brief Basic filter retained when optional RGB guidance is unavailable. */
	[[nodiscard]] constexpr Mode BasicMode(uint32_t filter)
	{
		return SanitizeFilter(filter) == kLanczos3Filter ? Mode::Lanczos3 : Mode::Bilinear;
	}

	/** @brief Resolves the effective model scale and validated per-eye extents before either eye dispatches. */
	[[nodiscard]] inline Mode Select(uint32_t filter, bool guided, bool rgbGuided, float scale,
		uint32_t width, uint32_t height, uint32_t modelWidth, uint32_t modelHeight)
	{
		if (!std::isfinite(scale) || scale <= 0.0f || scale >= 1.0f || !modelWidth || !modelHeight ||
			modelWidth >= width || modelHeight >= height)
			return Mode::Native;
		if (!guided)
			return BasicMode(filter);
		if (!rgbGuided)
			return Mode::BrightnessGuided;
		return scale <= kMaximumRgbGuidedScale ? Mode::RgbGuided : BasicMode(filter);
	}

	/** @brief Stable names for the effective reconstruction reported by devbench. */
	[[nodiscard]] constexpr const char* Name(Mode mode)
	{
		switch (mode) {
		case Mode::Bilinear:
			return "bilinear";
		case Mode::Lanczos3:
			return "lanczos3";
		case Mode::BrightnessGuided:
			return "brightness_guided";
		case Mode::RgbGuided:
			return "rgb_guided";
		default:
			return "native";
		}
	}
}
