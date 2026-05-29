#pragma once

#include <algorithm>
#include <cstdint>

// Pure helpers extracted from ShadowCasterManager so they can be unit-tested
// without the game/RE runtime. No engine types in any signature.
namespace ShadowCasterManager
{
	// A shadow-light accumulator slot can hold heap garbage between our prepass
	// and the engine's read. Treat a pointer as plausible only if it is
	// non-null, inside the x64 user-mode canonical range, and 8-byte aligned
	// (BSShadowLight is pointer-aligned). ForEachShadowLight stops iterating at
	// the first implausible entry rather than dereferencing garbage.
	inline bool IsPlausibleShadowLightPtr(std::uintptr_t raw) noexcept
	{
		return raw != 0 && raw < 0x0000800000000000ull && (raw & 0x7) == 0;
	}

	// 90th-percentile of the most-recent min(count, Window) frame-time samples
	// in `ring`. Percentile is order-independent, so the first `n` entries are
	// sampled directly (ring head/wraparound doesn't matter). Returns the 60fps
	// fallback (16.67 ms) before any samples exist.
	template <int Window>
	inline float FrameTimePercentile90(const float (&ring)[Window], int count)
	{
		if (count == 0)
			return 16.67f;
		const int n = std::min(count, Window);
		float tmp[Window];
		std::copy(ring, ring + n, tmp);
		const int idx = static_cast<int>(n * 0.9f);
		std::nth_element(tmp, tmp + idx, tmp + n);
		return tmp[idx];
	}
}
