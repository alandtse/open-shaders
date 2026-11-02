// Unit tests for the VR menu's ImGuiVRHelper combo decisions (src/Features/VR/HelperCombo.h).

#include "Features/VR/HelperCombo.h"

#include <catch2/catch_test_macros.hpp>

using VRHelperCombo::Action;
using VRHelperCombo::DecideOverlayToggle;
using VRHelperCombo::Edges;

namespace
{
	constexpr Edges kOpen{ true, false };
	constexpr Edges kClose{ false, true };
	constexpr Edges kIdle{};
}

TEST_CASE("Focused panel press toggles the overlay", "[vr][combo]")
{
	REQUIRE(DecideOverlayToggle(true, true, kOpen) == Action::Show);
	REQUIRE(DecideOverlayToggle(true, true, kClose) == Action::Hide);
}

TEST_CASE("A press off the panel never toggles the overlay", "[vr][combo]")
{
	REQUIRE(DecideOverlayToggle(true, false, kOpen) == Action::None);
	REQUIRE(DecideOverlayToggle(true, false, kClose) == Action::None);
}

TEST_CASE("A press without menu focus never toggles the overlay", "[vr][combo]")
{
	REQUIRE(DecideOverlayToggle(false, true, kOpen) == Action::None);
	REQUIRE(DecideOverlayToggle(false, true, kClose) == Action::None);
}

TEST_CASE("No edge means no action", "[vr][combo]")
{
	REQUIRE(DecideOverlayToggle(true, true, kIdle) == Action::None);
}

TEST_CASE("Open wins when both chords report the same frame", "[vr][combo]")
{
	REQUIRE(DecideOverlayToggle(true, true, { true, true }) == Action::Show);
}
