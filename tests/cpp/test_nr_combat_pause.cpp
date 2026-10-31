#include "Features/Upscaling/NeuralRendering/CombatPausePolicy.h"
#include "Features/Upscaling/NeuralRendering/Lifecycle.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

using NR::CombatPausePhase;
using NR::CombatPausePolicy;

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

TEST_CASE("Frame discontinuities and clock reversal restart recovery", "[nr][combat]")
{
	CombatPausePolicy policy;
	policy.Update(1, true, 10000, false);
	REQUIRE(policy.Update(3, true, 10010, false).remainingMs == 3000);
	REQUIRE(policy.Update(4, true, 11010, false).remainingMs == 2000);
	REQUIRE(policy.Update(5, true, 1000, false).remainingMs == 3000);
	REQUIRE(policy.Update(6, true, 3999, false).IsPaused());
	REQUIRE_FALSE(policy.Update(7, true, 4000, false).IsPaused());
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

TEST_CASE("Combat diagnostic phase identifiers are stable", "[nr][combat]")
{
	REQUIRE(std::string(NR::CombatPausePhaseName(CombatPausePhase::Disabled)) == "disabled");
	REQUIRE(std::string(NR::CombatPausePhaseName(CombatPausePhase::Ready)) == "ready");
	REQUIRE(std::string(NR::CombatPausePhaseName(CombatPausePhase::Combat)) == "combat");
	REQUIRE(std::string(NR::CombatPausePhaseName(CombatPausePhase::Cooldown)) == "cooldown");
	REQUIRE(std::string(NR::CombatPausePhaseName(CombatPausePhase::Unavailable)) == "unavailable");
}
