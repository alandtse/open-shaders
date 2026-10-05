// Unit tests for the NR material-strength helper, the material-map filter and the Tuning values they build.

#include "Features/Upscaling/NeuralRendering/MaterialMap.h"
#include "Features/Upscaling/NeuralRendering/MaterialStrength.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_CASE("MaterialStrength indices match the NeuralRenderingCategory ids", "[nr]")
{
	REQUIRE(NR::MaterialStrength::kNone == 0u);
	REQUIRE(NR::MaterialStrength::kSkin == 1u);
	REQUIRE(NR::MaterialStrength::kHair == 2u);
	REQUIRE(NR::MaterialStrength::kEyes == 3u);
	REQUIRE(NR::MaterialStrength::kFoliage == 4u);
	REQUIRE(NR::MaterialStrength::kLandscape == 5u);
	REQUIRE(NR::MaterialStrength::kCount == 6u);
}

TEST_CASE("MaterialStrength Sanitize bounds every strength and the softness radius", "[nr]")
{
	const NR::MaterialStrength::Values values{ { -1.0f, 2.0f, 0.25f, std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f }, 100u };
	const auto sanitized = NR::MaterialStrength::Sanitize(values);
	REQUIRE(sanitized.strength[NR::MaterialStrength::kNone] == 0.0f);
	REQUIRE(sanitized.strength[NR::MaterialStrength::kSkin] == 1.0f);
	REQUIRE(sanitized.strength[NR::MaterialStrength::kHair] == Catch::Approx(0.25f));
	REQUIRE(sanitized.strength[NR::MaterialStrength::kEyes] == NR::MaterialStrength::kDefaults[NR::MaterialStrength::kEyes]);
	REQUIRE(sanitized.strength[NR::MaterialStrength::kFoliage] == Catch::Approx(0.5f));
	REQUIRE(sanitized.strength[NR::MaterialStrength::kLandscape] == 1.0f);
	REQUIRE(sanitized.edgeSoftness == NR::MaterialStrength::kMaxEdgeSoftness);
}

TEST_CASE("MaterialStrengths places the tuning fields in category id order with the product defaults", "[nr]")
{
	const auto defaultValues = NR::Tuning{}.MaterialStrengths();
	REQUIRE(defaultValues.strength[NR::MaterialStrength::kNone] == 0.0f);
	REQUIRE(defaultValues.strength[NR::MaterialStrength::kSkin] == 1.0f);
	REQUIRE(defaultValues.strength[NR::MaterialStrength::kHair] == 1.0f);
	REQUIRE(defaultValues.strength[NR::MaterialStrength::kEyes] == 1.0f);
	REQUIRE(defaultValues.strength[NR::MaterialStrength::kFoliage] == 0.0f);
	REQUIRE(defaultValues.strength[NR::MaterialStrength::kLandscape] == 0.0f);
	REQUIRE(defaultValues.edgeSoftness == 2u);

	NR::Tuning tuning;
	tuning.strengthOther = 0.6f;
	tuning.strengthSkin = 0.1f;
	tuning.strengthHair = 0.2f;
	tuning.strengthEyes = 0.3f;
	tuning.strengthFoliage = 0.4f;
	tuning.strengthLandscape = 0.5f;
	tuning.strengthEdgeSoftness = 3;
	const auto values = tuning.MaterialStrengths();
	REQUIRE(values.strength[NR::MaterialStrength::kNone] == Catch::Approx(0.6f));
	REQUIRE(values.strength[NR::MaterialStrength::kSkin] == Catch::Approx(0.1f));
	REQUIRE(values.strength[NR::MaterialStrength::kHair] == Catch::Approx(0.2f));
	REQUIRE(values.strength[NR::MaterialStrength::kEyes] == Catch::Approx(0.3f));
	REQUIRE(values.strength[NR::MaterialStrength::kFoliage] == Catch::Approx(0.4f));
	REQUIRE(values.strength[NR::MaterialStrength::kLandscape] == Catch::Approx(0.5f));
	REQUIRE(values.edgeSoftness == 3u);
}

TEST_CASE("Tuning Sanitize writes the bounded material strengths back", "[nr]")
{
	NR::Tuning tuning;
	tuning.strengthSkin = 5.0f;
	tuning.strengthOther = -2.0f;
	tuning.strengthHair = std::numeric_limits<float>::infinity();
	tuning.strengthEdgeSoftness = 9;
	tuning.Sanitize();
	REQUIRE(tuning.strengthSkin == 1.0f);
	REQUIRE(tuning.strengthOther == 0.0f);
	REQUIRE(tuning.strengthHair == 1.0f);
	REQUIRE(tuning.strengthEdgeSoftness == NR::MaterialStrength::kMaxEdgeSoftness);
}

TEST_CASE("Material strength is off by default", "[nr]")
{
	REQUIRE_FALSE(NR::Tuning{}.materialStrength);
}

TEST_CASE("MaterialMap filter bits follow the NeuralRenderingCategory ids", "[nr]")
{
	REQUIRE(NR::MaterialMap::kBits == 6u);
	REQUIRE(NR::MaterialMap::kBits == NR::MaterialStrength::kCount);
	REQUIRE(NR::MaterialMap::kAllCategories == 0x3Fu);
	REQUIRE(NR::MaterialMap::Bit(NR::MaterialStrength::kNone) == 0x01u);
	REQUIRE(NR::MaterialMap::Bit(NR::MaterialStrength::kSkin) == 0x02u);
	REQUIRE(NR::MaterialMap::Bit(NR::MaterialStrength::kLandscape) == 0x20u);
	REQUIRE(NR::MaterialMap::Bit(NR::MaterialMap::kBits) == 0u);
}

TEST_CASE("MaterialMap Sanitize keeps only the six category bits", "[nr]")
{
	REQUIRE(NR::MaterialMap::Sanitize(0xFFFFFFFFu) == NR::MaterialMap::kAllCategories);
	REQUIRE(NR::MaterialMap::Sanitize(0u) == 0u);
	const uint32_t skinOnly = NR::MaterialMap::Bit(NR::MaterialStrength::kSkin);
	REQUIRE(NR::MaterialMap::Sanitize(skinOnly | 0xFFFFFFC0u) == skinOnly);
}

TEST_CASE("MaterialMap Contains follows one category's bit", "[nr]")
{
	const uint32_t skinAndEyes = NR::MaterialMap::Bit(NR::MaterialStrength::kSkin) | NR::MaterialMap::Bit(NR::MaterialStrength::kEyes);
	REQUIRE(NR::MaterialMap::Contains(skinAndEyes, NR::MaterialStrength::kSkin));
	REQUIRE(NR::MaterialMap::Contains(skinAndEyes, NR::MaterialStrength::kEyes));
	REQUIRE_FALSE(NR::MaterialMap::Contains(skinAndEyes, NR::MaterialStrength::kHair));
	REQUIRE_FALSE(NR::MaterialMap::Contains(0u, NR::MaterialStrength::kNone));
	REQUIRE(NR::MaterialMap::Contains(NR::MaterialMap::kAllCategories, NR::MaterialStrength::kLandscape));
	REQUIRE_FALSE(NR::MaterialMap::Contains(NR::MaterialMap::kAllCategories, NR::MaterialMap::kBits));
}

TEST_CASE("MaterialMap Set toggles one category and leaves the others", "[nr]")
{
	uint32_t filter = NR::MaterialMap::kAllCategories;
	filter = NR::MaterialMap::Set(filter, NR::MaterialStrength::kEyes, false);
	REQUIRE(filter == (NR::MaterialMap::kAllCategories & ~NR::MaterialMap::Bit(NR::MaterialStrength::kEyes)));
	REQUIRE_FALSE(NR::MaterialMap::Contains(filter, NR::MaterialStrength::kEyes));
	REQUIRE(NR::MaterialMap::Contains(filter, NR::MaterialStrength::kSkin));
	filter = NR::MaterialMap::Set(filter, NR::MaterialStrength::kEyes, true);
	REQUIRE(filter == NR::MaterialMap::kAllCategories);
	REQUIRE(NR::MaterialMap::Set(0u, NR::MaterialStrength::kNone, true) == NR::MaterialMap::Bit(NR::MaterialStrength::kNone));
}

TEST_CASE("Tuning Sanitize bounds the material map mode and filter", "[nr]")
{
	NR::Tuning tuning;
	REQUIRE_FALSE(tuning.showMaterialMap);
	REQUIRE(tuning.materialMapMode == static_cast<uint32_t>(NR::MaterialMap::Mode::kCategory));
	REQUIRE(tuning.materialMapFilter == NR::MaterialMap::kAllCategories);

	tuning.materialMapMode = 9;
	tuning.materialMapFilter = 0xFFFFFFFFu;
	tuning.Sanitize();
	REQUIRE(tuning.materialMapMode == NR::MaterialMap::kMaxMode);
	REQUIRE(tuning.materialMapFilter == NR::MaterialMap::kAllCategories);
}

TEST_CASE("MaterialMap legend colours match the shader's DebugColor", "[nr]")
{
	constexpr float expected[NR::MaterialMap::kBits][3]{
		{ 0.05f, 0.05f, 0.05f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.5f, 0.0f },
		{ 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }
	};
	for (uint32_t i = 0; i < NR::MaterialMap::kBits; ++i) {
		REQUIRE(NR::MaterialMap::kColors[i].r == expected[i][0]);
		REQUIRE(NR::MaterialMap::kColors[i].g == expected[i][1]);
		REQUIRE(NR::MaterialMap::kColors[i].b == expected[i][2]);
	}
}

TEST_CASE("Material scopes round trip through the tuning", "[nr]")
{
	using Scope = NR::MaterialStrength::Scope;
	NR::Tuning tuning;
	REQUIRE(tuning.MaterialScope() == Scope::kEverything);
	tuning.SetMaterialScope(Scope::kSkinHairEyes);
	REQUIRE(tuning.materialStrength);
	REQUIRE(tuning.MaterialScope() == Scope::kSkinHairEyes);
	tuning.SetMaterialScope(Scope::kSkinHairEyesFoliage);
	REQUIRE(tuning.strengthFoliage == 1.0f);
	REQUIRE(tuning.MaterialScope() == Scope::kSkinHairEyesFoliage);
	tuning.strengthLandscape = 0.5f;
	REQUIRE(tuning.MaterialScope() == Scope::kCustom);
	tuning.SetMaterialScope(Scope::kCustom);
	REQUIRE(tuning.strengthLandscape == 0.5f);
	tuning.SetMaterialScope(Scope::kEverything);
	REQUIRE_FALSE(tuning.materialStrength);
	REQUIRE(tuning.MaterialScope() == Scope::kEverything);
}

TEST_CASE("The default tuning keeps the by-material path off and its strengths on the character scope", "[nr]")
{
	NR::Tuning tuning;
	tuning.materialStrength = true;
	REQUIRE(tuning.MaterialScope() == NR::MaterialStrength::Scope::kSkinHairEyes);
}
