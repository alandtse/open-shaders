// Unit tests for the NR situation profiles: the situation decision, the per-situation tuning
// it yields, the situation-independent material lane and the history-reset rule.

#include "Features/Upscaling/NeuralRendering/ContextProfile.h"
#include "Features/Upscaling/NeuralRendering/MaterialStrength.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

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

	for (const auto& override : { Profiles{ {}, { .overrideMaterials = true, .materials = { .strength = NR::MaterialStrength::kCharactersOnly } } }, Profiles{ {}, { .region = RegionOverride::kFullFrame } } }) {
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

TEST_CASE("A material override sets the lane, strengths and edge softness in its situation only", "[nr]")
{
	NR::Tuning base;
	base.strengthEdgeSoftness = 3;
	NR::MaterialStrength::Values values;
	values.strength = NR::MaterialStrength::kCharactersOnly;
	values.edgeSoftness = 1;
	const Profiles profiles{ {}, { .overrideMaterials = true, .materials = values } };

	const auto dialogue = NR::Context::EffectiveTuning(base, profiles, Kind::kDialogue);
	REQUIRE(dialogue.materialStrength);
	REQUIRE(dialogue.MaterialStrengths() == values);

	const auto normal = NR::Context::EffectiveTuning(base, profiles, Kind::kNormal);
	REQUIRE(normal.materialStrength);
	REQUIRE(normal.strengthEdgeSoftness == 3);
	for (const auto strength : normal.MaterialStrengths().strength)
		REQUIRE(strength == NR::MaterialStrength::kMaxStrength);
}

TEST_CASE("A material override in either profile keeps the lane on everywhere", "[nr]")
{
	NR::MaterialStrength::Values values;
	values.strength = NR::MaterialStrength::kCharactersOnly;
	const Profiles dialogueOverride{ {}, { .overrideMaterials = true, .materials = values } };
	const Profiles normalOverride{ { .overrideMaterials = true, .materials = values }, {} };
	for (const auto& profiles : { dialogueOverride, normalOverride })
		for (const auto kind : { Kind::kNormal, Kind::kDialogue })
			REQUIRE(NR::Context::EffectiveTuning(NR::Tuning{}, profiles, kind).materialStrength);
}

TEST_CASE("The by-material selection is untouched where the profile has no material override", "[nr]")
{
	NR::Tuning base;
	base.SetMaterialSelected(NR::MaterialStrength::kFoliage, false);
	NR::MaterialStrength::Values everything;
	const Profiles profiles{ {}, { .overrideMaterials = true, .materials = everything } };
	REQUIRE(NR::Context::EffectiveTuning(base, profiles, Kind::kNormal).strengthFoliage == 0.0f);
	REQUIRE(NR::Context::EffectiveTuning(base, profiles, Kind::kDialogue).strengthFoliage == 1.0f);
}

TEST_CASE("Resolve resets history when only the edge softness differs between situations", "[nr]")
{
	NR::MaterialStrength::Values values;
	values.edgeSoftness = 4;
	const Profiles profiles{ {}, { .overrideMaterials = true, .materials = values } };
	ContextState state;
	NR::Context::Resolve(profiles, false, state);
	REQUIRE(NR::Context::Resolve(profiles, true, state).resetHistory);
}

TEST_CASE("MigrateLegacyScope turns each saved scope into per-material strengths once", "[nr]")
{
	NR::Tuning base;
	base.strengthEdgeSoftness = 3;
	Profiles profiles{ {}, { .scope = ScopeOverride::kSkinHairEyes } };
	NR::Context::MigrateLegacyScope(profiles, base);
	REQUIRE(profiles.dialogue.overrideMaterials);
	REQUIRE(profiles.dialogue.materials.strength == NR::MaterialStrength::kCharactersOnly);
	REQUIRE(profiles.dialogue.materials.edgeSoftness == 3);
	REQUIRE(profiles.dialogue.scope == ScopeOverride::kSameAsNormal);
	REQUIRE_FALSE(profiles.normal.overrideMaterials);

	const auto migrated = profiles;
	NR::Context::MigrateLegacyScope(profiles, base);
	REQUIRE(profiles.dialogue.materials == migrated.dialogue.materials);
	REQUIRE(profiles.dialogue.overrideMaterials);

	Profiles foliage{ { .scope = ScopeOverride::kSkinHairEyesFoliage }, { .scope = ScopeOverride::kEverything } };
	NR::Context::MigrateLegacyScope(foliage, base);
	REQUIRE(foliage.normal.materials.strength[NR::MaterialStrength::kFoliage] == NR::MaterialStrength::kMaxStrength);
	REQUIRE(foliage.normal.materials.strength[NR::MaterialStrength::kLandscape] == NR::MaterialStrength::kMinStrength);
	REQUIRE(foliage.dialogue.materials.strength == NR::MaterialStrength::kDefaults);
}

TEST_CASE("MigrateLegacyScope leaves a profile with no saved scope alone", "[nr]")
{
	Profiles profiles;
	NR::Context::MigrateLegacyScope(profiles, NR::Tuning{});
	REQUIRE_FALSE(profiles.normal.overrideMaterials);
	REQUIRE_FALSE(profiles.dialogue.overrideMaterials);
	REQUIRE(profiles.dialogue.materials == NR::MaterialStrength::Values{});
}

TEST_CASE("ContextProfile Sanitize clamps out-of-range overrides", "[nr]")
{
	ContextProfile profile;
	profile.scope = static_cast<ScopeOverride>(99);
	profile.region = static_cast<RegionOverride>(99);
	profile.materials.strength[NR::MaterialStrength::kSkin] = 5.0f;
	profile.materials.strength[NR::MaterialStrength::kHair] = std::numeric_limits<float>::quiet_NaN();
	profile.materials.edgeSoftness = 9;
	profile.Sanitize();
	REQUIRE(profile.scope == NR::Context::kMaxScope);
	REQUIRE(profile.region == NR::Context::kMaxRegion);
	REQUIRE(profile.materials.strength[NR::MaterialStrength::kSkin] == NR::MaterialStrength::kMaxStrength);
	REQUIRE(profile.materials.strength[NR::MaterialStrength::kHair] == NR::MaterialStrength::kDefaults[NR::MaterialStrength::kHair]);
	REQUIRE(profile.materials.edgeSoftness == NR::MaterialStrength::kMaxEdgeSoftness);

	Profiles profiles;
	profiles.dialogue.scope = static_cast<ScopeOverride>(99);
	profiles.Sanitize();
	REQUIRE(profiles.dialogue.scope == NR::Context::kMaxScope);
}

TEST_CASE("Selecting a material through the value helpers matches the tuning's own selection", "[nr]")
{
	NR::Tuning tuning;
	tuning.SetMaterialSelected(NR::MaterialStrength::kHair, false);
	auto values = tuning.MaterialStrengths();
	for (uint32_t category = 0; category < NR::MaterialStrength::kCount; ++category)
		REQUIRE(NR::MaterialStrength::Selected(values, category) == tuning.MaterialSelected(category));
	NR::MaterialStrength::SetSelected(values, NR::MaterialStrength::kHair, true);
	REQUIRE(NR::MaterialStrength::Selected(values, NR::MaterialStrength::kHair));
	REQUIRE_FALSE(NR::MaterialStrength::AnyBelowFull(values));
	NR::MaterialStrength::SetSelected(values, NR::MaterialStrength::kMetal, false);
	REQUIRE(NR::MaterialStrength::AnyBelowFull(values));
}
