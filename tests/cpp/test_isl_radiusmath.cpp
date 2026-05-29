// Unit tests for ISL radius/attenuation math (RadiusMath.h).

#include "Features/InverseSquareLighting/RadiusMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("SmoothStep is a clamped Hermite ramp", "[isl]")
{
	REQUIRE(ISLMath::SmoothStep(0.0f, 1.0f, 0.0f) == Approx(0.0f));
	REQUIRE(ISLMath::SmoothStep(0.0f, 1.0f, 1.0f) == Approx(1.0f));
	REQUIRE(ISLMath::SmoothStep(0.0f, 1.0f, 0.5f) == Approx(0.5f));  // t^2(3-2t) at 0.5
	// Clamps outside [edge0, edge1].
	REQUIRE(ISLMath::SmoothStep(0.0f, 1.0f, -1.0f) == Approx(0.0f));
	REQUIRE(ISLMath::SmoothStep(0.0f, 1.0f, 2.0f) == Approx(1.0f));
}

TEST_CASE("CalculateRadius matches the closed form for default cutoff", "[isl]")
{
	// cutoffOverride == 1 -> default cutoff (0.05 for non-shadow); size 0.
	// radius = sqrt(ScaledUnitsSq * (2*intensity / (2*0.05))) = sqrt(3920 * 20) = 280.
	REQUIRE(ISLMath::CalculateRadius(1.0f, false, 1.0f, 0.0f) == Approx(280.0f));
}

TEST_CASE("Shadow casters get a larger radius (smaller cutoff)", "[isl]")
{
	const float normal = ISLMath::CalculateRadius(1.0f, false, 1.0f, 0.0f);
	const float shadow = ISLMath::CalculateRadius(1.0f, true, 1.0f, 0.0f);
	REQUIRE(shadow > normal);
}

TEST_CASE("cutoffOverride replaces the default regardless of shadow flag", "[isl]")
{
	// Override of 0.05 forces the non-shadow default value even for a shadow caster.
	REQUIRE(ISLMath::CalculateRadius(1.0f, true, 0.05f, 0.0f) == Approx(280.0f));
}

TEST_CASE("CalculateRadius clamps a NaN (negative-sqrt) result to 1", "[isl]")
{
	// intensity 0 with a large size makes the sqrt argument negative -> NaN -> 1.
	REQUIRE(ISLMath::CalculateRadius(0.0f, false, 1.0f, 10.0f) == Approx(1.0f));
}

TEST_CASE("GetAttenuation peaks at the source and vanishes past the radius", "[isl]")
{
	// distance 0, radius 280, size 1: attenuation = 3920/(0 + 3920/2) = 2.0;
	// fade is 1 at the source.
	REQUIRE(ISLMath::GetAttenuation(0.0f, 280.0f, 1.0f) == Approx(2.0f));
	// Beyond the radius the SmoothStep fade clamps to zero.
	REQUIRE(ISLMath::GetAttenuation(300.0f, 280.0f, 1.0f) == Approx(0.0f));
}

TEST_CASE("GetAttenuation decreases monotonically with distance in range", "[isl]")
{
	// 'near'/'far' are legacy Windows.h macros, so use distinct names.
	const float attNear = ISLMath::GetAttenuation(10.0f, 280.0f, 1.0f);
	const float attFar = ISLMath::GetAttenuation(150.0f, 280.0f, 1.0f);
	REQUIRE(attNear > attFar);
	REQUIRE(attFar > 0.0f);
}
