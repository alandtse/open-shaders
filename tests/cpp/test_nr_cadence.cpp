// Unit tests for the Neural Rendering eye-stagger schedule: which eye runs the model each frame,
// when both must, and how the tuning bounds the gap mode. These exercise the production helpers only.

#include "Features/Upscaling/NeuralRendering/Cadence.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

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

TEST_CASE("Decide runs a one-view frame's model once per interval and reuses the gain between", "[nr]")
{
	for (const uint32_t interval : { 2u, 3u, 4u }) {
		NR::Cadence::State state;
		uint32_t evaluated = 0;
		for (uint64_t frame = 0; frame < 12; ++frame) {
			const NR::Cadence::Inputs inputs{ .enabled = true, .vr = false, .eyeCount = 1, .frame = frame, .invalidate = false, .interval = interval };
			const auto decision = NR::Cadence::Decide(inputs, state);
			// The first frame has nothing stored, so it runs; afterwards only multiples of the interval do.
			REQUIRE(decision.evaluate[0] == (frame % interval == 0));
			if (decision.evaluate[0]) {
				++evaluated;
				state.valid[0] = true;
			}
			REQUIRE(NR::Cadence::GapScale(frame, state.lastModelFrame[0]) <= static_cast<float>(interval));
		}
		REQUIRE(evaluated == (12 + interval - 1) / interval);
	}
}

TEST_CASE("Decide makes a one-view frame evaluate when its stored gain is missing or invalidated", "[nr]")
{
	NR::Cadence::State state;
	state.lastModelFrame = { 4, NR::Cadence::kNoModelFrame };
	state.valid = { false, false };
	const NR::Cadence::Inputs offTurn{ .enabled = true, .vr = false, .eyeCount = 1, .frame = 5, .invalidate = false, .interval = 3 };
	REQUIRE(NR::Cadence::Decide(offTurn, state).evaluate[0]);
	state.valid[0] = true;
	REQUIRE_FALSE(NR::Cadence::Decide({ .enabled = true, .vr = false, .eyeCount = 1, .frame = 4, .invalidate = false, .interval = 3 }, state).evaluate[0]);
	const NR::Cadence::Inputs reset{ .enabled = true, .vr = false, .eyeCount = 1, .frame = 5, .invalidate = true, .interval = 3 };
	REQUIRE(NR::Cadence::Decide(reset, state).evaluate[0]);
	REQUIRE_FALSE(state.valid[0]);
}

TEST_CASE("Decide runs the model off its turn on a high-motion one-view frame", "[nr]")
{
	NR::Cadence::State state;
	REQUIRE(NR::Cadence::Decide({ .enabled = true, .vr = false, .eyeCount = 1, .frame = 6, .invalidate = false, .interval = 3 }, state).evaluate[0]);
	state.valid[0] = true;
	REQUIRE_FALSE(NR::Cadence::Decide({ .enabled = true, .vr = false, .eyeCount = 1, .frame = 7, .invalidate = false, .interval = 3 }, state).evaluate[0]);
	state.valid[0] = true;
	const auto moving = NR::Cadence::Decide({ .enabled = true, .vr = false, .eyeCount = 1, .frame = 8, .invalidate = false, .interval = 3, .highMotion = true }, state);
	REQUIRE(moving.evaluate[0]);
	REQUIRE(state.lastModelFrame[0] == 8);
	REQUIRE_FALSE(state.valid[0]);
}

TEST_CASE("Decide leaves a one-view frame alone at interval 1 and on VR", "[nr]")
{
	NR::Cadence::State state = SteadyState(3);
	REQUIRE(NR::Cadence::Decide({ .enabled = true, .vr = false, .eyeCount = 1, .frame = 5, .invalidate = false, .interval = 1 }, state).evaluate[0]);
	state = SteadyState(3);
	REQUIRE(NR::Cadence::Decide({ .enabled = true, .vr = true, .eyeCount = 1, .frame = 5, .invalidate = false, .interval = 3 }, state).evaluate[0]);
}

TEST_CASE("GapScale is the frames since the last model run, and 1 when there is none", "[nr]")
{
	REQUIRE(NR::Cadence::GapScale(9, 7) == 2.0f);
	REQUIRE(NR::Cadence::GapScale(9, 6) == 3.0f);
	REQUIRE(NR::Cadence::GapScale(9, NR::Cadence::kNoModelFrame) == 1.0f);
	REQUIRE(NR::Cadence::GapScale(5, 9) == 1.0f);
}

TEST_CASE("Tuning bounds the flat reuse interval", "[nr]")
{
	NR::Tuning tuning;
	REQUIRE(tuning.flatReuseInterval == 1);
	tuning.flatReuseInterval = 0;
	tuning.Sanitize();
	REQUIRE(tuning.flatReuseInterval == 1);
	tuning.flatReuseInterval = 99;
	tuning.Sanitize();
	REQUIRE(tuning.flatReuseInterval == NR::Tuning::kMaxFlatReuseInterval);
	tuning.flatReuseMaxTurnDegrees = -3.0f;
	tuning.flatReuseMaxMoveUnits = std::numeric_limits<float>::quiet_NaN();
	tuning.Sanitize();
	REQUIRE(tuning.flatReuseMaxTurnDegrees == 0.0f);
	REQUIRE(tuning.flatReuseMaxMoveUnits == NR::Tuning::kDefaultFlatReuseMoveUnits);
}

TEST_CASE("IsHighMotion flags a turn or a move past its limit and honours a zero limit", "[nr]")
{
	const float still = 1.0f;
	const float twoDegrees = std::cos(2.0f * 3.14159265f / 180.0f);
	REQUIRE_FALSE(NR::Cadence::IsHighMotion(still, 0.0f, 1.0f, 12.0f));
	REQUIRE(NR::Cadence::IsHighMotion(twoDegrees, 0.0f, 1.0f, 12.0f));
	REQUIRE_FALSE(NR::Cadence::IsHighMotion(twoDegrees, 0.0f, 3.0f, 12.0f));
	REQUIRE(NR::Cadence::IsHighMotion(still, 13.0f, 1.0f, 12.0f));
	REQUIRE_FALSE(NR::Cadence::IsHighMotion(still, 11.0f, 1.0f, 12.0f));
	REQUIRE_FALSE(NR::Cadence::IsHighMotion(twoDegrees, 0.0f, 0.0f, 12.0f));
	REQUIRE_FALSE(NR::Cadence::IsHighMotion(still, 500.0f, 1.0f, 0.0f));
}

TEST_CASE("IsHighMotion clamps an out-of-range dot product and treats a non-finite sample as motion", "[nr]")
{
	REQUIRE_FALSE(NR::Cadence::IsHighMotion(1.0001f, 0.0f, 1.0f, 12.0f));
	REQUIRE(NR::Cadence::IsHighMotion(-1.0001f, 0.0f, 1.0f, 12.0f));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE(NR::Cadence::IsHighMotion(nan, 0.0f, 1.0f, 12.0f));
	REQUIRE(NR::Cadence::IsHighMotion(1.0f, nan, 1.0f, 12.0f));
	REQUIRE(NR::Cadence::IsHighMotion(1.0f, std::numeric_limits<float>::infinity(), 0.0f, 0.0f));
}
