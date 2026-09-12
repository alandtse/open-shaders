#pragma once

#include <algorithm>
#include <cmath>

namespace NR
{
	/** @brief Appearance controls for the full-resolution Feature 18 pass. */
	struct Tuning
	{
		static constexpr float kMinStrength = 0.0f, kMaxStrength = 2.0f;
		static constexpr float kDefaultStrength = 1.7f, kAutomaticSkinStructure = -1.0f;
		float intensity = kDefaultStrength;
		float localToneStrength = kDefaultStrength;
		float localStructureStrength = kDefaultStrength;
		float skinStructureStrength = kAutomaticSkinStructure;

		/** @brief Bounds user input to the reference runtime's tuning range. */
		void Sanitize()
		{
			for (auto* strength : { &intensity, &localToneStrength, &localStructureStrength })
				*strength = std::isfinite(*strength) ? std::clamp(*strength, kMinStrength, kMaxStrength) : kDefaultStrength;
			skinStructureStrength = std::isfinite(skinStructureStrength) ?
			                            std::clamp(skinStructureStrength, kAutomaticSkinStructure, kMaxStrength) :
			                            kAutomaticSkinStructure;
		}
	};
}
