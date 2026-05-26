// Unit tests for Util::Settings::BootSnapshot (restart-required settings diff).

#include "Utils/BootSnapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

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

TEST_CASE("BootSnapshot starts unlatched and ignores diffs", "[bootsnapshot]")
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
	REQUIRE(std::string(info->jsonKey) == "enabled");
	REQUIRE(std::string(info->label) == "Enabled");
}

namespace
{
	// Settings with a non-trivial member (std::string) -- not trivially-
	// copyable but still copy-assignable. Mirrors real cases like
	// ShadowCasterManager::Settings which carries exprtk formula strings
	// alongside the POD restart-gated fields.
	struct SettingsWithString
	{
		int32_t shadowLightCount = 0;
		bool enabled = false;
		std::string formula = "default";  // not registered as restart-gated
	};

	inline constexpr Util::Settings::RestartTable<SettingsWithString, 2> kStringFields{ {
		UTIL_RESTART_FIELD(SettingsWithString, shadowLightCount, "Shadow Light Count"),
		UTIL_RESTART_FIELD(SettingsWithString, enabled, "Enabled"),
	} };
}

TEST_CASE("BootSnapshot deep-copies non-trivial members on Latch", "[bootsnapshot]")
{
	// Regression: the original BootSnapshot static_asserted trivially-copyable
	// and Latch() used memcpy, which would shallow-copy std::string internals
	// (corrupting the boot snapshot's string when the live string later
	// reallocated). After the relaxation, Latch uses copy-assign so the
	// non-trivial member is deep-copied. The POD restart-gated fields still
	// drive HasPendingChange via memcmp.
	Util::Settings::BootSnapshot<SettingsWithString> snap{ kStringFields };
	SettingsWithString boot{};
	boot.shadowLightCount = 16;
	boot.enabled = true;
	const std::string originalFormula = "lightradius * lightintensity";
	boot.formula = originalFormula;

	snap.Latch(boot);
	REQUIRE(snap.IsLatched());
	REQUIRE(snap.Boot(&SettingsWithString::shadowLightCount) == 16);
	REQUIRE(snap.Boot(&SettingsWithString::enabled) == true);
	// Read the snapshot's std::string directly. Catches shallow-copy
	// regressions: if Latch were still doing a memcpy of SettingsWithString,
	// the boot copy would hold a stale pointer into live.formula's heap
	// buffer, and reading it (especially after live.formula reallocates)
	// would crash or return garbage. The POD-only assertions don't cover
	// this on their own.
	REQUIRE(snap.Boot(&SettingsWithString::formula) == originalFormula);

	// Mutating the live struct (including reallocating its string) must NOT
	// disturb the boot copy or produce false-positive diffs for unregistered
	// fields. Force a string reallocation by growing it well past SSO size,
	// then verify the boot copy's string is unchanged.
	SettingsWithString live = boot;
	live.formula = std::string(256, 'x');
	REQUIRE_FALSE(snap.HasPendingChange(live, &SettingsWithString::shadowLightCount));
	REQUIRE_FALSE(snap.HasPendingChange(live, &SettingsWithString::enabled));
	REQUIRE(snap.Boot(&SettingsWithString::formula) == originalFormula);

	// Now flip a registered POD field; the diff fires.
	live.shadowLightCount = 32;
	REQUIRE(snap.HasPendingChange(live, &SettingsWithString::shadowLightCount));
	REQUIRE_FALSE(snap.HasPendingChange(live, &SettingsWithString::enabled));
	REQUIRE(snap.Boot(&SettingsWithString::formula) == originalFormula);
}
