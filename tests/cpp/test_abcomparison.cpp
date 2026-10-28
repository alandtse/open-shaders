#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Features/PerformanceOverlay/ABTesting/ABComparison.h"

#include <optional>

namespace
{
	ABHeadlineInterval Interval(ABVariant variant, float ms, size_t frames = 30, std::optional<float> frame = std::nullopt)
	{
		ABHeadlineInterval interval{ variant, std::vector<float>(frames, ms) };
		if (frame)
			interval.frameMs.assign(frames, *frame);
		return interval;
	}
}

TEST_CASE("Comparison reports a slower B outside the noise", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f), Interval(ABVariant::B, 6.0f), Interval(ABVariant::A, 5.0f) });

	REQUIRE(result.valid);
	CHECK(result.deltaMs == Catch::Approx(1.0f));
	CHECK(result.meanAMs == Catch::Approx(5.0f));
	CHECK(result.meanBMs == Catch::Approx(6.0f));
	CHECK(result.verdict == ABVerdict::Slower);
}

TEST_CASE("Comparison reports a faster B", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 8.0f), Interval(ABVariant::B, 6.0f), Interval(ABVariant::A, 8.0f) });

	CHECK(result.deltaMs == Catch::Approx(-2.0f));
	CHECK(result.verdict == ABVerdict::Faster);
}

TEST_CASE("Drift between the bracketing baselines becomes uncertainty", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f), Interval(ABVariant::B, 5.6f), Interval(ABVariant::A, 6.0f) });

	REQUIRE(result.valid);
	CHECK(result.deltaMs == Catch::Approx(0.1f));
	CHECK(result.uncertaintyMs == Catch::Approx(0.5f));
	CHECK(result.verdict == ABVerdict::NoChange);
}

TEST_CASE("A delta under the noise floor is no change", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f), Interval(ABVariant::B, 5.04f), Interval(ABVariant::A, 5.0f) });

	CHECK(result.verdict == ABVerdict::NoChange);
}

TEST_CASE("Several pairs average and spread into uncertainty", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f), Interval(ABVariant::B, 6.0f), Interval(ABVariant::A, 5.0f),
		Interval(ABVariant::B, 6.4f), Interval(ABVariant::A, 5.0f) });

	CHECK(result.deltaMs == Catch::Approx(1.2f));
	CHECK(result.uncertaintyMs == Catch::Approx(0.2f));
	CHECK(result.verdict == ABVerdict::Slower);
}

TEST_CASE("Comparison is invalid without a usable bracketing baseline", "[abcomparison]")
{
	CHECK_FALSE(ComputeABComparison({}).valid);
	CHECK_FALSE(ComputeABComparison({ Interval(ABVariant::B, 6.0f) }).valid);
	CHECK_FALSE(ComputeABComparison({ Interval(ABVariant::A, 5.0f, 3), Interval(ABVariant::B, 6.0f) }).valid);
}

TEST_CASE("Comparison uses one baseline when only one side exists", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f), Interval(ABVariant::B, 7.0f) });

	REQUIRE(result.valid);
	CHECK(result.deltaMs == Catch::Approx(2.0f));
}

TEST_CASE("Frame time leads the verdict when it rises", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f, 30, 16.0f), Interval(ABVariant::B, 5.0f, 30, 20.0f), Interval(ABVariant::A, 5.0f, 30, 16.0f) }, true);

	REQUIRE(result.valid);
	REQUIRE(result.frame.valid);
	CHECK(result.frame.deltaMs == Catch::Approx(4.0f));
	CHECK(result.verdict == ABVerdict::Slower);
	CHECK_FALSE(result.headroomOnly);
}

TEST_CASE("A GPU-only change is headroom when the frame time holds", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f, 30, 16.0f), Interval(ABVariant::B, 6.0f, 30, 16.0f), Interval(ABVariant::A, 5.0f, 30, 16.0f) }, true);

	REQUIRE(result.frame.valid);
	CHECK(result.frame.verdict == ABVerdict::NoChange);
	CHECK(result.headroomOnly);
	CHECK(result.verdict == ABVerdict::Slower);
}

TEST_CASE("Frame time does not lead the verdict unless asked", "[abcomparison]")
{
	const auto result = ComputeABComparison({ Interval(ABVariant::A, 5.0f, 30, 16.0f), Interval(ABVariant::B, 5.0f, 30, 20.0f), Interval(ABVariant::A, 5.0f, 30, 16.0f) });

	CHECK(result.verdict == ABVerdict::NoChange);
	CHECK_FALSE(result.headroomOnly);
}

TEST_CASE("A frame series with too few samples falls back to the GPU verdict", "[abcomparison]")
{
	const auto shortFrames = [](ABVariant variant, float gpuMs) {
		ABHeadlineInterval interval{ variant, std::vector<float>(30, gpuMs) };
		interval.frameMs.assign(3, 16.0f);
		return interval;
	};
	const auto result = ComputeABComparison({ shortFrames(ABVariant::A, 5.0f), shortFrames(ABVariant::B, 6.0f), shortFrames(ABVariant::A, 5.0f) }, true);

	REQUIRE(result.valid);
	CHECK_FALSE(result.frame.valid);
	CHECK_FALSE(result.headroomOnly);
	CHECK(result.verdict == ABVerdict::Slower);
}
