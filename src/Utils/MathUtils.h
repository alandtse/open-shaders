#pragma once
#include <SimpleMath.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <string_view>

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

	/**
	 * @brief Clamps a floating-point value to [min, max] if finite, otherwise returns fallback default.
	 */
	template <std::floating_point T>
	[[nodiscard]] constexpr T ClampFinite(T a_value, T a_min, T a_max, T a_default) noexcept
	{
		return std::isfinite(a_value) ? std::clamp(a_value, a_min, a_max) : a_default;
	}

	/**
	 * @brief Alias for ClampFinite.
	 */
	template <std::floating_point T>
	[[nodiscard]] constexpr T ClampFiniteOrDefault(T a_value, T a_min, T a_max, T a_default) noexcept
	{
		return ClampFinite(a_value, a_min, a_max, a_default);
	}

	/** @brief Smallest range Smoothstep divides by, so equal edges give a step instead of NaN. */
	inline constexpr float kSmoothstepMinimumRange = 1e-4f;

	/**
	 * @brief Hermite smoothstep of @p a_value between two edges; 0 at or below @p a_edge0, 1 at or above @p a_edge1.
	 * @details A range narrower than kSmoothstepMinimumRange is widened to it, so equal edges never divide by zero.
	 */
	[[nodiscard]] inline float Smoothstep(float a_edge0, float a_edge1, float a_value) noexcept
	{
		const float range = std::max(a_edge1 - a_edge0, kSmoothstepMinimumRange);
		const float amount = std::clamp((a_value - a_edge0) / range, 0.0f, 1.0f);
		return amount * amount * (3.0f - 2.0f * amount);
	}

	/**
	 * @brief Fraction of the way a value moves toward its target in one step of an exponential approach with time constant @p a_tau.
	 * @param a_deltaTime Step length in seconds.
	 * @param a_tau Time constant in seconds; zero or less jumps to the target (1).
	 */
	template <std::floating_point T>
	[[nodiscard]] inline T ExpBlend(T a_deltaTime, T a_tau) noexcept
	{
		return a_tau > T(0) ? T(1) - std::exp(-a_deltaTime / a_tau) : T(1);
	}

	/** @brief True when every component of the vector is finite. */
	[[nodiscard]] inline bool IsFinite(const DirectX::SimpleMath::Vector3& a_value) noexcept
	{
		return std::isfinite(a_value.x) && std::isfinite(a_value.y) && std::isfinite(a_value.z);
	}

	/**
	 * @brief Unit vector along @p a_value, or the zero vector when it is not finite or no longer than @p a_minimumLength.
	 * @details DirectX's Vector3::Normalize has no zero guard.
	 */
	[[nodiscard]] inline DirectX::SimpleMath::Vector3 SafeNormalize(const DirectX::SimpleMath::Vector3& a_value, float a_minimumLength = 1e-4f) noexcept
	{
		const float length = a_value.Length();
		return std::isfinite(length) && length > a_minimumLength ? a_value / length : DirectX::SimpleMath::Vector3{};
	}

	/** @brief FNV-1a 32-bit offset basis and prime. */
	inline constexpr std::uint32_t kFnv1a32Basis = 2166136261u;
	inline constexpr std::uint32_t kFnv1a32Prime = 16777619u;
	/** @brief FNV-1a 64-bit offset basis and prime. */
	inline constexpr std::uint64_t kFnv1a64Basis = 14695981039346656037ull;
	inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

	/** @brief 32-bit FNV-1a of the bytes of @p a_text; pass a previous result as @p a_seed to hash several strings in sequence. */
	[[nodiscard]] constexpr std::uint32_t Fnv1a32(std::string_view a_text, std::uint32_t a_seed = kFnv1a32Basis) noexcept
	{
		std::uint32_t hash = a_seed;
		for (const char c : a_text) {
			hash ^= static_cast<unsigned char>(c);
			hash *= kFnv1a32Prime;
		}
		return hash;
	}

	/** @brief 64-bit FNV-1a of the bytes of @p a_text; pass a previous result as @p a_seed to hash several strings in sequence. */
	[[nodiscard]] constexpr std::uint64_t Fnv1a64(std::string_view a_text, std::uint64_t a_seed = kFnv1a64Basis) noexcept
	{
		std::uint64_t hash = a_seed;
		for (const char c : a_text) {
			hash ^= static_cast<unsigned char>(c);
			hash *= kFnv1a64Prime;
		}
		return hash;
	}
}
