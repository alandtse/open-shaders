#include "Features/SnowDeformationMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

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

TEST_CASE("A held item is told from a hand by capsule length", "[snow-deformation]")
{
	REQUIRE_FALSE(Math::IsHeldItemCapsule(8.0f, 24.0f));
	REQUIRE_FALSE(Math::IsHeldItemCapsule(24.0f, 24.0f));
	REQUIRE(Math::IsHeldItemCapsule(70.0f, 24.0f));
}

TEST_CASE("A hand print covers the hand's wider ground extent and never shrinks below the minimum", "[snow-deformation]")
{
	REQUIRE(Math::BlobRadius(16.0f, 10.0f, 4.0f) == Approx(8.0f));
	REQUIRE(Math::BlobRadius(10.0f, 16.0f, 4.0f) == Approx(8.0f));
	REQUIRE(Math::BlobRadius(2.0f, 3.0f, 4.0f) == Approx(4.0f));
}

TEST_CASE("A hand brushes the snow only when it moves across the ground", "[snow-deformation]")
{
	REQUIRE_FALSE(Math::IsBrushing(0.0f, 0.0f, 0.25f));
	REQUIRE_FALSE(Math::IsBrushing(0.1f, 0.1f, 0.25f));
	REQUIRE(Math::IsBrushing(0.25f, 0.0f, 0.25f));
	REQUIRE(Math::IsBrushing(-0.2f, 0.2f, 0.25f));
}

TEST_CASE("Contact pressure is weight over footprint area and 0 when unknown", "[snow-deformation]")
{
	REQUIRE(Math::ContactPressure(10.0f, 4.0f) == Approx(2.5f));
	REQUIRE(Math::ContactPressure(0.0f, 4.0f) == 0.0f);
	REQUIRE(Math::ContactPressure(10.0f, 0.0f) == 0.0f);
}

TEST_CASE("Footprints follow the shape on the ground", "[snow-deformation]")
{
	REQUIRE(Math::CircleFootprint(2.0f) == Approx(4.0f * Math::Pi));
	REQUIRE(Math::BoxFootprint(30.0f, 4.0f) == Approx(480.0f));
	REQUIRE(Math::BoxFootprint(-1.0f, 4.0f) == 0.0f);
	REQUIRE(Math::CapsuleFootprint(60.0f, 3.0f) == Approx(6.0f * 66.0f));
	REQUIRE(Math::CapsuleFootprint(0.0f, 3.0f) == Approx(36.0f));
	REQUIRE(Math::BoxFootprint(30.0f, 4.0f) < Math::CircleFootprint(37.5f));
}

TEST_CASE("Weight scale runs from nothing for light bodies to a full print for heavy ones", "[snow-deformation]")
{
	constexpr float light = 0.01f;
	constexpr float heavy = 1.0f;
	REQUIRE(Math::WeightScale(0.0f, light, heavy) == Approx(1.0f));
	REQUIRE(Math::WeightScale(light * 0.1f, light, heavy) == Approx(0.0f));
	REQUIRE(Math::WeightScale(light, light, heavy) == Approx(0.0f));
	REQUIRE(Math::WeightScale(heavy, light, heavy) == Approx(1.0f));
	REQUIRE(Math::WeightScale(heavy * 10.0f, light, heavy) == Approx(1.0f));
	REQUIRE(Math::WeightScale(0.1f, light, heavy) == Approx(0.5f));
	REQUIRE(Math::WeightScale(0.05f, light, heavy) < Math::WeightScale(0.2f, light, heavy));
}

TEST_CASE("Drag grows with burial and never speeds an object up", "[snow-deformation]")
{
	REQUIRE(Math::SubmergedFraction(10.0f, 10.0f, 8.0f) == Approx(0.0f));
	REQUIRE(Math::SubmergedFraction(6.0f, 10.0f, 8.0f) == Approx(0.5f));
	REQUIRE(Math::SubmergedFraction(-5.0f, 10.0f, 8.0f) == Approx(1.0f));
	REQUIRE(Math::DragFactor(3.0f, 0.0f, 0.1f) == Approx(1.0f));
	REQUIRE(Math::DragFactor(3.0f, 1.0f, 0.1f) == Approx(std::exp(-0.3f)));
	REQUIRE(Math::DragFactor(3.0f, 1.0f, 0.1f) < Math::DragFactor(3.0f, 0.5f, 0.1f));
	REQUIRE(Math::DragFactor(-1.0f, 1.0f, 0.1f) == Approx(1.0f));
}

TEST_CASE("Light objects rest near the snow top and heavy ones at the ground", "[snow-deformation]")
{
	REQUIRE(Math::RestHeight(20.0f, 0.0f, 0.0f) == Approx(20.0f));
	REQUIRE(Math::RestHeight(20.0f, 0.0f, 1.0f) == Approx(0.0f));
	REQUIRE(Math::RestHeight(20.0f, 0.0f, 0.25f) == Approx(15.0f));
	REQUIRE(Math::RestHeight(20.0f, 12.0f, 0.0f) == Approx(8.0f));
	REQUIRE(Math::RestHeight(20.0f, 12.0f, 0.5f) == Approx(0.0f));
	REQUIRE(Math::RestHeight(20.0f, 50.0f, 0.0f) == Approx(0.0f));
	REQUIRE(Math::FloorRiseSpeed(-2.0f, 6.0f, 60.0f) == Approx(0.0f));
	REQUIRE(Math::FloorRiseSpeed(5.0f, 6.0f, 60.0f) == Approx(30.0f));
	REQUIRE(Math::FloorRiseSpeed(50.0f, 6.0f, 60.0f) == Approx(60.0f));
}

TEST_CASE("Wading slows a mover in proportion to leg burial", "[snow-deformation]")
{
	REQUIRE(Math::WadeSlowdown(0.0f, 40.0f, 0.5f) == Approx(0.0f));
	REQUIRE(Math::WadeSlowdown(20.0f, 40.0f, 0.5f) == Approx(0.25f));
	REQUIRE(Math::WadeSlowdown(80.0f, 40.0f, 0.5f) == Approx(0.5f));
	REQUIRE(Math::WadeSlowdown(-5.0f, 40.0f, 0.5f) == Approx(0.0f));
	REQUIRE(Math::WadeSlowdown(20.0f, 40.0f, 3.0f) == Approx(0.5f));
}

TEST_CASE("Cold adapted races are recognised by name", "[snow-deformation]")
{
	REQUIRE(Math::IsColdAdaptedName("Frost Troll"));
	REQUIRE(Math::IsColdAdaptedName("Frostbite Spider"));
	REQUIRE(Math::IsColdAdaptedName("Ice Wraith"));
	REQUIRE(Math::IsColdAdaptedName("snowy sabre cat"));
	REQUIRE(Math::IsColdAdaptedName("Draugr Ghost"));
	REQUIRE_FALSE(Math::IsColdAdaptedName("Nord"));
	REQUIRE_FALSE(Math::IsColdAdaptedName("Troll"));
	REQUIRE_FALSE(Math::IsColdAdaptedName("Sliceblade"));
	REQUIRE_FALSE(Math::IsColdAdaptedName(""));
}
