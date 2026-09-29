#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NR
{
	/** @brief Appearance and evaluation-scope controls for the full-resolution Feature 18 pass. */
	struct Tuning
	{
		static constexpr float kMinStrength = 0.0f, kMaxStrength = 2.0f;
		static constexpr float kDefaultStrength = 1.0f, kAutomaticSkinStructure = -1.0f;
		static constexpr uint32_t kMaxStyle = 2;
		static constexpr uint32_t kRegionFitPadded = 0, kRegionFitTight = 1, kMaxRegionFit = kRegionFitTight;
		uint32_t style = 0;
		float intensity = kDefaultStrength;
		float localToneStrength = kDefaultStrength;
		float localStructureStrength = kDefaultStrength;
		float skinStructureStrength = kAutomaticSkinStructure;
		bool useAutoMask = true;
		/**
		 * @brief Restricts NR's evaluation to a crop around the most prominent visible actor.
		 *        Off by default: it trades full-frame NR quality for GPU time, and the
		 *        periphery falls back to pre-NR content. The published crop is always
		 *        stabilised, so a candidate that jitters does not reset the history.
		 */
		bool regionOfInterest = false;
		/**
		 * @brief Draws the evaluated crop as an outline over the frame and a preview in the settings
		 *        panel. Off by default; a debug view only, and it draws nothing unless a crop is active.
		 */
		bool regionOverlay = false;
		/**
		 * @brief How the crop is fit to the tracked actor's projected bounds: the padded crop, or the
		 *        aligned bounds alone. Developer-only; the tight fit is for checking what the crop covers.
		 */
		uint32_t regionFit = kRegionFitPadded;

		/** @brief Bounds user input to the reference runtime's tuning range. */
		void Sanitize()
		{
			style = std::min(style, kMaxStyle);
			regionFit = std::min(regionFit, kMaxRegionFit);
			for (auto* strength : { &intensity, &localToneStrength, &localStructureStrength })
				*strength = std::isfinite(*strength) ? std::clamp(*strength, kMinStrength, kMaxStrength) : kDefaultStrength;
			skinStructureStrength = std::isfinite(skinStructureStrength) ?
			                            std::clamp(skinStructureStrength, kAutomaticSkinStructure, kMaxStrength) :
			                            kAutomaticSkinStructure;
		}
	};
}
