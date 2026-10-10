// Unit tests for the VR eye evaluation mode: that the menu's single choice maps onto the three
// stored flags and back, and that a hand-edited config resolves by the precedence the pass applies.

#include "Features/Upscaling/NeuralRendering/EyeMode.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("The default tuning evaluates the eyes in parallel", "[nr]")
{
	REQUIRE(NR::GetEyeMode(NR::Tuning{}) == NR::EyeMode::kParallel);
}

TEST_CASE("Selecting each mode round-trips and sets exactly one flag", "[nr]")
{
	for (uint32_t i = 0; i < NR::kEyeModeCount; ++i) {
		const auto mode = static_cast<NR::EyeMode>(i);
		NR::Tuning tuning;
		NR::SetEyeMode(tuning, mode);
		REQUIRE(NR::GetEyeMode(tuning) == mode);
		const int set = (tuning.parallelEyes ? 1 : 0) + (tuning.sbsEvaluate ? 1 : 0) + (tuning.skipFrameReuse ? 1 : 0);
		REQUIRE(set == (mode == NR::EyeMode::kSeparate ? 0 : 1));
	}
}

TEST_CASE("Switching mode clears the flags of the previous one", "[nr]")
{
	NR::Tuning tuning;
	NR::SetEyeMode(tuning, NR::EyeMode::kSideBySide);
	NR::SetEyeMode(tuning, NR::EyeMode::kAlternate);
	REQUIRE_FALSE(tuning.sbsEvaluate);
	REQUIRE(tuning.skipFrameReuse);
	NR::SetEyeMode(tuning, NR::EyeMode::kSeparate);
	REQUIRE_FALSE(tuning.skipFrameReuse);
	REQUIRE_FALSE(tuning.parallelEyes);
}

TEST_CASE("Conflicting flags in a hand-edited config resolve by the pass's precedence", "[nr]")
{
	NR::Tuning tuning;
	tuning.parallelEyes = true;
	tuning.skipFrameReuse = true;
	REQUIRE(NR::GetEyeMode(tuning) == NR::EyeMode::kAlternate);
	tuning.sbsEvaluate = true;
	REQUIRE(NR::GetEyeMode(tuning) == NR::EyeMode::kSideBySide);
}
