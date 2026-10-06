#pragma once

#include "Tuning.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace NR::Context
{
	/** @brief Situation a frame is rendered in; each has its own profile. */
	enum class Kind : uint32_t
	{
		kNormal,   ///< No special situation.
		kDialogue  ///< The dialogue menu is open.
	};

	/** @brief Material scope a profile applies instead of the by-material selection. */
	enum class ScopeOverride : uint32_t
	{
		kSameAsNormal = 0,     ///< Use the by-material selection unchanged.
		kEverything,           ///< Neural Rendering on every material.
		kSkinHairEyes,         ///< Characters' skin, hair and eyes only.
		kSkinHairEyesFoliage,  ///< Skin, hair, eyes and foliage.
	};
	/** @brief Largest valid ScopeOverride; Sanitize clamps above it. */
	inline constexpr ScopeOverride kMaxScope = ScopeOverride::kSkinHairEyesFoliage;

	/** @brief Crop a profile applies instead of the Limit to Tracked Actor setting. */
	enum class RegionOverride : uint32_t
	{
		kSameAsNormal = 0,  ///< Use the tracked-actor crop setting unchanged.
		kFullFrame,         ///< Evaluate the whole view; no tracked-actor crop.
	};
	/** @brief Largest valid RegionOverride; Sanitize clamps above it. */
	inline constexpr RegionOverride kMaxRegion = RegionOverride::kFullFrame;

	/** @brief What Neural Rendering does in one situation. The defaults change nothing. */
	struct ContextProfile
	{
		/** @brief Evaluate Neural Rendering; false suspends the pass with its resources and runtime kept alive. */
		bool run = true;
		/** @brief Material scope applied in this situation. */
		ScopeOverride scope = ScopeOverride::kSameAsNormal;
		/** @brief Crop applied in this situation. */
		RegionOverride region = RegionOverride::kSameAsNormal;

		/** @brief Brings a hand-edited config's enums into the valid range. */
		void Sanitize()
		{
			scope = std::min(scope, kMaxScope);
			region = std::min(region, kMaxRegion);
		}

		/** @brief Whether this profile and another differ in what they evaluate, not in whether they run. */
		[[nodiscard]] bool OverridesDiffer(const ContextProfile& other) const
		{
			return scope != other.scope || region != other.region;
		}
	};

	/**
	 * @brief The profile of every situation. Dialogue-only evaluation is the normal profile not
	 *        running while the dialogue profile does.
	 */
	struct Profiles
	{
		ContextProfile normal;
		ContextProfile dialogue;

		/** @brief The profile for a situation. */
		[[nodiscard]] const ContextProfile& For(Kind kind) const
		{
			return kind == Kind::kDialogue ? dialogue : normal;
		}

		/** @brief Sanitizes every profile. */
		void Sanitize()
		{
			normal.Sanitize();
			dialogue.Sanitize();
		}

		/** @brief Whether any profile overrides the material scope, so the by-material lane must stay bound everywhere. */
		[[nodiscard]] bool ScopeOverridden() const
		{
			return normal.scope != ScopeOverride::kSameAsNormal || dialogue.scope != ScopeOverride::kSameAsNormal;
		}
	};

	/** @brief What the previous frame was, so a transition can be recognised. */
	struct ContextState
	{
		Kind previous = Kind::kNormal;
		bool wasSuspended = false;
	};

	/** @brief What one frame's situation decides. */
	struct Decision
	{
		Kind kind = Kind::kNormal;
		/** @brief The profile suspends the pass this frame. */
		bool suspended = false;
		/** @brief History from before this frame must not be blended: leaving a suspension, or entering a situation that evaluates differently. */
		bool resetHistory = false;
	};

	/** @brief The six strengths a scope override selects, in category id order; kSameAsNormal selects NR on every material. */
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

	/**
	 * @brief Decides this frame's situation and updates the state.
	 * @param profiles The profile of every situation.
	 * @param dialogueOpen Whether the dialogue menu is open.
	 * @param state The previous frame's situation, updated here.
	 * @return The situation, whether it suspends the pass, and whether history must be reset.
	 */
	inline Decision Resolve(const Profiles& profiles, bool dialogueOpen, ContextState& state)
	{
		Decision decision;
		decision.kind = dialogueOpen ? Kind::kDialogue : Kind::kNormal;
		const auto& profile = profiles.For(decision.kind);
		decision.suspended = !profile.run;
		const bool resumed = state.wasSuspended && !decision.suspended;
		const bool evaluatesDifferently = decision.kind != state.previous && profile.OverridesDiffer(profiles.For(state.previous));
		decision.resetHistory = resumed || evaluatesDifferently;
		state.previous = decision.kind;
		state.wasSuspended = decision.suspended;
		return decision;
	}

	/**
	 * @brief The tuning one frame evaluates with: the base tuning plus the situation's overrides.
	 *        With the default profiles the result equals the base field for field. A scope override
	 *        in any situation keeps the by-material lane on in every situation, because Feature 18
	 *        latches the UIAlpha binding at creation and a lane that followed the situation would
	 *        rebuild the eye features on every change.
	 */
	inline Tuning EffectiveTuning(const Tuning& base, const Profiles& profiles, Kind kind)
	{
		auto result = base;
		const auto& profile = profiles.For(kind);
		if (profile.region == RegionOverride::kFullFrame)
			result.regionOfInterest = false;
		if (profile.scope != ScopeOverride::kSameAsNormal) {
			result.materialStrength = true;
			result.ApplyStrengths(ScopeStrengths(profile.scope));
		} else if (profiles.ScopeOverridden() && !result.materialStrength) {
			result.materialStrength = true;
			result.ApplyStrengths(MaterialStrength::kDefaults);
		}
		return result;
	}
}
