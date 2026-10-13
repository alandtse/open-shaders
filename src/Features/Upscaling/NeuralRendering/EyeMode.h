#pragma once

#include "Tuning.h"

#include <cstdint>

namespace NR
{
	/** @brief How the two VR eyes are evaluated. The choices exclude each other. */
	enum class EyeMode : uint32_t
	{
		kParallel,    ///< Each eye on its own instance, the second on a second compute queue.
		kSideBySide,  ///< Both eyes as one image on a single instance.
		kAlternate,   ///< One eye's model per frame; the other reuses its gain.
		kSeparate,    ///< Each eye on its own instance, one after the other.
	};
	/** @brief Number of EyeMode values, for building a combo. */
	inline constexpr uint32_t kEyeModeCount = 4;

	/** @brief The mode a tuning selects, by the precedence the pass applies: side by side, then alternate, then parallel. */
	[[nodiscard]] inline EyeMode GetEyeMode(const Tuning& tuning)
	{
		if (tuning.sbsEvaluate)
			return EyeMode::kSideBySide;
		if (tuning.skipFrameReuse)
			return EyeMode::kAlternate;
		return tuning.parallelEyes ? EyeMode::kParallel : EyeMode::kSeparate;
	}

	/** @brief Selects a mode by setting exactly one of the three flags it is stored in. */
	inline void SetEyeMode(Tuning& tuning, EyeMode mode)
	{
		tuning.parallelEyes = mode == EyeMode::kParallel;
		tuning.sbsEvaluate = mode == EyeMode::kSideBySide;
		tuning.skipFrameReuse = mode == EyeMode::kAlternate;
	}
}
