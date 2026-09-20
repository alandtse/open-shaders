#pragma once

#include "../../Utils/MathUtils.h"

// Pure settings-sanitization helper extracted from LightLimitFix so it can be
// unit-tested without the game/RE runtime.
namespace LightLimitFixSanitize
{
	inline float SanitizeFloat(float v, float lo, float hi)
	{
		return Util::ClampFinite(v, lo, hi, lo);
	}
}
