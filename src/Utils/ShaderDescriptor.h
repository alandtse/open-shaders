#pragma once

#include <cstdint>

/// Helpers for the per-draw shader descriptors the plugin uploads to the GPU.
namespace Util::ShaderDescriptor
{
	/** @brief Visits each legal descriptor when the flags in the mask may vary independently. */
	constexpr void ForEachSpecialization(std::uint32_t a_base, std::uint32_t a_mask, auto&& a_visit)
	{
		a_base &= ~a_mask;
		for (std::uint32_t flags = a_mask;; flags = (flags - 1) & a_mask) {
			a_visit(a_base | flags);
			if (flags == 0)
				return;
		}
	}

	inline constexpr std::uint32_t kLightingLinearACEScg = 1u << 7;
	inline constexpr std::uint32_t kLightingIBLLuminanceRatio = 1u << 8;
	inline constexpr std::uint32_t kLightingNoLegacySubsurface = 1u << 17;
	/**
	 * @brief Pixel descriptor bits left in the per-draw upload.
	 *
	 * Bits that took part in the shader lookup are compile-time and cleared, so shaders only
	 * see non-shader bits. Bits in a_runtimeBits stay even when they are part of the lookup,
	 * for shaders that read them at runtime.
	 * @param a_pixelDescriptor Pixel descriptor the game requested.
	 * @param a_lookupDescriptor Pixel descriptor used for the shader lookup.
	 * @param a_runtimeBits Lookup bits the pixel shader also reads at runtime.
	 */
	[[nodiscard]] constexpr std::uint32_t UploadedPixelDescriptor(std::uint32_t a_pixelDescriptor, std::uint32_t a_lookupDescriptor, std::uint32_t a_runtimeBits)
	{
		return a_pixelDescriptor & ~(a_lookupDescriptor & ~a_runtimeBits);
	}
}
