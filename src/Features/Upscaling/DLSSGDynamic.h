#pragma once

#include <cmath>

namespace DLSSGDynamic
{
	/** @brief Whether to request DLSSGMode::eDynamic this frame, and the target frame rate to pass with it. */
	struct Selection
	{
		bool dynamic = false;
		float targetFrameRate = 0.0f;
	};

	/**
	 * @brief Selects Dynamic Multi-Frame Generation when frame generation is on, the runtime reports
	 * DLSSGState::bIsDynamicMFGSupported and the user enabled it. A target that is not a positive
	 * finite number becomes 0, which Streamline treats as the display refresh rate.
	 */
	inline Selection Select(bool a_frameGenerationEnabled, bool a_dynamicSupported, bool a_dynamicRequested, float a_targetFrameRate)
	{
		Selection selection;
		selection.dynamic = a_frameGenerationEnabled && a_dynamicSupported && a_dynamicRequested;
		if (selection.dynamic && std::isfinite(a_targetFrameRate) && a_targetFrameRate > 0.0f)
			selection.targetFrameRate = a_targetFrameRate;
		return selection;
	}
}
