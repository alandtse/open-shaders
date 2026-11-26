#pragma once

#include "Utils/MathUtils.h"

namespace TreeWindSettings
{
	using Range = Util::Range;

	inline constexpr Range kBend{ 0.0f, 4.0f };
	inline constexpr Range kLeafAmbient{ 0.0f, 4.0f };
	inline constexpr Range kUpperBendPercent{ 5.0f, 100.0f };
	inline constexpr Range kMaximumDisplacementPercent{ 0.0f, 10.0f };
	inline constexpr Range kGustInfluence{ 0.0f, 2.0f };
	inline constexpr Range kTransientInfluence{ 0.0f, 5.0f };
	inline constexpr Range kLeafTransientFlutterMaximum{ 0.0f, 20.0f };
	inline constexpr Range kTransientMaximumBendMultiplier{ 0.0f, 5.0f };
}
