// Unit tests for the settings-patch key-validation helper
// (src/Utils/SettingsKeys.cpp). Pure nlohmann::json; no game/D3D/Feature
// dependency (ApplyPatch itself needs a live Feature and isn't unit-testable
// here -- see DevBenchBridge.cpp / VRAPI::CSpluginapi.cpp for its callers).

#include "Presets/BaseSettingsCustomization.h"
#include "Utils/SettingsNumeric.h"
#include "Utils/SettingsPatch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

TEST_CASE("Base Settings replace ordinary values but preserve saved post-preset edits", "[settingspatch][base-customization]")
{
	const json active{ { "Feature", { { "Strength", 1.0 }, { "Enabled", true } } } };
	const json ordinary{ { "Feature", { { "Strength", 0.8 }, { "Other", 3 } } } };
	json sources = json::object();
	BaseSettingsCustomization::AddSource(sources, "Pack", active);
	const auto first = BaseSettingsCustomization::Reconcile(json::object(), sources, active);
	auto cleaned = ordinary;
	Util::Settings::RestoreSettings(cleaned, json::object(), first.removed);
	CHECK(cleaned == json{ { "Feature", { { "Other", 3 } } } });
	auto values = ordinary;
	Util::Settings::MergeSettings(values, active);
	CHECK(values["Feature"]["Strength"] == 1.0);
	CHECK(BaseSettingsCustomization::SaveEdits(first.state, values, values).at("edits").empty());
	const auto applied = values;
	values["Feature"]["Strength"] = 1.2;
	const auto saved = BaseSettingsCustomization::SaveEdits(first.state, values, applied);
	CHECK(saved.at("edits") == json{ { "Feature", { { "Strength", 1.2 } } } });
	const auto restarted = BaseSettingsCustomization::Reconcile(json::parse(saved.dump()), sources, active);
	CHECK(restarted.removed.empty());
	const auto personal = BaseSettingsCustomization::UserLayer(ordinary, restarted.state);
	CHECK(personal == json{ { "Feature", { { "Strength", 1.2 }, { "Other", 3 } } } });
}

TEST_CASE("Disabling a supplier retains shared customizations until the last owner is disabled", "[settingspatch][base-customization]")
{
	const json active{ { "Feature", { { "Strength", 1.0 } } } };
	json sources = json::object();
	BaseSettingsCustomization::AddSource(sources, "First", active);
	auto remaining = active;
	remaining["Feature"]["Strength"] = 2.0;
	BaseSettingsCustomization::AddSource(sources, "Second", remaining);
	auto state = BaseSettingsCustomization::Reconcile(json::object(), sources, active).state;
	state["edits"] = json{ { "Feature", { { "Strength", 1.2 } } } };
	const auto shared = BaseSettingsCustomization::Reconcile(state, sources, remaining);
	CHECK(shared.removed.empty());
	CHECK(shared.state.at("edits") == state.at("edits"));
	const auto disabled = BaseSettingsCustomization::Reconcile(state, sources, json::object());
	CHECK(disabled.state.at("edits").empty());
	auto settings = state.at("edits");
	Util::Settings::RestoreSettings(settings, json::object(), disabled.removed);
	CHECK(settings.empty());
}

TEST_CASE("Installed supplier changes clear affected customizations from both user layers", "[settingspatch][base-customization]")
{
	const json active{ { "Feature", { { "Strength", 1.0 }, { "Other", 2.0 } } } };
	json sources = json::object();
	BaseSettingsCustomization::AddSource(sources, "Original", active);
	auto state = BaseSettingsCustomization::Reconcile(json::object(), sources, active).state;
	state["edits"] = json{ { "Feature", { { "Strength", 1.2 }, { "Other", 2.2 } } } };
	SECTION("New supplier with the same value") { BaseSettingsCustomization::AddSource(sources, "New", json{ { "Feature", { { "Strength", 1.0 } } } }); }
	SECTION("Updated value") { sources["/Feature/Strength"]["Original"] = 1.3; }
	SECTION("Uninstalled supplier with another owner")
	{
		BaseSettingsCustomization::AddSource(state["sources"], "Removed", json{ { "Feature", { { "Strength", 1.0 } } } });
	}
	const auto changed = BaseSettingsCustomization::Reconcile(state, sources, active);
	auto settings = state.at("edits");
	Util::Settings::RestoreSettings(settings, json::object(), changed.removed);
	CHECK(settings == json{ { "Feature", { { "Other", 2.2 } } } });
	CHECK(changed.state.at("edits") == settings);
}

TEST_CASE("Array customizations are atomic and resetting to preset removes a saved edit", "[settingspatch][base-customization]")
{
	const json active{ { "Feature", { { "Tint", { 1.0, 1.0, 1.0 } } } } };
	json sources = json::object();
	BaseSettingsCustomization::AddSource(sources, "Pack", active);
	CHECK(sources.contains("/Feature/Tint"));
	CHECK(sources.size() == 1);
	auto state = BaseSettingsCustomization::Reconcile(json::object(), sources, active).state;
	const json changed{ { "Feature", { { "Tint", { 0.5, 1.0, 1.0 } } } } };
	state = BaseSettingsCustomization::SaveEdits(state, changed, active);
	REQUIRE_FALSE(state.at("edits").empty());
	state = BaseSettingsCustomization::SaveEdits(state, active, changed);
	CHECK(state.at("edits").empty());
}

TEST_CASE("Installing an inactive preset does not erase ordinary user settings", "[settingspatch][base-customization]")
{
	const auto previous = BaseSettingsCustomization::Reconcile(json::object(), json::object(), json::object());
	json sources = json::object();
	BaseSettingsCustomization::AddSource(sources, "New", json{ { "Feature", { { "Strength", 1.0 } } } });
	const auto changed = BaseSettingsCustomization::Reconcile(previous.state, sources, json::object());
	CHECK(changed.removed.empty());
}

namespace
{
	struct NumericMetadata
	{
		enum class Semantic
		{
			Generic,
			Numeric,
			Other
		};
		enum class Transform
		{
			Identity,
			Log2
		};
		Semantic editorSemantic = Semantic::Numeric;
		Transform numericTransform = Transform::Identity;
		double displayScale = 1.0;
		bool hasNumericBounds = true;
		double minimumValue = 0.0;
		double maximumValue = 1.0;
	};
}

TEST_CASE("Catalog numeric percentages keep persisted bounds and scale display bounds once", "[settings-numeric]")
{
	const NumericMetadata setting{ .displayScale = 100.0 };
	double minimum = 0.0;
	double maximum = 0.0;
	REQUIRE(Util::Settings::GetCatalogNumericDisplayBounds(setting, minimum, maximum));
	CHECK(minimum == 0.0);
	CHECK(maximum == 100.0);
	json value = 0.5;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 0.5);
	value = 1.25;
	CHECK(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 1.0);
	double display = 0.0;
	double stored = 0.0;
	REQUIRE(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.5, display));
	CHECK(display == 50.0);
	REQUIRE(Util::Settings::ConvertCatalogNumericDisplayToStored(setting, display, stored));
	CHECK(stored == 0.5);
}

TEST_CASE("Catalog numeric angles keep radian bounds while displaying degrees", "[settings-numeric]")
{
	const NumericMetadata setting{ .displayScale = 57.29577951308232,
		.minimumValue = 0.0008726646259971648,
		.maximumValue = 0.08726646259971647 };
	double minimum = 0.0;
	double maximum = 0.0;
	REQUIRE(Util::Settings::GetCatalogNumericDisplayBounds(setting, minimum, maximum));
	CHECK(minimum == Catch::Approx(0.05));
	CHECK(maximum == Catch::Approx(5.0));
	json value = 0.02;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 0.02);
	value = 1.0;
	CHECK(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value.get<double>() == Catch::Approx(setting.maximumValue));
}

TEST_CASE("Catalog numeric logarithms transform values and display bounds once", "[settings-numeric]")
{
	const NumericMetadata setting{ .numericTransform = NumericMetadata::Transform::Log2,
		.minimumValue = 0.0625,
		.maximumValue = 16.0 };
	double minimum = 0.0;
	double maximum = 0.0;
	REQUIRE(Util::Settings::GetCatalogNumericDisplayBounds(setting, minimum, maximum));
	CHECK(minimum == -4.0);
	CHECK(maximum == 4.0);
	json value = 0.5;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 0.5);
	value = 0.01;
	CHECK(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 0.0625);
	double display = 0.0;
	double stored = 0.0;
	REQUIRE(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.5, display));
	CHECK(display == -1.0);
	REQUIRE(Util::Settings::ConvertCatalogNumericDisplayToStored(setting, display, stored));
	CHECK(stored == 0.5);
	CHECK_FALSE(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.0, display));
}

TEST_CASE("Catalog numeric integer clamping uses whole numbers inside persisted bounds", "[settings-numeric]")
{
	NumericMetadata setting{ .minimumValue = 0.2, .maximumValue = 3.8 };
	json value = 9;
	REQUIRE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 3);
	CHECK(value.is_number_integer());
	value = -9;
	REQUIRE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 1);
	setting.maximumValue = 0.8;
	value = 9;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 9);
	setting.minimumValue = std::ldexp(1.0, std::numeric_limits<std::int64_t>::digits);
	setting.maximumValue = setting.minimumValue * 2.0;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK(value == 9);
}

TEST_CASE("Catalog numeric invalid values and bounds do not mutate persisted settings", "[settings-numeric]")
{
	NumericMetadata setting;
	for (const json value : { json(true), json("invalid"), json(std::numeric_limits<double>::infinity()) }) {
		json actual = value;
		CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, actual));
		CHECK(actual == value);
	}
	json value = 2.0;
	double minimum = 0.0;
	double maximum = 0.0;
	setting.minimumValue = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK_FALSE(Util::Settings::GetCatalogNumericDisplayBounds(setting, minimum, maximum));
	CHECK(value == 2.0);
	setting.minimumValue = 1.0;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK_FALSE(Util::Settings::GetCatalogNumericDisplayBounds(setting, minimum, maximum));
	CHECK(value == 2.0);
	setting.minimumValue = 0.0;
	setting.hasNumericBounds = false;
	CHECK_FALSE(Util::Settings::ClampCatalogNumericValue(setting, value));
	CHECK_FALSE(Util::Settings::GetCatalogNumericDisplayBounds(setting, minimum, maximum));
	CHECK(value == 2.0);
}

TEST_CASE("Catalog numeric generic values and invalid transforms preserve conversion validation", "[settings-numeric]")
{
	NumericMetadata setting{ .editorSemantic = NumericMetadata::Semantic::Generic, .displayScale = 100.0 };
	double result = 0.0;
	CHECK(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.5, result));
	CHECK(result == 0.5);
	CHECK(Util::Settings::ConvertCatalogNumericDisplayToStored(setting, 0.5, result));
	CHECK(result == 0.5);
	CHECK_FALSE(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, std::numeric_limits<double>::infinity(), result));
	setting.editorSemantic = NumericMetadata::Semantic::Other;
	CHECK_FALSE(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.5, result));
	CHECK_FALSE(Util::Settings::ConvertCatalogNumericDisplayToStored(setting, 0.5, result));
	setting.editorSemantic = NumericMetadata::Semantic::Numeric;
	setting.numericTransform = static_cast<NumericMetadata::Transform>(-1);
	CHECK_FALSE(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.5, result));
	CHECK_FALSE(Util::Settings::ConvertCatalogNumericDisplayToStored(setting, 0.5, result));
	setting.numericTransform = NumericMetadata::Transform::Identity;
	setting.displayScale = std::numeric_limits<double>::infinity();
	CHECK(Util::Settings::GetCatalogNumericDisplayScale(setting) == 1.0);
	CHECK(Util::Settings::ConvertCatalogNumericStoredToDisplay(setting, 0.5, result));
	CHECK(result == 0.5);
}

TEST_CASE("EraseSettingPath removes a setting and empty groups while retaining author metadata", "[settings-export]")
{
	json document{ { "_metadata", { { "description", "Keep the author metadata" } } },
		{ "Group", { { "Nested", { { "Remove", 2 } } }, { "Keep", 3 } } },
		{ "Vector", { 1, 2, 3 } } };
	CHECK(Util::Settings::EraseSettingPath(document, json::json_pointer("/Group/Nested/Remove")));
	CHECK(document == json{ { "_metadata", { { "description", "Keep the author metadata" } } },
						  { "Group", { { "Keep", 3 } } }, { "Vector", { 1, 2, 3 } } });
	CHECK(Util::Settings::EraseSettingPath(document, json::json_pointer("/Vector")));
	CHECK_FALSE(document.contains("Vector"));
	CHECK(Util::Settings::EraseSettingPath(document, json::json_pointer("/Group/Keep")));
	CHECK_FALSE(document.contains("Group"));
	CHECK(document.at("_metadata").at("description") == "Keep the author metadata");
}

TEST_CASE("EraseSettingPath respects pointer escaping and rejects metadata or partial arrays atomically", "[settings-export]")
{
	json document{ { "a/b", { { "~key", false }, { "Keep", true } } },
		{ "_metadata", { { "description", "Keep" } } }, { "Vector", { 1, 2, 3 } } };
	const auto original = document;
	for (const auto* path : { "", "/Missing", "/Vector/0", "/_metadata", "/_metadata/description" }) {
		CHECK_FALSE(Util::Settings::EraseSettingPath(document, json::json_pointer(path)));
		CHECK(document == original);
	}
	CHECK(Util::Settings::EraseSettingPath(document, json::json_pointer("/a~1b/~0key")));
	CHECK(document.at("a/b") == json{ { "Keep", true } });
}

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

TEST_CASE("Restored defaults stay above Base Settings through save and reapplication", "[settingspatch]")
{
	const json defaults{ { "Group", { { "Strength", 1.0 }, { "Enabled", true } } } };
	const json base{ { "Group", { { "Strength", 2.0 }, { "Enabled", false } } } };
	const auto personal = Util::Settings::BuildUserOverride(defaults, base, json::object());
	CHECK(personal == defaults);
	auto restored = base;
	Util::Settings::MergeSettings(restored, personal);
	CHECK(restored == defaults);
	const auto saved = json::parse(personal.dump());
	auto reapplied = base;
	Util::Settings::MergeSettings(reapplied, saved);
	CHECK(reapplied == defaults);
	Util::Settings::RestoreSettings(reapplied, base, base);
	CHECK(reapplied == base);
}

TEST_CASE("CollectUnknownSettingKeys accepts an all-known flat patch", "[settingspatch]")
{
	const json known{ { "Enabled", true }, { "Quality", 1 } };
	const json patch{ { "Enabled", false } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("Settings array validation preserves lists and rejects malformed fixed vectors", "[settingspatch]")
{
	const json known{ { "Tint", { 1.0, 1.0, 1.0 } }, { "Ids", { 1, 2 } } };
	json incoming{ { "Tint", { 1.0, "bad", 0.0 } }, { "Ids", { 3, 4, 5 } } };
	std::vector<std::string> rejected;
	Util::Settings::EraseMismatchedSettings(incoming, known, "", rejected);
	CHECK(rejected == std::vector<std::string>{ "Tint" });
	CHECK(incoming == json{ { "Ids", { 3, 4, 5 } } });

	CHECK(Util::Settings::IsCompatibleSettingValue(known.at("Ids"), json::array()));
	CHECK_FALSE(Util::Settings::IsCompatibleSettingValue(known.at("Tint"), json::array()));
	CHECK_FALSE(Util::Settings::IsCompatibleSettingValue(known.at("Ids"), json::array({ 3, "bad", 5 })));
	CHECK_FALSE(Util::Settings::IsCompatibleSettingValue(known.at("Tint"), json::array({ 1.0, 2.0 })));
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

TEST_CASE("Preset copy skips explicit personal leaves and keeps arrays indivisible", "[settingspatch]")
{
	const json source{ { "Group", { { "Value", 1.0 }, { "Other", 2.0 } } }, { "Color", { 1, 2, 3 } }, { "_metadata", { { "Version", 1 } } } };
	const json personal{ { "Group", { { "Value", 1.0 } } }, { "Color", { 4, 5, 6 } }, { "Unrelated", true } };
	const auto result = Util::Settings::SelectSettingsToCopy(source, personal, false);
	REQUIRE(result.values == json{ { "Group", { { "Other", 2.0 } } } });
	REQUIRE(result.copied == 1);
	REQUIRE(result.overwritten == 0);
	REQUIRE(result.skipped == 2);
	REQUIRE(personal.at("Color") == json{ 4, 5, 6 });
	REQUIRE(source.contains("_metadata"));
}

TEST_CASE("Preset copy replaces selected personal leaves without copying unrelated edits", "[settingspatch]")
{
	const json source{ { "Group", { { "Value", 1.0 }, { "Other", 2.0 } } }, { "Color", { 1, 2, 3 } } };
	const json personal{ { "Group", { { "Value", 5.0 }, { "Unrelated", 7 } } }, { "Color", { 4, 5, 6 } } };
	const auto result = Util::Settings::SelectSettingsToCopy(source, personal, true);
	REQUIRE(result.values == source);
	REQUIRE(result.copied == 1);
	REQUIRE(result.overwritten == 2);
	REQUIRE(result.skipped == 0);
	REQUIRE_FALSE(result.values.at("Group").contains("Unrelated"));
}

TEST_CASE("Record copies retain selected nested values and preserve other personal fields", "[settingspatch]")
{
	const json preset{ { "Settings", { { "Tint", { 1.0, 0.5, 0.0 } }, { "Amount", 0.4 }, { "Unselected", 2.0 } } }, { "_metadata", { { "Version", 1 } } } };
	const json personal{ { "Settings", { { "Amount", 0.9 }, { "Other", 3.0 } } }, { "_metadata", { { "Version", 2 } } } };
	const auto selected = Util::Settings::SelectSettingPaths(preset, { "/Settings/Tint", "/Settings/Amount" });
	SECTION("Skip preserves existing personal values")
	{
		const auto copy = Util::Settings::SelectSettingsToCopy(selected, personal, false);
		auto document = personal;
		Util::Settings::MergeSettings(document, copy.values);
		REQUIRE(document.at("Settings") == json{ { "Tint", { 1.0, 0.5, 0.0 } }, { "Amount", 0.9 }, { "Other", 3.0 } });
		REQUIRE(document.at("_metadata") == personal.at("_metadata"));
		REQUIRE(copy.copied == 1);
		REQUIRE(copy.skipped == 1);
		REQUIRE(copy.overwritten == 0);
	}
	SECTION("Replace changes only the selected personal values")
	{
		const auto copy = Util::Settings::SelectSettingsToCopy(selected, personal, true);
		auto document = personal;
		Util::Settings::MergeSettings(document, copy.values);
		REQUIRE(document.at("Settings") == json{ { "Tint", { 1.0, 0.5, 0.0 } }, { "Amount", 0.4 }, { "Other", 3.0 } });
		REQUIRE(document.at("_metadata") == personal.at("_metadata"));
		REQUIRE(copy.copied == 1);
		REQUIRE(copy.overwritten == 1);
		REQUIRE(copy.skipped == 0);
	}
	REQUIRE_FALSE(selected.contains("_metadata"));
	REQUIRE_FALSE(selected.at("Settings").contains("Unselected"));
}

TEST_CASE("Copied preset values remain personal when equal to the enabled Base Settings", "[settingspatch]")
{
	const json baseline{ { "Group", { { "Value", 1.0 }, { "Other", 2.0 } } } };
	const auto selected = Util::Settings::SelectSettingPaths(baseline, { "/Group/Value" });
	const auto copy = Util::Settings::SelectSettingsToCopy(selected, json::object(), false);
	const auto saved = Util::Settings::BuildUserOverride(baseline, baseline, copy.values);
	REQUIRE(saved == selected);
	const json laterBaseline{ { "Group", { { "Value", 3.0 }, { "Other", 4.0 } } } };
	auto resolved = laterBaseline;
	Util::Settings::MergeSettings(resolved, saved);
	REQUIRE(resolved.at("Group").at("Value") == 1.0);
	REQUIRE(resolved.at("Group").at("Other") == 4.0);
	REQUIRE(Util::Settings::SelectSettingsToCopy(selected, saved, false).skipped == 1);
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
