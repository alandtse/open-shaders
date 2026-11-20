#pragma once

#include "MaterialStrengthPolicy.h"
#include "Utils/MathUtils.h"

namespace NR::MaterialStrength
{
	/**
	 * @brief Clamps the strengths and the softness radius to the range CategoryAlphaCS.hlsl
	 *        reads, so a hand-edited config cannot feed the shader an out-of-contract value.
	 */
	inline Values Sanitize(Values values)
	{
		for (uint32_t i = 0; i < kCount; ++i)
			values.strength[i] = Util::ClampFinite(values.strength[i], kMinStrength, kMaxStrength, kDefaults[i]);
		values.edgeSoftness = std::min(values.edgeSoftness, kMaxEdgeSoftness);
		return values;
	}
}
