// Unit tests for Util string/path helpers (src/Utils/StringUtils.h).
//
// Header-only, pure logic. No game/D3D/ImGui dependency.

#include "Utils/StringUtils.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ToLowerAscii folds only ASCII letters", "[stringutils]")
{
	REQUIRE(Util::ToLowerAscii("MixedCASE") == "mixedcase");
	REQUIRE(Util::ToLowerAscii("already_lower-123") == "already_lower-123");
	REQUIRE(Util::ToLowerAscii("") == "");
	// Non-letters (digits, punctuation) pass through unchanged.
	REQUIRE(Util::ToLowerAscii("MX_LightGlow01.DDS") == "mx_lightglow01.dds");
}

TEST_CASE("GetLowercaseStem strips directory, extension, and lowercases", "[stringutils]")
{
	REQUIRE(Util::GetLowercaseStem("Data\\ParticleLights\\CandleGlow01.dds", ".dds") == "candleglow01");
	REQUIRE(Util::GetLowercaseStem("Data/ParticleLights/CandleGlow01.dds", ".dds") == "candleglow01");
	// Bare filename with no directory component.
	REQUIRE(Util::GetLowercaseStem("MX_LightGlow01.dds", ".dds") == "mx_lightglow01");
}

TEST_CASE("GetLowercaseStem matches the extension case-insensitively", "[stringutils]")
{
	REQUIRE(Util::GetLowercaseStem("foo.DDS", ".dds") == "foo");
	REQUIRE(Util::GetLowercaseStem("foo.DdS", ".dds") == "foo");
	REQUIRE(Util::GetLowercaseStem("bar.INI", ".ini") == "bar");
}

TEST_CASE("GetLowercaseStem rejects mismatched or degenerate paths", "[stringutils]")
{
	// Wrong extension.
	REQUIRE(Util::GetLowercaseStem("foo.ini", ".dds") == std::nullopt);
	// Empty path.
	REQUIRE(Util::GetLowercaseStem("", ".dds") == std::nullopt);
	// Extension-only filename leaves an empty stem.
	REQUIRE(Util::GetLowercaseStem(".dds", ".dds") == std::nullopt);
	REQUIRE(Util::GetLowercaseStem("Data\\Lights\\.dds", ".dds") == std::nullopt);
	// No extension at all.
	REQUIRE(Util::GetLowercaseStem("foo", ".dds") == std::nullopt);
}
