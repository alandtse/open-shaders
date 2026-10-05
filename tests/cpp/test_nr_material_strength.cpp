// Unit tests for the NR material-strength helper and the Tuning values it builds:
// the NeuralRenderingCategory id order, the bounds, and the product defaults.
// CategoryAlphaCS.hlsl reads the same order; TestNeuralRenderingCategory.hlsl
// covers the shader-side lookup.

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
