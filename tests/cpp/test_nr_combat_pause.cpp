#include "Features/Upscaling/NeuralRendering/CombatPausePolicy.h"
#include "Features/Upscaling/NeuralRendering/CombatState.h"
#include "Features/Upscaling/NeuralRendering/Lifecycle.h"

#include <catch2/catch_test_macros.hpp>

#include <latch>
#include <limits>
#include <thread>

using NR::CombatPausePhase;
using NR::CombatPausePolicy;

TEST_CASE("Combat samples publish unavailable state instead of retaining a peaceful player", "[nr][combat]")
{
	NR::CombatState state;
	REQUIRE_FALSE(state.GetSnapshot().inCombat.has_value());
	REQUIRE(state.GetSnapshot().gamePaused);
	state.Publish(false, false);
	const auto peaceful = state.GetSnapshot();
	state.Publish(std::nullopt, true);
	REQUIRE_FALSE(state.GetSnapshot().inCombat.has_value());
	REQUIRE(state.GetSnapshot().gamePaused);
	REQUIRE(peaceful.inCombat == false);
	REQUIRE_FALSE(peaceful.gamePaused);
}

TEST_CASE("A published combat change reaches both eyes on the next render frame", "[nr][combat]")
{
	NR::CombatState state;
	CombatPausePolicy policy;
	const auto evaluate = [&](std::uint32_t frame, std::uint64_t nowMs) {
		const auto sample = state.GetSnapshot();
		return policy.Update(frame, true, nowMs, sample.inCombat, sample.gamePaused);
	};
	state.Publish(false, false);
	REQUIRE(evaluate(1, 0).phase == CombatPausePhase::Ready);
	state.Publish(true, false);
	REQUIRE(evaluate(1, 1).phase == CombatPausePhase::Ready);
	REQUIRE(evaluate(2, 10).phase == CombatPausePhase::Combat);
	state.Publish(false, false);
	REQUIRE(evaluate(2, 20).phase == CombatPausePhase::Combat);
	REQUIRE(evaluate(3, 30).remainingMs == NR::kCombatResumeDelayMs);
	state.Publish(false, true);
	REQUIRE(evaluate(4, 40).phase == CombatPausePhase::Unavailable);
	state.Publish(false, false);
	REQUIRE(evaluate(5, 50).remainingMs == NR::kCombatResumeDelayMs);
	REQUIRE(evaluate(6, 3050).phase == CombatPausePhase::Ready);
}

TEST_CASE("Concurrent combat publication keeps player and pause state coherent", "[nr][combat]")
{
	NR::CombatState state;
	state.Publish(false, false);
	std::latch start{ 2 };
	std::jthread publisher([&] {
		start.arrive_and_wait();
		for (int i = 0; i < 10000; ++i)
			state.Publish(i % 2 != 0, i % 2 != 0);
	});
	start.arrive_and_wait();
	bool coherent = true;
	for (int i = 0; i < 10000; ++i) {
		const auto sample = state.GetSnapshot();
		coherent &= sample.inCombat.has_value() && *sample.inCombat == sample.gamePaused;
	}
	REQUIRE(coherent);
}

TEST_CASE("Combat pausing is opt-in and peaceful play needs no startup delay", "[nr][combat]")
{
	CombatPausePolicy policy;
	REQUIRE_FALSE(policy.GetSnapshot().sampled);
	REQUIRE_FALSE(policy.GetSnapshot().IsPaused());
	REQUIRE(policy.Update(1, false, 0, true).phase == CombatPausePhase::Disabled);
	REQUIRE(policy.Update(2, true, 10, false).phase == CombatPausePhase::Ready);
	REQUIRE_FALSE(policy.Update(3, true, 20, false).IsPaused());
	REQUIRE(policy.Update(4, true, 30, true).phase == CombatPausePhase::Combat);
	REQUIRE(policy.Update(5, false, 40, std::nullopt).phase == CombatPausePhase::Disabled);
}

TEST_CASE("Every hook and eye receives the first combat decision for its frame", "[nr][combat]")
{
	CombatPausePolicy policy;
	const auto first = policy.Update(40, true, 100, true);
	const auto duplicate = policy.Update(40, false, 5000, false);
	REQUIRE(policy.HasFrame(40));
	REQUIRE_FALSE(policy.HasFrame(41));
	REQUIRE(duplicate.phase == first.phase);
	REQUIRE(duplicate.frame == first.frame);
	REQUIRE(duplicate.remainingMs == first.remainingMs);
	REQUIRE(duplicate.IsPaused());
	REQUIRE(policy.Update(41, true, 5100, false).remainingMs == 3000);
}

TEST_CASE("NR resumes only after three continuous peaceful seconds", "[nr][combat]")
{
	CombatPausePolicy policy;
	REQUIRE(policy.Update(1, true, 0, true).IsPaused());
	const auto begin = policy.Update(2, true, 100, false);
	REQUIRE(begin.phase == CombatPausePhase::Cooldown);
	REQUIRE(begin.remainingMs == 3000);
	REQUIRE(policy.Update(3, true, 3099, false).remainingMs == 1);
	const auto resumed = policy.Update(4, true, 3100, false);
	REQUIRE(resumed.phase == CombatPausePhase::Ready);
	REQUIRE_FALSE(resumed.IsPaused());
	REQUIRE(resumed.remainingMs == 0);
}

TEST_CASE("Combat restarting discards the partial peaceful interval", "[nr][combat]")
{
	CombatPausePolicy policy;
	policy.Update(1, true, 0, true);
	policy.Update(2, true, 100, false);
	REQUIRE(policy.Update(3, true, 2000, false).remainingMs == 1100);
	REQUIRE(policy.Update(4, true, 2100, true).phase == CombatPausePhase::Combat);
	REQUIRE(policy.Update(5, true, 2200, false).remainingMs == 3000);
	REQUIRE(policy.Update(6, true, 5199, false).IsPaused());
	REQUIRE_FALSE(policy.Update(7, true, 5200, false).IsPaused());
}

TEST_CASE("Peaceful menus preserve NR but interrupt pending combat recovery", "[nr][combat]")
{
	CombatPausePolicy peaceful;
	REQUIRE_FALSE(peaceful.Update(1, true, 0, false).IsPaused());
	REQUIRE_FALSE(peaceful.Update(2, true, 100, false, true).IsPaused());
	REQUIRE_FALSE(peaceful.Update(3, true, 10000, false, true).IsPaused());
	REQUIRE_FALSE(peaceful.Update(4, true, 10010, false).IsPaused());

	CombatPausePolicy recovering;
	recovering.Update(1, true, 0, true);
	recovering.Update(2, true, 100, false);
	REQUIRE(recovering.Update(3, true, 1100, false).remainingMs == 2000);
	REQUIRE(recovering.Update(4, true, 1200, false, true).phase == CombatPausePhase::Unavailable);
	REQUIRE(recovering.Update(5, true, 10000, false, true).IsPaused());
	REQUIRE(recovering.Update(6, true, 10010, false).remainingMs == 3000);
	REQUIRE_FALSE(recovering.Update(7, true, 13010, false).IsPaused());
}

TEST_CASE("Unavailable player or world state requires a fresh peaceful interval", "[nr][combat]")
{
	CombatPausePolicy policy;
	REQUIRE(policy.Update(1, true, 0, std::nullopt).phase == CombatPausePhase::Unavailable);
	REQUIRE(policy.Update(2, true, 10, false).remainingMs == 3000);
	REQUIRE(policy.Update(3, true, 1000, std::nullopt).phase == CombatPausePhase::Unavailable);
	REQUIRE(policy.Update(4, true, 5000, false).remainingMs == 3000);
	REQUIRE_FALSE(policy.Update(5, true, 8000, false).IsPaused());
}

TEST_CASE("A frame gap or clock reversal while ready does not pause", "[nr][combat]")
{
	CombatPausePolicy policy;
	policy.Update(1, true, 10000, false);
	REQUIRE(policy.Update(3, true, 10010, false).phase == CombatPausePhase::Ready);
	REQUIRE(policy.Update(4, true, 1000, false).phase == CombatPausePhase::Ready);
}

TEST_CASE("A frame gap or clock reversal while paused restarts the cooldown", "[nr][combat]")
{
	CombatPausePolicy policy;
	policy.Update(1, true, 10000, false);
	REQUIRE(policy.Update(2, true, 10010, true).phase == CombatPausePhase::Combat);
	REQUIRE(policy.Update(4, true, 10020, false).remainingMs == NR::kCombatResumeDelayMs);
	REQUIRE(policy.Update(5, true, 13019, false).phase == CombatPausePhase::Cooldown);
	REQUIRE(policy.Update(6, true, 13020, false).phase == CombatPausePhase::Ready);

	REQUIRE(policy.Update(7, true, 13030, true).phase == CombatPausePhase::Combat);
	REQUIRE(policy.Update(8, true, 13040, false).phase == CombatPausePhase::Cooldown);
	REQUIRE(policy.Update(9, true, 1000, false).remainingMs == NR::kCombatResumeDelayMs);
}

TEST_CASE("The frame counter wraps without fabricating a combat pause", "[nr][combat]")
{
	CombatPausePolicy policy;
	const auto last = std::numeric_limits<std::uint32_t>::max();
	REQUIRE_FALSE(policy.Update(last, true, 10, false).IsPaused());
	REQUIRE_FALSE(policy.Update(0, true, 20, false).IsPaused());
	REQUIRE_FALSE(policy.Update(1, true, 30, false).IsPaused());
}

TEST_CASE("Combat suspension retains resources and leaves independent blockers intact", "[nr][combat]")
{
	CombatPausePolicy policy;
	NR::FrameInputs inputs{ .enabled = true, .worldRendered = true, .ready = true, .lastFrame = 0, .frameCount = 1 };
	inputs.suspended = policy.Update(1, true, 0, true).IsPaused();
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::Suspend);
	inputs.retryRequested = true;
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::Suspend);
	inputs.enabled = false;
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::ReleasePassResources);
	inputs.enabled = true;
	policy.Update(2, true, 100, false);
	inputs.suspended = policy.Update(3, true, 3100, false).IsPaused();
	inputs.frameCount = 3;
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::Run);
	inputs.failed = true;
	inputs.retryRequested = false;
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::SkipLatched);
	inputs.failed = false;
	inputs.worldRendered = false;
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::SkipNoWorld);
	inputs.suspended = true;
	REQUIRE(NR::DecideFrame(inputs) == NR::FrameAction::Suspend);
}
