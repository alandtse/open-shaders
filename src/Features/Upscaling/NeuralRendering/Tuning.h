#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NR
{
	/** @brief Appearance controls for the full-resolution Feature 18 pass. */
	struct Tuning
	{
		static constexpr float kMinStrength = 0.0f, kMaxStrength = 2.0f;
		static constexpr float kDefaultStrength = 1.0f, kAutomaticSkinStructure = -1.0f;
		static constexpr float kDefaultShadowLuminanceFloor = 1.0f / 512.0f;
		static constexpr uint32_t kMaxStyle = 2;
		uint32_t style = 0;
		float intensity = kDefaultStrength;
		float localToneStrength = kDefaultStrength;
		float localStructureStrength = kDefaultStrength;
		float skinStructureStrength = kAutomaticSkinStructure;
		bool useAutoMask = true;
		bool shadowLuminanceProtection = false;
		float shadowLuminanceFloor = kDefaultShadowLuminanceFloor;
		float maxShadowLuminanceRatio = 1.5f;
		float shadowProtectionStart = 0.02f;
		float shadowProtectionEnd = 0.18f;
		float minDarkNRStrength = 1.0f;

		/** @brief Bounds user input to the reference runtime's tuning range. */
		void Sanitize()
		{
			style = std::min(style, kMaxStyle);
			for (auto* strength : { &intensity, &localToneStrength, &localStructureStrength })
				*strength = std::isfinite(*strength) ? std::clamp(*strength, kMinStrength, kMaxStrength) : kDefaultStrength;
			skinStructureStrength = std::isfinite(skinStructureStrength) ?
			                            std::clamp(skinStructureStrength, kAutomaticSkinStructure, kMaxStrength) :
			                            kAutomaticSkinStructure;
			shadowLuminanceFloor = std::isfinite(shadowLuminanceFloor) ? std::clamp(shadowLuminanceFloor, 1e-6f, 0.25f) : kDefaultShadowLuminanceFloor;
			maxShadowLuminanceRatio = std::isfinite(maxShadowLuminanceRatio) ? std::clamp(maxShadowLuminanceRatio, 1.0f, 4.0f) : 1.5f;
			shadowProtectionStart = std::isfinite(shadowProtectionStart) ? std::clamp(shadowProtectionStart, 0.0f, 2.0f) : 0.02f;
			shadowProtectionEnd = std::isfinite(shadowProtectionEnd) ? std::clamp(shadowProtectionEnd, shadowProtectionStart + 1e-4f, 4.0f) : 0.18f;
			minDarkNRStrength = std::isfinite(minDarkNRStrength) ? std::clamp(minDarkNRStrength, 0.0f, 1.0f) : 1.0f;
		}
	};
}
