#pragma once

#include <algorithm>
#include <cstdint>

// Engine-free decisions behind the lit-area tile cap, unit-testable without the game runtime.
namespace LightLimitFixLitCap
{
	/// Bits the demand shader ORs per light, one for each way a lit sample resolved.
	enum Reason : uint32_t
	{
		kNoProjection = 1,
		kSpot = 2,
		kNoTile = 4,
		kFading = 8,
		kBehindHemisphere = 16,
		kLit = 32,
		kBlocked = 64,
	};

	/// Reasons that carry a verdict. A light with no tile yet or still fading in reports nothing
	/// and keeps its previous reading.
	inline constexpr uint32_t kMeasuredMask = kNoProjection | kSpot | kBehindHemisphere | kLit | kBlocked;
	/// Reasons that came from an actual shadow-map comparison.
	inline constexpr uint32_t kSampledMask = kLit | kBlocked;

	/// Share of the gap to a lower reading closed per drained sample (about 0.75 s at 60 fps).
	inline constexpr float kReleaseAlpha = 0.03f;
	/// A light is capped below this share of the best-lit light and released from this share up.
	inline constexpr float kCapOnRatio = 0.10f;
	inline constexpr float kCapOffRatio = 0.20f;
	/// Lights with a larger range are ambient fill and never set the reference.
	inline constexpr float kRefMaxRadius = 2000.0f;

	inline bool IsMeasured(uint32_t reason)
	{
		return (reason & kMeasuredMask) != 0;
	}

	/// Instant attack, slow release.
	inline float Smooth(float smoothed, float sample)
	{
		return sample > smoothed ? sample : smoothed + (sample - smoothed) * kReleaseAlpha;
	}

	/// Whether a light with this reason and range may set the reference lit area.
	inline bool SetsReference(uint32_t reason, float radius)
	{
		return radius <= kRefMaxRadius && (reason & kSampledMask) != 0;
	}

	/// Hysteresis latch on the share of the best-lit light; a zero reference leaves the state alone.
	inline bool NextCapped(bool capped, float smoothed, float reference)
	{
		if (reference <= 0.0f)
			return capped;
		const float ratio = smoothed / reference;
		if (!capped)
			return ratio < kCapOnRatio;
		return ratio < kCapOffRatio;
	}
}
