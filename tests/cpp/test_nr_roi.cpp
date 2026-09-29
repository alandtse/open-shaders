// Unit tests for the actor-crop wiring NR adds on top of Util::Region: the actor
// prominence score, the guide-rect conversion, the crop's own reset policy, and the
// reset-reason names DevBench reads. The region maths itself is covered by test_region.cpp.

#include "Features/Upscaling/NeuralRendering/ActorRegion.h"
#include "Features/Upscaling/NeuralRendering/Diagnostics.h"
#include "Features/Upscaling/NeuralRendering/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace
{
	/** @brief Bounds of a box centred on (a_centerX, a_centerY) with the given half-extents. */
	Util::Region::ScreenBounds Box(float a_centerX, float a_centerY, float a_halfWidth, float a_halfHeight)
	{
		return { a_centerX - a_halfWidth, a_centerY - a_halfHeight, a_centerX + a_halfWidth, a_centerY + a_halfHeight };
	}
}

TEST_CASE("Reset reason names are indexed by their enum bit", "[nr][roi]")
{
	const std::array<uint32_t, 8> bits{
		NR::Diagnostics::Requested,
		NR::Diagnostics::FirstFrame,
		NR::Diagnostics::FrameGap,
		NR::Diagnostics::CameraPosition,
		NR::Diagnostics::CameraDirection,
		NR::Diagnostics::Projection,
		NR::Diagnostics::FeatureCreated,
		NR::Diagnostics::RegionChanged,
	};
	REQUIRE(bits.size() == NR::Diagnostics::kResetReasonNames.size());
	for (size_t index = 0; index < bits.size(); ++index)
		REQUIRE(bits[index] == (1u << index));
	REQUIRE(std::string{ NR::Diagnostics::kResetReasonNames.back() } == "region");
}

TEST_CASE("ToGuideRegion carries the crop's pixels onto the guide rect", "[nr][roi]")
{
	const auto guide = NR::ToGuideRegion(Util::Subrect::PixelRegion{ 128, 64, 640, 480 });
	REQUIRE(guide.baseX == 128);
	REQUIRE(guide.baseY == 64);
	REQUIRE(guide.width == 640);
	REQUIRE(guide.height == 480);

	const auto empty = NR::ToGuideRegion(Util::Region::kEmptyRegion);
	REQUIRE(empty.baseX == 0);
	REQUIRE(empty.baseY == 0);
	REQUIRE(empty.width == 0);
	REQUIRE(empty.height == 0);
}

TEST_CASE("The crop reset tolerance is one alignment step", "[nr][roi]")
{
	REQUIRE(NR::ActorRegion::kHistoryTolerancePixels == static_cast<float>(Util::Region::kDefaultPixelAlignment));
}

TEST_CASE("The actor crop resets NR history on appearance and on a jump beyond one alignment step", "[nr][roi]")
{
	const auto region = [](bool active, uint32_t x) {
		Util::Region::StereoRegion next;
		next.active = active;
		next.eye.fill(Util::Subrect::PixelRegion{ x, 0, 512, 512 });
		return next;
	};
	const auto resets = [](const Util::Region::StereoRegion& current, const Util::Region::StereoRegion& previous) {
		return Util::Region::ShouldResetForRegion(NR::ActorRegion::kResetPolicy, current, previous, NR::ActorRegion::kHistoryTolerancePixels);
	};

	REQUIRE_FALSE(resets(region(true, 0), region(true, 0)));
	REQUIRE(resets(region(true, 0), region(false, 0)));
	REQUIRE(resets(region(false, 0), region(true, 0)));
	REQUIRE_FALSE(resets(region(true, 64), region(true, 0)));
	REQUIRE(resets(region(true, 128), region(true, 0)));
}

TEST_CASE("ActorScore favours larger coverage at the same centre", "[nr][roi]")
{
	const auto smaller = Box(0.5f, 0.5f, 0.1f, 0.1f);
	const auto larger = Box(0.5f, 0.5f, 0.2f, 0.2f);
	REQUIRE(NR::ActorRegion::ActorScore(larger, false) > NR::ActorRegion::ActorScore(smaller, false));
}

TEST_CASE("ActorScore falls off toward the frame edge", "[nr][roi]")
{
	const auto centred = Box(0.5f, 0.5f, 0.1f, 0.1f);
	const auto edge = Box(0.5f, 0.0f, 0.1f, 0.1f);
	REQUIRE(NR::ActorRegion::ActorScore(centred, false) > NR::ActorRegion::ActorScore(edge, false));
	REQUIRE(NR::ActorRegion::ActorScore(edge, false) < 0.1f * NR::ActorRegion::ActorScore(centred, false));

	float previous = std::numeric_limits<float>::max();
	for (const float offset : { 0.0f, 0.1f, 0.2f, 0.3f, 0.4f }) {
		const float score = NR::ActorRegion::ActorScore(Box(0.5f + offset, 0.5f, 0.1f, 0.1f), false);
		REQUIRE(score < previous);
		previous = score;
	}
}

TEST_CASE("ActorScore's incumbent bonus covers a challenger up to the bonus and no more", "[nr][roi]")
{
	const float half = 0.1f;
	const float within = half * std::sqrt(NR::ActorRegion::kIncumbentScoreBonus) * 0.99f;
	const float beyond = half * std::sqrt(NR::ActorRegion::kIncumbentScoreBonus) * 1.2f;
	const auto incumbent = Box(0.5f, 0.5f, half, half);
	REQUIRE(NR::ActorRegion::ActorScore(incumbent, true) > NR::ActorRegion::ActorScore(Box(0.5f, 0.5f, half, half), false));
	REQUIRE(NR::ActorRegion::ActorScore(incumbent, true) > NR::ActorRegion::ActorScore(Box(0.5f, 0.5f, within, within), false));
	REQUIRE(NR::ActorRegion::ActorScore(incumbent, true) < NR::ActorRegion::ActorScore(Box(0.5f, 0.5f, beyond, beyond), false));
}

TEST_CASE("A box below the minimum visible fraction falls under the tracking threshold", "[nr][roi]")
{
	const float side = std::sqrt(NR::ActorRegion::kMinVisibleAreaFraction) * 0.99f;
	REQUIRE(Util::Region::AreaFraction(Box(0.5f, 0.5f, side * 0.5f, side * 0.5f)) < NR::ActorRegion::kMinVisibleAreaFraction);
}
