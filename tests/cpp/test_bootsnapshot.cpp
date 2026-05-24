// Unit tests for Util::Settings::BootSnapshot (restart-required settings diff).

#include "Utils/BootSnapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
	struct TestSettings
	{
		uint32_t mode = 0;
		bool enabled = false;
		float value = 0.0f;
	};

	inline constexpr Util::Settings::RestartTable<TestSettings, 2> kFields{ {
		UTIL_RESTART_FIELD(TestSettings, mode, "Mode"),
		UTIL_RESTART_FIELD(TestSettings, enabled, "Enabled"),
	} };
}

TEST_CASE("BootSnapshot starts unlatching and ignores diffs", "[bootsnapshot]")
{
	Util::Settings::BootSnapshot<TestSettings> snap{ kFields };
	TestSettings live{};
	live.mode = 3;
	live.enabled = true;

	REQUIRE_FALSE(snap.IsLatched());
	REQUIRE(snap.RawBoot("mode") == nullptr);
	REQUIRE_FALSE(snap.HasPendingChange(live, &TestSettings::mode));
}

TEST_CASE("BootSnapshot detects member changes after latch", "[bootsnapshot]")
{
	Util::Settings::BootSnapshot<TestSettings> snap{ kFields };
	TestSettings boot{};
	boot.mode = 1;
	boot.enabled = false;

	snap.Latch(boot);
	REQUIRE(snap.IsLatched());
	REQUIRE(snap.Boot(&TestSettings::mode) == 1);
	REQUIRE(snap.Boot(&TestSettings::enabled) == false);

	TestSettings live = boot;
	REQUIRE_FALSE(snap.HasPendingChange(live, &TestSettings::mode));

	live.mode = 2;
	REQUIRE(snap.HasPendingChange(live, &TestSettings::mode));
	REQUIRE_FALSE(snap.HasPendingChange(live, &TestSettings::enabled));
}

TEST_CASE("BootSnapshot exposes field metadata by member", "[bootsnapshot]")
{
	Util::Settings::BootSnapshot<TestSettings> snap{ kFields };
	const auto* info = snap.FindField(&TestSettings::enabled);
	REQUIRE(info != nullptr);
	REQUIRE(std::string_view(info->jsonKey) == "enabled");
	REQUIRE(std::string_view(info->label) == "Enabled");
}

