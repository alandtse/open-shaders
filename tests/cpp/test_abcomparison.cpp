#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Features/PerformanceOverlay/ABTesting/ABComparison.h"

namespace
{
	ABHeadlineInterval Interval(ABVariant variant, float ms, size_t frames = 30)
	{
		return { variant, std::vector<float>(frames, ms) };
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
