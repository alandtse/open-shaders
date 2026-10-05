// Unit tests for the actor-crop wiring NR adds on top of Util::Region: the actor
// prominence score, the guide-rect conversion, the crop's own reset policy, and the
// reset-reason names DevBench reads. The region maths itself is covered by test_region.cpp.

#include "Features/Upscaling/NeuralRendering/ActorRegion.h"
#include "Features/Upscaling/NeuralRendering/CropCalibration.h"
#include "Features/Upscaling/NeuralRendering/Diagnostics.h"
#include "Features/Upscaling/NeuralRendering/FoveaClip.h"
#include "Features/Upscaling/NeuralRendering/Runtime.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("The tight crop fit aligns the bounds without padding them", "[nr][roi]")
{
	constexpr uint32_t width = 1920, height = 1080;
	const Util::Region::ScreenBounds bounds{ 0.35f, 0.35f, 0.65f, 0.65f };

	const auto tight = Util::Region::PixelRegionFromBounds(bounds, width, height, NR::ActorRegion::kTightPadding);
	REQUIRE(tight.x == 640u);
	REQUIRE(tight.y == 320u);
	REQUIRE(tight.w == 640u);
	REQUIRE(tight.h == 384u);
}

TEST_CASE("The padded crop fit is strictly larger than the tight one for the same bounds", "[nr][roi]")
{
	constexpr uint32_t width = 1920, height = 1080;
	const Util::Region::ScreenBounds bounds{ 0.35f, 0.35f, 0.65f, 0.65f };

	const auto tight = Util::Region::PixelRegionFromBounds(bounds, width, height, NR::ActorRegion::kTightPadding);
	const auto padded = Util::Region::PixelRegionFromBounds(bounds, width, height, NR::ActorRegion::kPadding);
	REQUIRE(padded.w > tight.w);
	REQUIRE(padded.h > tight.h);
	REQUIRE(padded.x <= tight.x);
	REQUIRE(padded.y <= tight.y);
	REQUIRE(padded.x + padded.w >= tight.x + tight.w);
	REQUIRE(padded.y + padded.h >= tight.y + tight.h);
}

TEST_CASE("The fovea clip pads and aligns each eye's region outward", "[nr][roi][fovea]")
{
	constexpr uint32_t width = 1920, height = 1080;
	const std::array<Util::Subrect::UVRegion, 2> uv{
		Util::Subrect::UVRegion{ 0.25f, 0.25f, 0.5f, 0.5f },
		Util::Subrect::UVRegion{ 0.5f, 0.25f, 0.25f, 0.5f },
	};
	const auto clip = NR::FoveaClip::BuildClip(uv, width, height);
	REQUIRE(clip.active);
	REQUIRE(clip.eye[0].x == 448u);
	REQUIRE(clip.eye[0].y == 192u);
	REQUIRE(clip.eye[0].w == 1024u);
	REQUIRE(clip.eye[0].h == 704u);
	REQUIRE(clip.eye[1].x == 896u);
	REQUIRE(clip.eye[1].w == 576u);

	REQUIRE(clip.eye[0].x + NR::FoveaClip::kFeatherBandPixels <= 480.0f);
	REQUIRE(clip.eye[0].y + NR::FoveaClip::kFeatherBandPixels <= 270.0f);
	REQUIRE(clip.eye[0].x + clip.eye[0].w - NR::FoveaClip::kFeatherBandPixels >= 1440.0f);
	REQUIRE(clip.eye[0].y + clip.eye[0].h - NR::FoveaClip::kFeatherBandPixels >= 810.0f);
}

TEST_CASE("The fovea clip is inactive when an eye's region does not resolve", "[nr][roi][fovea]")
{
	constexpr uint32_t width = 1920, height = 1080;
	const std::array<Util::Subrect::UVRegion, 2> resolved{
		Util::Subrect::UVRegion{ 0.25f, 0.25f, 0.5f, 0.5f },
		Util::Subrect::UVRegion{ 0.25f, 0.25f, 0.5f, 0.5f },
	};
	REQUIRE(NR::FoveaClip::BuildClip(resolved, width, height).active);

	std::array<Util::Subrect::UVRegion, 2> offscreen = resolved;
	offscreen[1] = Util::Subrect::UVRegion{ 1.5f, 0.0f, 0.25f, 0.25f };
	REQUIRE_FALSE(NR::FoveaClip::BuildClip(offscreen, width, height).active);

	REQUIRE_FALSE(NR::FoveaClip::BuildClip(resolved, 0, 0).active);
}

TEST_CASE("The fovea clip narrows the feather subject and drops boxes outside it", "[nr][roi][fovea]")
{
	Util::Region::StereoRegion subject;
	subject.active = true;
	subject.eye.fill(Util::Subrect::PixelRegion{ 0, 0, 100, 100 });
	Util::Region::StereoRegion clip;
	clip.active = true;
	clip.eye[0] = Util::Subrect::PixelRegion{ 50, 50, 100, 100 };
	clip.eye[1] = Util::Subrect::PixelRegion{ 200, 200, 100, 100 };

	NR::FoveaClip::ClipSubject(subject, clip);
	REQUIRE(subject.active);
	REQUIRE(subject.eye[0].x == 50u);
	REQUIRE(subject.eye[0].w == 50u);
	REQUIRE(subject.eye[1].w == 0u);

	NR::FoveaClip::ClipSubject(subject, clip);
	REQUIRE(subject.eye[0].w == 50u);

	Util::Region::StereoRegion outside;
	outside.active = true;
	outside.eye.fill(Util::Subrect::PixelRegion{ 0, 0, 20, 20 });
	NR::FoveaClip::ClipSubject(outside, clip);
	REQUIRE_FALSE(outside.active);

	Util::Region::StereoRegion unchanged;
	unchanged.active = true;
	unchanged.eye.fill(Util::Subrect::PixelRegion{ 0, 0, 20, 20 });
	NR::FoveaClip::ClipSubject(unchanged, Util::Region::StereoRegion{});
	REQUIRE(unchanged.active);
	REQUIRE(unchanged.eye[0].w == 20u);
}

TEST_CASE("Tuning::Sanitize clamps the crop fit to the supported values", "[nr][roi]")
{
	NR::Tuning tuning;
	tuning.regionOfInterest = true;
	tuning.regionFit = NR::Tuning::kMaxRegionFit + 5;
	tuning.Sanitize();
	REQUIRE(tuning.regionFit == NR::Tuning::kMaxRegionFit);

	tuning.regionFit = NR::Tuning::kRegionFitTight;
	tuning.Sanitize();
	REQUIRE(tuning.regionFit == NR::Tuning::kRegionFitTight);

	tuning.regionFit = NR::Tuning::kRegionFitPadded;
	tuning.Sanitize();
	REQUIRE(tuning.regionFit == NR::Tuning::kRegionFitPadded);
}

TEST_CASE("Tuning::Sanitize clears the crop controls with the tracked actor", "[nr][roi]")
{
	NR::Tuning tracking{ .regionOfInterest = true, .regionOverlay = true, .regionFit = NR::Tuning::kRegionFitTight, .regionGroup = true };

	// Set with a tracked actor, every crop control survives sanitizing.
	tracking.Sanitize();
	REQUIRE(tracking.regionOverlay);
	REQUIRE(tracking.regionFit == NR::Tuning::kRegionFitTight);
	REQUIRE(tracking.regionGroup);

	// Without one they cannot stay set: the pass ignores them, so a retained value would read
	// as active while nothing reads it.
	tracking.regionOfInterest = false;
	tracking.Sanitize();
	REQUIRE_FALSE(tracking.regionOverlay);
	REQUIRE(tracking.regionFit == NR::Tuning::kRegionFitPadded);
	REQUIRE_FALSE(tracking.regionGroup);
}

TEST_CASE("Tuning::Sanitize keeps the fovea crop without a tracked actor", "[nr][roi][fovea]")
{
	NR::Tuning tuning;
	REQUIRE(tuning.regionFollowFoveation);
	REQUIRE_FALSE(tuning.regionOfInterest);
	tuning.Sanitize();
	REQUIRE(tuning.regionFollowFoveation);

	tuning.regionFollowFoveation = false;
	tuning.Sanitize();
	REQUIRE_FALSE(tuning.regionFollowFoveation);
}

namespace
{
	using NR::CropCalibration;

	template <typename CostFunction>
	CropCalibration RunSweep(CostFunction a_cost, float a_settleSpike = 0.0f)
	{
		CropCalibration calibration;
		calibration.Start();
		uint32_t frameInStep = 0;
		while (calibration.Running()) {
			const float fraction = calibration.CurrentFraction();
			const bool settling = frameInStep < CropCalibration::kSettleFrames;
			calibration.AddFrame(settling && a_settleSpike > 0.0f ? a_settleSpike : a_cost(fraction));
			frameInStep = (frameInStep + 1) % (CropCalibration::kSettleFrames + CropCalibration::kSampleFrames);
		}
		return calibration;
	}
}

TEST_CASE("CropCalibration finds the largest crop that costs no more than the cheapest", "[nr][roi][calibration]")
{
	const auto calibration = RunSweep([](float a_fraction) { return std::max(2.5f, 3.6f * a_fraction); });
	const auto& result = calibration.GetResult();
	REQUIRE(result.state == CropCalibration::State::kDone);
	REQUIRE(result.floorMs == Catch::Approx(2.5f));
	REQUIRE(result.stepMs[0] == Catch::Approx(3.6f));
	REQUIRE(result.kneeFraction == Catch::Approx(0.6f));
}

TEST_CASE("CropCalibration discards the settle frames after each crop change", "[nr][roi][calibration]")
{
	const auto calibration = RunSweep([](float) { return 2.0f; }, 50.0f);
	const auto& result = calibration.GetResult();
	REQUIRE(result.state == CropCalibration::State::kDone);
	for (const float stepMs : result.stepMs)
		REQUIRE(stepMs == Catch::Approx(2.0f));
	REQUIRE(result.kneeFraction == Catch::Approx(CropCalibration::kFractions.front()));
}

TEST_CASE("CropCalibration keeps each step's fastest pass and reports how unsteady the passes were", "[nr][roi][calibration]")
{
	uint32_t framesSeen = 0;
	const uint32_t framesPerPass = CropCalibration::kSteps * (CropCalibration::kSettleFrames + CropCalibration::kSampleFrames);
	CropCalibration calibration;
	calibration.Start();
	while (calibration.Running()) {
		const bool slowPass = (framesSeen++ / framesPerPass) % 2 == 0;
		calibration.AddFrame(slowPass ? 9.0f : 3.0f);
	}
	const auto& result = calibration.GetResult();
	REQUIRE(result.state == CropCalibration::State::kDone);
	for (const float stepMs : result.stepMs)
		REQUIRE(stepMs == Catch::Approx(3.0f));
	REQUIRE(result.stabilityRatio == Catch::Approx(3.0f));
}

TEST_CASE("CropCalibration fails when a step gets no timed frames and can be restarted", "[nr][roi][calibration]")
{
	CropCalibration calibration;
	calibration.Start();
	for (uint32_t frame = 0; frame < CropCalibration::kSettleFrames + CropCalibration::kSampleFrames && calibration.Running(); ++frame)
		calibration.AddFrame(0.0f);
	REQUIRE(calibration.GetResult().state == CropCalibration::State::kFailed);
	REQUIRE(calibration.GetResult().failure == CropCalibration::Failure::kNoSamples);
	REQUIRE_FALSE(calibration.Running());

	calibration.Start();
	REQUIRE(calibration.Running());
	REQUIRE(calibration.GetResult().failure == CropCalibration::Failure::kNone);
	REQUIRE(calibration.CurrentFraction() == Catch::Approx(CropCalibration::kFractions.front()));
}

TEST_CASE("ConsumeCapture accepts each resolved profiler capture once", "[nr][roi][calibration]")
{
	uint32_t lastConsumed = 0;

	// Nothing has resolved yet, so the first frame the sweep sees is already the one it marks.
	REQUIRE_FALSE(CropCalibration::ConsumeCapture(0, lastConsumed));

	// A resolved capture is sampled once, and the repeated reads before the next one resolves
	// are refused rather than counted as fresh frames.
	REQUIRE(CropCalibration::ConsumeCapture(7, lastConsumed));
	REQUIRE(lastConsumed == 7);
	REQUIRE_FALSE(CropCalibration::ConsumeCapture(7, lastConsumed));
	REQUIRE_FALSE(CropCalibration::ConsumeCapture(7, lastConsumed));
	REQUIRE(lastConsumed == 7);

	// The next capture is accepted, whatever moved its frame count.
	REQUIRE(CropCalibration::ConsumeCapture(9, lastConsumed));
	REQUIRE(lastConsumed == 9);
}

TEST_CASE("CenteredBounds covers the requested area around the frame centre", "[nr][roi][calibration]")
{
	for (const float fraction : CropCalibration::kFractions) {
		const auto bounds = NR::CenteredBounds(fraction);
		REQUIRE(Util::Region::AreaFraction(bounds) == Catch::Approx(fraction));
		REQUIRE(Util::Region::NormalizedCenterDistance(bounds) == Catch::Approx(0.0f).margin(1e-6));
	}
}

TEST_CASE("GroupAreaCap follows the calibrated knee within bounds and defaults without one", "[nr][roi][calibration]")
{
	REQUIRE(NR::ActorRegion::GroupAreaCap(0.0f) == Catch::Approx(NR::ActorRegion::kMaxGroupAreaFraction));
	REQUIRE(NR::ActorRegion::GroupAreaCap(0.4f) == Catch::Approx(0.4f));
	REQUIRE(NR::ActorRegion::GroupAreaCap(0.01f) == Catch::Approx(NR::ActorRegion::kMinCalibratedGroupAreaFraction));
	REQUIRE(NR::ActorRegion::GroupAreaCap(1.0f) == Catch::Approx(NR::ActorRegion::kMaxCalibratedGroupAreaFraction));
}
