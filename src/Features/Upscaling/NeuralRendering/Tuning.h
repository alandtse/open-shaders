#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NR
{
	/** @brief Appearance and model-resolution controls for the full-frame Feature 18 pass. */
	struct Tuning
	{
		static constexpr float kMinStrength = 0.0f, kMaxStrength = 2.0f;
		static constexpr float kDefaultStrength = 1.0f, kAutomaticSkinStructure = -1.0f;
		static constexpr uint32_t kMaxStyle = 2;
		/**
		 * @brief Model resolution scale: Feature 18 evaluates at round(size * workingScale)
		 *        while the frame stays full size and only the neural delta is composited back.
		 *        0.75 costs about half the GPU time for a near-identical image; 0.5 buys large
		 *        gains but smears fine detail such as hair. Color, depth and motion guides are
		 *        all scaled together so temporal history stays synchronized.
		 */
		static constexpr float kMinWorkingScale = 0.5f, kMaxWorkingScale = 1.0f, kDefaultWorkingScale = 0.75f;
		uint32_t style = 0;
		float intensity = kDefaultStrength;
		float localToneStrength = kDefaultStrength;
		float localStructureStrength = kDefaultStrength;
		float skinStructureStrength = kAutomaticSkinStructure;
		bool useAutoMask = true;
		float workingScale = kDefaultWorkingScale;

		/** @brief Bounds user input to the reference runtime's tuning range. */
		void Sanitize()
		{
			style = std::min(style, kMaxStyle);
			for (auto* strength : { &intensity, &localToneStrength, &localStructureStrength })
				*strength = std::isfinite(*strength) ? std::clamp(*strength, kMinStrength, kMaxStrength) : kDefaultStrength;
			skinStructureStrength = std::isfinite(skinStructureStrength) ?
			                            std::clamp(skinStructureStrength, kAutomaticSkinStructure, kMaxStrength) :
			                            kAutomaticSkinStructure;
			workingScale = std::isfinite(workingScale) ?
			                   std::clamp(workingScale, kMinWorkingScale, kMaxWorkingScale) :
			                   kDefaultWorkingScale;
		}
	};
}
