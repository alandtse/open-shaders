// Unit tests for the shared preview region overlay's pixel-rect to screen-rect mapping. Pure maths;
// the drawing itself needs an ImGui context and is exercised through the settings panel in game.

#include "Utils/RegionOverlay.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using Util::RegionOverlay::MapToScreen;
using Util::RegionOverlay::ScreenRect;
using Util::Subrect::PixelRegion;

namespace
{
	constexpr ImVec2 kOrigin{ 10.0f, 20.0f };
	constexpr ImVec2 kImage{ 200.0f, 100.0f };
}

TEST_CASE("MapToScreen scales a pixel rect onto the image rect", "[regionoverlay]")
{
	const ScreenRect screen = MapToScreen(PixelRegion{ 100, 50, 200, 100 }, 1000, 500, kOrigin, kImage);
	REQUIRE(screen.valid);
	CHECK(screen.min.x == Approx(30.0f));
	CHECK(screen.min.y == Approx(30.0f));
	CHECK(screen.max.x == Approx(70.0f));
	CHECK(screen.max.y == Approx(50.0f));
}

TEST_CASE("MapToScreen maps a full-frame rect to the whole image", "[regionoverlay]")
{
	const ScreenRect screen = MapToScreen(PixelRegion{ 0, 0, 1000, 500 }, 1000, 500, kOrigin, kImage);
	REQUIRE(screen.valid);
	CHECK(screen.min.x == Approx(kOrigin.x));
	CHECK(screen.min.y == Approx(kOrigin.y));
	CHECK(screen.max.x == Approx(kOrigin.x + kImage.x));
	CHECK(screen.max.y == Approx(kOrigin.y + kImage.y));
}

TEST_CASE("MapToScreen rejects an empty rect", "[regionoverlay]")
{
	CHECK_FALSE(MapToScreen(PixelRegion{ 10, 10, 0, 100 }, 1000, 500, kOrigin, kImage).valid);
	CHECK_FALSE(MapToScreen(PixelRegion{ 10, 10, 100, 0 }, 1000, 500, kOrigin, kImage).valid);
}

TEST_CASE("MapToScreen rejects a degenerate source frame or image", "[regionoverlay]")
{
	CHECK_FALSE(MapToScreen(PixelRegion{ 0, 0, 100, 100 }, 0, 500, kOrigin, kImage).valid);
	CHECK_FALSE(MapToScreen(PixelRegion{ 0, 0, 100, 100 }, 1000, 0, kOrigin, kImage).valid);
	CHECK_FALSE(MapToScreen(PixelRegion{ 0, 0, 100, 100 }, 1000, 500, kOrigin, ImVec2{ 0.0f, 100.0f }).valid);
	CHECK_FALSE(MapToScreen(PixelRegion{ 0, 0, 100, 100 }, 1000, 500, kOrigin, ImVec2{ 200.0f, 0.0f }).valid);
}

TEST_CASE("MapToScreen clamps a rect that runs past the source frame", "[regionoverlay]")
{
	const ScreenRect screen = MapToScreen(PixelRegion{ 500, 0, 900, 500 }, 1000, 500, kOrigin, kImage);
	REQUIRE(screen.valid);
	CHECK(screen.min.x == Approx(kOrigin.x + 0.5f * kImage.x));
	CHECK(screen.min.y == Approx(kOrigin.y));
	CHECK(screen.max.x == Approx(kOrigin.x + kImage.x));
	CHECK(screen.max.y == Approx(kOrigin.y + kImage.y));
}

TEST_CASE("MapToScreen rejects a rect entirely outside the source frame", "[regionoverlay]")
{
	CHECK_FALSE(MapToScreen(PixelRegion{ 1200, 10, 100, 100 }, 1000, 500, kOrigin, kImage).valid);
	CHECK_FALSE(MapToScreen(PixelRegion{ 10, 900, 100, 100 }, 1000, 500, kOrigin, kImage).valid);
}

TEST_CASE("MapToScreen scales each axis against its own source extent", "[regionoverlay]")
{
	// Non-square source drawn into an image with a different aspect: a uniform scale would put the
	// quarter-frame rect at 0.5 of the image on both axes, which this catches.
	const ScreenRect screen = MapToScreen(PixelRegion{ 0, 0, 960, 540 }, 1920, 1080, kOrigin, ImVec2{ 400.0f, 100.0f });
	REQUIRE(screen.valid);
	CHECK(screen.min.x == Approx(kOrigin.x));
	CHECK(screen.min.y == Approx(kOrigin.y));
	CHECK(screen.max.x == Approx(kOrigin.x + 200.0f));
	CHECK(screen.max.y == Approx(kOrigin.y + 50.0f));
}
