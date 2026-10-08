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
		static constexpr float kMinModelScale = 0.25f, kMaxModelScale = 1.0f;
		static constexpr uint32_t kRegionFitPadded = 0, kRegionFitTight = 1, kMaxRegionFit = kRegionFitTight;
		static constexpr uint32_t kMaxSkipFrameGapMode = 1;
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
		 * @brief Evaluates the second eye on a second compute queue so the two eyes' launch-bound model runs
		 *        overlap. Developer-only, VR only; the output is the same as one queue.
		 */
		bool parallelEyes = false;
		/**
		 * @brief Applies NR by material through the graded protection lane, so a material's
		 *        strength decides how strongly NR shows there. On exactly when some strength is below
		 *        full (SyncMaterialSwitch keeps it so); it needs the deferred pass's material lane, and
		 *        without it the whole frame is processed. It changes where the effect appears, not how
		 *        much GPU time it costs.
		 */
		bool materialStrength = false;
		/** @brief NR strength per material, 1 applies it fully and 0 bypasses it there. Unlabelled pixels use strengthOther. */
		float strengthSkin = 1.0f, strengthHair = 1.0f, strengthEyes = 1.0f;
		float strengthFoliage = 1.0f, strengthLandscape = 1.0f, strengthCloth = 1.0f, strengthMetal = 1.0f, strengthOther = 1.0f;
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
		/** @brief Which categories the map draws, one bit per NeuralRenderingCategory id; all eight by default. */
		uint32_t materialMapFilter = MaterialMap::kAllCategories;
		/**
		 * @brief Evaluates the model for one eye per frame and reuses the other eye's reprojected gain.
		 *        Developer-only and VR-only: it halves the model cost at the price of a synthesized
		 *        gain for one eye, so it is off by default and unvalidated.
		 */
		bool skipFrameReuse = false;
		/** @brief What a gap evaluation tells the model: 0 doubles its motion and frame time, 1 resets its history. */
		uint32_t skipFrameGapMode = 0;
		/**
		 * @brief Runs the model at this fraction of the eye size and upsamples its tone gain onto the
		 *        full-resolution frame. Developer-only; 1 is the full-resolution pass.
		 */
		float modelScale = kMaxModelScale;
		/** @brief Weights the model-size gain by how close each sample's brightness is to the full-resolution pixel, instead of a plain bilinear blend. */
		bool modelGuidedUpsample = true;
		/**
		 * @brief Evaluates both eyes as one side-by-side image on a single Feature 18 instance, which saves
		 *        the per-instance cost of the second eye but changes the model's result by position.
		 *        Developer-only and VR-only; it takes precedence over the eye stagger and combines with the
		 *        model scale.
		 */
		bool sbsEvaluate = false;
		/**
		 * @brief With a model scale or sbsEvaluate, also evaluates each eye alone at full size every frame
		 *        so a verify capture can compare the two. Developer-only; roughly doubles the model cost.
		 */
		bool modelVerify = false;
		/** @brief Feeds eye 0's model inputs to both eyes, so the two eyes' model outputs can be compared like for like. Developer-only; the right eye's picture is wrong while on. */
		bool mirrorEyes = false;

		/** @brief The strength field of each material, indexed by NeuralRenderingCategory id; the one place that mapping lives. */
		static constexpr std::array<float Tuning::*, MaterialStrength::kCount> StrengthMembers()
		{
			return { &Tuning::strengthOther, &Tuning::strengthSkin, &Tuning::strengthHair, &Tuning::strengthEyes, &Tuning::strengthFoliage, &Tuning::strengthLandscape, &Tuning::strengthCloth, &Tuning::strengthMetal };
		}

		/** @brief Bounds and orders the material strengths for the shader's cbuffer and NeuralRenderingCategory ids. */
		[[nodiscard]] MaterialStrength::Values MaterialStrengths() const
		{
			MaterialStrength::Values values;
			const auto members = StrengthMembers();
			for (uint32_t category = 0; category < MaterialStrength::kCount; ++category)
				values.strength[category] = this->*members[category];
			values.edgeSoftness = strengthEdgeSoftness;
			return MaterialStrength::Sanitize(values);
		}

		/** @brief Whether the material with this NeuralRenderingCategory id has any strength, the state its checkbox shows. */
		[[nodiscard]] bool MaterialSelected(uint32_t category) const
		{
			return category < MaterialStrength::kCount && MaterialStrengths().strength[category] > MaterialStrength::kMinStrength;
		}

		/** @brief Selects or clears one material (strength 1 or 0), then keeps the by-material switch in step. */
		void SetMaterialSelected(uint32_t category, bool selected)
		{
			if (category >= MaterialStrength::kCount)
				return;
			this->*StrengthMembers()[category] = selected ? MaterialStrength::kMaxStrength : MaterialStrength::kMinStrength;
			SyncMaterialSwitch();
		}

		/** @brief Turns by-material on exactly when some material is below full strength, so all at full is the unfiltered frame. */
		void SyncMaterialSwitch()
		{
			const auto values = MaterialStrengths().strength;
			materialStrength = std::any_of(values.begin(), values.end(), [](float value) { return value < MaterialStrength::kMaxStrength; });
		}

		/** @brief Writes the eight strengths, in NeuralRenderingCategory id order, leaving the by-material switch alone. */
		void ApplyStrengths(const std::array<float, MaterialStrength::kCount>& strengths)
		{
			const auto members = StrengthMembers();
			for (uint32_t category = 0; category < MaterialStrength::kCount; ++category)
				this->*members[category] = strengths[category];
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
			strengthCloth = materialStrengths.strength[MaterialStrength::kCloth];
			strengthMetal = materialStrengths.strength[MaterialStrength::kMetal];
			strengthEdgeSoftness = materialStrengths.edgeSoftness;
			materialMapMode = std::min(materialMapMode, MaterialMap::kMaxMode);
			materialMapFilter = MaterialMap::Sanitize(materialMapFilter);
			skipFrameGapMode = std::min(skipFrameGapMode, kMaxSkipFrameGapMode);
			modelScale = std::isfinite(modelScale) ? std::clamp(modelScale, kMinModelScale, kMaxModelScale) : kMaxModelScale;
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
