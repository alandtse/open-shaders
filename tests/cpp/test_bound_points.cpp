// Unit tests for Util::BoundPoints and Util::PointBox, the engine-free containers the actor
// bound queries fill and the crop projection reads.

#include "Utils/BoundPoints.h"
#include "Utils/Region.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("PointBox is invalid until a point is included and then spans every point", "[boundpoints]")
{
	Util::PointBox box;
	REQUIRE_FALSE(box.Valid());

	box.Include({ 1.0f, -2.0f, 3.0f });
	REQUIRE(box.Valid());
	box.Include({ -4.0f, 5.0f, 0.0f });
	REQUIRE(box.min.x == Catch::Approx(-4.0f));
	REQUIRE(box.min.y == Catch::Approx(-2.0f));
	REQUIRE(box.min.z == Catch::Approx(0.0f));
	REQUIRE(box.max.x == Catch::Approx(1.0f));
	REQUIRE(box.max.y == Catch::Approx(5.0f));
	REQUIRE(box.max.z == Catch::Approx(3.0f));
}

TEST_CASE("PointBox::Expand grows every face by the margin", "[boundpoints]")
{
	Util::PointBox box;
	box.Include({ 0.0f, 0.0f, 0.0f });
	box.Include({ 2.0f, 4.0f, 6.0f });
	box.Expand(1.5f);
	REQUIRE(box.min.x == Catch::Approx(-1.5f));
	REQUIRE(box.max.z == Catch::Approx(7.5f));
}

TEST_CASE("BoundPoints::AddBox appends the eight distinct corners", "[boundpoints]")
{
	Util::BoundPoints bounds;
	bounds.AddBox({ 0.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 3.0f });
	REQUIRE(bounds.count == 8);
	Util::PointBox extent;
	for (const auto& point : bounds.View())
		extent.Include(point);
	REQUIRE(extent.min.x == Catch::Approx(0.0f));
	REQUIRE(extent.max.y == Catch::Approx(2.0f));
	REQUIRE(extent.max.z == Catch::Approx(3.0f));
	for (size_t a = 0; a < bounds.count; ++a)
		for (size_t b = a + 1; b < bounds.count; ++b)
			REQUIRE(bounds.points[a] != bounds.points[b]);
}

TEST_CASE("BoundPoints drops points beyond its capacity instead of overflowing", "[boundpoints]")
{
	Util::BoundPoints bounds;
	for (size_t i = 0; i < Util::BoundPoints::kCapacity + 5; ++i)
		bounds.Add({ static_cast<float>(i), 0.0f, 0.0f });
	REQUIRE(bounds.count == Util::BoundPoints::kCapacity);
	REQUIRE(bounds.View().size() == Util::BoundPoints::kCapacity);
}

TEST_CASE("ProjectBounds reports an empty point set as offscreen", "[boundpoints][region]")
{
	Util::BoundPoints none;
	Util::Region::ScreenBounds out;
	REQUIRE(Util::Region::ProjectBounds(DirectX::SimpleMath::Matrix::Identity, none.View(), out) ==
			Util::Region::ProjectionResult::kOffscreen);
}
