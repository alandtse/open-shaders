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
	//
	// Precondition: lo and hi are finite with lo <= hi. Only the value is
	// untrusted; the bounds are compile-time constants at every call site (the
	// feature's fixed setting ranges), so they are not re-validated here -- a
	// NaN lo would defeat the fallback and hi < lo is std::clamp UB.
	inline float SanitizeFloat(float v, float lo, float hi)
	{
		return std::isfinite(v) ? std::clamp(v, lo, hi) : lo;
	}
}
