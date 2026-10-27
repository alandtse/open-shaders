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

TEST_CASE("A tab label matches the hidden id with or without the marker", "[tablabel]")
{
	const std::string_view alpha = "Neural Rendering [ALPHA]###NeuralRendering";
	REQUIRE(Util::TabLabelMatches(alpha, "NeuralRendering"));
	REQUIRE(Util::TabLabelMatches(alpha, "neuralrendering"));
	REQUIRE(Util::TabLabelMatches(alpha, "###NeuralRendering"));
	REQUIRE_FALSE(Util::TabLabelMatches(alpha, "Neural"));
	REQUIRE_FALSE(Util::TabLabelMatches("Label##extra", "extra"));
}

TEST_CASE("A tab label rejects degenerate input", "[tablabel]")
{
	REQUIRE_FALSE(Util::TabLabelMatches("", "Upscaling"));
	REQUIRE_FALSE(Util::TabLabelMatches("", ""));
	REQUIRE_FALSE(Util::TabLabelMatches("Upscaling", "###"));
	REQUIRE_FALSE(Util::TabLabelMatches("Label###", "###"));
	REQUIRE_FALSE(Util::TabLabelMatches("Upscaling", " Upscaling"));
	REQUIRE_FALSE(Util::TabLabelMatches("Upscaling", "Upscaling "));
	REQUIRE_FALSE(Util::TabLabelMatches("Upscaling", "Neural Rendering###NeuralRendering"));
}

TEST_CASE("A tab label handles brackets in the visible text", "[tablabel]")
{
	REQUIRE(Util::TabLabelMatches("Label [", "label"));
	REQUIRE(Util::TabLabelMatches("Label [x] more", "Label"));
	REQUIRE(Util::TabLabelMatches("[x] Tab", "[x] Tab"));
	REQUIRE_FALSE(Util::TabLabelMatches("[x] Tab", "Tab"));
}
