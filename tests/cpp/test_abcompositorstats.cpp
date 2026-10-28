// Unit tests for the SteamVR compositor accumulator the A/B test uses: per-variant means, outlier
// rejection and the B-minus-A delta. These exercise the production accumulator only.

#include "Features/PerformanceOverlay/ABTesting/ABCompositorStats.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
	CompositorSample Sample(float gpuMs, uint32_t dropped = 0, bool reprojected = false)
	{
		return { .applicationGpuMs = gpuMs - 1.0f, .totalRenderGpuMs = gpuMs, .compositorRenderGpuMs = 1.0f, .compositorRenderCpuMs = 0.5f, .clientFrameIntervalMs = 11.1f, .droppedFrames = dropped, .reprojected = reprojected };
	}
}

TEST_CASE("Compositor accumulator averages each variant separately", "[abtest][compositor]")
{
	CompositorStatsAccumulator stats;
	REQUIRE(stats.Add(ABVariant::A, Sample(6.0f)));
	REQUIRE(stats.Add(ABVariant::A, Sample(8.0f)));
	REQUIRE(stats.Add(ABVariant::B, Sample(10.0f)));

	const auto a = stats.Summarize(ABVariant::A);
	const auto b = stats.Summarize(ABVariant::B);
	CHECK(a.frames == 2);
	CHECK(a.meanTotalRenderGpuMs == Catch::Approx(7.0f));
	CHECK(a.meanApplicationGpuMs == Catch::Approx(6.0f));
	CHECK(a.meanCompositorRenderGpuMs == Catch::Approx(1.0f));
	CHECK(a.meanClientFrameIntervalMs == Catch::Approx(11.1f));
	CHECK(b.frames == 1);
	CHECK(b.meanTotalRenderGpuMs == Catch::Approx(10.0f));
	CHECK(stats.DeltaTotalRenderGpuMs() == Catch::Approx(3.0f));
	CHECK(stats.DeltaApplicationGpuMs() == Catch::Approx(3.0f));
}

TEST_CASE("Compositor accumulator rejects impossible frame costs", "[abtest][compositor]")
{
	CompositorStatsAccumulator stats;
	CHECK_FALSE(stats.Add(ABVariant::A, Sample(0.0f)));
	CHECK_FALSE(stats.Add(ABVariant::A, Sample(-1.0f)));
	CHECK_FALSE(stats.Add(ABVariant::A, Sample(kMaxOutlierFrameTime + 1.0f)));
	CHECK(stats.Add(ABVariant::A, Sample(kMaxOutlierFrameTime)));
	CHECK(stats.RejectedFrames() == 3);
	CHECK(stats.Summarize(ABVariant::A).frames == 1);
}

TEST_CASE("Compositor accumulator counts dropped and reprojected frames", "[abtest][compositor]")
{
	CompositorStatsAccumulator stats;
	stats.Add(ABVariant::B, Sample(9.0f, 2, true));
	stats.Add(ABVariant::B, Sample(9.0f, 1, false));
	stats.Add(ABVariant::B, Sample(9.0f, 0, true));

	const auto b = stats.Summarize(ABVariant::B);
	CHECK(b.droppedFrames == 3);
	CHECK(b.reprojectedFrames == 2);
}

TEST_CASE("Compositor accumulator delta needs both variants and Reset clears", "[abtest][compositor]")
{
	CompositorStatsAccumulator stats;
	CHECK(stats.DeltaTotalRenderGpuMs() == 0.0f);
	CHECK(stats.Summarize(ABVariant::A).meanTotalRenderGpuMs == 0.0f);

	stats.Add(ABVariant::A, Sample(5.0f));
	CHECK(stats.DeltaTotalRenderGpuMs() == 0.0f);

	stats.Add(ABVariant::B, Sample(7.0f));
	CHECK(stats.DeltaTotalRenderGpuMs() == Catch::Approx(2.0f));

	stats.Add(ABVariant::A, Sample(0.0f));
	stats.Reset();
	CHECK(stats.Summarize(ABVariant::A).frames == 0);
	CHECK(stats.RejectedFrames() == 0);
	CHECK(stats.DeltaTotalRenderGpuMs() == 0.0f);
}
