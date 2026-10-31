// Unit tests for the index mapping behind Util::Combo: what a dropdown shows for a stored value, and
// when its pick is written back.

#include "Utils/ComboIndex.h"

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

TEST_CASE("A dropdown shows the item matching the stored value", "[ui][combo]")
{
	REQUIRE(Util::ComboShownIndex(uint32_t{ 0 }, 3) == 0);
	REQUIRE(Util::ComboShownIndex(uint32_t{ 2 }, 3) == 2);
	REQUIRE(Util::ComboShownIndex(Scope::kEverything, 3) == 1);
}

TEST_CASE("A stored value outside the list shows the nearest item", "[ui][combo]")
{
	REQUIRE(Util::ComboShownIndex(uint32_t{ 7 }, 3) == 2);
	REQUIRE(Util::ComboShownIndex(-4, 3) == 0);
	REQUIRE(Util::ComboShownIndex(uint32_t{ 5 }, 0) == 0);
}

TEST_CASE("A changed pick is stored in the setting's own type", "[ui][combo]")
{
	uint32_t number = 0;
	REQUIRE(Util::CommitComboIndex(number, 0, 1));
	REQUIRE(number == 1);

	Scope scope = Scope::kSame;
	REQUIRE(Util::CommitComboIndex(scope, 0, 2));
	REQUIRE(scope == Scope::kCharacters);
}

TEST_CASE("An unchanged pick leaves the stored value alone, so a reset made elsewhere survives", "[ui][combo]")
{
	// The dropdown showed item 2; a reset to default then set the stored value to 0 while it still shows item 2.
	uint32_t stored = 0;
	REQUIRE_FALSE(Util::CommitComboIndex(stored, 2, 2));
	REQUIRE(stored == 0);
}

TEST_CASE("Picking the item an out-of-range value was shown as stores it", "[ui][combo]")
{
	// Shown as the last item; choosing another item then changing back is a change from what was shown.
	uint32_t stored = 7;
	const int shown = Util::ComboShownIndex(stored, 3);
	REQUIRE(Util::CommitComboIndex(stored, shown, 1));
	REQUIRE(stored == 1);
}
