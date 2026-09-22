#include "Features/Upscaling/NeuralRendering/AdaptiveController.h"
#include "Features/Upscaling/NeuralRendering/RuntimePolicy.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using NRController = NeuralRendering::AdaptiveController;

TEST_CASE("VRAM pressure ceiling survives headroom and budget edits until explicit reset", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.memoryPressure = true;
	config.minimumDwellFrames = 4;
	for (unsigned frame = 0; frame < 4; ++frame)
		controller.Update(frame, config, true, 40.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
	REQUIRE(controller.MemoryCeiling() == 95);
	config.memoryPressure = false;
	config.targetFps = 15;
	for (unsigned frame = 4; frame < 2000; ++frame)
		controller.Update(frame, config, true, 10.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
	controller.Update(2000, config, false, 10.0f, 25.0f);
	REQUIRE(controller.MemoryCeiling() == 95);
	for (unsigned frame = 2001; frame < 2200; ++frame)
		controller.Update(frame, config, true, 10.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
	controller.Reset();
	controller.Update(2200, config, true, 10.0f, 25.0f);
	REQUIRE(controller.MemoryCeiling() == 100);
	REQUIRE(controller.ActiveResolution() == 100);
}
TEST_CASE("Memory ceiling still permits recovery from ordinary workload downshifts", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.memoryPressure = true;
	config.minimumDwellFrames = 4;
	for (unsigned frame = 0; frame < 4; ++frame)
		controller.Update(frame, config, true, 40.0f, 25.0f);
	config.memoryPressure = false;
	for (unsigned frame = 4; frame < 200; ++frame)
		controller.Update(frame, config, true, 40.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 70);
	REQUIRE(controller.MemoryCeiling() == 95);
	for (unsigned frame = 200; frame < 1000; ++frame)
		controller.Update(frame, config, true, 10.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
}

TEST_CASE("Adaptive tier changes preserve the handoff but route changes do not", "[adaptive][nr]")
{
	NeuralRendering::TemporalHistoryConfig previous;
	previous.adaptive = true;
	for (const auto resolution : NRController::ResolutionBuckets()) {
		auto next = previous;
		next.modelResolution = resolution;
		REQUIRE(previous.PreservesHandoff(next));
		previous = next;
	}
	auto next = previous;
	next.adaptive = false;
	REQUIRE_FALSE(previous.PreservesHandoff(next));
	next = previous;
	next.passes = 2;
	REQUIRE_FALSE(previous.PreservesHandoff(next));
	next = previous;
	next.cadence = 2;
	REQUIRE_FALSE(previous.PreservesHandoff(next));
	next = previous;
	next.depthThreshold = 0.1f;
	REQUIRE_FALSE(previous.PreservesHandoff(next));
}

TEST_CASE("NR residency stays bounded across a full ladder and repeated frames", "[adaptive][nr]")
{
	NeuralRendering::TierResidency residency;
	for (unsigned tier = 0; tier < 7; ++tier) {
		REQUIRE(residency.Select(tier, true));
		for (unsigned frame = 0; frame < 10; ++frame)
			REQUIRE_FALSE(residency.Select(tier, true));
		unsigned count = 0;
		for (unsigned candidate = 0; candidate < 7; ++candidate)
			count += residency.Contains(candidate);
		REQUIRE(count == (tier == 0 ? 1 : 2));
	}
	residency.Select(5, true);
	REQUIRE(residency.Contains(6));
	REQUIRE(residency.Contains(5));
	residency.Select(5, false);
	REQUIRE_FALSE(residency.Contains(6));
}

TEST_CASE("NR downshift honors consecutive pressure setting", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.minimumDwellFrames = 4;
	config.downshiftFrames = 11;
	for (unsigned frame = 0; frame < 10; ++frame)
		controller.Update(frame, config, true, 26.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 100);
	controller.Update(10, config, true, 26.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
}

TEST_CASE("NR does not restore a tier with marginal headroom", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.minimumDwellFrames = 30;
	for (unsigned frame = 0; frame < 31; ++frame)
		controller.Update(frame, config, true, 32.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
	config.allowDownshift = false;
	for (unsigned frame = 31; frame < 200; ++frame)
		controller.Update(frame, config, true, 22.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
}

TEST_CASE("NR recursive weights follow an elapsed-time smoothstep", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.minimumDwellFrames = 30;
	config.downshiftFrames = 4;
	for (unsigned frame = 0; frame < 30; ++frame)
		controller.Update(frame, config, true, 32.0f, 25.0f);
	REQUIRE(controller.ActiveResolution() == 95);
	float oldWeight = 1.0f;
	config.allowDownshift = false;
	for (unsigned step = 1; step <= 3; ++step) {
		controller.Update(29 + step, config, true, 25.0f, 30.0f);
		oldWeight *= 1.0f - controller.HandoffAlpha();
		const float t = step * 30.0f / 120.0f;
		REQUIRE(1.0f - oldWeight == Catch::Approx(t * t * (3.0f - 2.0f * t)));
	}
}

TEST_CASE("adaptive NR ignores unavailable paced workload", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.refreshHz = 72;
	config.minimumDwellFrames = 4;
	for (unsigned frame = 0; frame < 100; ++frame)
		controller.Update(frame, config, true);
	REQUIRE(controller.ActiveResolution() == 100);
	REQUIRE_FALSE(controller.LastSampleOverBudget());
	controller.Update(101, config, true, std::numeric_limits<float>::quiet_NaN());
	REQUIRE_FALSE(controller.LastSampleHadHeadroom());
}

TEST_CASE("adaptive NR supports a custom FPS budget", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.targetFps = 60;
	controller.Update(0, config, true);
	REQUIRE(controller.ApplicationTargetFps() == Catch::Approx(60.0f));
	REQUIRE(controller.ApplicationDeadlineMs() == Catch::Approx(1000.0f / 60.0f));

	config.targetFps = 15;
	controller.Update(1, config, true);
	REQUIRE(controller.ApplicationTargetFps() == Catch::Approx(15.0f));
	REQUIRE(controller.ApplicationDeadlineMs() == Catch::Approx(1000.0f / 15.0f));
}

TEST_CASE("adaptive NR clamps custom FPS budgets", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.targetFps = 1;
	controller.Update(0, config, true);
	REQUIRE(controller.ApplicationTargetFps() == Catch::Approx(15.0f));

	config.targetFps = 120;
	controller.Update(1, config, true);
	REQUIRE(controller.ApplicationTargetFps() == Catch::Approx(60.0f));
}

TEST_CASE("adaptive NR recovers one tier from workload headroom", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.refreshHz = 72;
	config.minimumDwellFrames = 30;
	config.upshiftFrames = 12;
	for (unsigned frame = 0; frame < 31; ++frame)
		controller.Update(frame, config, true, 32.0f, 27.78f);
	REQUIRE(controller.ActiveResolution() == 95);
	config.allowDownshift = false;
	for (unsigned frame = 31; frame < 70; ++frame)
		controller.Update(frame, config, true, 20.0f, 27.78f);
	REQUIRE(controller.ActiveResolution() == 100);
}

TEST_CASE("crop transition blocks NR restoration and legal config edits preserve tier", "[adaptive][nr]")
{
	NRController controller;
	NRController::Config config;
	config.enabled = true;
	config.minimumDwellFrames = 30;
	for (unsigned frame = 0; frame < 31; ++frame)
		controller.Update(frame, config, true, 32.0f, 27.78f);
	REQUIRE(controller.ActiveResolution() == 95);
	config.allowDownshift = false;
	config.allowUpshift = false;
	config.guardTimeMs = 2.0f;
	for (unsigned frame = 31; frame < 100; ++frame)
		controller.Update(frame, config, true, 15.0f, 27.78f);
	REQUIRE(controller.ActiveResolution() == 95);
	controller.Update(101, config, false, 15.0f);
	REQUIRE(controller.ActiveResolution() == 100);
}
