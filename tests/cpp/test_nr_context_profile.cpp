// Unit tests for the NR dialogue profile: the context gate, the per-context tuning it
// yields, the context-independent material-lane need and the history-reset rule.

#include "Features/Upscaling/NeuralRendering/ContextProfile.h"
#include "Features/Upscaling/NeuralRendering/MaterialStrength.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	using NR::Context::DialogueProfile;
	using NR::Context::Kind;
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
		tuning.strengthOther = 0.6f;
		tuning.strengthEdgeSoftness = 3;
		tuning.showMaterialMap = true;
		tuning.materialMapMode = 1;
		tuning.materialMapFilter = 0x2Au;
		return tuning;
	}
}

TEST_CASE("ResolveContext reads the dialogue menu state", "[nr]")
{
	REQUIRE(NR::Context::ResolveContext(false) == Kind::kDefault);
	REQUIRE(NR::Context::ResolveContext(true) == Kind::kDialogue);
}

TEST_CASE("The default dialogue profile is identity for both contexts", "[nr]")
{
	const auto profile = DialogueProfile{};
	for (const auto kind : { Kind::kDefault, Kind::kDialogue }) {
		RequireTuningEqual(NR::Context::EffectiveTuning(NR::Tuning{}, profile, kind), NR::Tuning{});
		const auto moved = MovedTuning();
		RequireTuningEqual(NR::Context::EffectiveTuning(moved, profile, kind), moved);
	}
}

TEST_CASE("Suspended holds the pass only outside dialogue when only-in-dialogue is on", "[nr]")
{
	const DialogueProfile off{}, on{ .onlyInDialogue = true };
	REQUIRE_FALSE(NR::Context::Suspended(off, Kind::kDefault));
	REQUIRE_FALSE(NR::Context::Suspended(off, Kind::kDialogue));
	REQUIRE(NR::Context::Suspended(on, Kind::kDefault));
	REQUIRE_FALSE(NR::Context::Suspended(on, Kind::kDialogue));
}

TEST_CASE("A region override drops only the tracked crop, and only in dialogue", "[nr]")
{
	auto base = MovedTuning();
	base.regionOfInterest = true;
	base.regionOverlay = true;
	base.regionGroup = true;
	base.regionFit = NR::Tuning::kRegionFitTight;
	const DialogueProfile profile{ .region = RegionOverride::kFullFrame };

	RequireTuningEqual(NR::Context::EffectiveTuning(base, profile, Kind::kDefault), base);

	const auto dialogue = NR::Context::EffectiveTuning(base, profile, Kind::kDialogue);
	REQUIRE_FALSE(dialogue.regionOfInterest);
	REQUIRE(dialogue.regionOverlay == base.regionOverlay);
	REQUIRE(dialogue.regionGroup == base.regionGroup);
	REQUIRE(dialogue.regionFit == base.regionFit);
	REQUIRE(dialogue.regionFollowFoveation == base.regionFollowFoveation);
	REQUIRE(dialogue.materialStrength == base.materialStrength);
	REQUIRE(dialogue.strengthSkin == base.strengthSkin);
}

TEST_CASE("A scope override sets the lane and strengths in dialogue and keeps edge softness", "[nr]")
{
	NR::Tuning base;
	base.materialStrength = false;
	base.strengthEdgeSoftness = 3;
	const DialogueProfile profile{ .scope = ScopeOverride::kSkinHairEyesFoliage };

	const auto dialogue = NR::Context::EffectiveTuning(base, profile, Kind::kDialogue);
	REQUIRE(dialogue.materialStrength);
	const auto expected = NR::Context::ScopeStrengths(ScopeOverride::kSkinHairEyesFoliage);
	REQUIRE(expected[NR::MaterialStrength::kFoliage] == 1.0f);
	REQUIRE(expected[NR::MaterialStrength::kLandscape] == 0.0f);
	REQUIRE(dialogue.strengthOther == expected[NR::MaterialStrength::kNone]);
	REQUIRE(dialogue.strengthSkin == expected[NR::MaterialStrength::kSkin]);
	REQUIRE(dialogue.strengthHair == expected[NR::MaterialStrength::kHair]);
	REQUIRE(dialogue.strengthEyes == expected[NR::MaterialStrength::kEyes]);
	REQUIRE(dialogue.strengthFoliage == expected[NR::MaterialStrength::kFoliage]);
	REQUIRE(dialogue.strengthLandscape == expected[NR::MaterialStrength::kLandscape]);
	REQUIRE(dialogue.strengthEdgeSoftness == base.strengthEdgeSoftness);
}

TEST_CASE("A scope override leaves the normal frame unprotected", "[nr]")
{
	NR::Tuning base;
	base.materialStrength = false;
	const DialogueProfile profile{ .scope = ScopeOverride::kSkinHairEyes };

	const auto normal = NR::Context::EffectiveTuning(base, profile, Kind::kDefault);
	REQUIRE(normal.materialStrength);
	for (const float strength : { normal.strengthOther, normal.strengthSkin, normal.strengthHair,
			 normal.strengthEyes, normal.strengthFoliage, normal.strengthLandscape })
		REQUIRE(strength == NR::MaterialStrength::kMaxStrength);
}

TEST_CASE("The material lane follows the settings, never the context", "[nr]")
{
	for (const auto kind : { Kind::kDefault, Kind::kDialogue })
		REQUIRE_FALSE(NR::Context::EffectiveTuning(NR::Tuning{}, DialogueProfile{}, kind).materialStrength);

	NR::Tuning onMaterial;
	onMaterial.materialStrength = true;
	for (const auto kind : { Kind::kDefault, Kind::kDialogue })
		REQUIRE(NR::Context::EffectiveTuning(onMaterial, DialogueProfile{}, kind).materialStrength);

	for (const auto scope : { ScopeOverride::kEverything, ScopeOverride::kSkinHairEyes, ScopeOverride::kSkinHairEyesFoliage }) {
		const DialogueProfile profile{ .scope = scope };
		const auto normal = NR::Context::EffectiveTuning(NR::Tuning{}, profile, Kind::kDefault);
		const auto dialogue = NR::Context::EffectiveTuning(NR::Tuning{}, profile, Kind::kDialogue);
		REQUIRE(normal.materialStrength);
		REQUIRE(dialogue.materialStrength);
	}
}

TEST_CASE("ContextChangeNeedsReset requests a reset only when an override crosses contexts", "[nr]")
{
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(DialogueProfile{}, Kind::kDefault, Kind::kDialogue));
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(DialogueProfile{}, Kind::kDialogue, Kind::kDefault));
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(DialogueProfile{}, Kind::kDefault, Kind::kDefault));
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(DialogueProfile{}, Kind::kDialogue, Kind::kDialogue));

	// Suspension alone already requests its one resume reset, so it needs no context reset.
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(DialogueProfile{ .onlyInDialogue = true }, Kind::kDefault, Kind::kDialogue));
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(DialogueProfile{ .onlyInDialogue = true }, Kind::kDialogue, Kind::kDefault));

	const DialogueProfile scoped{ .scope = ScopeOverride::kSkinHairEyes };
	REQUIRE(NR::Context::ContextChangeNeedsReset(scoped, Kind::kDefault, Kind::kDialogue));
	REQUIRE(NR::Context::ContextChangeNeedsReset(scoped, Kind::kDialogue, Kind::kDefault));
	REQUIRE_FALSE(NR::Context::ContextChangeNeedsReset(scoped, Kind::kDialogue, Kind::kDialogue));

	const DialogueProfile cropped{ .region = RegionOverride::kFullFrame };
	REQUIRE(NR::Context::ContextChangeNeedsReset(cropped, Kind::kDefault, Kind::kDialogue));
	REQUIRE(NR::Context::ContextChangeNeedsReset(cropped, Kind::kDialogue, Kind::kDefault));
}

TEST_CASE("DialogueProfile Sanitize clamps out-of-range overrides", "[nr]")
{
	DialogueProfile profile;
	profile.scope = static_cast<ScopeOverride>(99);
	profile.region = static_cast<RegionOverride>(99);
	profile.Sanitize();
	REQUIRE(profile.scope == NR::Context::kMaxScope);
	REQUIRE(profile.region == NR::Context::kMaxRegion);

	profile.scope = ScopeOverride::kCount;
	profile.region = RegionOverride::kCount;
	profile.Sanitize();
	REQUIRE(profile.scope == NR::Context::kMaxScope);
	REQUIRE(profile.region == NR::Context::kMaxRegion);

	profile.scope = ScopeOverride::kSkinHairEyes;
	profile.region = RegionOverride::kFullFrame;
	profile.Sanitize();
	REQUIRE(profile.scope == ScopeOverride::kSkinHairEyes);
	REQUIRE(profile.region == RegionOverride::kFullFrame);
	REQUIRE(profile.onlyInDialogue == false);
}
