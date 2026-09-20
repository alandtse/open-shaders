#include "Utils/VersionGate.h"

#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

namespace
{
	// REL::Version orders lexicographically over this same array and cannot be included in this target.
	using Version = std::array<std::uint16_t, 4>;

	constexpr Version minimum{ 2, 0, 0, 0 };
}

TEST_CASE("Version gate blocks versions below the minimum", "[versiongate]")
{
	REQUIRE(Util::IsBelowMinimum(std::optional<Version>{ Version{ 1, 9, 9, 9 } }, minimum));
	REQUIRE(Util::IsBelowMinimum(std::optional<Version>{ Version{ 0, 0, 0, 0 } }, minimum));
}

TEST_CASE("Version gate accepts the minimum and newer versions", "[versiongate]")
{
	REQUIRE_FALSE(Util::IsBelowMinimum(std::optional<Version>{ Version{ 2, 0, 0, 0 } }, minimum));
	REQUIRE_FALSE(Util::IsBelowMinimum(std::optional<Version>{ Version{ 2, 0, 0, 1 } }, minimum));
	REQUIRE_FALSE(Util::IsBelowMinimum(std::optional<Version>{ Version{ 3, 0, 0, 0 } }, minimum));
}

TEST_CASE("Version gate never blocks an unreadable version", "[versiongate]")
{
	REQUIRE_FALSE(Util::IsBelowMinimum(std::optional<Version>{}, minimum));
}
