#pragma once

#include "Tuning.h"

#include <array>
#include <cstdint>

namespace NR::Context
{
	/** @brief Which of the dialogue profile's two contexts a frame is in. */
	enum class Kind : uint32_t
	{
		kDefault,  ///< The dialogue menu is closed; the profile's overrides do not apply.
		kDialogue  ///< The dialogue menu is open; the profile's overrides apply.
	};

	/** @brief The context a frame is in, from whether the dialogue menu is open. */
	inline Kind ResolveContext(bool dialogueOpen)
	{
		return dialogueOpen ? Kind::kDialogue : Kind::kDefault;
	}

	/** @brief Material scope the dialogue profile applies instead of the by-material setting. */
	enum class ScopeOverride : uint32_t
	{
		kSameAsNormal = 0,     ///< Use the by-material setting unchanged.
		kEverything,           ///< Neural Rendering on every material.
		kSkinHairEyes,         ///< Characters' skin, hair and eyes only.
		kSkinHairEyesFoliage,  ///< Skin, hair, eyes and foliage.
		kCount
	};
	/** @brief Largest valid ScopeOverride; Sanitize clamps above it. */
	inline constexpr ScopeOverride kMaxScope = ScopeOverride::kSkinHairEyesFoliage;

	/** @brief Crop the dialogue profile applies instead of the Limit to Tracked Actor setting. */
	enum class RegionOverride : uint32_t
	{
		kSameAsNormal = 0,  ///< Use the tracked-actor crop setting unchanged.
		kFullFrame,         ///< Evaluate the whole view; no tracked-actor crop.
		kCount
	};
	/** @brief Largest valid RegionOverride; Sanitize clamps above it. */
	inline constexpr RegionOverride kMaxRegion = RegionOverride::kFullFrame;

	/** @brief What Neural Rendering does while a dialogue is open, versus normally. The default is inert. */
	struct DialogueProfile
	{
		/** @brief Suspend the pass outside dialogue, keeping its resources and runtime alive. */
		bool onlyInDialogue = false;
		/** @brief Material scope applied while a dialogue is open. */
		ScopeOverride scope = ScopeOverride::kSameAsNormal;
		/** @brief Crop applied while a dialogue is open. */
		RegionOverride region = RegionOverride::kSameAsNormal;

		/** @brief Brings a hand-edited config's enums into the valid range. */
		void Sanitize()
		{
			if (scope > kMaxScope)
				scope = kMaxScope;
			if (region > kMaxRegion)
				region = kMaxRegion;
		}
	};

	/** @brief The six strengths an override selects, in category id order; kSameAsNormal selects NR on every material. */
	inline std::array<float, MaterialStrength::kCount> ScopeStrengths(ScopeOverride scope)
	{
		switch (scope) {
		case ScopeOverride::kSkinHairEyes:
			return MaterialStrength::kCharactersOnly;
		case ScopeOverride::kSkinHairEyesFoliage:
			{
				auto strengths = MaterialStrength::kCharactersOnly;
				strengths[MaterialStrength::kFoliage] = MaterialStrength::kMaxStrength;
				return strengths;
			}
		default:
			return MaterialStrength::kDefaults;
		}
	}

	/** @brief Writes the six strengths into a tuning, in category id order, leaving the by-material switch alone. */
	inline void ApplyStrengths(Tuning& tuning, const std::array<float, MaterialStrength::kCount>& strengths)
	{
		for (uint32_t category = 0; category < MaterialStrength::kCount; ++category)
			tuning.StrengthField(category) = strengths[category];
	}

	/** @brief Whether the pass is suspended in this context: only in dialogue, and only outside it. */
	inline bool Suspended(const DialogueProfile& profile, Kind kind)
	{
		return profile.onlyInDialogue && kind == Kind::kDefault;
	}

	/**
	 * @brief The tuning one frame evaluates with: the base tuning plus the profile's overrides in
	 *        dialogue. With the default profile the result equals the base field for field.
	 *        A scope override keeps the by-material lane on in both contexts, because Feature 18
	 *        latches the UIAlpha binding at creation and a lane that followed the context would
	 *        rebuild the eye features on every dialogue open and close.
	 */
	inline Tuning EffectiveTuning(const Tuning& base, const DialogueProfile& profile, Kind kind)
	{
		auto result = base;
		const bool scopeOverride = profile.scope != ScopeOverride::kSameAsNormal;
		if (kind == Kind::kDefault) {
			if (!base.materialStrength && scopeOverride) {
				result.materialStrength = true;
				ApplyStrengths(result, MaterialStrength::kDefaults);
			}
			return result;
		}
		if (profile.region == RegionOverride::kFullFrame)
			result.regionOfInterest = false;
		if (scopeOverride) {
			result.materialStrength = true;
			ApplyStrengths(result, ScopeStrengths(profile.scope));
		}
		return result;
	}

	/**
	 * @brief Whether crossing between contexts changes the crop or the strengths, so history must
	 *        not blend across it. A default profile changes nothing and requests no reset.
	 */
	inline bool ContextChangeNeedsReset(const DialogueProfile& profile, Kind previous, Kind current)
	{
		if (previous == current)
			return false;
		return profile.scope != ScopeOverride::kSameAsNormal || profile.region != RegionOverride::kSameAsNormal;
	}
}
