#include "Features/SnowDeformationMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
namespace Math = SnowDeformationMath;

TEST_CASE("A capsule gets one sample per diameter of length, within bounds", "[snow-deformation]")
{
	REQUIRE(Math::CapsuleSampleCount(0.0f, 2.0f, 4) == 1);
	REQUIRE(Math::CapsuleSampleCount(3.9f, 2.0f, 4) == 1);
	REQUIRE(Math::CapsuleSampleCount(8.0f, 2.0f, 4) == 2);
	REQUIRE(Math::CapsuleSampleCount(80.0f, 2.0f, 4) == 4);
	REQUIRE(Math::CapsuleSampleCount(80.0f, 0.0f, 4) == 4);
	REQUIRE(Math::CapsuleSampleCount(80.0f, 2.0f, 0) == 1);
}

TEST_CASE("Samples run from point A to point B, a lone sample at the middle", "[snow-deformation]")
{
	REQUIRE(Math::CapsuleSampleParameter(0, 1) == Approx(0.5f));
	REQUIRE(Math::CapsuleSampleParameter(0, 3) == Approx(0.0f));
	REQUIRE(Math::CapsuleSampleParameter(1, 3) == Approx(0.5f));
	REQUIRE(Math::CapsuleSampleParameter(2, 3) == Approx(1.0f));
}

TEST_CASE("A hand touches snow only between the underground limit and the snow surface", "[snow-deformation]")
{
	constexpr float radius = 2.0f;
	constexpr float land = 100.0f;
	constexpr float reach = 20.0f;
	constexpr float below = 32.0f;
	REQUIRE(Math::TouchesSnow(land + radius, radius, land, reach, below));
	REQUIRE(Math::TouchesSnow(land + radius + reach, radius, land, reach, below));
	REQUIRE_FALSE(Math::TouchesSnow(land + radius + reach + 0.5f, radius, land, reach, below));
	REQUIRE(Math::TouchesSnow(land + radius - below, radius, land, reach, below));
	REQUIRE_FALSE(Math::TouchesSnow(land + radius - below - 0.5f, radius, land, reach, below));
}
