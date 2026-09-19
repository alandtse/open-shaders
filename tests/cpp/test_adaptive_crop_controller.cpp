#include "Features/Upscaling/NeuralRendering/AdaptiveCropController.h"

#include <catch2/catch_test_macros.hpp>

using NeuralRendering::AdaptiveCropController;

namespace
{
	AdaptiveCropController::Config TestConfig()
	{
		AdaptiveCropController::Config config;
		config.enabled = true;
		config.minimumCoverage = 75;
		config.downshiftFrames = 2;
		config.upshiftFrames = 8;
		config.minimumDwellFrames = 16;
		config.transitionFrames = 4;
		return config;
	}

	void Update(AdaptiveCropController& controller, std::uint32_t frame,
		bool nrAtMinimum, bool nrAtMaximum, bool overBudget, bool headroom,
		bool eyeTracking = false)
	{
		controller.Update(frame, TestConfig(), true, 100, true, eyeTracking,
			nrAtMinimum, false, nrAtMaximum, overBudget, headroom);
	}
}

TEST_CASE("Adaptive crop waits for NR floor before downshifting", "[adaptive][crop]")
{
	AdaptiveCropController controller;

	// Pressure while NR still has room must not move the crop.
	for (std::uint32_t frame = 0; frame < 20; ++frame)
		Update(controller, frame, false, false, true, false);
	REQUIRE(controller.ActiveCoverage() == 100);

	// Once NR is at its floor, sustained pressure moves one crop tier only.
	Update(controller, 20, true, false, true, false);
	REQUIRE(controller.ActiveCoverage() == 95);
	REQUIRE(controller.IsTransitioning());
	REQUIRE(controller.HandoffAlpha() < 1.0f);
	for (std::uint32_t frame = 21; frame < 30; ++frame)
		Update(controller, frame, true, false, true, false);
	REQUIRE(controller.ActiveCoverage() == 95);
	REQUIRE(controller.TargetCoverage() == 95);
}

TEST_CASE("Adaptive crop restores only after NR reaches maximum", "[adaptive][crop]")
{
	AdaptiveCropController controller;
	for (std::uint32_t frame = 0; frame < 20; ++frame)
		Update(controller, frame, true, false, true, false);
	REQUIRE(controller.ActiveCoverage() == 95);

	// Headroom while NR is below 100% is reserved for NR restoration.
	for (std::uint32_t frame = 20; frame < 60; ++frame)
		Update(controller, frame, false, false, false, true);
	REQUIRE(controller.ActiveCoverage() == 95);

	// Only after NR is back at maximum may the crop expand.
	Update(controller, 60, false, true, false, true);
	REQUIRE(controller.ActiveCoverage() == 95);
	for (std::uint32_t frame = 61; frame < 72; ++frame)
		Update(controller, frame, false, true, false, true);
	REQUIRE(controller.ActiveCoverage() == 100);
}

TEST_CASE("Eye tracking hard-disables adaptive crop", "[adaptive][crop][gaze]")
{
	AdaptiveCropController controller;
	for (std::uint32_t frame = 0; frame < 20; ++frame)
		Update(controller, frame, true, false, true, false, true);

	REQUIRE_FALSE(controller.IsRuntimeActive());
	REQUIRE(controller.IsEyeTrackingBlocked());
	REQUIRE(controller.ActiveCoverage() == 100);
}
