// Unit tests for the value mapping behind Util::Combo and the other controls that edit a setting
// through a converted copy: what a dropdown shows, and when the copy is written back.

#include "Utils/ControlValue.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
	enum class Scope : uint32_t
	{
		kSame,
		kEverything,
		kCharacters
	};
}

TEST_CASE("A dropdown shows the item matching the stored value", "[ui][control]")
{
	REQUIRE(Util::ComboShownIndex(uint32_t{ 0 }, 3) == 0);
	REQUIRE(Util::ComboShownIndex(uint32_t{ 2 }, 3) == 2);
	REQUIRE(Util::ComboShownIndex(Scope::kEverything, 3) == 1);
}

TEST_CASE("A stored value outside the list shows the nearest item", "[ui][control]")
{
	REQUIRE(Util::ComboShownIndex(uint32_t{ 7 }, 3) == 2);
	REQUIRE(Util::ComboShownIndex(-4, 3) == 0);
	REQUIRE(Util::ComboShownIndex(uint32_t{ 5 }, 0) == 0);
}

TEST_CASE("A changed copy is stored in the setting's own type", "[ui][control]")
{
	uint32_t number = 0;
	REQUIRE(Util::CommitDisplayedValue(number, 0, 1));
	REQUIRE(number == 1);

	Scope scope = Scope::kSame;
	REQUIRE(Util::CommitDisplayedValue(scope, 0, 2));
	REQUIRE(scope == Scope::kCharacters);
}

TEST_CASE("An unchanged copy leaves the stored value alone, so a reset made elsewhere survives", "[ui][control]")
{
	// The control showed 2; a reset to default then set the stored value to 0 while it still shows 2.
	uint32_t stored = 0;
	REQUIRE_FALSE(Util::CommitDisplayedValue(stored, 2, 2));
	REQUIRE(stored == 0);

	bool enabled = true;
	REQUIRE_FALSE(Util::CommitDisplayedValue(enabled, false, false, [](bool shown) { return !shown; }));
	REQUIRE(enabled);
}

TEST_CASE("An inverted copy is stored inverted", "[ui][control]")
{
	bool disabled = false;
	// Shown as "enabled" (true); the user unticks it.
	REQUIRE(Util::CommitDisplayedValue(disabled, true, false, [](bool shown) { return !shown; }));
	REQUIRE(disabled);
}

TEST_CASE("A converted float is not rewritten when untouched", "[ui][control]")
{
	float stored = 0.37f;
	const float shown = stored * 1e2f;
	REQUIRE_FALSE(Util::CommitDisplayedValue(stored, shown, shown, [](float percentage) { return percentage * 1e-2f; }));
	REQUIRE(stored == 0.37f);

	REQUIRE(Util::CommitDisplayedValue(stored, shown, 50.0f, [](float percentage) { return percentage * 1e-2f; }));
	REQUIRE(stored == 50.0f * 1e-2f);
}

TEST_CASE("Picking away from an out-of-range value's shown item stores the pick", "[ui][control]")
{
	uint32_t stored = 7;
	const int shown = Util::ComboShownIndex(stored, 3);
	REQUIRE(Util::CommitDisplayedValue(stored, shown, 1));
	REQUIRE(stored == 1);
}
