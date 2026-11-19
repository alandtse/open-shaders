#include "Features/Upscaling/NeuralRendering/ModelReconstruction.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace NR::Reconstruction;

TEST_CASE("NR reconstruction retains brightness guidance and bilinear", "[nr]")
{
	REQUIRE(Select(true, false, 0.75f, 1024, 1024, 768, 768) == Mode::BrightnessGuided);
	REQUIRE(Select(true, false, 0.5f, 1024, 1024, 512, 512) == Mode::BrightnessGuided);
	REQUIRE(Select(false, false, 0.5f, 1024, 1024, 512, 512) == Mode::Bilinear);
	REQUIRE(Select(false, true, 0.5f, 1024, 1024, 512, 512) == Mode::Bilinear);
}

TEST_CASE("NR RGB guidance follows the actual reduced grid at every scale", "[nr]")
{
	REQUIRE(Select(true, true, 0.25f, 1024, 1024, 256, 256) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 0.5f, 1080, 1200, 544, 600) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 0.75f, 1024, 1024, 768, 768) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 0.99f, 1024, 1024, 1016, 1016) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 1.0f, 1024, 1024, 768, 768) == Mode::RgbGuided);
	REQUIRE(Select(true, false, 1.0f, 1024, 1024, 768, 768) == Mode::BrightnessGuided);
	REQUIRE(Select(false, true, 1.0f, 1024, 1024, 768, 768) == Mode::Bilinear);
}

TEST_CASE("NR RGB guidance accepts a rounded unchanged axis", "[nr]")
{
	REQUIRE(Select(true, true, 0.99f, 80, 72, 72, 72) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 0.99f, 72, 80, 72, 72) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 1.0f, 80, 72, 72, 72) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 1.0f, 72, 80, 72, 72) == Mode::RgbGuided);
	REQUIRE(Select(true, true, 0.99f, 72, 72, 72, 72) == Mode::Native);
	REQUIRE(Select(true, true, 1.0f, 1024, 1024, 1024, 1024) == Mode::Native);
}

TEST_CASE("NR reconstruction rejects invalid scales and model extents", "[nr]")
{
	for (const auto scale : { -1.0f, 0.0f, 1.01f, 2.0f, std::numeric_limits<float>::infinity(),
			 -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
		REQUIRE(Select(true, true, scale, 1024, 1024, 512, 512) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 1024, 1024, 0, 512) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 1024, 1024, 512, 0) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 1024, 1024, 0, 0) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 0, 1024, 512, 512) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 1024, 0, 512, 512) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 1024, 1024, 1032, 512) == Mode::Native);
	REQUIRE(Select(true, true, 0.5f, 1024, 1024, 512, 1032) == Mode::Native);
}
