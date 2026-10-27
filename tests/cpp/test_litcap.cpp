// Unit tests for the lit-area tile cap decisions (src/Features/LightLimitFix/LitCap.h).

#include "Features/LightLimitFix/LitCap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace LightLimitFixLitCap;

TEST_CASE("IsMeasured accepts verdict reasons and rejects missing tiles", "[llf]")
{
	CHECK(IsMeasured(kLit));
	CHECK(IsMeasured(kBlocked | kLit));
	CHECK(IsMeasured(kNoProjection));
	CHECK(IsMeasured(kSpot));
	CHECK_FALSE(IsMeasured(0));
	CHECK_FALSE(IsMeasured(kNoTile));
	CHECK_FALSE(IsMeasured(kFading));
}

TEST_CASE("Smooth attacks instantly and releases slowly", "[llf]")
{
	CHECK(Smooth(100.0f, 500.0f) == Approx(500.0f));
	CHECK(Smooth(100.0f, 0.0f) == Approx(97.0f));
	CHECK(Smooth(100.0f, 100.0f) == Approx(100.0f));
}

TEST_CASE("SetsReference ignores spots, fill lights and unsampled lights", "[llf]")
{
	CHECK(SetsReference(kLit | kBlocked, 880.0f));
	CHECK(SetsReference(kLit, kRefMaxRadius));
	CHECK_FALSE(SetsReference(kLit, kRefMaxRadius + 1.0f));
	CHECK_FALSE(SetsReference(kSpot, 880.0f));
	CHECK_FALSE(SetsReference(kNoProjection, 880.0f));
	CHECK_FALSE(SetsReference(0, 880.0f));
}

TEST_CASE("NextCapped latches with hysteresis", "[llf]")
{
	CHECK(NextCapped(false, 5.0f, 100.0f));
	CHECK_FALSE(NextCapped(false, 15.0f, 100.0f));
	CHECK(NextCapped(true, 15.0f, 100.0f));
	CHECK_FALSE(NextCapped(true, 20.0f, 100.0f));
	CHECK_FALSE(NextCapped(false, 100.0f, 100.0f));
}

TEST_CASE("NextCapped keeps its state without a reference", "[llf]")
{
	CHECK(NextCapped(true, 0.0f, 0.0f));
	CHECK_FALSE(NextCapped(false, 0.0f, 0.0f));
}
