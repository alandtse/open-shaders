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
		constexpr float kFlatFrameBudgetMs = 1000.0f / 60.0f;
		constexpr uint32_t kMaxWarmupFrames = 600;
		constexpr uint32_t kMaxMeasureFrames = 1800;
		constexpr uint32_t kMaxRepeats = 10;
		constexpr float kMaxCountdownSeconds = 30.0f;
		constexpr float kResultsCardSeconds = 60.0f;

		/** @brief Where a run takes its per-frame timing: the SteamVR compositor in VR, the profiler on flat. */
		enum class TimingSource
		{
			Compositor,
			Profiler,
		};

		TimingSource CurrentSource()
		{
			return globals::game::isVR ? TimingSource::Compositor : TimingSource::Profiler;
		}

		float BudgetMs(TimingSource source)
		{
			return source == TimingSource::Compositor ? kVrFrameBudgetMs : kFlatFrameBudgetMs;
		}

		struct Run
		{
			Engine engine;
			json original;
			std::vector<Variant> variants;  ///< The variants of this run, in engine order.
			std::vector<Result> results;
			std::string error;
			std::filesystem::path resultPath;
			std::chrono::steady_clock::time_point finishedAt;
			std::chrono::steady_clock::time_point lastTick;
			uint32_t lastCompositorFrame = UINT32_MAX;
			TimingSource source = TimingSource::Compositor;
			bool profilerWasEnabled = false;
			uint32_t lastProfilerFrame = UINT32_MAX;
		};

		Run run;
		Config config;
		std::vector<char> selected;

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
				{ "gpuDeltaPercentOfBudget", result.gpuDeltaMs / BudgetMs(run.source) * 100.0f },
				{ "cpuDeltaMs", result.cpuDeltaMs },
				{ "frameDeltaMs", result.frameDeltaMs },
				{ "frameBaselineMs", result.frameBaselineMs },
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
			json out{ { "runtime", globals::game::isVR ? "vr" : "flat" },
				{ "timingSource", run.source == TimingSource::Compositor ? "compositor" : "profiler" },
				{ "frameBudgetMs", BudgetMs(run.source) },
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
			if (run.source == TimingSource::Profiler && !run.profilerWasEnabled && globals::profiler)
				globals::profiler->SetUserEnabled(false);
			const auto phase = run.engine.GetPhase();
			if (phase == Phase::Finished) {
				run.results = run.engine.Results();
				run.finishedAt = std::chrono::steady_clock::now();
				WriteResults();
				logger::info("[SettingsBenchmark] Finished {} variants, results in {}", run.results.size(), run.resultPath.string());
			} else if (phase == Phase::Failed) {
				run.error = run.engine.FailureReason();
				logger::warn("[SettingsBenchmark] Failed: {}", run.error);
			} else {
				logger::info("[SettingsBenchmark] Cancelled, settings restored");
			}
		}

		/**
		 * @brief Requests a profiler capture and reads its newest resolved totals.
		 *
		 * Capture is request-driven and the GPU result trails the frame by a few frames, so a sample is only
		 * returned when the resolved frame count has moved since `lastFrame`.
		 */
		bool ReadProfilerSample(uint32_t& lastFrame, float frameMs, Sample& sample)
		{
			auto* profiler = globals::profiler;
			profiler->RequestCapture(Profiler::CaptureMode::Both);
			const uint32_t captured = profiler->GetCapturedGpuFrameCount();
			if (captured == 0 || captured == lastFrame)
				return false;

			lastFrame = captured;
			sample = { profiler->GetResolvedTotalTimeMs(), profiler->GetResolvedCpuTotalTimeMs(), frameMs };
			return true;
		}

		bool CanMeasure()
		{
			auto* menu = Menu::GetSingleton();
			return !globals::state->IsMainOrLoadingMenuOpen() && !(menu && menu->IsEnabled);
		}
	}

	bool Available(const Variant& variant)
	{
		return globals::game::isVR || !variant.vrOnly;
	}

	const std::vector<Variant>& Variants()
	{
		static const std::vector<Variant> variants = [] {
			std::vector<Variant> list;
			for (auto* feature : Feature::GetFeatureList()) {
				if (!feature->loaded)
					continue;
				for (auto& offered : feature->GetBenchmarkVariants())
					list.push_back({ std::move(offered.id), std::move(offered.label), std::move(offered.group), std::move(offered.note),
						{ { feature->GetShortName(), std::move(offered.settings) } }, offered.vrOnly });
			}
			return list;
		}();
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
			if (!globals::game::isVR && !globals::profiler)
				return Fail(error, "The profiler is not available.");
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

			run.source = CurrentSource();
			if (run.source == TimingSource::Profiler) {
				run.profilerWasEnabled = globals::profiler->IsUserEnabled();
				globals::profiler->SetUserEnabled(true);
			}
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
			std::ranges::copy_if(builtIn, std::back_inserter(chosen), Available);
		} else {
			for (const auto& id : variantIds) {
				const auto it = std::ranges::find(builtIn, id, &Variant::id);
				if (it == builtIn.end())
					return Fail(error, "Unknown variant: " + id);
				if (!Available(*it))
					return Fail(error, "Variant " + id + " needs VR.");
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

		Sample sample;
		bool hasSample = false;
		if (run.source == TimingSource::Compositor) {
			CompositorSample frame;
			hasSample = ReadLatestCompositorSample(frame, run.lastCompositorFrame);
			sample = { frame.applicationGpuMs, frame.compositorRenderCpuMs, frame.clientFrameIntervalMs };
		} else {
			hasSample = ReadProfilerSample(run.lastProfilerFrame, delta * 1000.0f, sample);
		}
		const auto directive = run.engine.Tick(sample, hasSample, delta, CanMeasure());
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
			variants.push_back({ { "id", variant.id }, { "label", variant.label }, { "group", variant.group }, { "note", variant.note }, { "vrOnly", variant.vrOnly }, { "available", Available(variant) } });

		return { { "phase", PhaseName(progress.phase) },
			{ "running", run.engine.IsRunning() },
			{ "block", progress.block },
			{ "blockCount", progress.blockCount },
			{ "framesInBlock", progress.framesInBlock },
			{ "framesNeeded", progress.framesNeeded },
			{ "countdownRemaining", progress.countdownRemaining },
			{ "currentVariant", progress.currentVariant == kBaseline ? json("baseline") : json(run.variants[progress.currentVariant].id) },
			{ "timingSource", (run.engine.IsRunning() ? run.source : CurrentSource()) == TimingSource::Compositor ? "compositor" : "profiler" },
			{ "frameBudgetMs", BudgetMs(run.engine.IsRunning() ? run.source : CurrentSource()) },
			{ "error", run.error },
			{ "resultPath", run.resultPath.string() },
			{ "config", ConfigJson() },
			{ "variants", std::move(variants) },
			{ "results", std::move(results) } };
	}

	namespace
	{
		const char* VerdictText(Verdict verdict)
		{
			switch (verdict) {
			case Verdict::Faster:
				return T(TKEY("verdict_faster"), "faster");
			case Verdict::Slower:
				return T(TKEY("verdict_slower"), "slower");
			default:
				return T(TKEY("verdict_no_change"), "no measurable change");
			}
		}

		bool ResultsCardVisible()
		{
			if (run.results.empty() || run.engine.IsRunning())
				return false;
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - run.finishedAt).count() < kResultsCardSeconds;
		}

		void DrawResultsCard()
		{
			const float budget = BudgetMs(run.source);
			ImGui::TextUnformatted(T(TKEY("card_title"), "Settings benchmark result"));
			ImGui::Separator();
			for (const auto& result : run.results) {
				if (!result.error.empty()) {
					ImGui::Text("%s: %s", result.label.c_str(), result.error.c_str());
					continue;
				}
				const auto verdict = ClassifyDelta(result);
				const ImVec4 color = verdict == Verdict::Faster ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :
				                     verdict == Verdict::Slower ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f) :
				                                                  ImGui::GetStyleColorVec4(ImGuiCol_Text);
				ImGui::TextColored(color, "%s: %s", result.label.c_str(), VerdictText(verdict));
				ImGui::TextDisabled(T(TKEY("card_detail"), "GPU %+.2f +/- %.2f ms (%+.1f%% of frame budget), CPU %+.2f ms"),
					result.gpuDeltaMs, result.gpuUncertaintyMs, result.gpuDeltaMs / budget * 100.0f, result.cpuDeltaMs);
			}
			ImGui::TextDisabled("%s", T(TKEY("card_hint"), "Full table: Performance page."));
		}
	}

	void DrawOverlayUI()
	{
		const bool running = run.engine.IsRunning();
		if (!running && !ResultsCardVisible())
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

		if (!running) {
			DrawResultsCard();
		} else if (progress.phase == Phase::Countdown) {
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

		ImGui::TextWrapped("%s", T(TKEY("description"),
									 "Applies each selected variant over your current settings, measures the application GPU time between baseline blocks, then restores your settings. Close the menu while it runs."));
		ImGui::TextDisabled("%s", CurrentSource() == TimingSource::Compositor ?
									  T(TKEY("source_compositor"), "Timing: SteamVR compositor application GPU time.") :
									  T(TKEY("source_profiler"), "Timing: GPU time of Open Shaders' own passes (the profiler total); the game's own passes are not included."));

		ImGui::TextWrapped("%s", T(TKEY("right_click_hint"), "To benchmark one setting, right-click it in any feature menu and choose Benchmark vs. The result pops up in the overlay when it finishes."));

		const bool running = IsRunning();
		const bool anyPreset = std::ranges::any_of(variants, [](const Variant& variant) { return Available(variant); });
		ImGui::BeginDisabled(running);
		if (anyPreset && ImGui::CollapsingHeader(T(TKEY("presets"), "Feature presets"))) {
			std::string group;
			for (size_t i = 0; i < variants.size(); ++i) {
				if (!Available(variants[i]))
					continue;
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
			if (anyPreset && ImGui::Button(T(TKEY("start"), "Benchmark selected presets"))) {
				std::vector<std::string> ids;
				for (size_t i = 0; i < variants.size(); ++i) {
					if (selected[i] && Available(variants[i]))
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

		if (ImGui::BeginTable("##SettingsBenchmarkResults", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn(T(TKEY("col_variant"), "Variant"));
			ImGui::TableSetupColumn(T(TKEY("col_gpu_delta"), "GPU delta (ms)"));
			ImGui::TableSetupColumn(T(TKEY("col_budget"), "% of frame budget"));
			ImGui::TableSetupColumn(T(TKEY("col_cpu_delta"), "CPU delta (ms)"));
			ImGui::TableSetupColumn(T(TKEY("col_frame_delta"), "Frame time delta (ms)"));
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
				ImGui::Text("%+.1f%%", result.gpuDeltaMs / BudgetMs(run.source) * 100.0f);
				ImGui::TableNextColumn();
				ImGui::Text("%+.2f", result.cpuDeltaMs);
				ImGui::TableNextColumn();
				ImGui::Text("%+.2f", result.frameDeltaMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", result.gpuBaselineMs);
			}
			ImGui::EndTable();
		}

		ImGui::TextDisabled(T(TKEY("budget_note"), "Frame budget: %.1f ms"), BudgetMs(run.source));
		ImGui::TextDisabled("%s", T(TKEY("frame_note"), "Frame time follows vsync or a frame limiter: a cost below the headroom does not show in it."));
		if (!run.resultPath.empty())
			ImGui::TextDisabled("%s", run.resultPath.string().c_str());
	}
}
#undef I18N_KEY_PREFIX
