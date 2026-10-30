#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <latch>
#include <thread>
#include <winrt/base.h>

#include "CSEditor/SceneManager/SceneEntryDraft.h"
#include "Presets/LegacyPreset.h"
#include "Presets/PresetActivation.h"
#include "Presets/PresetContent.h"
#include "Presets/PresetState.h"
#include "Utils/RenderTaskQueue.h"
#include "Utils/SettingsPatch.h"

TEST_CASE("Legacy feature overwrites translate into upstream Baseline payloads", "[presets][legacy-presets]")
{
	const std::array<LegacyPreset::FeatureNames, 2> features{
		std::pair{ "First", "First Feature" }, std::pair{ "Second", "Second Feature" }
	};
	const nlohmann::json values{
		{ "First Feature", { { "Amount", 0.5 }, { "Layers", { 1, 2, 3 } } } },
		{ "Second Feature", { { "Enabled", false } } },
		{ "Advanced", { { "CompilerThreads", 99 } } },
		{ "Unknown", { { "Amount", 5 } } }
	};
	const auto translated = LegacyPreset::Translate(values, "", features);
	REQUIRE(translated.size() == 2);
	REQUIRE(translated.at("First") == values.at("First Feature"));
	REQUIRE(translated.at("Second") == values.at("Second Feature"));
	REQUIRE(LegacyPreset::Translate(values.at("First Feature"), "First", features).at("First") == values.at("First Feature"));
	REQUIRE(LegacyPreset::Translate(values, "Unknown", features).empty());
	REQUIRE(LegacyPreset::Translate(nlohmann::json::array(), "First", features).empty());
}

TEST_CASE("Retired legacy keys do not reject neighboring supported settings", "[presets][legacy-presets]")
{
	const nlohmann::json schema{ { "Enabled", true }, { "Group", { { "Amount", 1.0 } } }, { "Layers", { 1, 2 } } };
	const nlohmann::json legacy{
		{ "Enabled", false }, { "RetiredToggle", true },
		{ "Group", { { "Amount", 0.7 }, { "OldAmount", 2.0 } } }, { "Layers", { 3, 4, 5 } }
	};
	const auto normalized = LegacyPreset::NormalizePayload(legacy, schema);
	REQUIRE(normalized.values == nlohmann::json{
									 { "Enabled", false }, { "Group", { { "Amount", 0.7 } } }, { "Layers", { 3, 4, 5 } } });
	REQUIRE(normalized.retiredKeys == std::vector<std::string>{ "Group.OldAmount", "RetiredToggle" });
	REQUIRE(legacy.contains("RetiredToggle"));
	const auto malformed = LegacyPreset::NormalizePayload({ { "Enabled", "invalid" }, { "RetiredToggle", true } }, schema);
	std::vector<std::string> invalid;
	Util::Settings::CollectUnknownSettingKeys(malformed.values, schema, "", invalid);
	REQUIRE(invalid == std::vector<std::string>{ "Enabled (wrong type)" });
	REQUIRE(malformed.retiredKeys == std::vector<std::string>{ "RetiredToggle" });
}

TEST_CASE("Legacy companion values remain personal edits above Base Settings", "[presets][legacy-presets]")
{
	const std::array<LegacyPreset::FeatureNames, 1> features{ std::pair{ "First", "First Feature" } };
	nlohmann::json settings{ { "First Feature", { { "Amount", 0.3 }, { "Kept", 2 } } } };
	LegacyPreset::ImportPersonalEdits(settings,
		{ { "First Feature", { { "Amount", 0.7 } } }, { "Unknown", { { "Amount", 5 } } } }, features);
	REQUIRE(settings.at("First Feature").at("Amount") == 0.7);
	REQUIRE(settings.at("First Feature").at("Kept") == 2);
	REQUIRE_FALSE(settings.contains("Unknown"));
	REQUIRE(PresetState::HasNormalizedFeatureEdits(settings.at("Presets"), "First Feature"));
	LegacyPreset::ImportPersonalEdits(settings, { { "First Feature", nullptr } }, features);
	REQUIRE(settings.at("First Feature").at("Amount") == 0.7);
}

TEST_CASE("Legacy boot settings use the upstream manifest contract", "[presets][legacy-presets]")
{
	const std::array<LegacyPreset::FeatureNames, 2> features{
		std::pair{ "First", "First Feature" }, std::pair{ "Second", "Second Feature" }
	};
	const auto flags = LegacyPreset::TranslateBoot({ { "Disable at Boot", { { "First", true }, { "Second", 1 }, { "Unknown", false } } } }, features);
	REQUIRE(flags.size() == 1);
	REQUIRE(flags.at("First"));
	REQUIRE(LegacyPreset::TranslateBoot({ { "Disable at Boot", nullptr } }, features).empty());
	REQUIRE(LegacyPreset::TranslateBoot(nlohmann::json::object(), features).empty());
}

TEST_CASE("Imported legacy globals retain their lower priority", "[presets][legacy-presets]")
{
	const auto global = LegacyPreset::BuildPackId("Z Pack", true);
	const auto feature = LegacyPreset::BuildPackId("A Pack", false);
	REQUIRE(global < feature);
	REQUIRE(PresetState::IsPackId(global));
	REQUIRE(PresetState::IsPackId(feature));
	REQUIRE(LegacyPreset::BuildPackId("Same Pack", true) != LegacyPreset::BuildPackId("Same Pack", false));
}

TEST_CASE("Base Settings default to all enabled and retain explicit disabled choices", "[presets][legacy-presets]")
{
	const std::array<std::string, 3> order{ "Z Pack", "Alpha", "!Priority" };
	auto state = PresetState::DiscoverBaselines(nlohmann::json::object(), order);
	REQUIRE(state.at("baselinePackIds") == order);
	PresetState::SetDisabled(state, "disabledBaselinePackIds", "Alpha", true);
	REQUIRE(PresetState::DiscoverBaselines(state, order).at("baselinePackIds") == nlohmann::json({ "Z Pack", "!Priority" }));
	PresetState::SetDisabled(state, "disabledBaselinePackIds", "Alpha", false);
	REQUIRE(PresetState::DiscoverBaselines(state, order).at("baselinePackIds") == order);
	state["baseSettingsOrderCustomized"] = true;
	state["baselinePackIds"] = { "!Priority", "Z Pack", "Alpha" };
	REQUIRE(PresetState::DiscoverBaselines(state, order).at("baselinePackIds") == state.at("baselinePackIds"));
	PresetState::SetDisabled(state, "disabledBaselinePackIds", "Alpha", true);
	REQUIRE(PresetState::DiscoverBaselines(state, order).at("baselinePackIds") == nlohmann::json({ "!Priority", "Z Pack" }));
	const auto migrated = PresetState::DiscoverBaselines({ { "knownBaselinePackIds", { "Alpha", "Z Pack" } },
															 { "baselinePackIds", { "Z Pack" } } },
		order);
	REQUIRE(migrated.at("disabledBaselinePackIds") == nlohmann::json({ "Alpha" }));
	const auto manual = PresetState::DiscoverBaselines({ { "baselinePackIds", { "Z Pack" } } }, order, false);
	REQUIRE(manual.at("baselinePackIds") == nlohmann::json({ "Z Pack" }));
	REQUIRE(PresetState::DiscoverBaselines(manual, order, true).at("baselinePackIds") == order);
	REQUIRE(PresetState::DiscoverBaselines(nlohmann::json::object(), order, false).at("baselinePackIds").empty());
}

TEST_CASE("Upstream preset-state saves do not discard OS preferences", "[presets]")
{
	const nlohmann::json privateState{ { "disabledBaselinePackIds", { "Off" } }, { "legacyUserOverrideFeatures", { "Feature" } },
		{ "removedLegacyPackIds", { "Legacy - Old" } }, { "automaticPresets", false } };
	const auto restored = PresetState::RestorePrivateState({ { "activePackId", "CS Selection" } }, privateState);
	REQUIRE(restored.at("activePackId") == "CS Selection");
	REQUIRE(restored.at("disabledBaselinePackIds") == nlohmann::json({ "Off" }));
	REQUIRE(restored.at("legacyUserOverrideFeatures") == nlohmann::json({ "Feature" }));
	REQUIRE(restored.at("removedLegacyPackIds") == nlohmann::json({ "Legacy - Old" }));
	REQUIRE(restored.at("automaticPresets") == false);
	REQUIRE(PresetState::RestorePrivateState({ { "automaticPresets", true } }, privateState).at("automaticPresets") == true);
}

TEST_CASE("Disabled Base Settings retain priority across startup and reactivation", "[presets]")
{
	const std::vector<std::string> installed{ "Z Pack", "Alpha", "!Priority" };
	const std::vector<std::string> enabled{ "Z Pack", "!Priority" };
	const auto order = PresetState::RetainBaselineOrder(installed, enabled, installed, false);
	REQUIRE(order == installed);
	REQUIRE(PresetState::OrderEnabledBaselines(order, enabled) == enabled);
	auto reenabled = enabled;
	reenabled.push_back("Alpha");
	REQUIRE(PresetState::OrderEnabledBaselines(order, reenabled) == installed);
	const std::vector<std::string> custom{ "!Priority", "Alpha", "Z Pack" };
	const std::vector<std::string> customEnabled{ "!Priority", "Z Pack" };
	REQUIRE(PresetState::RetainBaselineOrder(custom, customEnabled, installed, true) == custom);
	const auto restored = PresetState::RestorePrivateState({ { "baselinePackIds", customEnabled } }, { { "baselinePackOrder", custom } });
	REQUIRE(PresetState::ReadIds(restored, "baselinePackOrder") == custom);
	REQUIRE(PresetState::RetainBaselineOrder(custom, enabled, installed, true) == installed);
	const std::vector<std::string> removed{ "Z Pack", "!Priority" };
	REQUIRE(PresetState::RetainBaselineOrder(custom, customEnabled, removed, true) == customEnabled);
	auto missing = enabled;
	missing.push_back("Missing");
	REQUIRE(PresetState::OrderEnabledBaselines(order, missing) == missing);
}

TEST_CASE("Automatic scene presets keep conflicting new presets entirely disabled", "[presets]")
{
	using namespace PresetActivation;
	const std::vector<Candidate> candidates{
		{ "First", { { "weather/day/Feature/Value", 1 } } },
		{ "Second", { { "weather/day/Feature/Value", 2 }, { "location/Feature/Other", 3 } } },
		{ "Third", { { "weather/night/Feature/Value", 4 } } }
	};
	auto selected = Select(candidates, {}, {}, {}, true);
	REQUIRE(selected.enabled == std::vector<std::string>{ "First", "Third" });
	REQUIRE(selected.blocked.at("Second") == std::vector<std::string>{ "First" });
	const std::array<std::string, 1> existing{ "Second" };
	selected = Select(candidates, existing, {}, {}, true);
	REQUIRE(selected.enabled == std::vector<std::string>{ "Second", "Third" });
	REQUIRE(selected.blocked.at("First") == std::vector<std::string>{ "Second" });
	REQUIRE(Select(candidates, {}, {}, {}, false).enabled.empty());
	REQUIRE(Select(candidates, {}, existing, {}, true).enabled == std::vector<std::string>{ "First", "Third" });
	REQUIRE(Select(candidates, {}, {}, existing, true).enabled == std::vector<std::string>{ "First", "Third" });
	REQUIRE_FALSE(Conflicts({ { "same", 1 } }, { { "same", 1.0 } }));
	REQUIRE(Conflicts({ { "exclusive/e11", true } }, { { "exclusive/e11", true } }));
}

TEST_CASE("Preset conflicts use setting addresses rather than filenames", "[presets]")
{
	using PresetContent::Describe;
	const nlohmann::json first{ { "_feature", "Example" }, { "Group", { { "Amount", 0.5 } } } };
	const auto footprint = Describe("Weather/0x123~example.esp/Day/One.json", first);
	REQUIRE(PresetActivation::Conflicts(footprint, Describe("Weather/0x123~EXAMPLE.esp/Day/Other.json",
													   { { "_feature", "Example" }, { "Group", { { "Amount", 0.8 } } } })));
	REQUIRE_FALSE(PresetActivation::Conflicts(footprint, Describe("Weather/0x123~example.esp/Night/Other.json", first)));
	REQUIRE_FALSE(PresetActivation::Conflicts(footprint, Describe("Weather/0x123~example.esp/Day/Other.json",
															 { { "_feature", "Example" }, { "Group", { { "Other", 1.0 } } } })));
	REQUIRE(Describe("TimeOfDay/Day/Legacy_Example.json", { { "Group", { { "Amount", 0.5 } } } }).contains("scene/timeofday/day/Example/Group/Amount"));
	REQUIRE(Describe("InteriorOnly/Invalid.json", { { "_feature", false } }).empty());
	REQUIRE(PresetActivation::Conflicts(Describe("Forms/One.json", { { "value", 1 } }), Describe("Forms/One.json", { { "value", 2 } })));
	const auto base = Describe("Baseline/Example.json", { { "Amount", 0.5 } });
	REQUIRE_FALSE(base.empty());
	REQUIRE_FALSE(PresetActivation::Conflicts(base, Describe("Baseline/Example.json", { { "Amount", 0.8 } })));
}

TEST_CASE("Scene mode, transitions and deletions participate in preset conflicts", "[presets]")
{
	const nlohmann::json original{ { "_feature", "Example" }, { "Amount", 0.5 }, { "_metadata", { { "timeOfDayEnabled", false } } } };
	auto deleted = original;
	deleted["_metadata"]["entryDeletions"]["Amount"] = true;
	const auto first = PresetContent::Describe("Weather/0x123~example.esp/One.json", original);
	REQUIRE(PresetActivation::Conflicts(first, PresetContent::Describe("Weather/0x123~example.esp/Two.json", deleted)));
	auto transition = original;
	transition["_metadata"]["entryTransitions"]["Amount"] = 2.0;
	REQUIRE(PresetActivation::Conflicts(first, PresetContent::Describe("Weather/0x123~example.esp/Two.json", transition)));
	auto periodic = original;
	periodic["_metadata"]["timeOfDayEnabled"] = true;
	REQUIRE(PresetActivation::Conflicts(first, PresetContent::Describe("Weather/0x123~example.esp/Day/Two.json", periodic)));
	REQUIRE_FALSE(PresetContent::IsSceneFile("InteriorOnly/../Baseline/Example.json"));
	REQUIRE(PresetContent::PathKey(std::filesystem::path(u8"Weather/0x123~日本語.esp/Day")) == "weather/0x123~日本語.esp/day");
}

TEST_CASE("Deleting selected preset settings preserves other values and metadata", "[presets]")
{
	nlohmann::json document{
		{ "_feature", "Example" }, { "Group", { { "Amount", 0.5 }, { "Kept", 2 } } },
		{ "_metadata", { { "author", "Author" }, { "entryTransitions", { { "Group", { { "Amount", 2.0 }, { "Kept", 3.0 } } } } },
						   { "entryDeletions", { { "Group", { { "Amount", true } } } } } } }
	};
	const std::array<std::string, 1> paths{ "/Group/Amount" };
	REQUIRE(PresetContent::RemoveSettings(document, paths));
	REQUIRE_FALSE(document.at("Group").contains("Amount"));
	REQUIRE(document.at("Group").at("Kept") == 2);
	REQUIRE(document.at("_metadata").at("author") == "Author");
	REQUIRE(document.at("_metadata").at("entryTransitions").at("Group").at("Kept") == 3.0);
	REQUIRE(document.at("_metadata").at("entryDeletions").empty());
	REQUIRE_FALSE(PresetContent::RemoveSettings(document, std::array<std::string, 1>{ "/_feature" }));
	REQUIRE(document.at("_feature") == "Example");
}

TEST_CASE("Updated enabled scene presets cannot retain conflicting selections", "[presets]")
{
	const std::vector<PresetActivation::Candidate> candidates{ { "First", { { "field", 1 } } }, { "Second", { { "field", 2 } } } };
	const std::array<std::string, 2> enabled{ "Second", "First" };
	const auto result = PresetActivation::Select(candidates, enabled, {}, enabled, false);
	REQUIRE(result.enabled == std::vector<std::string>{ "Second" });
	REQUIRE(result.blocked.at("First") == std::vector<std::string>{ "Second" });
}

TEST_CASE("Scene drafts suspend and resume only their owned entries", "[presets][scene-draft]")
{
	using Entry = nlohmann::json;
	std::vector<Entry> entries{
		{ { "owner", "first" }, { "period", 0 }, { "source", "user" }, { "value", 1 } },
		{ { "owner", "first" }, { "period", 0 }, { "source", "pack" }, { "value", 2 } },
		{ { "owner", "second" }, { "period", 0 }, { "source", "user" }, { "value", 3 } },
		{ { "owner", "first" }, { "period", 1 }, { "source", "user" }, { "value", 4 } }
	};
	const auto matches = [](const Entry& entry) {
		return entry.at("owner") == "first" && entry.at("period") == 0 && entry.at("source") == "user";
	};
	SceneEntryDraft<Entry> draft;
	draft.Begin(entries, matches);
	REQUIRE_FALSE(draft.HasChanges(entries, matches));
	entries[0]["value"] = 10;
	entries[0]["future"] = "kept";
	entries[2]["value"] = 30;
	REQUIRE(draft.HasChanges(entries, matches));
	const auto persisted = draft.SavedView(entries, matches);
	REQUIRE(persisted[0].at("value") == 1);
	REQUIRE(persisted[1].at("value") == 2);
	REQUIRE(persisted[2].at("value") == 30);
	REQUIRE(persisted[3].at("value") == 4);
	REQUIRE(entries[0].at("value") == 10);
	draft.Suspend(entries, matches);
	REQUIRE_FALSE(draft.IsActive());
	REQUIRE(entries[0].at("value") == 1);
	REQUIRE(entries[2].at("value") == 30);
	REQUIRE(draft.HasChanges(entries, matches));
	entries[2]["value"] = 31;
	draft.Resume(entries, matches);
	REQUIRE(draft.IsActive());
	REQUIRE(entries[0].at("value") == 10);
	REQUIRE(entries[0].at("future") == "kept");
	REQUIRE(entries[2].at("value") == 31);
	draft.Accept(entries, matches);
	REQUIRE_FALSE(draft.HasChanges(entries, matches));
	draft.Suspend(entries, matches);
	REQUIRE(entries[0].at("value") == 10);
}

TEST_CASE("Scene drafts retain unsaved additions and deletions until accepted", "[presets][scene-draft]")
{
	using Entry = nlohmann::json;
	const auto matches = [](const Entry& entry) { return entry.at("owner") == "first"; };
	std::vector<Entry> entries{ { { "owner", "other" }, { "value", 1 } } };
	SceneEntryDraft<Entry> draft;
	draft.Begin(entries, matches);
	entries.push_back({ { "owner", "first" }, { "value", 2 } });
	REQUIRE(draft.HasChanges(entries, matches));
	REQUIRE(draft.SavedView(entries, matches).size() == 1);
	draft.Suspend(entries, matches);
	REQUIRE(entries.size() == 1);
	draft.Resume(entries, matches);
	REQUIRE(entries.size() == 2);
	REQUIRE(draft.HasChanges(entries, matches));
	draft.Accept(entries, matches);
	entries.erase(entries.begin() + 1);
	REQUIRE(draft.HasChanges(entries, matches));
	REQUIRE(draft.SavedView(entries, matches).size() == 2);
	draft.Suspend(entries, matches);
	REQUIRE(entries.size() == 2);
	draft.Resume(entries, matches);
	REQUIRE(entries.size() == 1);
	draft.Accept(entries, matches);
	REQUIRE_FALSE(draft.HasChanges(entries, matches));
}

TEST_CASE("Scene draft undo snapshots keep authored contents when the preview closes", "[presets][scene-draft]")
{
	using Entry = nlohmann::json;
	const auto matches = [](const Entry& entry) { return entry.at("owner") == "first"; };
	std::vector<Entry> entries{
		{ { "owner", "first" }, { "value", 1 } },
		{ { "owner", "other" }, { "value", 2 } },
		{ { "owner", "first" }, { "value", 3 } }
	};
	SceneEntryDraft<Entry> draft;
	draft.Begin(entries, matches);
	entries[0]["value"] = 10;
	entries.push_back({ { "owner", "other" }, { "value", 4 } });
	entries.push_back({ { "owner", "first" }, { "value", 5 } });
	const auto authored = draft.WorkingView(entries, matches);
	draft.Suspend(entries, matches);
	REQUIRE(draft.WorkingView(entries, matches) == authored);
	REQUIRE(entries[0].at("value") == 1);
	entries.push_back({ { "owner", "new" }, { "value", 6 } });
	auto nextAuthored = authored;
	nextAuthored.push_back(entries.back());
	REQUIRE(draft.WorkingView(entries, matches) == nextAuthored);
	draft.Resume(entries, matches);
	REQUIRE(entries == nextAuthored);
	entries.erase(entries.begin() + 2);
	const auto removed = entries;
	draft.Suspend(entries, matches);
	REQUIRE(draft.WorkingView(entries, matches) == removed);
	draft.Resume(entries, matches);
	REQUIRE(entries == removed);
	draft.Suspend(entries, matches);
	REQUIRE(draft.WorkingView(entries, matches) == removed);
	entries = authored;
	entries[0]["value"] = 5;
	entries[1]["value"] = 20;
	draft.RestoreWorking(entries, matches);
	REQUIRE_FALSE(draft.IsActive());
	REQUIRE(entries[0].at("value") == 1);
	REQUIRE(entries[1].at("value") == 20);
	REQUIRE(draft.WorkingView(entries, matches)[0].at("value") == 5);
	REQUIRE(draft.SavedView(entries, matches)[0].at("value") == 1);
	draft.Resume(entries, matches);
	REQUIRE(entries[0].at("value") == 5);
	entries[0]["value"] = 10;
	draft.RestoreWorking(entries, matches);
	REQUIRE(entries[0].at("value") == 10);
	REQUIRE(draft.SavedView(entries, matches)[0].at("value") == 1);
	draft.Accept(entries, matches);
	draft.Suspend(entries, matches);
	REQUIRE(entries[0].at("value") == 10);
	REQUIRE_FALSE(draft.HasChanges(entries, matches));
}

TEST_CASE("Render tasks run on the consumer and defer nested requests", "[presets][render-tasks]")
{
	Util::RenderTaskQueue queue;
	const auto owner = std::this_thread::get_id();
	std::vector<int> calls;
	std::thread::id callbackThread;
	std::jthread producer([&]() {
		queue.Enqueue([&]() {
			callbackThread = std::this_thread::get_id();
			calls.push_back(1);
			queue.Enqueue([&]() { calls.push_back(3); });
		});
		queue.Enqueue([&]() { calls.push_back(2); });
	});
	producer.join();
	REQUIRE(calls.empty());
	for (auto& task : queue.TakePending())
		task();
	REQUIRE(callbackThread == owner);
	REQUIRE(calls == std::vector{ 1, 2 });
	for (auto& task : queue.TakePending())
		task();
	REQUIRE(calls == std::vector{ 1, 2, 3 });
	REQUIRE(queue.TakePending().empty());
}

TEST_CASE("Concurrent render task producers retain every request across drains", "[presets][render-tasks]")
{
	Util::RenderTaskQueue queue;
	constexpr size_t producerCount = 4;
	constexpr size_t tasksPerProducer = 64;
	std::array<int, producerCount * tasksPerProducer> calls{};
	std::atomic<size_t> finished{ 0 };
	std::latch start{ 1 };
	std::array<std::jthread, producerCount> producers;
	for (size_t producer = 0; producer < producerCount; ++producer) {
		producers[producer] = std::jthread([&, producer]() {
			start.wait();
			for (size_t task = 0; task < tasksPerProducer; ++task)
				queue.Enqueue([&, index = producer * tasksPerProducer + task]() { ++calls[index]; });
			finished.fetch_add(1, std::memory_order_release);
		});
	}
	start.count_down();
	while (finished.load(std::memory_order_acquire) != producerCount) {
		for (auto& task : queue.TakePending())
			task();
		std::this_thread::yield();
	}
	for (auto& producer : producers)
		producer.join();
	for (auto& task : queue.TakePending())
		task();
	for (const auto count : calls)
		REQUIRE(count == 1);
	REQUIRE(queue.TakePending().empty());
}

TEST_CASE("Preset state retains metadata and ordered enabled layers", "[presets]")
{
	const json input{
		{ "activePackId", "Landscape" },
		{ "baselinePackIds", { "Foundation", "Landscape", "Foundation", "Night" } },
		{ "pipeline", "future-pipeline" },
		{ "pipelineRevision", 12 },
		{ "sceneControl", { { "Landscape", { { "futureSetting", true } } } } }
	};
	const auto result = PresetState::Normalize(input);
	REQUIRE(result["activePackId"] == "Landscape");
	REQUIRE(result["baselinePackIds"] == json::array({ "Foundation", "Landscape", "Night" }));
	REQUIRE(result["pipeline"] == input["pipeline"]);
	REQUIRE(result["pipelineRevision"] == input["pipelineRevision"]);
	REQUIRE(result["sceneControl"] == input["sceneControl"]);
	REQUIRE(PresetState::Normalize(result) == result);
}

TEST_CASE("Primary preset files shadow complete fallback documents before parsing", "[presets]")
{
	const auto directory = std::filesystem::temp_directory_path() /
	                       std::format("preset_roots_{}_{}", ::GetCurrentProcessId(), std::chrono::steady_clock::now().time_since_epoch().count());
	struct RemoveDirectory
	{
		std::filesystem::path path;
		~RemoveDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	} cleanup{ directory };
	const std::array roots{ directory / "primary", directory / "fallback" };
	const std::filesystem::path relative = "Weather/Target/Pack_Settings.json";
	const auto write = [&](const std::filesystem::path& path, std::string_view data) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << data;
		REQUIRE(output.good());
	};
	write(roots[0] / relative, R"({"Value":1})");
	write(roots[1] / relative, R"({"Value":2,"FallbackOnly":3})");
	const auto primary = PresetState::FindFirstExistingPayload(roots, relative);
	REQUIRE(primary == roots[0] / relative);

	write(primary, "{invalid");
	REQUIRE(PresetState::FindFirstExistingPayload(roots, relative) == primary);
	std::filesystem::remove(primary);
	REQUIRE(PresetState::FindFirstExistingPayload(roots, relative) == roots[1] / relative);
	REQUIRE(PresetState::FindFirstExistingPayload(roots, "missing.json").empty());
	REQUIRE(PresetState::FindFirstExistingPayload(roots, "../outside.json").empty());
	REQUIRE(PresetState::FindFirstExistingPayload(roots, roots[0] / relative).empty());
}

TEST_CASE("Malformed preset state cannot escape the preset root", "[presets]")
{
	for (const json invalid : { json(nullptr), json(7), json("pack"), json::array() }) {
		const auto result = PresetState::Normalize(invalid);
		REQUIRE(result["activePackId"] == "");
		REQUIRE(result["baselinePackIds"] == json::array());
	}
	const auto normalized = PresetState::Normalize({ { "activePackId", "../outside" },
		{ "baselinePackIds", { "Good Pack", "../outside", ".", "_private", "C:\\outside", "a/b", "a\\b", 1, nullptr, "Good Pack" } } });
	REQUIRE(normalized["activePackId"] == "");
	REQUIRE(normalized["baselinePackIds"] == json::array({ "Good Pack" }));
}

TEST_CASE("Preset state retains opaque provider selections", "[presets]")
{
	for (const auto* id : { "provider:Imported", "legacy:Existing" })
		REQUIRE(PresetState::Normalize({ { "activePackId", id } })["activePackId"] == id);
	for (const auto* id : { ":", "provider:", ":pack", "provider:../outside", "provider:pack:extra", "bad/name:pack", "provider:COM1" })
		REQUIRE(PresetState::Normalize({ { "activePackId", id } })["activePackId"] == "");
}

TEST_CASE("Preset normalization validates provider ids without losing independent fields", "[presets]")
{
	const auto registered = [](std::string_view id) { return id == "provider:library:selection"; };
	const json settings{
		{ "activePackId", 42 },
		{ "baselinePackIds", { "Foundation", "Foundation" } },
		{ "pipeline", "Vanilla" },
		{ "sceneControl", { { "Foundation", { { "enabled", true } } } } }
	};
	const auto normalized = PresetState::Normalize(settings, registered);
	REQUIRE(normalized["activePackId"] == "");
	REQUIRE(normalized["baselinePackIds"] == json::array({ "Foundation" }));
	REQUIRE(normalized["pipeline"] == settings["pipeline"]);
	REQUIRE(normalized["sceneControl"] == settings["sceneControl"]);
	REQUIRE(PresetState::Normalize({ { "activePackId", "provider:library:selection" } }, registered)["activePackId"] == "provider:library:selection");
}

TEST_CASE("Normalized feature edits are distinguished from legacy migration input", "[presets]")
{
	constexpr auto name = "Active Settings";
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(json::object(), name));
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(json(nullptr), name));
	for (const json version : { json(true), json("1"), json(PresetState::kFeatureSettingsVersion + 1) })
		REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits({ { PresetState::kFeatureSettingsVersionKey, version },
																 { PresetState::kNormalizedFeatureNamesKey, { name } } },
			name));
	const json state{ { PresetState::kFeatureSettingsVersionKey, PresetState::kFeatureSettingsVersion }, { "activePackId", "Portable" } };
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(PresetState::Normalize(state), name));
}

TEST_CASE("Saving normalized features retains inactive legacy migration status", "[presets]")
{
	json state{ { "activePackId", "Portable" }, { "futureMetadata", { { "value", 7 } } } };
	PresetState::MarkNormalizedFeatureEdits(state, "Active Settings");
	REQUIRE(PresetState::HasNormalizedFeatureEdits(state, "Active Settings"));
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(state, "Dormant Settings"));
	REQUIRE(state["futureMetadata"]["value"] == 7);

	auto nextSave = PresetState::Normalize(state);
	PresetState::MarkNormalizedFeatureEdits(nextSave, "Other Settings");
	PresetState::MarkNormalizedFeatureEdits(nextSave, "Other Settings");
	REQUIRE(PresetState::HasNormalizedFeatureEdits(nextSave, "Active Settings"));
	REQUIRE(PresetState::HasNormalizedFeatureEdits(nextSave, "Other Settings"));
	REQUIRE_FALSE(PresetState::HasNormalizedFeatureEdits(nextSave, "Dormant Settings"));
	REQUIRE(nextSave[PresetState::kNormalizedFeatureNamesKey].size() == 2);

	PresetState::MarkNormalizedFeatureEdits(nextSave, "Dormant Settings");
	REQUIRE(PresetState::HasNormalizedFeatureEdits(nextSave, "Dormant Settings"));
}

TEST_CASE("Preset identifiers reject Windows device names and ambiguous components", "[presets]")
{
	for (const auto* id : { "CON", "nul.json", "COM1", "LPT9.json", "trailing.", "trailing ", "a\nb", "a\tb" })
		REQUIRE_FALSE(PresetState::IsPackId(id));
	REQUIRE(PresetState::IsPackId("Day and Night"));
	REQUIRE(PresetState::IsPackId("My.Preset"));
}

TEST_CASE("Baseline shape validation accepts partial trees and complete arrays", "[presets]")
{
	const auto accepts = [](const json& values, const json& known) {
		std::vector<std::string> rejected;
		Util::Settings::CollectUnknownSettingKeys(values, known, "", rejected);
		return rejected.empty();
	};
	const json known{
		{ "Lighting", { { "Intensity", 1.0 }, { "Enabled", true } } },
		{ "Tint", { 1.0, 1.0, 1.0 } },
		{ "Asset", "default.dds" }
	};
	REQUIRE(accepts({ { "Lighting", { { "Intensity", 2 } } } }, known));
	REQUIRE(accepts({ { "Tint", { 1, 0.5, 0 } } }, known));
	REQUIRE(accepts({ { "Asset", "preset.dds" } }, known));
	REQUIRE_FALSE(accepts({ { "Lighting", { { "Unknown", 1.0 } } } }, known));
	REQUIRE_FALSE(accepts({ { "Lighting", false } }, known));
	REQUIRE_FALSE(accepts({ { "Tint", { 1.0, 0.5 } } }, known));
	REQUIRE_FALSE(accepts({ { "Tint", { 1.0, "bad", 0.0 } } }, known));
	REQUIRE_FALSE(accepts({ { "Lighting", { { "Enabled", 1 } } } }, known));
}

TEST_CASE("Baseline numeric validation prevents unsafe integer conversions", "[presets]")
{
	const auto accepts = [](const json& values, const json& known) {
		std::vector<std::string> rejected;
		Util::Settings::CollectUnknownSettingKeys(values, known, "", rejected);
		return rejected.empty();
	};
	const json unsignedKnown{ { "Count", std::uint32_t{ 1 } } };
	const json signedKnown{ { "Count", std::int32_t{ 1 } } };
	REQUIRE(accepts({ { "Count", 2 } }, unsignedKnown));
	REQUIRE_FALSE(accepts({ { "Count", -1 } }, unsignedKnown));
	REQUIRE_FALSE(accepts({ { "Count", 1.5 } }, unsignedKnown));
	REQUIRE_FALSE(accepts({ { "Count", 1.5 } }, signedKnown));
	REQUIRE_FALSE(accepts({ { "Count", "2" } }, signedKnown));
	REQUIRE_FALSE(accepts({ { "Count", std::uint64_t{ 4294967296 } } }, unsignedKnown));
	REQUIRE_FALSE(accepts({ { "Count", std::int64_t{ 2147483648 } } }, signedKnown));
}

TEST_CASE("Baseline switching retains personal deltas without serializing pack values", "[presets]")
{
	const json defaults{ { "Intensity", 1.0 }, { "Tint", { 1.0, 1.0, 1.0 } }, { "Enabled", true } };
	auto previousBase = defaults;
	previousBase.update({ { "Intensity", 2.0 } }, true);
	auto current = previousBase;
	current["Enabled"] = false;
	const auto personal = Util::Settings::BuildUserOverride(current, previousBase);
	REQUIRE(personal == json{ { "Enabled", false } });
	auto nextBase = defaults;
	nextBase.update({ { "Intensity", 3.0 }, { "Tint", { 0.2, 0.3, 0.4 } } }, true);
	auto resolved = nextBase;
	resolved.update(personal, true);
	REQUIRE(resolved["Intensity"] == 3.0);
	REQUIRE(resolved["Tint"] == nextBase["Tint"]);
	REQUIRE(resolved["Enabled"] == false);
	REQUIRE(Util::Settings::BuildUserOverride(resolved, nextBase) == personal);
}

TEST_CASE("Saving personal settings retains explicit values equal to the active Baseline", "[presets]")
{
	const json base{ { "Group", { { "Intensity", 2.0 }, { "Contrast", 1.0 } } }, { "Enabled", true } };
	const json savedKeys{ { "Group", { { "Intensity", 2.0 }, { "Removed", 7 } } } };
	const auto saved = Util::Settings::BuildUserOverride(base, base, savedKeys);
	REQUIRE(saved == json{ { "Group", { { "Intensity", 2.0 } } } });
	auto nextBase = base;
	nextBase["Group"]["Intensity"] = 3.0;
	nextBase["Group"]["Contrast"] = 1.5;
	auto resolved = nextBase;
	resolved.update(saved, true);
	REQUIRE(resolved["Group"]["Intensity"] == 2.0);
	REQUIRE(resolved["Group"]["Contrast"] == 1.5);
	REQUIRE(Util::Settings::BuildUserOverride(resolved, nextBase, saved) == saved);
}

TEST_CASE("Saving explicit boot preferences retains intent when the Baseline matches", "[presets]")
{
	const json base{ { "Optional", false }, { "Other", false } };
	const json preference{ { "Optional", false } };
	const auto saved = Util::Settings::BuildUserOverride(base, base, preference);
	REQUIRE(saved == preference);
	auto nextBase = base;
	nextBase["Optional"] = true;
	auto resolved = nextBase;
	resolved.update(saved, true);
	REQUIRE(resolved["Optional"] == false);
	REQUIRE_FALSE(saved.contains("Other"));
}

TEST_CASE("Climate period blending remains normalized through midnight and zero-width transitions", "[presets]")
{
	const std::array boundaries{ 5.0f, 7.5f, 10.0f, 16.0f, 18.5f, 21.0f };
	for (const auto width : { 0.0f, 1.0f, 2.0f }) {
		for (const auto ease : { 0.0f, 0.5f, 1.0f }) {
			for (int tick = 0; tick <= 96; ++tick) {
				const auto factors = PresetState::BlendPeriods(boundaries, width, ease, static_cast<float>(tick) * 0.25f);
				float sum = 0.0f;
				for (const auto factor : factors) {
					REQUIRE(std::isfinite(factor));
					REQUIRE(factor >= 0.0f);
					REQUIRE(factor <= 1.0f);
					sum += factor;
				}
				REQUIRE(sum == Catch::Approx(1.0f));
			}
		}
	}
	REQUIRE(PresetState::BlendPeriods(boundaries, 1.0f, 1.0f, 0.0f)[5] == 1.0f);
	REQUIRE(PresetState::BlendPeriods(boundaries, 1.0f, 1.0f, 12.0f)[2] == 1.0f);
	REQUIRE(PresetState::BlendPeriods(boundaries, 1.0f, 1.0f, 6.0f)[0] == 1.0f);
}

TEST_CASE("Climate period midpoints preview the selected phase", "[presets]")
{
	const std::array boundaries{ 5.0f, 7.5f, 10.0f, 16.0f, 18.5f, 21.0f };
	for (const auto width : { 0.0f, 1.0f, 2.0f }) {
		const auto blended = PresetState::BlendPeriodBoundaries(boundaries, width);
		for (size_t index = 0; index < boundaries.size(); ++index) {
			const auto end = index + 1 < boundaries.size() ? blended.starts[index + 1] : blended.starts.front() + 24.0f;
			const auto hour = std::fmod((blended.starts[index] + end) * 0.5f, 24.0f);
			const auto factors = PresetState::BlendPeriods(boundaries, width, 1.0f, hour);
			REQUIRE(factors[index] == Catch::Approx(1.0f));
		}
	}
}
