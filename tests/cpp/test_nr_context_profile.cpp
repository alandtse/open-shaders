// Unit tests for the NR situation profiles: the situation decision, the per-situation tuning
// it yields, the situation-independent material lane and the history-reset rule.

#include "Features/Upscaling/NeuralRendering/ContextProfile.h"
#include "Features/Upscaling/NeuralRendering/MaterialStrength.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	using NR::Context::ContextProfile;
	using NR::Context::ContextState;
	using NR::Context::Kind;
	using NR::Context::Profiles;
	using NR::Context::RegionOverride;
	using NR::Context::ScopeOverride;

	/** @brief Requires every field of two tunings to match, so an "unchanged" claim covers all of them. */
	void RequireTuningEqual(const NR::Tuning& a_left, const NR::Tuning& a_right)
	{
		REQUIRE(a_left.style == a_right.style);
		REQUIRE(a_left.intensity == a_right.intensity);
		REQUIRE(a_left.localToneStrength == a_right.localToneStrength);
		REQUIRE(a_left.localStructureStrength == a_right.localStructureStrength);
		REQUIRE(a_left.skinStructureStrength == a_right.skinStructureStrength);
		REQUIRE(a_left.skinToneStrength == a_right.skinToneStrength);
		REQUIRE(a_left.hairToneStrength == a_right.hairToneStrength);
		REQUIRE(a_left.eyeToneStrength == a_right.eyeToneStrength);
		REQUIRE(a_left.foliageToneStrength == a_right.foliageToneStrength);
		REQUIRE(a_left.landscapeToneStrength == a_right.landscapeToneStrength);
		REQUIRE(a_left.useAutoMask == a_right.useAutoMask);
		REQUIRE(a_left.regionOfInterest == a_right.regionOfInterest);
		REQUIRE(a_left.regionOverlay == a_right.regionOverlay);
		REQUIRE(a_left.regionFit == a_right.regionFit);
		REQUIRE(a_left.regionGroup == a_right.regionGroup);
		REQUIRE(a_left.regionFollowFoveation == a_right.regionFollowFoveation);
		REQUIRE(a_left.materialStrength == a_right.materialStrength);
		REQUIRE(a_left.strengthSkin == a_right.strengthSkin);
		REQUIRE(a_left.strengthHair == a_right.strengthHair);
		REQUIRE(a_left.strengthEyes == a_right.strengthEyes);
		REQUIRE(a_left.strengthFoliage == a_right.strengthFoliage);
		REQUIRE(a_left.strengthLandscape == a_right.strengthLandscape);
		REQUIRE(a_left.strengthCloth == a_right.strengthCloth);
		REQUIRE(a_left.strengthMetal == a_right.strengthMetal);
		REQUIRE(a_left.strengthOther == a_right.strengthOther);
		REQUIRE(a_left.strengthEdgeSoftness == a_right.strengthEdgeSoftness);
		REQUIRE(a_left.showMaterialMap == a_right.showMaterialMap);
		REQUIRE(a_left.materialMapMode == a_right.materialMapMode);
		REQUIRE(a_left.materialMapFilter == a_right.materialMapFilter);
	}

	/** @brief A tuning with every field moved off its default, so an identity claim is not vacuous. */
	NR::Tuning MovedTuning()
	{
		NR::Tuning tuning;
		tuning.style = 2;
		tuning.intensity = 1.5f;
		tuning.localToneStrength = 0.5f;
		tuning.localStructureStrength = 1.25f;
		tuning.skinStructureStrength = 0.75f;
		tuning.skinToneStrength = 0.5f;
		tuning.hairToneStrength = 0.25f;
		tuning.eyeToneStrength = 1.75f;
		tuning.foliageToneStrength = 0.5f;
		tuning.landscapeToneStrength = 1.25f;
		tuning.useAutoMask = false;
		tuning.regionOfInterest = true;
		tuning.regionOverlay = true;
		tuning.regionFit = NR::Tuning::kRegionFitTight;
		tuning.regionGroup = true;
		tuning.regionFollowFoveation = false;
		tuning.materialStrength = true;
		tuning.strengthSkin = 0.1f;
		tuning.strengthHair = 0.2f;
		tuning.strengthEyes = 0.3f;
		tuning.strengthFoliage = 0.4f;
		tuning.strengthLandscape = 0.5f;
		tuning.strengthCloth = 0.35f;
		tuning.strengthMetal = 0.45f;
		tuning.strengthOther = 0.6f;
		tuning.strengthEdgeSoftness = 3;
		tuning.showMaterialMap = true;
		tuning.materialMapMode = 1;
		tuning.materialMapFilter = 0x2Au;
		return tuning;
	}
}

TEST_CASE("The default profiles are identity for every situation", "[nr]")
{
	const auto base = MovedTuning();
	for (const auto kind : { Kind::kNormal, Kind::kDialogue })
		RequireTuningEqual(NR::Context::EffectiveTuning(base, Profiles{}, kind), base);

	NR::Tuning plain;
	for (const auto kind : { Kind::kNormal, Kind::kDialogue })
		RequireTuningEqual(NR::Context::EffectiveTuning(plain, Profiles{}, kind), plain);
}

TEST_CASE("Resolve reads the dialogue menu state and suspends only a profile that does not run", "[nr]")
{
	ContextState state;
	REQUIRE(NR::Context::Resolve(Profiles{}, false, state).kind == Kind::kNormal);
	REQUIRE(NR::Context::Resolve(Profiles{}, true, state).kind == Kind::kDialogue);
	REQUIRE_FALSE(NR::Context::Resolve(Profiles{}, true, state).suspended);

	Profiles onlyInDialogue;
	onlyInDialogue.normal.run = false;
	ContextState gated;
	REQUIRE(NR::Context::Resolve(onlyInDialogue, false, gated).suspended);
	REQUIRE_FALSE(NR::Context::Resolve(onlyInDialogue, true, gated).suspended);
}

TEST_CASE("Resolve flags exactly the first frame after a suspension", "[nr]")
{
	Profiles onlyInDialogue;
	onlyInDialogue.normal.run = false;
	ContextState state;
	REQUIRE_FALSE(NR::Context::Resolve(onlyInDialogue, false, state).resetHistory);
	REQUIRE_FALSE(NR::Context::Resolve(onlyInDialogue, false, state).resetHistory);
	REQUIRE(NR::Context::Resolve(onlyInDialogue, true, state).resetHistory);
	REQUIRE_FALSE(NR::Context::Resolve(onlyInDialogue, true, state).resetHistory);
}

TEST_CASE("Resolve resets history on a situation change only when the profiles evaluate differently", "[nr]")
{
	ContextState state;
	NR::Context::Resolve(Profiles{}, false, state);
	REQUIRE_FALSE(NR::Context::Resolve(Profiles{}, true, state).resetHistory);
	REQUIRE_FALSE(NR::Context::Resolve(Profiles{}, false, state).resetHistory);

	for (const auto& override : { Profiles{ {}, { .scope = ScopeOverride::kSkinHairEyes } }, Profiles{ {}, { .region = RegionOverride::kFullFrame } } }) {
		ContextState changing;
		NR::Context::Resolve(override, false, changing);
		REQUIRE(NR::Context::Resolve(override, true, changing).resetHistory);
		REQUIRE_FALSE(NR::Context::Resolve(override, true, changing).resetHistory);
		REQUIRE(NR::Context::Resolve(override, false, changing).resetHistory);
	}
}

TEST_CASE("A region override drops only the tracked crop, and only in its situation", "[nr]")
{
	auto base = MovedTuning();
	base.regionOfInterest = true;
	const Profiles profiles{ {}, { .region = RegionOverride::kFullFrame } };

	RequireTuningEqual(NR::Context::EffectiveTuning(base, profiles, Kind::kNormal), base);

	auto expected = base;
	expected.regionOfInterest = false;
	RequireTuningEqual(NR::Context::EffectiveTuning(base, profiles, Kind::kDialogue), expected);
}

TEST_CASE("A scope override sets the lane and strengths in its situation and keeps edge softness", "[nr]")
{
	NR::Tuning base;
	base.strengthEdgeSoftness = 3;
	const Profiles profiles{ {}, { .scope = ScopeOverride::kSkinHairEyesFoliage } };

	const auto dialogue = NR::Context::EffectiveTuning(base, profiles, Kind::kDialogue);
	REQUIRE(dialogue.materialStrength);
	const auto expected = NR::Context::ScopeStrengths(ScopeOverride::kSkinHairEyesFoliage);
	REQUIRE(expected[NR::MaterialStrength::kFoliage] == 1.0f);
	REQUIRE(expected[NR::MaterialStrength::kLandscape] == 0.0f);
	REQUIRE(expected[NR::MaterialStrength::kCloth] == 0.0f);
	REQUIRE(expected[NR::MaterialStrength::kMetal] == 0.0f);
	REQUIRE(dialogue.MaterialStrengths().strength == expected);
	REQUIRE(dialogue.strengthEdgeSoftness == base.strengthEdgeSoftness);
}

TEST_CASE("A scope override in any profile keeps the lane on everywhere, unprotected where it has no override", "[nr]")
{
	for (const auto scope : { ScopeOverride::kEverything, ScopeOverride::kSkinHairEyes, ScopeOverride::kSkinHairEyesFoliage }) {
		const Profiles profiles{ {}, { .scope = scope } };
		const auto normal = NR::Context::EffectiveTuning(NR::Tuning{}, profiles, Kind::kNormal);
		const auto dialogue = NR::Context::EffectiveTuning(NR::Tuning{}, profiles, Kind::kDialogue);
		REQUIRE(normal.materialStrength);
		REQUIRE(dialogue.materialStrength);
		for (const auto strength : normal.MaterialStrengths().strength)
			REQUIRE(strength == NR::MaterialStrength::kMaxStrength);
	}

	const Profiles flipped{ { .scope = ScopeOverride::kSkinHairEyes }, {} };
	REQUIRE(NR::Context::EffectiveTuning(NR::Tuning{}, flipped, Kind::kNormal).materialStrength);
	REQUIRE(NR::Context::EffectiveTuning(NR::Tuning{}, flipped, Kind::kDialogue).materialStrength);
}

TEST_CASE("The by-material selection is untouched where the profile has no scope override", "[nr]")
{
	NR::Tuning base;
	base.SetMaterialSelected(NR::MaterialStrength::kFoliage, false);
	const Profiles profiles{ {}, { .scope = ScopeOverride::kEverything } };
	const auto normal = NR::Context::EffectiveTuning(base, profiles, Kind::kNormal);
	REQUIRE(normal.strengthFoliage == 0.0f);
	REQUIRE(NR::Context::EffectiveTuning(base, profiles, Kind::kDialogue).strengthFoliage == 1.0f);
}

TEST_CASE("ContextProfile Sanitize clamps out-of-range overrides", "[nr]")
{
	ContextProfile profile;
	profile.scope = static_cast<ScopeOverride>(99);
	profile.region = static_cast<RegionOverride>(99);
	profile.Sanitize();
	REQUIRE(profile.scope == NR::Context::kMaxScope);
	REQUIRE(profile.region == NR::Context::kMaxRegion);

	Profiles profiles;
	profiles.dialogue.scope = static_cast<ScopeOverride>(99);
	profiles.Sanitize();
	REQUIRE(profiles.dialogue.scope == NR::Context::kMaxScope);
}
