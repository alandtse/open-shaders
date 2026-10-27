// Unit tests for Util string/path helpers (src/Utils/StringUtils.h).
//
// Header-only, pure logic. No game/D3D/ImGui dependency.

#include "Utils/StringUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <sstream>

TEST_CASE("Config names cannot navigate between directories", "[stringutils]")
{
	for (std::string_view name : { "Tamriel", "WindhelmWorld", "Interior.Name" })
		REQUIRE(Util::IsSinglePathComponent(name));
	for (std::string_view name : { "", ".", "..", "a/b", "a\\b", "C:Tamriel", "../Tamriel" })
		REQUIRE_FALSE(Util::IsSinglePathComponent(name));
}

TEST_CASE("Relative asset paths reject roots and parent navigation", "[stringutils]")
{
	const std::filesystem::path base = "Data";
	REQUIRE(Util::JoinRelativePath(base, "Shaders/SnowCover/main") == base / "Shaders/SnowCover/main");
	REQUIRE(Util::JoinRelativePath(base, "") == base / "");
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "../outside"), std::invalid_argument);
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "Textures/../outside"), std::invalid_argument);
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "/outside"), std::invalid_argument);
#ifdef _WIN32
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "C:outside"), std::invalid_argument);
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "C:\\outside"), std::invalid_argument);
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "Textures\\..\\outside"), std::invalid_argument);
	REQUIRE_THROWS_AS(Util::JoinRelativePath(base, "\\\\server\\share\\outside"), std::invalid_argument);
#endif
}

TEST_CASE("Config lists append owned lines while stripping comments and whitespace", "[stringutils]")
{
	std::istringstream input("# ignored\r\n  first name  # inline\r\n\t\r\n second\t\nthird");
	std::vector<std::string> lines{ "existing" };
	Util::ReadConfigLines(input, lines);
	REQUIRE(lines == std::vector<std::string>{ "existing", "first name", "second", "third" });
	std::istringstream empty;
	Util::ReadConfigLines(empty, lines);
	REQUIRE(lines.size() == 4);
}

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

TEST_CASE("ToUpperAscii folds only ASCII letters", "[stringutils]")
{
	REQUIRE(Util::ToUpperAscii("MixedCASE_1.dds") == "MIXEDCASE_1.DDS");
	REQUIRE(Util::ToUpperAscii("") == "");
}

TEST_CASE("IStartsWith and IEndsWith ignore ASCII case", "[stringutils]")
{
	REQUIRE(Util::IStartsWith("Textures\\Armor", "textures\\"));
	REQUIRE_FALSE(Util::IStartsWith("tex", "textures"));
	REQUIRE(Util::IStartsWith("abc", ""));
	REQUIRE(Util::IEndsWith("Plate_SHELL.DDS", "_shell.dds"));
	REQUIRE_FALSE(Util::IEndsWith("dds", "plate.dds"));
	REQUIRE(Util::IEndsWith("abc", ""));
}

TEST_CASE("IContains finds a needle in any case and accepts an empty needle", "[stringutils]")
{
	REQUIRE(Util::IContains("Dragonsreach Keep", "REACH"));
	REQUIRE_FALSE(Util::IContains("Dragonsreach", "whiterun"));
	REQUIRE(Util::IContains("anything", ""));
	REQUIRE(Util::IContains("", ""));
	REQUIRE_FALSE(Util::IContains("", "a"));
	REQUIRE_FALSE(Util::IContains("ab", "abc"));
	REQUIRE(Util::IContains("caf\xC3\xA9", "F\xC3\xA9"));
}

TEST_CASE("TrimAscii removes surrounding whitespace only", "[stringutils]")
{
	REQUIRE(std::string(Util::TrimAscii("  \t value with spaces \r\n")) == "value with spaces");
	REQUIRE(std::string(Util::TrimAscii("none")) == "none");
	REQUIRE(Util::TrimAscii("   ").empty());
	REQUIRE(Util::TrimAscii("").empty());
}
