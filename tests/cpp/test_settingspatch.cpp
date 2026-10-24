// Unit tests for the settings-patch key-validation helper
// (src/Utils/SettingsKeys.cpp). Pure nlohmann::json; no game/D3D/Feature
// dependency (ApplyPatch itself needs a live Feature and isn't unit-testable
// here -- see DevBenchBridge.cpp / VRAPI::CSpluginapi.cpp for its callers).

#include "Utils/SettingsPatch.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

TEST_CASE("OverlayRecognizedRootSettings ignores a non-object document", "[settingsoverlay]")
{
	json defaults{ { "General", { { "Enable Async", true } } } };
	const json original = defaults;

	Util::Settings::OverlayRecognizedRootSettings(defaults, json::array({ "unexpected" }));

	CHECK(defaults == original);
}

TEST_CASE("OverlayRecognizedRootSettings applies a nested partial config", "[settingsoverlay]")
{
	json defaults{
		{ "General", { { "Enable Async", true }, { "Language", "en" } } },
		{ "Feature", { { "Enabled", false } } }
	};
	const json user{ { "General", { { "Language", "fr" } } } };

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults["General"]["Enable Async"] == true);
	CHECK(defaults["General"]["Language"] == "fr");
	CHECK(defaults["Feature"]["Enabled"] == false);
}

TEST_CASE("OverlayRecognizedRootSettings ignores unknown roots and malformed recognized values", "[settingsoverlay]")
{
	json defaults{
		{ "General", { { "Enable Async", true }, { "Language", "en" } } },
		{ "Feature", { { "Enabled", false }, { "Strength", 1.0f } } }
	};
	const json user{
		{ "General", { { "Enable Async", "yes" }, { "Unknown", 7 } } },
		{ "Feature", { { "Enabled", { { "value", true } } }, { "Strength", "high" } } },
		{ "Obsolete Feature", { { "Enabled", true } } }
	};
	json expected = defaults;
	expected["General"]["Unknown"] = 7;

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults == expected);
}

TEST_CASE("OverlayRecognizedRootSettings keeps late-defined settings under a recognized owner", "[settingsoverlay]")
{
	json defaults{ { "Post Processing", { { "Enabled", true } } } };
	const json user{
		{ "Post Processing",
			{ { "Enabled", false }, { "Physical Glare", { { "Quality", 2 }, { "Intensity", 0.75f } } } } },
		{ "Removed Feature", { { "Enabled", true } } }
	};

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults["Post Processing"]["Enabled"] == false);
	CHECK(defaults["Post Processing"]["Physical Glare"] == user["Post Processing"]["Physical Glare"]);
	CHECK_FALSE(defaults.contains("Removed Feature"));
}

TEST_CASE("OverlayRecognizedRootSettings accepts safe numeric representations", "[settingsoverlay]")
{
	json defaults{
		{ "Signed", std::int32_t{ -1 } },
		{ "Unsigned", std::uint32_t{ 1 } },
		{ "Float", 1.0f },
		{ "Integral Float", std::int32_t{ 0 } }
	};
	const json user{
		{ "Signed", std::uint32_t{ 7 } },
		{ "Unsigned", std::int32_t{ 9 } },
		{ "Float", std::int32_t{ 2 } },
		{ "Integral Float", 6.0 }
	};

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults["Signed"] == 7);
	CHECK(defaults["Unsigned"] == 9);
	CHECK(defaults["Float"] == 2);
	CHECK(defaults["Integral Float"] == 6.0);
	CHECK(defaults["Integral Float"].is_number_integer());
	CHECK(defaults["Unsigned"].is_number_unsigned());
	CHECK(defaults["Float"].is_number_float());
}

TEST_CASE("OverlayRecognizedRootSettings rejects unsafe numeric values", "[settingsoverlay]")
{
	json defaults{
		{ "Signed", std::int32_t{ 4 } },
		{ "Unsigned", std::uint32_t{ 5 } },
		{ "Float", 1.0f }
	};
	const json user{
		{ "Signed", std::uint64_t{ std::numeric_limits<std::uint32_t>::max() } },
		{ "Unsigned", std::int32_t{ -1 } },
		{ "Float", std::numeric_limits<double>::infinity() }
	};
	const json expected = defaults;

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults == expected);
}

TEST_CASE("OverlayRecognizedRootSettings filters arrays conservatively", "[settingsoverlay]")
{
	json defaults{
		{ "Vector", { 1.0f, 2.0f } },
		{ "Malformed Vector", { 5.0f, 6.0f } },
		{ "Wrong Length Vector", { 8.0f, 9.0f } },
		{ "Hotkey", { std::uint32_t{ 35 } } },
		{ "Hotkey List", { std::uint32_t{ 42 }, std::uint32_t{ 35 } } }
	};
	const json user{
		{ "Vector", { 3, 4 } },
		{ "Malformed Vector", { 7, "bad" } },
		{ "Wrong Length Vector", { 10, 11, 12 } },
		{ "Hotkey", { std::uint32_t{ 42 }, std::uint32_t{ 35 } } },
		{ "Hotkey List", { std::uint32_t{ 35 } } }
	};

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults["Vector"] == json{ 3, 4 });
	CHECK(defaults["Malformed Vector"] == json{ 5.0f, 6.0f });
	CHECK(defaults["Wrong Length Vector"] == json{ 8.0f, 9.0f });
	CHECK(defaults["Hotkey"] == json{ 42, 35 });
	CHECK(defaults["Hotkey List"] == json{ 35 });
}

TEST_CASE("Input binding overlay accepts a combo when the default is scalar", "[settingsoverlay]")
{
	json defaults{ { "ToggleKey", std::uint32_t{ 35 } } };
	const json user{ { "ToggleKey", { std::uint32_t{ 17 }, std::uint32_t{ 35 } } } };

	Util::Settings::OverlayInputBinding(defaults["ToggleKey"], user["ToggleKey"]);

	CHECK(defaults["ToggleKey"] == json{ 17, 35 });
}

TEST_CASE("Input binding overlay accepts a scalar when the default is a combo", "[settingsoverlay]")
{
	json defaults{ { "ToggleKey", { std::uint32_t{ 17 }, std::uint32_t{ 35 } } } };
	const json user{ { "ToggleKey", std::uint32_t{ 36 } } };

	Util::Settings::OverlayInputBinding(defaults["ToggleKey"], user["ToggleKey"]);

	CHECK(defaults["ToggleKey"] == 36);
}

TEST_CASE("Settings overlay cannot turn ordinary scalars into input combos", "[settingsoverlay]")
{
	json defaults{
		{ "Enabled", std::uint32_t{ 0 } },
		{ "Samples", { 1, 2 } },
		{ "Strength", 1.0f }
	};
	const json original = defaults;
	const json user{ { "Enabled", { 1, 2 } }, { "Samples", { { 1 }, { 2 } } }, { "Strength", { 2.0f } } };

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults == original);
}

TEST_CASE("Input binding overlay rejects malformed combos without partial changes", "[settingsoverlay]")
{
	const json original = { 17u, 35u };
	for (const json& malformed : std::vector<json>{ -1, true, "35", { 17, "35" }, { { 17 }, { 35 } },
			 std::uint64_t{ std::numeric_limits<std::uint32_t>::max() } + 1, 35.5 }) {
		json binding = original;
		Util::Settings::OverlayInputBinding(binding, malformed);
		CHECK(binding == original);
	}
	json binding = original;
	Util::Settings::OverlayInputBinding(binding, 35.0);
	CHECK(binding.is_number_unsigned());
	CHECK(binding == 35);
	Util::Settings::OverlayInputBinding(binding, json::array());
	CHECK(binding == json::array());
}

TEST_CASE("OverlayRecognizedRootSettings keeps recognized arrays with empty defaults", "[settingsoverlay]")
{
	json defaults{ { "Screenshot", { { "CropPresets", json::array() } } } };
	const json cropPresets = {
		{ { "Name", "Portrait" }, { "CropLeft", 0.1f }, { "CropRight", 0.1f } }
	};
	const json user{ { "Screenshot", { { "CropPresets", cropPresets } } } };

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults["Screenshot"]["CropPresets"] == cropPresets);
}

TEST_CASE("OverlayRecognizedRootSettings preserves valid explicit character lighting", "[settingsoverlay]")
{
	json defaults{ { "Subsurface Scattering", { { "EnableCharacterLighting", std::uint32_t{ 0 } }, { "Strength", 1.0f } } } };
	const json user{ { "Subsurface Scattering", { { "EnableCharacterLighting", std::uint32_t{ 1 } } } } };

	Util::Settings::OverlayRecognizedRootSettings(defaults, user);

	CHECK(defaults["Subsurface Scattering"]["EnableCharacterLighting"] == 1);
	CHECK(defaults["Subsurface Scattering"]["Strength"] == 1.0f);
}

TEST_CASE("Export selection retains only selected complete setting paths", "[settingspatch]")
{
	const json values{ { "Group", { { "Pick", 2 }, { "Skip", false } } }, { "Vector", { 1, 2, 3 } },
		{ "0", { { "1", 4 } } }, { "a/b", { { "~key", true } } }, { "_metadata", { { "description", "skip" } } } };
	REQUIRE(Util::Settings::SelectSettingPaths(values, { "/Group/Pick", "/Vector" }) ==
			json{ { "Group", { { "Pick", 2 } } }, { "Vector", { 1, 2, 3 } } });
	REQUIRE(Util::Settings::SelectSettingPaths(values, { "/0/1", "/a~1b/~0key" }) ==
			json{ { "0", { { "1", 4 } } }, { "a/b", { { "~key", true } } } });
	REQUIRE(Util::Settings::SelectSettingPaths(values, { "/Vector/0", "/Group", "/_metadata/description" }).empty());
	REQUIRE(Util::Settings::SelectSettingPaths(values, {}).empty());
}

TEST_CASE("Restoring overwrite baselines retains unrelated user changes", "[settingspatch]")
{
	json values{ { "Group", { { "Owned", 4.0 }, { "Unowned", 6.0 }, { "Extra", true } } } };
	const json baseline{ { "Group", { { "Owned", 1.0 }, { "Unowned", 2.0 } } } };
	const json mask{ { "Group", { { "Owned", 4.0 }, { "Extra", true } } } };
	Util::Settings::RestoreSettings(values, baseline, mask);
	REQUIRE(values == json{ { "Group", { { "Owned", 1.0 }, { "Unowned", 6.0 } } } });
}

TEST_CASE("Selecting legacy values preserves only currently owned keys", "[settingspatch]")
{
	const json legacy{ { "Group", { { "Owned", 4.0 }, { "Orphan", 6.0 } } }, { "Other", true } };
	const json mask{ { "Group", { { "Owned", 1.0 } } } };
	REQUIRE(Util::Settings::SelectSettings(legacy, mask) == json{ { "Group", { { "Owned", 4.0 } } } });
}

TEST_CASE("BuildUserOverride ignores values outside the mod override", "[settingspatch]")
{
	const json current{
		{ "Disable at Boot", { { "ScreenSpaceGI", true }, { "Skylighting", true } } }
	};
	const json overridden{
		{ "Disable at Boot", { { "ScreenSpaceGI", true } } }
	};

	REQUIRE(Util::Settings::BuildUserOverride(current, overridden).empty());
}

TEST_CASE("BuildUserOverride saves only changed nested override values", "[settingspatch]")
{
	const json current{
		{ "Disable at Boot", { { "ScreenSpaceGI", false }, { "Skylighting", true } } },
		{ "General", { { "Enable Async", true } } }
	};
	const json overridden{
		{ "Disable at Boot", { { "ScreenSpaceGI", true } } }
	};
	const json expected{
		{ "Disable at Boot", { { "ScreenSpaceGI", false } } }
	};

	REQUIRE(Util::Settings::BuildUserOverride(current, overridden) == expected);
}

TEST_CASE("BuildUserOverride tolerates floating-point serialization noise", "[settingspatch]")
{
	const json current{ { "Strength", 1.000001 } };
	const json overridden{ { "Strength", 1.0 } };

	REQUIRE(Util::Settings::BuildUserOverride(current, overridden).empty());
}

TEST_CASE("CollectUnknownSettingKeys accepts an all-known flat patch", "[settingspatch]")
{
	const json known{ { "Enabled", true }, { "Quality", 1 } };
	const json patch{ { "Enabled", false } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("CollectUnknownSettingKeys reports an unknown flat key", "[settingspatch]")
{
	const json known{ { "Enabled", true } };
	const json patch{ { "Enabeld", false } };  // typo
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "Enabeld" });
}

TEST_CASE("CollectUnknownSettingKeys accepts a valid nested patch", "[settingspatch]")
{
	const json known{ { "ShadowSettings", { { "Distance", 1.0f }, { "Resolution", 512 } } } };
	const json patch{ { "ShadowSettings", { { "Distance", 2.0f } } } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("CollectUnknownSettingKeys reports a mis-nested key as a dotted path", "[settingspatch]")
{
	const json known{ { "ShadowSettings", { { "Distance", 1.0f } } } };
	const json patch{ { "ShadowSettings", { { "Distnace", 2.0f } } } };  // typo, still nested
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "ShadowSettings.Distnace" });
}

TEST_CASE("CollectUnknownSettingKeys reports a flattened key that should be nested", "[settingspatch]")
{
	// A caller that flattens {"Distance": 2.0} instead of nesting it under
	// ShadowSettings must be rejected, not silently dropped.
	const json known{ { "ShadowSettings", { { "Distance", 1.0f } } } };
	const json patch{ { "Distance", 2.0f } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "Distance" });
}

TEST_CASE("CollectUnknownSettingKeys reports every key when known is not an object", "[settingspatch]")
{
	// A feature with no SaveSettings override reports null here. It's ApplyPatch's
	// own is_object() guard (untested here -- needs a live Feature) that skips this
	// check in that case rather than rejecting the patch outright.
	const json known = nullptr;
	const json patch{ { "Enabled", true } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "Enabled" });
}

TEST_CASE("CollectUnknownSettingKeys on a non-object incoming patch reports nothing", "[settingspatch]")
{
	const json known{ { "Enabled", true } };
	const json patch = 5;
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("CollectUnknownSettingKeys reports nesting under a known scalar field", "[settingspatch]")
{
	// Inverse of the flattened-key case: a caller that nests under a field
	// the feature defines as a scalar, not a group.
	const json known{ { "A", 1 } };
	const json originalKnown = known;
	const json patch{ { "A", { { "B", 2 } } } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "A (wrong type)" });
	REQUIRE(known == originalKnown);
}
