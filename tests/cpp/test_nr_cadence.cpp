// Unit tests for the Neural Rendering eye-stagger schedule: which eye runs the model each frame,
// when both must, and how the tuning bounds the gap mode. These exercise the production helpers only.

#include "Features/Upscaling/NeuralRendering/Cadence.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace
{
	/** @brief A VR frame the developer has switched the stagger on for. */
	NR::Cadence::Inputs StaggerFrame(uint64_t frame)
	{
		return { .enabled = true, .vr = true, .eyeCount = 2, .frame = frame, .invalidate = false };
	}

	/** @brief A state whose eyes both hold a gain stored on the frame before, so neither bootstraps. */
	NR::Cadence::State SteadyState(uint64_t frame)
	{
		NR::Cadence::State state;
		state.lastModelFrame = { frame, frame };
		state.valid = { true, true };
		return state;
	}
}

TEST_CASE("Decide gives every eye the model when the stagger is off", "[nr]")
{
	const std::array<NR::Cadence::Inputs, 3> off{
		NR::Cadence::Inputs{ .enabled = false, .vr = true, .eyeCount = 2, .frame = 4 },
		NR::Cadence::Inputs{ .enabled = true, .vr = false, .eyeCount = 2, .frame = 4 },
		NR::Cadence::Inputs{ .enabled = true, .vr = true, .eyeCount = 1, .frame = 4 },
	};
	for (const auto& inputs : off) {
		auto state = SteadyState(3);
		const auto decision = NR::Cadence::Decide(inputs, state);
		REQUIRE(decision.evaluate[0]);
		REQUIRE(decision.evaluate[1]);
		REQUIRE_FALSE(state.valid[0]);
		REQUIRE_FALSE(state.valid[1]);
	}
}

TEST_CASE("Decide alternates the model between the eyes on a two-eye frame", "[nr]")
{
	auto state = SteadyState(0);
	for (uint64_t frame = 1; frame <= 10; ++frame) {
		const auto decision = NR::Cadence::Decide(StaggerFrame(frame), state);
		REQUIRE(decision.evaluate[0] != decision.evaluate[1]);
		state.valid[frame % 2] = true;
		REQUIRE(decision.evaluate[frame % 2] == true);
		REQUIRE(decision.evaluate[frame % 2 == 0 ? 1 : 0] == false);
		REQUIRE(state.lastModelFrame[frame % 2] == frame);
	}
}

TEST_CASE("Decide makes an eye whose gain is not stored yet evaluate off its turn", "[nr]")
{
	auto state = SteadyState(0);
	state.valid[0] = false;
	// Frame 1 is eye 1's; eye 0 has nothing to reuse, so both run.
	const auto decision = NR::Cadence::Decide(StaggerFrame(1), state);
	REQUIRE(decision.evaluate[0]);
	REQUIRE(decision.evaluate[1]);
	REQUIRE_FALSE(state.valid[0]);
	REQUIRE_FALSE(state.valid[1]);
}

TEST_CASE("Decide bootstraps both eyes on the first staggered frame", "[nr]")
{
	NR::Cadence::State state;
	const auto decision = NR::Cadence::Decide(StaggerFrame(0), state);
	REQUIRE(decision.evaluate[0]);
	REQUIRE(decision.evaluate[1]);
}

TEST_CASE("Decide hands both eyes the model and drops the gains when the schedule is invalidated", "[nr]")
{
	auto state = SteadyState(8);
	auto inputs = StaggerFrame(9);
	inputs.invalidate = true;
	const auto decision = NR::Cadence::Decide(inputs, state);
	REQUIRE(decision.evaluate[0]);
	REQUIRE(decision.evaluate[1]);
	REQUIRE_FALSE(state.valid[0]);
	REQUIRE_FALSE(state.valid[1]);
}

TEST_CASE("Decide keeps the frame the eye last ran the model on", "[nr]")
{
	auto state = SteadyState(0);
	NR::Cadence::Decide(StaggerFrame(1), state);
	state.valid[1] = true;
	REQUIRE(state.lastModelFrame[1] == 1);
	REQUIRE(state.lastModelFrame[0] == 0);
	NR::Cadence::Decide(StaggerFrame(2), state);
	REQUIRE(state.lastModelFrame[0] == 2);
	REQUIRE(state.lastModelFrame[1] == 1);
}

TEST_CASE("Tuning clamps the eye-stagger gap mode", "[nr]")
{
	NR::Tuning tuning;
	REQUIRE(tuning.skipFrameGapMode == 0);
	REQUIRE_FALSE(tuning.skipFrameReuse);
	tuning.skipFrameGapMode = NR::Tuning::kMaxSkipFrameGapMode;
	tuning.Sanitize();
	REQUIRE(tuning.skipFrameGapMode == NR::Tuning::kMaxSkipFrameGapMode);
	tuning.skipFrameGapMode = NR::Tuning::kMaxSkipFrameGapMode + 1;
	tuning.Sanitize();
	REQUIRE(tuning.skipFrameGapMode == NR::Tuning::kMaxSkipFrameGapMode);
	tuning.skipFrameGapMode = 0xFFFFFFFFu;
	tuning.Sanitize();
	REQUIRE(tuning.skipFrameGapMode == NR::Tuning::kMaxSkipFrameGapMode);
}
