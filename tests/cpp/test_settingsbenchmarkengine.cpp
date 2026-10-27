// Unit tests for the settings benchmark engine: block scheduling, bracketing baselines, drift
// uncertainty, hitch rejection and cancellation. These exercise the production engine only.

#include "Features/PerformanceOverlay/ABTesting/SettingsBenchmarkEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

namespace
{
	using namespace SettingsBenchmark;

	std::vector<VariantInfo> Variants(size_t count)
	{
		std::vector<VariantInfo> variants;
		for (size_t i = 0; i < count; ++i)
			variants.push_back({ "v" + std::to_string(i), "Variant " + std::to_string(i), "Group" });
		return variants;
	}

	Config FastConfig()
	{
		return { .warmupFrames = 2, .measureFrames = 10, .repeats = 1, .countdownSeconds = 0.0f };
	}

	struct Run
	{
		std::vector<int> applied;
		bool restored = false;
	};

	/** @brief Drives the engine to completion; `gpuFor(appliedVariant, frameInBlock)` supplies each sample. */
	Run Drive(Engine& engine, const std::function<float(int, uint32_t)>& gpuFor, uint32_t maxFrames = 100000)
	{
		Run run;
		int active = kBaseline;
		uint32_t frame = 0;
		for (uint32_t i = 0; i < maxFrames && engine.IsRunning(); ++i) {
			const auto directive = engine.Tick({ gpuFor(active, frame++), 1.0f }, true, 0.011f, true);
			if (directive.apply) {
				active = directive.variant;
				run.applied.push_back(active);
				frame = 0;
			}
			run.restored |= directive.restore;
		}
		return run;
	}
}

TEST_CASE("Benchmark brackets every variant between baselines", "[settingsbenchmark]")
{
	Engine engine;
	REQUIRE(engine.Start(Variants(2), FastConfig()));
	const auto run = Drive(engine, [](int, uint32_t) { return 5.0f; });

	CHECK(run.applied == std::vector<int>{ kBaseline, 0, kBaseline, 1, kBaseline });
	CHECK(run.restored);
	CHECK(engine.GetPhase() == Phase::Finished);
}

TEST_CASE("Benchmark reports the variant cost against the baseline", "[settingsbenchmark]")
{
	Engine engine;
	REQUIRE(engine.Start(Variants(2), FastConfig()));
	Drive(engine, [](int variant, uint32_t) { return variant == 0 ? 7.0f : variant == 1 ? 4.5f :
		                                                                                  5.0f; });

	const auto results = engine.Results();
	REQUIRE(results.size() == 2);
	CHECK(results[0].gpuDeltaMs == Catch::Approx(2.0f));
	CHECK(results[0].gpuUncertaintyMs == Catch::Approx(0.0f).margin(1e-4));
	CHECK(results[0].gpuBaselineMs == Catch::Approx(5.0f));
	CHECK(results[0].gpuVariantMs == Catch::Approx(7.0f));
	CHECK(results[1].gpuDeltaMs == Catch::Approx(-0.5f));
	CHECK(results[0].repeats == 1);
}

TEST_CASE("Benchmark turns baseline drift into uncertainty", "[settingsbenchmark]")
{
	Engine engine;
	REQUIRE(engine.Start(Variants(1), FastConfig()));
	int baselineBlocks = 0;
	int lastActive = 99;
	Drive(engine, [&](int variant, uint32_t frame) {
		if (variant != lastActive) {
			lastActive = variant;
			if (variant == kBaseline)
				++baselineBlocks;
		}
		(void)frame;
		if (variant == 0)
			return 6.0f;
		return baselineBlocks == 1 ? 5.0f : 5.4f;
	});

	const auto results = engine.Results();
	REQUIRE(results.size() == 1);
	CHECK(results[0].gpuBaselineMs == Catch::Approx(5.2f));
	CHECK(results[0].gpuDeltaMs == Catch::Approx(0.8f));
	CHECK(results[0].gpuUncertaintyMs == Catch::Approx(0.2f));
}

TEST_CASE("Benchmark rejects hitches and missing samples", "[settingsbenchmark]")
{
	Engine engine;
	REQUIRE(engine.Start(Variants(1), FastConfig()));
	Drive(engine, [](int, uint32_t frame) {
		if (frame % 5 == 4)
			return kMaxFrameMs + 50.0f;
		if (frame % 7 == 6)
			return 0.0f;
		return 5.0f;
	});

	const auto results = engine.Results();
	REQUIRE(results.size() == 1);
	CHECK(results[0].gpuVariantMs == Catch::Approx(5.0f));
	CHECK(results[0].gpuDeltaMs == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("Benchmark waits for the countdown and for a measurable frame", "[settingsbenchmark]")
{
	Engine engine;
	auto config = FastConfig();
	config.countdownSeconds = 1.0f;
	REQUIRE(engine.Start(Variants(1), config));

	for (int i = 0; i < 50; ++i)
		CHECK_FALSE(engine.Tick({ 5.0f, 1.0f }, true, 0.011f, true).apply);
	CHECK(engine.GetPhase() == Phase::Countdown);

	bool applied = false;
	for (int i = 0; i < 60 && !applied; ++i)
		applied = engine.Tick({ 5.0f, 1.0f }, true, 0.011f, false).apply;
	CHECK_FALSE(applied);
	CHECK(engine.GetPhase() == Phase::Switching);

	CHECK(engine.Tick({ 5.0f, 1.0f }, true, 0.011f, true).apply);
	CHECK(engine.GetPhase() == Phase::Warmup);
}

TEST_CASE("Benchmark pauses frame counting while measurement is blocked", "[settingsbenchmark]")
{
	Engine engine;
	REQUIRE(engine.Start(Variants(1), FastConfig()));
	engine.Tick({ 5.0f, 1.0f }, true, 0.011f, true);  // applies the first baseline
	engine.Tick({ 5.0f, 1.0f }, true, 0.011f, true);
	engine.Tick({ 5.0f, 1.0f }, true, 0.011f, true);  // warm-up done
	REQUIRE(engine.GetPhase() == Phase::Measuring);

	for (int i = 0; i < 100; ++i)
		engine.Tick({ 5.0f, 1.0f }, true, 0.011f, false);
	CHECK(engine.GetProgress().framesInBlock == 0);
	engine.Tick({ 5.0f, 1.0f }, true, 0.011f, true);
	CHECK(engine.GetProgress().framesInBlock == 1);
}

TEST_CASE("Benchmark fails and restores when no valid timing arrives", "[settingsbenchmark]")
{
	Engine engine;
	REQUIRE(engine.Start(Variants(1), FastConfig()));

	bool restored = false;
	for (int i = 0; i < 100000 && engine.IsRunning(); ++i)
		restored |= engine.Tick({}, false, 0.011f, true).restore;

	CHECK(restored);
	CHECK(engine.GetPhase() == Phase::Failed);
	CHECK_FALSE(engine.FailureReason().empty());
}

TEST_CASE("Benchmark cancel restores once and refuses a second start while running", "[settingsbenchmark]")
{
	Engine engine;
	CHECK_FALSE(engine.Start({}, FastConfig()));
	REQUIRE(engine.Start(Variants(1), FastConfig()));
	CHECK_FALSE(engine.Start(Variants(1), FastConfig()));

	CHECK(engine.Cancel().restore);
	CHECK(engine.GetPhase() == Phase::Cancelled);
	CHECK_FALSE(engine.Cancel().restore);
	CHECK(engine.Results()[0].error == "Not measured");
	CHECK(engine.Start(Variants(1), FastConfig()));
}

TEST_CASE("Benchmark combines repeats", "[settingsbenchmark]")
{
	Engine engine;
	auto config = FastConfig();
	config.repeats = 2;
	REQUIRE(engine.Start(Variants(1), config));
	const auto run = Drive(engine, [](int variant, uint32_t) { return variant == 0 ? 6.0f : 5.0f; });

	CHECK(run.applied.size() == 6);
	const auto results = engine.Results();
	REQUIRE(results.size() == 1);
	CHECK(results[0].repeats == 2);
	CHECK(results[0].gpuDeltaMs == Catch::Approx(1.0f));
}
