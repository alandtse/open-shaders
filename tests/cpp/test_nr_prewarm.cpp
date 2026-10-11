// Unit tests for the startup-only NR feature creation scheduler.

#include "Features/Upscaling/NeuralRendering/StartupPrewarm.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
	NR::PrewarmInputs ReadyMenuFrame(uint32_t frame)
	{
		NR::PrewarmInputs input;
		input.requested = true;
		input.enabled = true;
		input.startupMenu = true;
		input.deviceReady = true;
		input.frame = frame;
		return input;
	}

	NR::PrewarmExtent MonoExtent() { return { 1920, 1080, 1 }; }
	NR::PrewarmExtent StereoExtent() { return { 1920, 1080, 2 }; }
}

TEST_CASE("Startup prewarm defaults to disabled and honors every prerequisite", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	auto input = ReadyMenuFrame(1);
	input.requested = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "disabled");

	input.requested = true;
	input.enabled = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "nr-disabled");

	input.enabled = true;
	input.runtimeFailed = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "runtime-failed");
	input.runtimeFailed = false;

	input.passResources = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "already-running");
	input.passResources = false;

	input.worldRendered = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "world-rendered");
	input.worldRendered = false;

	input.startupMenu = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "outside-startup-menu");
	input.startupMenu = true;

	input.deviceReady = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "device-unavailable");
}

TEST_CASE("Runtime initialization precedes extent qualification and only one step runs per frame", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	auto input = ReadyMenuFrame(10);
	input.runtimeReady = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Initialize);
	REQUIRE(std::string(scheduler.reason) == "initializing");

	// A second call in the same presented frame cannot begin feature creation,
	// even if initialization has completed synchronously.
	input.runtimeReady = true;
	scheduler.SetExtent(MonoExtent());
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "frame-step-complete");

	input.frame++;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye0);
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "frame-step-complete");
}

TEST_CASE("The first operation is allowed when the frame counter is UINT32_MAX", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	auto input = ReadyMenuFrame(UINT32_MAX);
	input.runtimeReady = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Initialize);
	REQUIRE(std::string(scheduler.reason) == "initializing");

	// The scheduler still enforces one operation for that frame after a successful step.
	input.runtimeReady = true;
	scheduler.SetExtent(MonoExtent());
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "frame-step-complete");
}

TEST_CASE("An unavailable or malformed extent blocks eye creation", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	auto input = ReadyMenuFrame(1);
	input.runtimeReady = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "extent-unavailable");

	for (const auto extent : { NR::PrewarmExtent{ 0, 1080, 1 }, NR::PrewarmExtent{ 1920, 0, 1 },
			 NR::PrewarmExtent{ 1920, 1080, 0 }, NR::PrewarmExtent{ 1920, 1080, 3 } }) {
		scheduler.SetExtent(extent);
		input.frame++;
		REQUIRE_FALSE(extent.Valid());
		REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
		REQUIRE(std::string(scheduler.reason) == "extent-unavailable");
	}
}

TEST_CASE("An unavailable cached extent blocks use of a previously valid extent", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	scheduler.SetExtent(StereoExtent());
	auto input = ReadyMenuFrame(1);
	input.runtimeReady = true;
	input.configuredExtentReady = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "extent-unavailable");
	REQUIRE(scheduler.preparedEyes == 0);
}

TEST_CASE("Mono and stereo extents advance one eye at a time", "[nr][prewarm]")
{
	SECTION("mono")
	{
		NR::StartupPrewarm scheduler;
		scheduler.SetExtent(MonoExtent());
		auto input = ReadyMenuFrame(1);
		input.runtimeReady = true;
		REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye0);
		scheduler.Complete(NR::PrewarmStep::Eye0);
		REQUIRE(scheduler.preparedEyes == 1);
		REQUIRE(std::string(scheduler.reason) == "ready");
		REQUIRE(scheduler.Matches(MonoExtent()));

		input.frame++;
		REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
		REQUIRE(std::string(scheduler.reason) == "ready");
	}

	SECTION("stereo")
	{
		NR::StartupPrewarm scheduler;
		scheduler.SetExtent(StereoExtent());
		auto input = ReadyMenuFrame(20);
		input.runtimeReady = true;
		REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye0);
		scheduler.Complete(NR::PrewarmStep::Eye0);
		REQUIRE(scheduler.preparedEyes == 1);
		REQUIRE(std::string(scheduler.reason) == "staged");
		REQUIRE(scheduler.Matches(StereoExtent()));

		input.frame++;
		REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye1);
		scheduler.Complete(NR::PrewarmStep::Eye1);
		REQUIRE(scheduler.preparedEyes == 2);
		REQUIRE(std::string(scheduler.reason) == "ready");
		REQUIRE(scheduler.Matches(StereoExtent()));
	}
}

TEST_CASE("Material-strength guides permit runtime setup but defer eye creation", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	scheduler.SetExtent(StereoExtent());
	auto input = ReadyMenuFrame(1);
	input.materialStrength = true;
	input.runtimeReady = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Initialize);

	input.frame++;
	input.runtimeReady = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "material-guides-required");
	REQUIRE(scheduler.preparedEyes == 0);

	// Once guide-dependent configuration is no longer active, the ordinary
	// startup scheduler can still prepare both eyes.
	input.materialStrength = false;
	input.frame++;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye0);
}

TEST_CASE("Temporary world, menu, device and pass gates preserve partial progress", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	scheduler.SetExtent(StereoExtent());
	auto input = ReadyMenuFrame(1);
	input.runtimeReady = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye0);
	scheduler.Complete(NR::PrewarmStep::Eye0);

	input.frame++;
	input.worldRendered = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "world-rendered");
	REQUIRE(scheduler.preparedEyes == 1);

	input.frame++;
	input.worldRendered = false;
	input.startupMenu = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "outside-startup-menu");

	input.frame++;
	input.startupMenu = true;
	input.deviceReady = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "device-unavailable");

	input.frame++;
	input.deviceReady = true;
	input.passResources = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "already-running");

	input.frame++;
	input.passResources = false;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye1);
	REQUIRE(scheduler.preparedEyes == 1);
}

TEST_CASE("Partial prepared-eye reuse matches only the exact extent", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	scheduler.SetExtent(StereoExtent());
	scheduler.Complete(NR::PrewarmStep::Eye0);
	REQUIRE(scheduler.Matches(StereoExtent()));
	REQUIRE_FALSE(scheduler.Matches({ 1921, 1080, 2 }));
	REQUIRE_FALSE(scheduler.Matches({ 1920, 1081, 2 }));
	REQUIRE_FALSE(scheduler.Matches({ 1920, 1080, 1 }));

	// Extent changes invalidate the warmed-eye count, so stale feature handles
	// can never be claimed as reusable for a new render size or eye count.
	scheduler.SetExtent({ 2048, 1080, 2 });
	REQUIRE(scheduler.preparedEyes == 0);
	REQUIRE_FALSE(scheduler.Matches({ 2048, 1080, 2 }));
	auto input = ReadyMenuFrame(2);
	input.runtimeReady = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::Eye0);
}

TEST_CASE("World use blocks further prewarm scheduling until reset", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	scheduler.usedInWorld = true;
	auto input = ReadyMenuFrame(1);
	input.runtimeReady = true;
	scheduler.SetExtent(MonoExtent());
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "already-running");

	scheduler.Reset();
	REQUIRE_FALSE(scheduler.usedInWorld);
	REQUIRE(scheduler.preparedEyes == 0);
	REQUIRE(std::string(scheduler.reason) == "disabled");
}

TEST_CASE("A failed prewarm attempt stays latched until reset", "[nr][prewarm]")
{
	NR::StartupPrewarm scheduler;
	scheduler.SetExtent(MonoExtent());
	scheduler.failed = true;
	auto input = ReadyMenuFrame(1);
	input.runtimeReady = true;
	REQUIRE(scheduler.Next(input) == NR::PrewarmStep::None);
	REQUIRE(std::string(scheduler.reason) == "failed");
	REQUIRE_FALSE(scheduler.Matches(MonoExtent()));

	scheduler.Reset();
	REQUIRE_FALSE(scheduler.failed);
	REQUIRE(scheduler.Next(ReadyMenuFrame(2)) == NR::PrewarmStep::Initialize);
}
