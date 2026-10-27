#pragma once

#include <bit>
#include <cstdint>
#include <immintrin.h>
#include <intrin.h>

#if defined(__clang__)
#	define CS_TARGET_AVX2 __attribute__((target("avx2")))
#else
#	define CS_TARGET_AVX2
#endif

namespace Util
{
	/** @brief True when the CPU and the operating system both support AVX2. */
	[[nodiscard]] inline bool HasAvx2()
	{
		static const bool supported = [] {
			int regs[4]{};
			__cpuidex(regs, 0, 0);
			if (regs[0] < 7)
				return false;
			__cpuidex(regs, 1, 0);
			constexpr int kOsXsave = 1 << 27;
			constexpr int kAvx = 1 << 28;
			if ((regs[2] & kOsXsave) == 0 || (regs[2] & kAvx) == 0)
				return false;
			constexpr unsigned long long kXmmYmmState = 0x6;
			if ((_xgetbv(0) & kXmmYmmState) != kXmmYmmState)
				return false;
			__cpuidex(regs, 7, 0);
			constexpr int kAvx2 = 1 << 5;
			return (regs[1] & kAvx2) != 0;
		}();
		return supported;
	}

	/** @brief Returns the index of a_needle in a_data, or a_size when it is absent. One element at a time. */
	[[nodiscard]] inline std::uint32_t FindPointerScalar(const void* const* a_data, std::uint32_t a_size, const void* a_needle)
	{
		for (std::uint32_t i = 0; i < a_size; ++i) {
			if (a_data[i] == a_needle)
				return i;
		}
		return a_size;
	}

	/** @brief AVX2 form of FindPointerScalar. Callers must check HasAvx2 first. */
	[[nodiscard]] CS_TARGET_AVX2 inline std::uint32_t FindPointerAvx2(const void* const* a_data, std::uint32_t a_size, const void* a_needle)
	{
		constexpr std::uint32_t kLanes = 4;
		const auto needle = _mm256_set1_epi64x(static_cast<long long>(reinterpret_cast<std::uintptr_t>(a_needle)));
		std::uint32_t i = 0;
		for (; i + kLanes <= a_size; i += kLanes) {
			const auto equal = _mm256_cmpeq_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_data + i)), needle);
			if (const auto mask = static_cast<std::uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(equal))))
				return i + static_cast<std::uint32_t>(std::countr_zero(mask));
		}
		return i + FindPointerScalar(a_data + i, a_size - i, a_needle);
	}

	/** @brief Returns the index of the first a_needle in a_data, or a_size when it is absent. */
	[[nodiscard]] inline std::uint32_t FindPointer(const void* const* a_data, std::uint32_t a_size, const void* a_needle)
	{
		return HasAvx2() ? FindPointerAvx2(a_data, a_size, a_needle) : FindPointerScalar(a_data, a_size, a_needle);
	}
}
