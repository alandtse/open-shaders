#include "Features/Upscaling/NeuralRendering/ModelReconstruction.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace NR::Reconstruction;

TEST_CASE("NR reconstruction retains legacy brightness guidance", "[nr]")
{
	REQUIRE(Select(kBilinearFilter, true, false, 0.75f, 1024, 1024, 768, 768) == Mode::BrightnessGuided);
	REQUIRE(Select(kLanczos3Filter, true, false, 0.5f, 1024, 1024, 512, 512) == Mode::BrightnessGuided);
	REQUIRE(Select(kBilinearFilter, false, false, 0.5f, 1024, 1024, 512, 512) == Mode::Bilinear);
	REQUIRE(Select(kLanczos3Filter, false, true, 0.5f, 1024, 1024, 512, 512) == Mode::Lanczos3);
}

TEST_CASE("NR RGB guidance requires reduced geometry and the qualified scale", "[nr]")
{
	REQUIRE(Select(kBilinearFilter, true, true, 0.25f, 1024, 1024, 256, 256) == Mode::RgbGuided);
	REQUIRE(Select(kBilinearFilter, true, true, 0.5f, 1080, 1200, 544, 600) == Mode::RgbGuided);
	REQUIRE(Select(kBilinearFilter, true, true, 0.75f, 1024, 1024, 768, 768) == Mode::Bilinear);
	REQUIRE(Select(kLanczos3Filter, true, true, 0.75f, 1024, 1024, 768, 768) == Mode::Lanczos3);
	REQUIRE(Select(kLanczos3Filter, true, true, 1.0f, 1024, 1024, 0, 0) == Mode::Native);
	REQUIRE(Select(kLanczos3Filter, true, true, 0.5f, 1024, 1024, 512, 1024) == Mode::Native);
	REQUIRE(Select(kLanczos3Filter, true, true, 0.5f, 1024, 1024, 1024, 512) == Mode::Native);
}

TEST_CASE("NR reconstruction rejects invalid effective scales and extents", "[nr]")
{
	for (const auto scale : { -1.0f, 0.0f, 1.0f, 2.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
		REQUIRE(Select(kBilinearFilter, true, true, scale, 1024, 1024, 512, 512) == Mode::Native);
	REQUIRE(Select(kBilinearFilter, true, true, 0.5f, 1024, 1024, 0, 512) == Mode::Native);
	REQUIRE(Select(kBilinearFilter, true, true, 0.5f, 1024, 1024, 512, 0) == Mode::Native);
	REQUIRE(Select(kBilinearFilter, true, true, 0.5f, 0, 1024, 512, 512) == Mode::Native);
}

TEST_CASE("NR unknown filters fall back to bilinear", "[nr]")
{
	for (const auto filter : { 2u, 3u, std::numeric_limits<uint32_t>::max() }) {
		REQUIRE(SanitizeFilter(filter) == kBilinearFilter);
		REQUIRE(BasicMode(filter) == Mode::Bilinear);
		REQUIRE(Select(filter, false, false, 0.5f, 1024, 1024, 512, 512) == Mode::Bilinear);
	}
	REQUIRE(SanitizeFilter(kLanczos3Filter) == kLanczos3Filter);
}
