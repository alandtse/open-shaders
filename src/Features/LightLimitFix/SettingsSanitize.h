#pragma once

#include <algorithm>
#include <cmath>

// Pure settings-sanitization helper extracted from LightLimitFix so it can be
// unit-tested without the game/RE runtime.
namespace LightLimitFixSanitize
{
	// Clamp a user/config float to [lo, hi]. std::clamp passes NaN through
	// unchanged (every NaN comparison is false), which would let a non-finite
	// value reach the GPU and cause divisions / infinite loops / corruption, so
	// reject non-finite inputs explicitly and fall back to the lower bound for
	// degraded-but-stable behavior.
	inline float SanitizeFloat(float v, float lo, float hi)
	{
		return std::isfinite(v) ? std::clamp(v, lo, hi) : lo;
	}
}
