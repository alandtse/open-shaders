#pragma once

#include "MaterialStrength.h"

#include <cstdint>

namespace NR::MaterialMap
{
	/** @brief One filter bit per NeuralRenderingCategory id, None through Metal. */
	inline constexpr uint32_t kBits = MaterialStrength::kCount;
	/** @brief Every category enabled: the filter's default, and what Sanitize keeps. */
	inline constexpr uint32_t kAllCategories = (1u << kBits) - 1u;

	/**
	 * @brief What the material map draws in place of the composited pixel, mirrored by
	 *        kMaterialMapCategory / kMaterialMapStrength in ModeValues.hlsli.
	 */
	enum class Mode : uint32_t
	{
		kCategory = 0,  ///< The category's fixed debug colour, blended over the composited image.
		kStrength = 1   ///< Grayscale ramp of the strength Apply Neural Rendering by Material gives the pixel.
	};
	/** @brief Highest Mode value a hand-edited config may select. */
	inline constexpr uint32_t kMaxMode = static_cast<uint32_t>(Mode::kStrength);

	/** @brief Filter bit for a category id; an id outside the range has no bit, so it is never drawn. */
	inline constexpr uint32_t Bit(uint32_t category) { return category < kBits ? 1u << category : 0u; }

	/** @brief Drops filter bits above the category range, so a hand-edited config cannot set them. */
	inline constexpr uint32_t Sanitize(uint32_t filter) { return filter & kAllCategories; }

	/** @brief True when a category's bit is set in the filter. */
	inline constexpr bool Contains(uint32_t filter, uint32_t category) { return (filter & Bit(category)) != 0u; }

	/** @brief Sets or clears one category's bit and leaves the others alone. */
	inline constexpr uint32_t Set(uint32_t filter, uint32_t category, bool enabled)
	{
		return enabled ? filter | Bit(category) : filter & ~Bit(category);
	}

	/**
	 * @brief Fixed debug colour per category, indexed by NeuralRenderingCategory id, for the settings
	 *        panel's legend swatches. DebugColor in package/Shaders/Common/NeuralRenderingCategory.hlsli
	 *        draws the same colours and the two must agree.
	 */
	struct Color
	{
		float r, g, b;
	};
	inline constexpr Color kColors[kBits]{
		{ 0.05f, 0.05f, 0.05f },
		{ 1.0f, 0.0f, 0.0f },
		{ 1.0f, 0.5f, 0.0f },
		{ 1.0f, 1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
		{ 0.0f, 1.0f, 1.0f },
		{ 1.0f, 0.0f, 1.0f },
		{ 1.0f, 1.0f, 1.0f },
	};

	/**
	 * @brief Colour ramp the Strength view reads, from strength 0 to 1 in even steps, for the settings
	 *        panel's legend bar. StrengthColor in package/Shaders/Common/NeuralRenderingCategory.hlsli
	 *        draws the same stops and the two must agree.
	 */
	inline constexpr Color kStrengthRamp[5]{
		{ 0.19f, 0.07f, 0.23f },
		{ 0.25f, 0.55f, 0.99f },
		{ 0.21f, 0.91f, 0.51f },
		{ 0.96f, 0.81f, 0.20f },
		{ 0.48f, 0.02f, 0.01f },
	};
}
