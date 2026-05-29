// Unit tests for the restart-gated settings metadata helpers
// (src/Utils/RestartSettings.h). Pure std; no game/D3D/ImGui dependency.

#include "Utils/RestartSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <string>  // compare via std::string (Catch2 lacks a linked StringMaker for string_view)

namespace
{
	// Standard-layout stand-in for a feature Settings struct.
	struct FakeSettings
	{
		int width = 0;
		float scale = 0.0f;
		bool enabled = false;
	};

	constexpr Util::Settings::RestartTable<FakeSettings, 3> kFields{
		UTIL_RESTART_FIELD(FakeSettings, width, "Width"),
		UTIL_RESTART_FIELD(FakeSettings, scale, "Scale"),
		UTIL_RESTART_FIELD(FakeSettings, enabled, "Enabled"),
	};
}

TEST_CASE("UTIL_RESTART_FIELD records jsonKey, label, offset, size", "[restartsettings]")
{
	REQUIRE(std::string(kFields[0].jsonKey) == "width");
	REQUIRE(std::string(kFields[0].label) == "Width");
	REQUIRE(kFields[0].offset == offsetof(FakeSettings, width));
	REQUIRE(kFields[0].size == sizeof(int));

	REQUIRE(kFields[1].offset == offsetof(FakeSettings, scale));
	REQUIRE(kFields[1].size == sizeof(float));
	REQUIRE(kFields[2].offset == offsetof(FakeSettings, enabled));
	REQUIRE(kFields[2].size == sizeof(bool));
}

TEST_CASE("FindRestartField returns the matching descriptor", "[restartsettings]")
{
	const auto* f = Util::Settings::FindRestartField(kFields, "scale");
	REQUIRE(f != nullptr);
	REQUIRE(std::string(f->label) == "Scale");
	REQUIRE(f->offset == offsetof(FakeSettings, scale));
}

TEST_CASE("FindRestartField returns nullptr on miss and is case-sensitive", "[restartsettings]")
{
	REQUIRE(Util::Settings::FindRestartField(kFields, "missing") == nullptr);
	// jsonKey is the lowercase member name; "Width" must not match "width".
	REQUIRE(Util::Settings::FindRestartField(kFields, "Width") == nullptr);
}

TEST_CASE("FindRestartField over an empty span returns nullptr", "[restartsettings]")
{
	REQUIRE(Util::Settings::FindRestartField({}, "width") == nullptr);
}
