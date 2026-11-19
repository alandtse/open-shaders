#pragma once

#include <algorithm>
#include <array>
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
	};

	/** @brief Whether sanitized strengths reduce the neural edit on any material. */
	[[nodiscard]] constexpr bool HasEdits(const Values& values) noexcept
	{
		return std::any_of(values.strength.begin(), values.strength.end(), [](float strength) { return strength < kMaxStrength; });
	}

	/** @brief Work needed to retain the provider's protection binding without repeating an identity mask. */
	enum class AlphaUpdate
	{
		kReuse,
		kClear,
		kDraw
	};

	/** @brief An identity mask is cleared once after allocation or the last category edit. */
	[[nodiscard]] constexpr AlphaUpdate PlanAlphaUpdate(const Values& values, bool identityCached) noexcept
	{
		return HasEdits(values) ? AlphaUpdate::kDraw : (identityCached ? AlphaUpdate::kReuse : AlphaUpdate::kClear);
	}

}
