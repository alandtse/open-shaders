#pragma once

#include "MaterialMap.h"
#include "MaterialStrength.h"

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
		/** @brief Per-category multipliers on the composited tone edit, from the deferred Masks2 category. */
		float skinToneStrength = kDefaultStrength;
		float hairToneStrength = kDefaultStrength;
		float eyeToneStrength = kDefaultStrength;
		float foliageToneStrength = kDefaultStrength;
		float landscapeToneStrength = kDefaultStrength;
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
		/**
		 * @brief Grows the crop to cover the next most prominent actors as long as it stays small.
		 *        Developer-only; the default tracks a single actor.
		 */
		bool regionGroup = false;
		/** @brief On VR with foveation active, evaluates only the foveated region, intersected with the tracked crop when one is set. */
		bool regionFollowFoveation = true;
		/**
		 * @brief Applies NR by material through the graded protection lane, so a material's
		 *        strength decides how strongly NR shows there. Off by default; it needs the deferred
		 *        pass's material lane, and without it the whole frame is processed. It changes where
		 *        the effect appears, not how much GPU time it costs.
		 */
		bool materialStrength = false;
		/** @brief NR strength per material, 1 applies it fully and 0 bypasses it there. Unlabelled pixels use strengthOther. */
		float strengthSkin = 1.0f, strengthHair = 1.0f, strengthEyes = 1.0f;
		float strengthFoliage = 0.0f, strengthLandscape = 0.0f, strengthOther = 0.0f;
		/** @brief Box-filter radius, in pixels, that blends each material's strength into its neighbours. */
		uint32_t strengthEdgeSoftness = 2;
		/**
		 * @brief Draws the deferred material lane instead of the composited image, so the pixels each
		 *        material covers can be checked on a real character. A debug view; off by default and
		 *        needing the deferred pass, and it costs no GPU time beyond the existing composite.
		 */
		bool showMaterialMap = false;
		/** @brief MaterialMap::Mode the map draws: the category's debug colour, or its by-material strength. */
		uint32_t materialMapMode = static_cast<uint32_t>(MaterialMap::Mode::kCategory);
		/** @brief Which categories the map draws, one bit per NeuralRenderingCategory id; all six by default. */
		uint32_t materialMapFilter = MaterialMap::kAllCategories;

		/** @brief Bounds and orders the material strengths for the shader's cbuffer and NeuralRenderingCategory ids. */
		[[nodiscard]] MaterialStrength::Values MaterialStrengths() const
		{
			return MaterialStrength::Sanitize({ { strengthOther, strengthSkin, strengthHair, strengthEyes, strengthFoliage, strengthLandscape }, strengthEdgeSoftness });
		}

		/** @brief Bounds user input to the reference runtime's tuning range and to the crop's dependencies. */
		void Sanitize()
		{
			style = std::min(style, kMaxStyle);
			regionFit = std::min(regionFit, kMaxRegionFit);
			for (auto* strength : { &intensity, &localToneStrength, &localStructureStrength,
					 &skinToneStrength, &hairToneStrength, &eyeToneStrength, &foliageToneStrength, &landscapeToneStrength })
				*strength = std::isfinite(*strength) ? std::clamp(*strength, kMinStrength, kMaxStrength) : kDefaultStrength;
			skinStructureStrength = std::isfinite(skinStructureStrength) ?
			                            std::clamp(skinStructureStrength, kAutomaticSkinStructure, kMaxStrength) :
			                            kAutomaticSkinStructure;
			const auto materialStrengths = MaterialStrengths();
			strengthOther = materialStrengths.strength[MaterialStrength::kNone];
			strengthSkin = materialStrengths.strength[MaterialStrength::kSkin];
			strengthHair = materialStrengths.strength[MaterialStrength::kHair];
			strengthEyes = materialStrengths.strength[MaterialStrength::kEyes];
			strengthFoliage = materialStrengths.strength[MaterialStrength::kFoliage];
			strengthLandscape = materialStrengths.strength[MaterialStrength::kLandscape];
			strengthEdgeSoftness = materialStrengths.edgeSoftness;
			materialMapMode = std::min(materialMapMode, MaterialMap::kMaxMode);
			materialMapFilter = MaterialMap::Sanitize(materialMapFilter);
			// The crop controls act only through a tracked crop, so none of them can stay set without
			// it: a retained value reads as active while the pass ignores it, whether it came from a
			// config file or from switching the crop off in the panel.
			if (!regionOfInterest) {
				regionOverlay = false;
				regionGroup = false;
				regionFit = kRegionFitPadded;
			}
		}
	};
}
