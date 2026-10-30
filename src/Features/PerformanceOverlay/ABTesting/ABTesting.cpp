#include "ABTesting.h"
#include "Feature.h"
#include "Features/PerformanceOverlay.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "PrerequisiteChain.h"
#include "SceneSettingsManager.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.perf_overlay."

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <fstream>
#include <imgui.h>
#include <set>

namespace
{
	constexpr const char* kNoSamplesNote = "No timing samples were collected.";
	constexpr uint32_t kMinLiveIntervalSeconds = 1;
	constexpr uint32_t kMaxLiveIntervalSeconds = 10;

	float FrameBudgetMs();

	void ReplaceAll(std::string& value, std::string_view from, std::string_view to)
	{
		for (auto position = value.find(from); position != std::string::npos; position = value.find(from, position + to.size())) {
			value.replace(position, from.size(), to);
		}
	}
}

std::string ABTestingManager::GetSettingsPathDisplayName(std::string path)
{
	for (auto* feature : Feature::GetFeatureList()) {
		const auto featureName = feature->GetName();
		const auto pathPrefix = "/" + featureName;
		if (path == pathPrefix || path.starts_with(pathPrefix + "/")) {
			path.replace(1, featureName.size(), feature->GetDisplayName());
			break;
		}
	}

	ReplaceAll(path, "CSEditorToggleKey", "OS Editor Toggle Key");
	ReplaceAll(path, "ShowCSPasses", "Show OS Passes");
	ReplaceAll(path, "Community Shaders", Plugin::DISPLAY_NAME);
	ReplaceAll(path, "CommunityShaders", Plugin::DISPLAY_NAME);
	ReplaceAll(path, "CS Editor", "OS Editor");
	ReplaceAll(path, "CS Utility", "OS Utility");
	ReplaceAll(path, "CSEditor", "OS Editor");
	ReplaceAll(path, "CSUtility", "OS Utility");
	return path;
}

ABTestingManager* ABTestingManager::GetSingleton()
{
	static ABTestingManager singleton;
	return &singleton;
}

void ABTestingManager::SetTestInterval(uint32_t interval)
{
	testInterval = interval;
}

void ABTestingManager::BeginRun()
{
	globals::state->LoadFromJson(testConfigSnapshot);
	usingTestConfig = true;
	abTestingEnabled = true;

	aggregator.Clear();
	aggregator.OnABSwitch(ABVariant::B);
	compositorStats.Reset();
	lastCompositorFrameIndex = UINT32_MAX;
	lastProfilerFrame = UINT32_MAX;
	AcquireProfiler();

	// Initialize QueryPerformanceCounter timing
	if (timingFrequency.QuadPart == 0) {
		QueryPerformanceFrequency(&timingFrequency);
	}
	QueryPerformanceCounter(&lastTestSwitch);
	lastFrameTime = std::chrono::steady_clock::now();
}

bool ABTestingManager::Enable()
{
	if (abTestingEnabled)
		return true;

	testInterval = std::clamp(testInterval, kMinLiveIntervalSeconds, kMaxLiveIntervalSeconds);
	auto* state = globals::state;
	auto& performanceOverlay = globals::features::performanceOverlay;

	logger::info("Starting A/B testing with current settings as Variant B (TEST), interval {} seconds.", testInterval);

	bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;

	// Save current settings as TEST variant (Variant B) in memory
	testConfigSnapshot = nlohmann::json::object();
	state->SaveToJson(testConfigSnapshot);
	hasTestSnapshot = true;

	// Load and cache USER settings in memory (to avoid disk I/O during swaps)
	userConfigSnapshot = nlohmann::json::object();
	state->Load(State::ConfigMode::USER);
	state->SaveToJson(userConfigSnapshot);
	hasUserSnapshot = true;

	if (userConfigSnapshot == testConfigSnapshot) {
		state->LoadFromJson(testConfigSnapshot);
		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;
		liveNotice = T(TKEY("live_no_changes"), "No unsaved changes. Change a setting, or queue changes to benchmark them.");
		logger::info("[A/B] Live test not started: {}", liveNotice);
		return false;
	}

	aggregator.SetSettleFrames(runOptions.settleFrames);
	BeginRun();

	// Restore overlay enabled state after config operations
	performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;
	liveNotice.clear();

	logger::info("A/B Testing enabled - starting with Variant B (TEST). Both variants cached in memory for unbiased swapping.");
	return true;
}

void ABTestingManager::Disable()
{
	if (queueActive) {
		cancelRequested = true;
		return;
	}
	if (abTestingEnabled) {
		auto* state = globals::state;
		auto& performanceOverlay = globals::features::performanceOverlay;

		// Preserve overlay enabled state
		bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;

		logger::info("Disabling A/B testing. Restoring to Variant B (TEST) config from memory.");

		// Restore TEST config from memory snapshot (no disk read)
		if (hasTestSnapshot) {
			state->LoadFromJson(testConfigSnapshot);
		} else {
			logger::warn("No TEST snapshot available, staying with current config.");
		}

		abTestingEnabled = false;
		aggregator.OnTestEnd();
		ReleaseProfiler();

		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;

		const auto& intervals = aggregator.GetHeadlineIntervals();
		if (!intervals.empty()) {
			const auto comparison = ComputeABComparison(intervals, !globals::game::isVR);
			runResults.push_back({ T(TKEY("live_label"), "Unsaved changes (saved vs current)"), comparison, FrameBudgetMs(),
				comparison.valid ? "" : kNoSamplesNote });
			logger::info("[A/B] Live test: {:+.3f} +/- {:.3f} ms", comparison.deltaMs, comparison.uncertaintyMs);
		}
	}
}

void ABTestingManager::Update()
{
	if (cancelRequested) {
		cancelRequested = false;
		if (queueActive)
			EndComparisons(false);
	}
	if (!abTestingEnabled)
		return;

	auto* state = globals::state;
	auto& performanceOverlay = globals::features::performanceOverlay;

	// Check if it's time to swap
	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);
	float seconds = (currentTime.QuadPart - lastTestSwitch.QuadPart) / static_cast<float>(timingFrequency.QuadPart);
	auto remaining = static_cast<float>(testInterval) - seconds;

	if (remaining < 0.0f && queueActive && aggregator.CurrentVariant() == ABVariant::A &&
		aggregator.MeasuredIntervalCount() >= 2 * queueOptions.repeats + 1) {
		FinishComparison();
		return;
	}

	if (remaining < 0.0f) {
		bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;

		// Swap between variants
		usingTestConfig = !usingTestConfig;
		logger::info("A/B Test swap to {} (from memory snapshot)",
			usingTestConfig ? "Variant B (TEST)" : "Variant A (USER)");

		if (usingTestConfig) {
			// Swap to TEST - load from memory snapshot (no disk I/O)
			if (hasTestSnapshot) {
				state->LoadFromJson(testConfigSnapshot);
			} else {
				logger::error("TEST snapshot missing! Cannot swap to Variant B.");
				usingTestConfig = false;  // Stay on USER
			}
		} else {
			// Swap to USER - load from memory snapshot (no disk I/O)
			if (hasUserSnapshot) {
				state->LoadFromJson(userConfigSnapshot);
			} else {
				logger::error("USER snapshot missing! Cannot swap to Variant A.");
				usingTestConfig = true;  // Stay on TEST
			}
		}

		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;
		QueryPerformanceCounter(&lastTestSwitch);

		// Notify the A/B test aggregator of the variant switch
		aggregator.OnABSwitch(usingTestConfig ? ABVariant::B : ABVariant::A);
	}
}

namespace
{
	/** @brief Frames back to read: the newest frame is still in flight and reports zero GPU time. */
	constexpr uint32_t kCompletedFrameOffset = 1;

	/** @brief Reprojection flags that mean the compositor reprojected a missed frame; the async flag only names the mode. */
	constexpr uint32_t kReprojectedMask = vr::VRCompositor_ReprojectionReason_Cpu | vr::VRCompositor_ReprojectionReason_Gpu;
}

bool ReadLatestCompositorSample(CompositorSample& sample, uint32_t& lastFrameIndex, bool* timingAvailable)
{
	if (!globals::game::isVR)
		return false;

	auto* compositor = RE::BSOpenVR::GetIVRCompositor();
	if (!compositor)
		return false;

	vr::Compositor_FrameTiming timing{};
	timing.m_nSize = sizeof(timing);
	if (!compositor->GetFrameTiming(&timing, kCompletedFrameOffset))
		return false;

	if (timingAvailable)
		*timingAvailable = true;
	if (timing.m_nFrameIndex == lastFrameIndex)
		return false;
	lastFrameIndex = timing.m_nFrameIndex;

	sample = { .applicationGpuMs = timing.m_flPreSubmitGpuMs + timing.m_flPostSubmitGpuMs,
		.totalRenderGpuMs = timing.m_flTotalRenderGpuMs,
		.compositorRenderGpuMs = timing.m_flCompositorRenderGpuMs,
		.compositorRenderCpuMs = timing.m_flCompositorRenderCpuMs,
		.clientFrameIntervalMs = timing.m_flClientFrameIntervalMs,
		.droppedFrames = timing.m_nNumDroppedFrames,
		.reprojected = (timing.m_nReprojectionFlags & kReprojectedMask) != 0 };
	return true;
}

void ABTestingManager::SampleCompositor()
{
	if (!abTestingEnabled || aggregator.IsWarmingUp() || aggregator.IsSettling())
		return;

	CompositorSample sample;
	if (ReadLatestCompositorSample(sample, lastCompositorFrameIndex, &compositorTimingAvailable)) {
		compositorStats.Add(usingTestConfig ? ABVariant::B : ABVariant::A, sample);
		aggregator.OnHeadline(sample.applicationGpuMs);
		aggregator.OnFrameTime(sample.clientFrameIntervalMs);
	}
}

void ABTestingManager::AcquireProfiler()
{
	if (globals::game::isVR || !globals::profiler || profilerOwned)
		return;
	profilerWasEnabled = globals::profiler->IsUserEnabled();
	globals::profiler->SetUserEnabled(true);
	profilerOwned = true;
}

void ABTestingManager::ReleaseProfiler()
{
	if (!profilerOwned)
		return;
	if (!profilerWasEnabled && globals::profiler)
		globals::profiler->SetUserEnabled(false);
	profilerOwned = false;
}

/**
 * Capture is request-driven and the GPU result trails the frame by a few frames, so a sample is only
 * taken when the resolved frame count has moved since the last one.
 */
void ABTestingManager::SampleProfilerHeadline()
{
	if (globals::game::isVR || !profilerOwned || !globals::profiler)
		return;

	auto* profiler = globals::profiler;
	profiler->RequestCapture(Profiler::CaptureMode::Both);
	const uint32_t captured = profiler->GetCapturedGpuFrameCount();
	if (captured == 0 || captured == lastProfilerFrame)
		return;

	lastProfilerFrame = captured;
	aggregator.OnHeadline(profiler->GetResolvedTotalTimeMs());
}

void ABTestingManager::RecordFrame(const std::vector<DrawCallRow>& rows)
{
	aggregator.OnFrame(rows);
	if (!globals::game::isVR) {
		const auto now = std::chrono::steady_clock::now();
		aggregator.OnFrameTime(std::chrono::duration<float, std::milli>(now - lastFrameTime).count());
		lastFrameTime = now;
	}
	SampleCompositor();
	SampleProfilerHeadline();
}

namespace
{
	constexpr float kVrFrameBudgetMs = 1000.0f / 90.0f;
	constexpr float kFlatFrameBudgetMs = 1000.0f / 60.0f;
	constexpr uint32_t kMaxIntervalSeconds = 30;
	constexpr uint32_t kMaxRepeats = 10;
	constexpr uint32_t kMaxSettleFrames = 600;

	float FrameBudgetMs()
	{
		return globals::game::isVR ? kVrFrameBudgetMs : kFlatFrameBudgetMs;
	}

	bool Refuse(std::string* error, std::string message)
	{
		logger::warn("[A/B] {}", message);
		if (error)
			*error = std::move(message);
		return false;
	}

	/** @brief Names of the loaded features that have a change waiting for a restart. */
	std::set<std::string> FeaturesPendingRestart()
	{
		std::set<std::string> pending;
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasAnyPendingRestart())
				pending.insert(feature->GetDisplayName());
		}
		return pending;
	}

	/** @brief Change that sets one catalogued setting of a feature, leaving its other settings as they are. */
	std::function<void()> SettingSetter(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, nlohmann::json value)
	{
		return [&feature, setting, value = std::move(value)] {
			nlohmann::json settings;
			feature.SaveSettings(settings);
			if (auto* slot = Util::Settings::GetCatalogSerializedValue(settings, setting))
				*slot = value;
			feature.LoadSettings(settings);
		};
	}

	/** @brief Request that sets one catalogued setting to `target`, preparing it and the settings that enable it to `from` first when given. */
	ABComparisonRequest SettingRequest(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target,
		std::string label, const std::optional<nlohmann::json>& from = std::nullopt)
	{
		ABComparisonRequest request{ std::move(label), SettingSetter(feature, setting, target) };
		std::vector<std::function<void()>> prepares;
		if (const auto prerequisites = ABTestingManager::GetSingleton()->PrerequisitesFor(feature, setting)) {
			for (const auto& prerequisite : *prerequisites)
				prepares.push_back(SettingSetter(feature, *prerequisite.setting, prerequisite.requiredValue));
		}
		if (from)
			prepares.push_back(SettingSetter(feature, setting, *from));
		if (!prepares.empty())
			request.prepare = [prepares = std::move(prepares)] {
				for (const auto& prepare : prepares)
					prepare();
			};
		return request;
	}

	/** @brief The whole change a request measures: its prepare, then its apply. */
	std::function<void()> ChangeClosure(const ABComparisonRequest& request)
	{
		if (!request.prepare)
			return request.apply;
		return [prepare = request.prepare, apply = request.apply] {
			prepare();
			apply();
		};
	}
}

std::string ABTestingManager::RestartNeededBy(const std::function<void()>& apply)
{
	auto* state = globals::state;
	nlohmann::json original = nlohmann::json::object();
	state->SaveToJson(original);
	const auto pendingBefore = FeaturesPendingRestart();
	{
		SceneSettingsManager::SceneLayerGuard guard(*SceneSettingsManager::GetSingleton());
		apply();
	}
	std::string restartNeeded;
	for (const auto& name : FeaturesPendingRestart()) {
		if (!pendingBefore.contains(name))
			restartNeeded += (restartNeeded.empty() ? "" : ", ") + name;
	}
	state->LoadFromJson(original);
	return restartNeeded;
}

bool ABTestingManager::StartComparisons(std::vector<ABComparisonRequest> requests, const ABRunOptions& options, std::string* error)
{
	if (abTestingEnabled)
		return Refuse(error, "An A/B test is already running; stop it first.");
	if (requests.empty())
		return Refuse(error, "There is nothing to compare.");
	if (globals::state->IsMainOrLoadingMenuOpen())
		return Refuse(error, "Load a save first; there is nothing to measure at the main menu.");
	if (!globals::game::isVR && !globals::profiler)
		return Refuse(error, "The profiler is not available.");

	liveTestInterval = testInterval;
	queueOptions = options;
	queueOptions.intervalSeconds = std::clamp<uint32_t>(options.intervalSeconds, 1, kMaxIntervalSeconds);
	queueOptions.repeats = std::clamp<uint32_t>(options.repeats, 1, kMaxRepeats);
	queueOptions.settleFrames = std::min(options.settleFrames, kMaxSettleFrames);

	queueOriginal = nlohmann::json::object();
	globals::state->SaveToJson(queueOriginal);
	queue = std::move(requests);
	queueIndex = 0;
	runResults.clear();
	queueActive = true;

	menuClosedByRun = false;
	if (auto* menu = Menu::GetSingleton(); menu && queueOptions.closeMenu && menu->IsEnabled) {
		menu->RequestVisibility(Menu::VisibilityRequest::Close);
		menuClosedByRun = true;
	}

	logger::info("[A/B] Comparing {} settings change(s), {} s intervals, {} repeats", queue.size(), queueOptions.intervalSeconds, queueOptions.repeats);
	BeginComparison(0);
	RevealOverlay();
	return true;
}

void ABTestingManager::BeginComparison(size_t index)
{
	auto* state = globals::state;
	const auto& request = queue[index];

	state->LoadFromJson(queueOriginal);
	const auto restartNeeded = RestartNeededBy(ChangeClosure(request));
	{
		SceneSettingsManager::SceneLayerGuard guard(*SceneSettingsManager::GetSingleton());
		if (request.prepare)
			request.prepare();
		userConfigSnapshot = nlohmann::json::object();
		state->SaveToJson(userConfigSnapshot);
		request.apply();
		testConfigSnapshot = nlohmann::json::object();
		state->SaveToJson(testConfigSnapshot);
	}
	state->LoadFromJson(queueOriginal);

	const bool unchanged = testConfigSnapshot == userConfigSnapshot;
	if (unchanged || !restartNeeded.empty()) {
		runResults.push_back({ request.label, {}, FrameBudgetMs(),
			restartNeeded.empty() ? "Changes no saved setting, so there is nothing to compare." :
									"Needs a restart to take effect (" + restartNeeded + "), so it cannot be measured live." });
		logger::info("[A/B] {}: {}", request.label, runResults.back().note);
		if (index + 1 < queue.size()) {
			queueIndex = index + 1;
			BeginComparison(queueIndex);
		} else {
			EndComparisons(true);
		}
		return;
	}

	hasUserSnapshot = true;
	hasTestSnapshot = true;
	testInterval = queueOptions.intervalSeconds;
	aggregator.SetSettleFrames(queueOptions.settleFrames);
	BeginRun();
	logger::info("[A/B] Comparing {} ({} of {})", request.label, index + 1, queue.size());
}

void ABTestingManager::FinishComparison()
{
	aggregator.OnTestEnd();
	const auto comparison = ComputeABComparison(aggregator.GetHeadlineIntervals(), !globals::game::isVR);
	runResults.push_back({ queue[queueIndex].label, comparison, FrameBudgetMs(), comparison.valid ? "" : kNoSamplesNote });
	logger::info("[A/B] {}: {:+.3f} +/- {:.3f} ms", queue[queueIndex].label, comparison.deltaMs, comparison.uncertaintyMs);

	if (++queueIndex < queue.size())
		BeginComparison(queueIndex);
	else
		EndComparisons(true);
}

void ABTestingManager::EndComparisons(bool finished)
{
	if (abTestingEnabled)
		aggregator.OnTestEnd();
	auto original = queueOriginal;
	globals::state->LoadFromJson(original);
	abTestingEnabled = false;
	queueActive = false;
	testInterval = std::clamp(liveTestInterval, kMinLiveIntervalSeconds, kMaxLiveIntervalSeconds);
	ReleaseProfiler();

	if (finished)
		RevealResults();
	else
		logger::info("[A/B] Comparison cancelled, settings restored");
}

void ABTestingManager::RevealResults()
{
	RevealOverlay();
	if (menuClosedByRun) {
		if (auto* menu = Menu::GetSingleton())
			menu->RequestVisibility(Menu::VisibilityRequest::Open);
	}
	menuClosedByRun = false;
}

nlohmann::json ABTestingManager::StatusJson() const
{
	auto runs = nlohmann::json::array();
	for (const auto& run : runResults) {
		const auto& c = run.comparison;
		const char* verdict = c.verdict == ABVerdict::Faster ? "faster" : c.verdict == ABVerdict::Slower ? "slower" :
		                                                                                                   "noChange";
		runs.push_back({ { "label", run.label },
			{ "valid", c.valid },
			{ "meanAMs", c.meanAMs },
			{ "meanBMs", c.meanBMs },
			{ "deltaMs", c.deltaMs },
			{ "uncertaintyMs", c.uncertaintyMs },
			{ "frameMeanAMs", c.frame.meanAMs },
			{ "frameMeanBMs", c.frame.meanBMs },
			{ "frameDeltaMs", c.frame.deltaMs },
			{ "frameUncertaintyMs", c.frame.uncertaintyMs },
			{ "fpsA", c.frame.valid && c.frame.meanAMs > 0.0f ? 1000.0f / c.frame.meanAMs : 0.0f },
			{ "fpsB", c.frame.valid && c.frame.meanBMs > 0.0f ? 1000.0f / c.frame.meanBMs : 0.0f },
			{ "percentOfBudget", run.budgetMs > 0.0f ? c.deltaMs / run.budgetMs * 100.0f : 0.0f },
			{ "headroomOnly", c.headroomOnly },
			{ "verdict", verdict },
			{ "note", run.note } });
	}
	auto pending = nlohmann::json::array();
	for (const auto& item : comparisonQueue)
		pending.push_back(item.request.label);
	return { { "comparisonRunning", queueActive },
		{ "queue", std::move(pending) },
		{ "queueNotice", queueNotice },
		{ "liveNotice", liveNotice },
		{ "comparison", queueActive ? nlohmann::json(queue[queueIndex].label) : nlohmann::json(nullptr) },
		{ "comparisonIndex", queueActive ? queueIndex + 1 : 0 },
		{ "comparisonCount", queue.size() },
		{ "frameBudgetMs", FrameBudgetMs() },
		{ "runs", std::move(runs) } };
}

std::string ABTestingManager::SettingKey(const SceneSettingsCatalog::SettingMetadata& setting)
{
	return std::format("setting:{}/{}/{}", setting.featureShortName, setting.serializedPath, setting.serializedKey);
}

bool ABTestingManager::IsQueued(const std::string& key) const
{
	return std::ranges::any_of(comparisonQueue, [&](const ABQueuedComparison& item) { return item.key == key; });
}

bool ABTestingManager::Unqueue(const std::string& key)
{
	return std::erase_if(comparisonQueue, [&](const ABQueuedComparison& item) { return item.key == key; }) > 0;
}

void ABTestingManager::ClearQueue()
{
	comparisonQueue.clear();
	queueNotice.clear();
}

/** The overlay shows the comparison in progress and its results, so it is forced visible rather than left to its toggle. */
void ABTestingManager::RevealOverlay()
{
	globals::features::performanceOverlay.settings.ShowInOverlay = true;
	if (auto* menu = Menu::GetSingleton())
		menu->overlayVisible = true;
}

bool ABTestingManager::Enqueue(ABQueuedComparison item)
{
	if (queueActive)
		return false;
	Unqueue(item.key);
	comparisonQueue.push_back(std::move(item));
	queueNotice.clear();
	return true;
}

bool ABTestingManager::EnqueueSetting(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, ABComparisonRequest request, std::string* error)
{
	nlohmann::json probe;
	feature.SaveSettings(probe);
	if (!Util::Settings::GetCatalogSerializedValue(probe, setting))
		return Refuse(error, "The setting is not present in the feature's saved settings.");
	if (!Enqueue({ SettingKey(setting), std::move(request) }))
		return Refuse(error, "A comparison is running; wait for it to finish.");
	return true;
}

bool ABTestingManager::QueueSetting(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target, std::string label, std::string* error)
{
	return EnqueueSetting(feature, setting, SettingRequest(feature, setting, target, std::move(label)), error);
}

bool ABTestingManager::QueueSettingChange(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& from, const nlohmann::json& to, std::string label, std::string* error)
{
	return EnqueueSetting(feature, setting, SettingRequest(feature, setting, to, std::move(label), std::optional<nlohmann::json>{ from }), error);
}

std::optional<nlohmann::json> ABTestingManager::SavedSettingValue(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting)
{
	std::ifstream file(Util::PathHelpers::GetSettingsUserPath());
	if (!file.is_open())
		return std::nullopt;

	nlohmann::json saved;
	try {
		file >> saved;
	} catch (const nlohmann::json::exception&) {
		return std::nullopt;
	}

	const auto featureSettings = saved.find(feature.GetName());
	if (featureSettings == saved.end() || !featureSettings->is_object())
		return std::nullopt;
	if (const auto* value = Util::Settings::GetCatalogSerializedValue(*featureSettings, setting))
		return std::optional<nlohmann::json>{ *value };
	return std::nullopt;
}

std::optional<std::vector<ABSettingPrerequisite>> ABTestingManager::PrerequisitesFor(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting)
{
	nlohmann::json current;
	feature.SaveSettings(current);
	const auto steps = ABPrerequisites::ResolvePrerequisites<SceneSettingsCatalog::SettingMetadata, nlohmann::json>(setting, [&setting](std::string_view key, std::string_view path) { return SceneSettingsCatalog::FindSetting(setting.featureShortName, path, key); }, [&current](const SceneSettingsCatalog::SettingMetadata& prerequisite) {
			const auto* value = Util::Settings::GetCatalogSerializedValue(current, prerequisite);
			return value ? *value : nlohmann::json(); }, [](const SceneSettingsCatalog::SettingMetadata& prerequisite, std::int8_t required) {
			const bool on = required != 0;
			return prerequisite.valueType == SceneSettingsCatalog::ValueType::Boolean ?
			           nlohmann::json(on) :
			           nlohmann::json(on ? 1 : 0); });
	if (!steps)
		return std::nullopt;

	std::vector<ABSettingPrerequisite> prerequisites;
	for (const auto& step : *steps)
		prerequisites.push_back({ step.setting, step.required });
	return prerequisites;
}

std::string ABTestingManager::SettingRestartReason(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target, const std::optional<nlohmann::json>& from)
{
	const auto request = SettingRequest(feature, setting, target, {}, from);
	return RestartNeededBy(ChangeClosure(request));
}

bool ABTestingManager::QueueSettingByKey(const std::string& featureShortName, const std::string& key, const std::string& path, const nlohmann::json& value, std::string* error)
{
	Feature* owner = nullptr;
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->GetShortName() == featureShortName)
			owner = feature;
	}
	if (!owner)
		return Refuse(error, "Unknown feature: " + featureShortName);

	for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
		if (setting.featureShortName == featureShortName && setting.serializedKey == key && setting.serializedPath == path)
			return QueueSetting(*owner, setting, value, Util::Settings::GetCatalogLeafDisplayName(setting) + " = " + value.dump(), error);
	}
	return Refuse(error, std::format("No catalogued setting {}/{}/{}.", featureShortName, path, key));
}

bool ABTestingManager::QueueProfiles(const std::string& featureShortName, std::string* error)
{
	Feature* only = nullptr;
	if (!featureShortName.empty()) {
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->GetShortName() == featureShortName)
				only = feature;
		}
		if (!only)
			return Refuse(error, "Unknown feature: " + featureShortName);
	}

	if (queueActive)
		return Refuse(error, "A comparison is running; wait for it to finish.");

	const std::pair<Feature::PerfProfile, const char*> profiles[] = {
		{ Feature::PerfProfile::Performance, T("feature.perf_overlay.profile_performance", "Performance") },
		{ Feature::PerfProfile::Balanced, T("feature.perf_overlay.profile_balanced", "Balanced") },
		{ Feature::PerfProfile::Quality, T("feature.perf_overlay.profile_quality", "Quality") },
	};
	const std::string scope = only ? only->GetDisplayName() : std::string(T("feature.perf_overlay.all_features", "All features"));
	std::string notAdded;
	bool anyQueued = false;
	for (const auto& [profile, name] : profiles) {
		const auto apply = [only, profile] {
			if (only)
				only->ApplyPerformanceProfile(profile);
			else
				Feature::ApplyPerformanceProfileToAll(profile);
		};
		const auto restartNeeded = RestartNeededBy(apply);
		if (!restartNeeded.empty()) {
			notAdded += std::string(notAdded.empty() ? "" : "; ") + std::format("{} ({} needs a restart)", name, restartNeeded);
			continue;
		}
		const bool queued = Enqueue({ std::format("profile:{}:{}", featureShortName, name),
			{ std::format("{} preset ({})", name, scope), apply } });
		if (!queued)
			return Refuse(error, "A comparison is running; wait for it to finish.");
		anyQueued = true;
	}
	if (!notAdded.empty())
		queueNotice = "Not added: " + notAdded;
	if (!anyQueued)
		return Refuse(error, queueNotice);
	return true;
}

bool ABTestingManager::RunQueue(std::string* error)
{
	if (runOptions.together) {
		if (comparisonQueue.size() < 2)
			return Refuse(error, "All together needs at least two queued changes; queue another change or measure them one at a time.");
		for (const auto& item : comparisonQueue) {
			if (!item.key.starts_with("setting:"))
				return Refuse(error, "All together can only combine changes to individual settings; a preset replaces the settings it covers, so it cannot be combined.");
		}

		std::vector<std::function<void()>> prepares;
		std::vector<std::function<void()>> applies;
		for (const auto& item : comparisonQueue) {
			if (item.request.prepare)
				prepares.push_back(item.request.prepare);
			applies.push_back(item.request.apply);
		}
		ABComparisonRequest combined{
			fmt::format(fmt::runtime(T(TKEY("together_label"), "{} changes together")), applies.size()),
			[applies = std::move(applies)] {
				for (const auto& apply : applies)
					apply();
			},
			[prepares = std::move(prepares)] {
				for (const auto& prepare : prepares)
					prepare();
			}
		};
		if (!StartComparisons({ std::move(combined) }, runOptions, error))
			return false;
		comparisonQueue.clear();
		queueNotice.clear();
		return true;
	}

	std::vector<ABComparisonRequest> requests;
	for (const auto& item : comparisonQueue)
		requests.push_back(item.request);
	if (!StartComparisons(std::move(requests), runOptions, error))
		return false;
	comparisonQueue.clear();
	queueNotice.clear();
	return true;
}

std::vector<ABBenchmarkRow> ABTestingManager::GetBenchmarkRows() const
{
	std::vector<ABBenchmarkRow> rows;
	rows.reserve(runResults.size() + comparisonQueue.size());
	for (const auto& result : runResults)
		rows.push_back({ {}, result, result.comparison.valid ? ABBenchmarkRow::Status::Done : ABBenchmarkRow::Status::Skipped });

	if (queueActive) {
		rows.push_back({ {}, { queue[queueIndex].label, {}, FrameBudgetMs(), {} }, ABBenchmarkRow::Status::Running });
		for (size_t index = queueIndex + 1; index < queue.size(); ++index)
			rows.push_back({ {}, { queue[index].label, {}, FrameBudgetMs(), {} }, ABBenchmarkRow::Status::Queued });
	}

	for (const auto& item : comparisonQueue)
		rows.push_back({ item.key, { item.request.label, {}, FrameBudgetMs(), {} }, ABBenchmarkRow::Status::Queued });
	return rows;
}

void ABTestingManager::DrawQueueButtons()
{
	ImGui::BeginDisabled(queueActive || abTestingEnabled || comparisonQueue.empty());
	if (ImGui::Button(T(TKEY("queue_run"), "Run benchmark"))) {
		std::string error;
		if (!RunQueue(&error))
			queueNotice = error;
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("queue_clear_all"), "Clear all")))
		ClearQueue();
	ImGui::EndDisabled();
}

void ABTestingManager::DrawBenchmarkTab()
{
	ImGui::TextWrapped("%s", T(TKEY("benchmark_intro"),
								 "Measures what a change costs on your scene. Queued changes are measured against your current settings, one at a time or all together: "
								 "they alternate, the first frames of every swap are ignored, and your settings are restored afterwards. "
								 "Add changes from a setting's right-click menu or from Presets. The menu closes while it runs and the result appears in the performance overlay."));
	ImGui::Spacing();
	ImGui::SeparatorText(T(TKEY("queue_title"), "Benchmark queue"));

	const bool canCombine = comparisonQueue.size() >= 2 &&
	                        std::ranges::none_of(comparisonQueue, [](const ABQueuedComparison& item) { return item.key.starts_with("profile:"); });
	if (runOptions.together && !canCombine)
		runOptions.together = false;

	if (ImGui::RadioButton(T(TKEY("queue_mode_one"), "One at a time"), !runOptions.together))
		runOptions.together = false;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("queue_mode_one_tooltip"), "Measures each change on its own, so you can see which one costs what."));
	ImGui::SameLine();
	ImGui::BeginDisabled(!canCombine);
	if (ImGui::RadioButton(T(TKEY("queue_mode_together"), "All together"), runOptions.together))
		runOptions.together = true;
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("queue_mode_together_tooltip"), "Applies every queued change at once and measures the bundle. It cannot tell you which change is responsible."));
		if (!canCombine)
			ImGui::Text("%s", T(TKEY("queue_mode_together_blocked"), "Needs at least two queued changes and no preset."));
	}

	DrawQueueButtons();
	if (!queueNotice.empty()) {
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, Menu::GetSingleton()->GetTheme().StatusPalette.Warning);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", queueNotice.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}
	if (!queueActive && comparisonQueue.empty() && runResults.empty()) {
		ImGui::TextDisabled("%s", T(TKEY("queue_empty"),
									  "Nothing queued. Add changes from a setting's right-click menu (Add to benchmark) or from Presets. They appear in the table below."));
	}
	globals::features::performanceOverlay.DrawABRunSummary(true);

	ImGui::Spacing();
	int interval = static_cast<int>(runOptions.intervalSeconds);
	int repeats = static_cast<int>(runOptions.repeats);
	if (ImGui::SliderInt(T(TKEY("run_interval"), "Seconds per swap"), &interval, 1, static_cast<int>(kMaxIntervalSeconds)))
		runOptions.intervalSeconds = static_cast<uint32_t>(interval);
	if (ImGui::SliderInt(T(TKEY("run_repeats"), "Repeats per change"), &repeats, 1, static_cast<int>(kMaxRepeats)))
		runOptions.repeats = static_cast<uint32_t>(repeats);

	if (queueActive) {
		ImGui::Text(T(TKEY("run_progress"), "Comparing %s (%zu of %zu)"), queue[queueIndex].label.c_str(), queueIndex + 1, queue.size());
		if (ImGui::Button(T(TKEY("run_cancel"), "Cancel")))
			cancelRequested = true;
	}

	ImGui::Spacing();
	ImGui::SeparatorText(T(TKEY("live_title"), "Live comparison"));
	DrawSettingsUI();
}

void ABTestingManager::DrawSettingsUI()
{
	int interval = static_cast<int>(testInterval);
	if (ImGui::SliderInt(T(TKEY("live_interval"), "Live comparison seconds per swap"), &interval, static_cast<int>(kMinLiveIntervalSeconds), static_cast<int>(kMaxLiveIntervalSeconds)))
		testInterval = static_cast<uint32_t>(interval);

	ImGui::BeginDisabled(queueActive);
	if (abTestingEnabled) {
		if (ImGui::Button(T(TKEY("live_stop"), "Stop live comparison")))
			Disable();
	} else if (ImGui::Button(T(TKEY("live_start"), "Start live comparison"))) {
		Enable();
	}
	ImGui::EndDisabled();

	ImGui::TextDisabled("%s", T(TKEY("live_hint"), "Compares your saved settings with your current settings. Change settings first, then start."));

	if (!liveNotice.empty())
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Warning, "%s", liveNotice.c_str());
}

std::vector<std::string> ABTestingManager::GetConfigDifferencesForDisplay() const
{
	std::vector<std::string> differences;

	if (!hasTestSnapshot || !hasUserSnapshot)
		return differences;

	auto diffEntries = Util::FileSystem::DiffJson(userConfigSnapshot, testConfigSnapshot, 0.0001f);

	// Format diff entries for display
	for (const auto& entry : diffEntries) {
		std::string path = GetSettingsPathDisplayName(entry.path);
		std::string aVal = entry.aValue;
		std::string bVal = entry.bValue;

		// Clean up JSON path (remove leading slash and simplify)
		if (!path.empty() && path[0] == '/') {
			path = path.substr(1);
			// Replace slashes with dots for readability
			std::replace(path.begin(), path.end(), '/', '.');
		}

		// Skip version changes (not relevant to user)
		if (path == "Version")
			continue;

		// Truncate long values
		if (aVal.length() > 30)
			aVal = aVal.substr(0, 27) + "...";
		if (bVal.length() > 30)
			bVal = bVal.substr(0, 27) + "...";

		// Format: "path: oldValue -> newValue"
		differences.push_back(fmt::format("{}: {} -> {}", path, aVal, bVal));
	}

	return differences;
}

std::vector<SettingsDiffEntry> ABTestingManager::GetConfigDiffEntries(float epsilon) const
{
	if (!hasTestSnapshot || !hasUserSnapshot)
		return {};

	return Util::FileSystem::DiffJson(userConfigSnapshot, testConfigSnapshot, epsilon);
}

void ABTestingManager::ClearCachedSnapshots()
{
	try {
		testConfigSnapshot.clear();
		userConfigSnapshot.clear();
		hasTestSnapshot = false;
		hasUserSnapshot = false;
	} catch (...) {
		// No-op if clear fails
	}
}

std::vector<std::string> ABTestingManager::GetConfigDifferences() const
{
	// Keep this for DrawOverlayUI to avoid circular dependencies
	return GetConfigDifferencesForDisplay();
}

void ABTestingManager::DrawOverlayUI()
{
	if (!abTestingEnabled)
		return;

	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);
	float seconds = (currentTime.QuadPart - lastTestSwitch.QuadPart) / static_cast<float>(timingFrequency.QuadPart);
	auto remaining = static_cast<float>(testInterval) - seconds;

	// Scale position for resolution
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * Util::GetUIScale();
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::SetNextWindowPos(ImVec2(pos, pos));
	if (!ImGui::Begin("Benchmark", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::End();
		return;
	}

	remaining = std::max(0.0f, remaining);

	if (queueActive)
		ImGui::Text(T(TKEY("overlay_comparing"), "Comparing %s (%zu of %zu)"), queue[queueIndex].label.c_str(), queueIndex + 1, queue.size());

	// Show current variant and time
	const bool warmingUp = aggregator.IsWarmingUp();
	const char* variantLabel = nullptr;
	if (queueActive) {
		variantLabel = warmingUp       ? T(TKEY("variant_change_warmup"), "Change (warm-up)") :
		               usingTestConfig ? T(TKEY("variant_change"), "Change") :
		                                 T(TKEY("variant_baseline"), "Baseline");
	} else {
		variantLabel = warmingUp       ? T(TKEY("variant_current_warmup"), "Current settings (warm-up)") :
		               usingTestConfig ? T(TKEY("variant_current"), "Current settings") :
		                                 T(TKEY("variant_saved"), "Saved settings");
	}
	ImGui::Text("%s", fmt::format(fmt::runtime(T(TKEY("variant_time_left_fmt"), "{} : {:.1f}s left")),
						  variantLabel, remaining)
						  .c_str());

	// Show what changed (for both variants); during a queue run the snapshots hold the item being measured, not the USER/TEST pair
	if (hasTestSnapshot && !queueActive) {
		auto differences = GetConfigDifferences();

		if (!differences.empty()) {
			ImGui::Separator();

			constexpr size_t MAX_CHANGES_DISPLAYED = 10;  // Show max 10 individual changes, otherwise show count
			if (differences.size() <= MAX_CHANGES_DISPLAYED) {
				Util::Text::Info("%s", T(TKEY("changes_from_user"), "Changes from USER:"));
				for (const auto& diff : differences) {
					ImGui::BulletText("%s", diff.c_str());
				}
			} else {
				Util::Text::Info(T(TKEY("settings_changed_count"), "%zu settings changed"), differences.size());
			}
		}
	}

	ImGui::End();
}

#undef I18N_KEY_PREFIX
