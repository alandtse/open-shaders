#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace NR::MaterialStrength
{
	/** @brief Index of each strength, matching the NeuralRenderingCategory ids Masks2 encodes. */
	inline constexpr uint32_t kNone = 0, kSkin = 1, kHair = 2, kEyes = 3, kFoliage = 4, kLandscape = 5, kCloth = 6, kMetal = 7;
	/** @brief Number of strengths, one per NeuralRenderingCategory id. */
	inline constexpr uint32_t kCount = 8;
	/** @brief Range the shader's cbuffer reads: 1 applies NR fully to a material, 0 bypasses NR there. */
	inline constexpr float kMinStrength = 0.0f, kMaxStrength = 1.0f;
	/** @brief Largest edge-softness radius in pixels CategoryAlphaCS.hlsl's box filter runs. */
	inline constexpr uint32_t kMaxEdgeSoftness = 4;
	/** @brief Strength a category gets by default and when the config supplies a non-finite value: NR on every material. */
	inline constexpr std::array<float, kCount> kDefaults{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	/** @brief The Characters Only quick selection: skin, hair and eyes at full strength, every other material off. */
	inline constexpr std::array<float, kCount> kCharactersOnly{ 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	/**
	 * @brief Per-category NR strength in NeuralRenderingCategory id order (None, Skin, Hair,
	 *        Eyes, Foliage, Landscape, Cloth, Metal): one applies NR fully to the material, zero bypasses it.
	 *        Edge softness is the box-filter radius, in pixels, that blends a material's strength
	 *        into its neighbours.
	 */
	struct Values
	{
		std::array<float, kCount> strength = kDefaults;
		uint32_t edgeSoftness = 0;

		bool operator==(const Values&) const = default;
	};

	/**
	 * @brief Clamps the strengths and the softness radius to the range CategoryAlphaCS.hlsl
	 *        reads, so a hand-edited config cannot feed the shader an out-of-contract value.
	 */
	inline Values Sanitize(Values values)
	{
		for (uint32_t i = 0; i < kCount; ++i)
			values.strength[i] = std::isfinite(values.strength[i]) ? std::clamp(values.strength[i], kMinStrength, kMaxStrength) : kDefaults[i];
		values.edgeSoftness = std::min(values.edgeSoftness, kMaxEdgeSoftness);
		return values;
	}

	/** @brief Whether the material with this NeuralRenderingCategory id has any strength, the state its checkbox shows. */
	[[nodiscard]] inline bool Selected(const Values& values, uint32_t category)
	{
		return category < kCount && values.strength[category] > kMinStrength;
	}

	/** @brief Selects or clears one material (strength 1 or 0). */
	inline void SetSelected(Values& values, uint32_t category, bool selected)
	{
		if (category < kCount)
			values.strength[category] = selected ? kMaxStrength : kMinStrength;
	}

	/** @brief Whether some material is below full strength, so by-material needs to be on. */
	[[nodiscard]] inline bool AnyBelowFull(const Values& values)
	{
		return std::any_of(values.strength.begin(), values.strength.end(), [](float value) { return value < kMaxStrength; });
	}
}
