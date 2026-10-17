// Unit tests for the tab-label matcher behind the devbench menu call's tab argument.

#include "Utils/TabLabel.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("A tab label matches the visible text ignoring case", "[tablabel]")
{
	REQUIRE(Util::TabLabelMatches("Upscaling", "Upscaling"));
	REQUIRE(Util::TabLabelMatches("Upscaling", "upscaling"));
	REQUIRE(Util::TabLabelMatches("Frame Generation", "FRAME GENERATION"));
	REQUIRE_FALSE(Util::TabLabelMatches("NVIDIA Reflex", "reflex"));
	REQUIRE_FALSE(Util::TabLabelMatches("Upscaling", "Up"));
	REQUIRE_FALSE(Util::TabLabelMatches("Upscaling", ""));
}

TEST_CASE("A tab label ignores a release-stage suffix and an id", "[tablabel]")
{
	const std::string_view alpha = "Neural Rendering [ALPHA]###NeuralRendering";
	REQUIRE(Util::TabLabelMatches(alpha, "Neural Rendering"));
	REQUIRE(Util::TabLabelMatches(alpha, "neural rendering"));
	REQUIRE_FALSE(Util::TabLabelMatches(alpha, "Neural Rendering [ALPHA]"));
	REQUIRE(Util::TabLabelMatches("Label##extra", "label"));
}

TEST_CASE("A tab label matches the hidden id exactly", "[tablabel]")
{
	const std::string_view alpha = "Neural Rendering [ALPHA]###NeuralRendering";
	REQUIRE(Util::TabLabelMatches(alpha, "NeuralRendering"));
	REQUIRE_FALSE(Util::TabLabelMatches(alpha, "neuralrendering"));
	REQUIRE_FALSE(Util::TabLabelMatches(alpha, "Neural"));
}
