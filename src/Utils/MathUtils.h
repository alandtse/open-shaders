#pragma once
#include <bit>
#include <cmath>
#include <cstdint>

namespace Util
{
	/** @brief Tolerance for near-zero/near-equal comparisons on normalized [0,1] values (e.g. a degenerate slider range). Not a universal epsilon -- other precision needs (near-zero vector length, perceptual UI thresholds) use their own tolerance. */
	inline constexpr float kNormalizedRangeEpsilon = 1e-5f;

	/**
     * @brief Mixes a 32-bit value into a running 64-bit hash. boost::hash_combine
     * constants (0x9e3779b9, golden-ratio reciprocal) chosen for bit distribution.
     * @param h Running hash to fold into.
     * @param v Value to mix in.
     * @return Updated hash.
     */
	inline std::uint64_t HashCombine(std::uint64_t h, std::uint32_t v) noexcept
	{
		return h ^ (static_cast<std::uint64_t>(v) + 0x9e3779b9ull + (h << 6) + (h >> 2));
	}

	/**
     * @brief Mixes a float into a running 64-bit hash via its bit pattern.
     * @param h Running hash to fold into.
     * @param f Value to mix in.
     * @return Updated hash.
     */
	inline std::uint64_t HashCombineFloat(std::uint64_t h, float f) noexcept
	{
		return HashCombine(h, std::bit_cast<std::uint32_t>(f));
	}

	/**
     * @brief Rounds a value to the nearest multiple of step, e.g. to collapse
     * sub-unit jitter before hashing so it doesn't defeat cache validity.
     * @param f Value to quantize.
     * @param step Quantization step.
     * @return f rounded to the nearest multiple of step.
     */
	inline float QuantizeFloat(float f, float step) noexcept
	{
		return std::round(f / step) * step;
	}
}
