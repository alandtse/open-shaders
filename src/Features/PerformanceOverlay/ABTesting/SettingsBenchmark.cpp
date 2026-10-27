#include "SettingsBenchmark.h"

#include "ABTesting.h"
#include "Feature.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "SettingsPatch.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.perf_overlay.benchmark."

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <imgui.h>

namespace SettingsBenchmark
{
	namespace
	{
		constexpr float kVrFrameBudgetMs = 1000.0f / 90.0f;
		constexpr uint32_t kMaxWarmupFrames = 600;
		constexpr uint32_t kMaxMeasureFrames = 1800;
		constexpr uint32_t kMaxRepeats = 10;
		constexpr float kMaxCountdownSeconds = 30.0f;

		struct Run
		{
			Engine engine;
			json original;
			std::vector<Variant> variants;  ///< The variants of this run, in engine order.
			std::vector<Result> results;
			std::string error;
			std::filesystem::path resultPath;
			std::chrono::steady_clock::time_point lastTick;
			uint32_t lastCompositorFrame = UINT32_MAX;
		};

		Run run;
		Config config;
		std::vector<char> selected;

		json Settings(const char* text)
		{
			return json::parse(text);
		}

		Variant NrVariant(std::string id, std::string label, const char* tuning, std::string note)
		{
			return { std::move(id), std::move(label), "Neural Rendering", std::move(note),
				{ { "Upscaling", json{ { "neuralRenderingTuning", Settings(tuning) } } } } };
		}

		std::vector<Variant> BuildVariants()
		{
			return {
				NrVariant("nr-eye-stagger-on", "Eye-staggered reuse on", R"({"skipFrameReuse":true})",
					"Evaluates the model for one eye per frame and reuses the other eye's reprojected gain."),
				NrVariant("nr-eye-stagger-off", "Eye-staggered reuse off", R"({"skipFrameReuse":false})",
					"Evaluates the model for both eyes every frame."),
				NrVariant("nr-parallel-eyes-off", "Parallel eye queue off", R"({"parallelEyes":false})",
					"Runs both eyes' model evaluations on one queue."),
				NrVariant("nr-follow-foveation-off", "Follow foveation off", R"({"regionFollowFoveation":false})",
					"Evaluates the whole eye instead of only the foveated region."),
				NrVariant("nr-actor-crop-on", "Actor crop on", R"({"regionOfInterest":true})",
					"Restricts evaluation to a crop around the most prominent visible actor."),
				NrVariant("nr-model-preset-newest", "Newest model preset", R"({"modelPreset":1})",
					"Uses the newest network a 310.9 or newer runtime ships."),
			};
		}

		/** @brief True when every key in `patch` already exists in `base`, so the patch cannot silently do nothing. */
		bool PatchApplies(const json& base, const json& patch, std::string& missing, const std::string& path)
		{
			if (!patch.is_object())
				return true;
			if (!base.is_object()) {
				missing = path;
				return false;
			}
			for (const auto& [key, value] : patch.items()) {
				const auto child = path + "/" + key;
				const auto it = base.find(key);
				if (it == base.end()) {
					missing = child;
					return false;
				}
				if (!PatchApplies(*it, value, missing, child))
					return false;
			}
			return true;
		}

		Feature* FindFeature(const std::string& shortName)
		{
			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->GetShortName() == shortName)
					return feature;
			}
			return nullptr;
		}

		void ApplySettings(const json& settings)
		{
			auto copy = settings;
			globals::state->LoadFromJson(copy);
		}

		void ApplyVariant(int engineVariant)
		{
			if (engineVariant == kBaseline) {
				ApplySettings(run.original);
				return;
			}

			auto merged = run.original;
			const auto& variant = run.variants[engineVariant];
			for (const auto& patch : variant.patches) {
				if (auto* feature = FindFeature(patch.feature))
					merged[feature->GetName()].merge_patch(patch.settings);
			}
			ApplySettings(merged);
		}

		json ResultJson(const Result& result)
		{
			return { { "id", result.id },
				{ "label", result.label },
				{ "group", result.group },
				{ "error", result.error },
				{ "gpuDeltaMs", result.gpuDeltaMs },
				{ "gpuUncertaintyMs", result.gpuUncertaintyMs },
				{ "gpuVariantMs", result.gpuVariantMs },
				{ "gpuBaselineMs", result.gpuBaselineMs },
				{ "gpuDeltaPercentOfBudget", result.gpuDeltaMs / kVrFrameBudgetMs * 100.0f },
				{ "cpuDeltaMs", result.cpuDeltaMs },
				{ "repeats", result.repeats } };
		}

		json ConfigJson()
		{
			return { { "warmupFrames", config.warmupFrames },
				{ "measureFrames", config.measureFrames },
				{ "repeats", config.repeats },
				{ "countdownSeconds", config.countdownSeconds } };
		}

		void WriteResults()
		{
			json out{ { "runtime", "vr" },
				{ "frameBudgetMs", kVrFrameBudgetMs },
				{ "config", ConfigJson() },
				{ "results", json::array() } };
			for (const auto& result : run.results)
				out["results"].push_back(ResultJson(result));

			run.resultPath = Util::PathHelpers::GetLogPath().parent_path() /
			                 std::format("{}-SettingsBenchmark.json", Plugin::LOG_NAME);
			std::ofstream file(run.resultPath);
			if (!file || !(file << out.dump(2))) {
				logger::warn("[SettingsBenchmark] Could not write {}", run.resultPath.string());
				run.resultPath.clear();
			}
		}

		void Conclude(const Directive& directive)
		{
			if (!directive.restore)
				return;

			ApplySettings(run.original);
			const auto phase = run.engine.GetPhase();
			if (phase == Phase::Finished) {
				run.results = run.engine.Results();
				WriteResults();
				logger::info("[SettingsBenchmark] Finished {} variants, results in {}", run.results.size(), run.resultPath.string());
			} else if (phase == Phase::Failed) {
				run.error = run.engine.FailureReason();
				logger::warn("[SettingsBenchmark] Failed: {}", run.error);
			} else {
				logger::info("[SettingsBenchmark] Cancelled, settings restored");
			}
		}

		bool CanMeasure()
		{
			auto* menu = Menu::GetSingleton();
			return !globals::state->IsMainOrLoadingMenuOpen() && !(menu && menu->IsEnabled);
		}
	}

	const std::vector<Variant>& Variants()
	{
		static const std::vector<Variant> variants = BuildVariants();
		return variants;
	}

	Config& Configuration()
	{
		return config;
	}

	bool IsRunning()
	{
		return run.engine.IsRunning();
	}

	namespace
	{
		bool Fail(std::string* error, std::string message)
		{
			if (!IsRunning())
				run.error = message;
			if (error)
				*error = std::move(message);
			return false;
		}

		bool StartVariants(std::vector<Variant> variants, std::string* error)
		{
			if (!globals::game::isVR)
				return Fail(error, "The settings benchmark needs VR: it reads the SteamVR compositor's frame timing.");
			if (IsRunning())
				return Fail(error, "A settings benchmark is already running.");
			if (ABTestingManager::GetSingleton()->IsEnabled())
				return Fail(error, "Stop the A/B test first; both swap settings.");

			json snapshot = json::object();
			globals::state->SaveToJson(snapshot);

			std::vector<VariantInfo> infos;
			for (const auto& variant : variants) {
				for (const auto& patch : variant.patches) {
					auto* feature = FindFeature(patch.feature);
					std::string missing;
					if (!feature || !PatchApplies(snapshot[feature->GetName()], patch.settings, missing, "/" + patch.feature))
						return Fail(error, std::format("Variant {} does not apply: setting {} is not present.", variant.id, feature ? missing : patch.feature));
				}
				infos.push_back({ variant.id, variant.label, variant.group });
			}

			auto effective = config;
			effective.warmupFrames = std::min(effective.warmupFrames, kMaxWarmupFrames);
			effective.measureFrames = std::clamp(effective.measureFrames, kMinimumValidFrames, kMaxMeasureFrames);
			effective.repeats = std::clamp<uint32_t>(effective.repeats, 1, kMaxRepeats);
			effective.countdownSeconds = std::clamp(effective.countdownSeconds, 0.0f, kMaxCountdownSeconds);

			run = {};
			run.original = std::move(snapshot);
			run.variants = std::move(variants);
			if (!run.engine.Start(std::move(infos), effective))
				return Fail(error, "There is nothing to run.");

			run.lastTick = std::chrono::steady_clock::now();
			logger::info("[SettingsBenchmark] Started {} variants, {} frames each, {} repeats", run.variants.size(), effective.measureFrames, effective.repeats);
			return true;
		}
	}

	bool Start(const std::vector<std::string>& variantIds, std::string* error)
	{
		const auto& builtIn = Variants();
		std::vector<Variant> chosen;
		if (variantIds.empty()) {
			chosen = builtIn;
		} else {
			for (const auto& id : variantIds) {
				const auto it = std::ranges::find(builtIn, id, &Variant::id);
				if (it == builtIn.end())
					return Fail(error, "Unknown variant: " + id);
				chosen.push_back(*it);
			}
		}
		return StartVariants(std::move(chosen), error);
	}

	bool StartForSetting(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const json& target, std::string label, std::string* error)
	{
		json current;
		feature.SaveSettings(current);
		auto changed = current;
		auto* slot = Util::Settings::GetCatalogSerializedValue(changed, setting);
		if (!slot)
			return Fail(error, "The setting is not present in the feature's saved settings.");
		*slot = target;

		Variant variant{ std::format("setting:{}/{}/{}", setting.featureShortName, setting.serializedPath, setting.serializedKey), std::move(label), "Setting",
			"Changes this one setting; everything else stays as it is now.",
			{ { std::string(setting.featureShortName), MakePatch(current, changed) } } };
		return StartVariants({ std::move(variant) }, error);
	}

	bool StartForSettingKey(const std::string& featureShortName, const std::string& key, const std::string& path, const json& value, std::string* error)
	{
		auto* feature = FindFeature(featureShortName);
		if (!feature)
			return Fail(error, "Unknown feature: " + featureShortName);

		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName == featureShortName && setting.serializedKey == key && setting.serializedPath == path) {
				const auto label = Util::Settings::GetCatalogLeafDisplayName(setting) + " = " + value.dump();
				return StartForSetting(*feature, setting, value, label, error);
			}
		}
		return Fail(error, std::format("No catalogued setting {}/{}/{}.", featureShortName, path, key));
	}

	void Cancel()
	{
		Conclude(run.engine.Cancel());
	}

	void Tick()
	{
		if (!run.engine.IsRunning())
			return;

		const auto now = std::chrono::steady_clock::now();
		const float delta = std::chrono::duration<float>(now - run.lastTick).count();
		run.lastTick = now;

		CompositorSample frame;
		const bool hasSample = ReadLatestCompositorSample(frame, run.lastCompositorFrame);
		const auto directive = run.engine.Tick({ frame.totalRenderGpuMs, frame.compositorRenderCpuMs }, hasSample, delta, CanMeasure());
		if (directive.apply)
			ApplyVariant(directive.variant);
		Conclude(directive);
	}

	json StatusJson()
	{
		const auto progress = run.engine.GetProgress();
		json results = json::array();
		for (const auto& result : run.results)
			results.push_back(ResultJson(result));

		json variants = json::array();
		for (const auto& variant : Variants())
			variants.push_back({ { "id", variant.id }, { "label", variant.label }, { "group", variant.group }, { "note", variant.note } });

		return { { "phase", PhaseName(progress.phase) },
			{ "running", run.engine.IsRunning() },
			{ "block", progress.block },
			{ "blockCount", progress.blockCount },
			{ "framesInBlock", progress.framesInBlock },
			{ "framesNeeded", progress.framesNeeded },
			{ "countdownRemaining", progress.countdownRemaining },
			{ "currentVariant", progress.currentVariant == kBaseline ? json("baseline") : json(run.variants[progress.currentVariant].id) },
			{ "error", run.error },
			{ "resultPath", run.resultPath.string() },
			{ "config", ConfigJson() },
			{ "variants", std::move(variants) },
			{ "results", std::move(results) } };
	}

	void DrawOverlayUI()
	{
		if (!run.engine.IsRunning())
			return;

		const auto progress = run.engine.GetProgress();
		const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * Util::GetUIScale();
		ImGui::SetNextWindowBgAlpha(0.85f);
		ImGui::SetNextWindowPos(ImVec2(pos, pos));
		constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
		                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs;
		if (!ImGui::Begin("Settings Benchmark", nullptr, flags)) {
			ImGui::End();
			return;
		}

		if (progress.phase == Phase::Countdown) {
			ImGui::Text(T(TKEY("overlay_countdown"), "Settings benchmark starts in %.0f s. Close the menu."), progress.countdownRemaining);
		} else {
			const auto current = progress.currentVariant == kBaseline ? std::string(T(TKEY("baseline"), "Baseline")) : run.variants[progress.currentVariant].label;
			ImGui::Text(T(TKEY("overlay_progress"), "Settings benchmark: %s (block %u of %u)"), current.c_str(), progress.block, progress.blockCount);
			if (!CanMeasure())
				ImGui::TextUnformatted(T(TKEY("overlay_paused"), "Paused while a menu is open"));
		}
		ImGui::End();
	}

	void DrawPanel()
	{
		const auto& variants = Variants();
		if (selected.size() != variants.size())
			selected.assign(variants.size(), 1);

		ImGui::Spacing();
		ImGui::SeparatorText(T(TKEY("title"), "Settings Benchmark"));

		if (!globals::game::isVR) {
			ImGui::TextDisabled("%s", T(TKEY("vr_only"), "VR only: the benchmark reads the SteamVR compositor's frame timing."));
			return;
		}

		ImGui::TextWrapped("%s", T(TKEY("description"),
									 "Applies each selected variant over your current settings, measures the application GPU time between baseline blocks, then restores your settings. Close the menu while it runs."));

		const bool running = IsRunning();
		ImGui::BeginDisabled(running);
		std::string group;
		for (size_t i = 0; i < variants.size(); ++i) {
			if (variants[i].group != group) {
				group = variants[i].group;
				ImGui::TextUnformatted(group.c_str());
			}
			bool checked = selected[i] != 0;
			if (ImGui::Checkbox(variants[i].label.c_str(), &checked))
				selected[i] = checked ? 1 : 0;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(variants[i].note.c_str());
		}

		int warmup = static_cast<int>(config.warmupFrames);
		int measure = static_cast<int>(config.measureFrames);
		int repeats = static_cast<int>(config.repeats);
		if (ImGui::SliderInt(T(TKEY("warmup_frames"), "Warm-up frames"), &warmup, 0, static_cast<int>(kMaxWarmupFrames)))
			config.warmupFrames = static_cast<uint32_t>(warmup);
		if (ImGui::SliderInt(T(TKEY("measure_frames"), "Measured frames"), &measure, static_cast<int>(kMinimumValidFrames), static_cast<int>(kMaxMeasureFrames)))
			config.measureFrames = static_cast<uint32_t>(measure);
		if (ImGui::SliderInt(T(TKEY("repeats"), "Repeats"), &repeats, 1, static_cast<int>(kMaxRepeats)))
			config.repeats = static_cast<uint32_t>(repeats);
		ImGui::SliderFloat(T(TKEY("countdown"), "Countdown (s)"), &config.countdownSeconds, 0.0f, kMaxCountdownSeconds, "%.0f");
		ImGui::EndDisabled();

		if (!running) {
			if (ImGui::Button(T(TKEY("start"), "Start Benchmark"))) {
				std::vector<std::string> ids;
				for (size_t i = 0; i < variants.size(); ++i) {
					if (selected[i])
						ids.push_back(variants[i].id);
				}
				if (ids.empty())
					run.error = T(TKEY("none_selected"), "Select at least one variant.");
				else
					Start(ids, nullptr);
			}
		} else if (ImGui::Button(T(TKEY("cancel"), "Cancel"))) {
			Cancel();
		}

		const auto progress = run.engine.GetProgress();
		if (running) {
			if (progress.phase == Phase::Countdown) {
				ImGui::Text(T(TKEY("starting_in"), "Starting in %.0f s"), progress.countdownRemaining);
			} else {
				const auto current = progress.currentVariant == kBaseline ?
				                         std::string(T(TKEY("baseline"), "Baseline")) :
				                         run.variants[progress.currentVariant].label;
				ImGui::Text(T(TKEY("progress"), "%s: block %u of %u"), current.c_str(), progress.block, progress.blockCount);
				const float fraction = progress.framesNeeded ? static_cast<float>(progress.framesInBlock) / static_cast<float>(progress.framesNeeded) : 0.0f;
				ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1.0f, 0.0f));
			}
		}

		if (!run.error.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", run.error.c_str());

		if (run.results.empty())
			return;

		if (ImGui::BeginTable("##SettingsBenchmarkResults", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn(T(TKEY("col_variant"), "Variant"));
			ImGui::TableSetupColumn(T(TKEY("col_gpu_delta"), "GPU delta (ms)"));
			ImGui::TableSetupColumn(T(TKEY("col_budget"), "% of 11.1 ms"));
			ImGui::TableSetupColumn(T(TKEY("col_cpu_delta"), "Compositor CPU delta (ms)"));
			ImGui::TableSetupColumn(T(TKEY("col_baseline"), "Baseline (ms)"));
			ImGui::TableHeadersRow();
			for (const auto& result : run.results) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(result.label.c_str());
				if (!result.error.empty()) {
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", result.error.c_str());
					continue;
				}
				ImGui::TableNextColumn();
				ImGui::Text("%+.2f +/- %.2f", result.gpuDeltaMs, result.gpuUncertaintyMs);
				ImGui::TableNextColumn();
				ImGui::Text("%+.1f%%", result.gpuDeltaMs / kVrFrameBudgetMs * 100.0f);
				ImGui::TableNextColumn();
				ImGui::Text("%+.2f", result.cpuDeltaMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", result.gpuBaselineMs);
			}
			ImGui::EndTable();
		}

		if (!run.resultPath.empty())
			ImGui::TextDisabled("%s", run.resultPath.string().c_str());
	}
}
#undef I18N_KEY_PREFIX
