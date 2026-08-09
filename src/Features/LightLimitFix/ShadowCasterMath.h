#pragma once

#include "../../Utils/MathUtils.h"

#include <algorithm>
#include <cstdint>

// Pure helpers extracted from ShadowCasterManager so they can be unit-tested
// without the game/RE runtime.
namespace ShadowCasterManager
{
	using Util::HashCombine;
	using Util::HashCombineFloat;
	using Util::QuantizeFloat;

	/// Buckets a stall length for ShadowScheduler's stallHistogram: 0, 1-2,
	/// 3-7, 8-15, 16-44, 45+ (top boundary matches kSleepRedrawIntervalFrames).
	inline uint32_t StallBucket(uint32_t v) noexcept
	{
		if (v == 0)
			return 0;
		if (v <= 2)
			return 1;
		if (v <= 7)
			return 2;
		if (v <= 15)
			return 3;
		if (v <= 44)
			return 4;
		return 5;
	}

	/// Buckets a streak length for ShadowScheduler's demand-streak histograms:
	/// 0, 1-2, 3-7, 8-15, 16-31, 32-63, 64-127, 128-255, 256+.
	inline uint32_t DemandStreakBucket(uint32_t v) noexcept
	{
		if (v == 0)
			return 0;
		if (v <= 2)
			return 1;
		if (v <= 7)
			return 2;
		if (v <= 15)
			return 3;
		if (v <= 31)
			return 4;
		if (v <= 63)
			return 5;
		if (v <= 127)
			return 6;
		if (v <= 255)
			return 7;
		return 8;
	}

	// Rejects heap garbage an accumulator slot can hold between our prepass and
	// the engine's read: below the low 64 KiB null-guard region (near-null like
	// 0x8 would AV), outside the x64 canonical range, or not 8-byte aligned
	// (BSShadowLight is pointer-aligned). Callers stop at the first implausible entry.
	inline bool IsPlausibleShadowLightPtr(std::uintptr_t raw) noexcept
	{
		return raw >= 0x10000ull && raw < 0x0000800000000000ull && (raw & 0x7) == 0;
	}

	// The tile class ladder, shared by the coverage classifier below and the
	// atlas allocator's order mapping (the floor class == one allocator cell).
	inline constexpr float kTileScaleFull = 1.0f;
	inline constexpr float kTileScaleHalf = 0.5f;
	inline constexpr float kTileScaleQuarter = 0.25f;
	inline constexpr float kTileScaleEighth = 0.125f;
	inline constexpr float kTileScaleSixteenth = 0.0625f;
	inline constexpr float kTileScaleFloor = kTileScaleSixteenth;

	// Shadow texels per unit of projected light size. sizeProxy 1.0 (a light
	// dominating the view, or sitting on the viewer) maps past the full
	// class at any sane base resolution; calibration knob.
	inline constexpr float kShadowSizeToTexels = 3072.0f;

	// Demotion evaluates with the size inflated by this factor, so a light
	// hovering at a class boundary doesn't flip classes (a class change
	// invalidates the cached shadow map and forces a redraw).
	inline constexpr float kDemoteHeadroom = 1.4f;

	/// Smallest ladder class whose resolution still meets the target texel
	/// count for a light of the given projected size.
	inline float TileScaleTarget(float sizeProxy, float baseTileTexels) noexcept
	{
		const float targetTexels = sizeProxy * kShadowSizeToTexels;
		float scale = kTileScaleFull;
		while (scale > kTileScaleFloor && baseTileTexels * scale * 0.5f >= targetTexels)
			scale *= 0.5f;
		return scale;
	}

	// Promotion is immediate (quality first); demotion only when even the
	// headroom-inflated size stays below the current class.
	inline float TileScaleForCoverage(float sizeProxy, float baseTileTexels, float currentScale) noexcept
	{
		const float target = TileScaleTarget(sizeProxy, baseTileTexels);
		if (target >= currentScale)
			return target;
		return (TileScaleTarget(sizeProxy * kDemoteHeadroom, baseTileTexels) < currentScale) ? target : currentScale;
	}

	// Percentile (default 90th) of the last min(count, Window) samples in
	// `ring`. Returns the 60fps fallback (16.67 ms) when count <= 0, which
	// also guards std::copy/std::nth_element against a negative n.
	template <int Window>
	inline float FrameTimePercentile(const float (&ring)[Window], int count, float percentile = 0.9f)
	{
		if (count <= 0)
			return 16.67f;
		const int n = std::min(count, Window);
		float tmp[Window];
		std::copy(ring, ring + n, tmp);
		const int idx = std::clamp(static_cast<int>(n * percentile), 0, n - 1);
		std::nth_element(tmp, tmp + idx, tmp + n);
		return tmp[idx];
	}
}
