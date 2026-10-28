#include "ABTesting.h"
#include "Feature.h"
#include "Features/PerformanceOverlay.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
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
}

void ABTestingManager::Enable()
{
	if (!abTestingEnabled) {
		auto* state = globals::state;
		auto& performanceOverlay = globals::features::performanceOverlay;

		logger::info("Starting A/B testing with current settings as Variant B (TEST), interval {} seconds.", testInterval);

		// Preserve overlay enabled state before config operations
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

		aggregator.SetSettleFrames(0);
		BeginRun();

		// Restore overlay enabled state after config operations
		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;

		logger::info("A/B Testing enabled - starting with Variant B (TEST). Both variants cached in memory for unbiased swapping.");
	}
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

	/** @brief Request that sets one catalogued setting of a feature, leaving its other settings as they are. */
	ABComparisonRequest SettingRequest(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target, std::string label)
	{
		return { std::move(label), [&feature, setting, target] {
					nlohmann::json settings;
					feature.SaveSettings(settings);
					if (auto* slot = Util::Settings::GetCatalogSerializedValue(settings, setting))
						*slot = target;
					feature.LoadSettings(settings);
				} };
	}
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
	return true;
}

void ABTestingManager::BeginComparison(size_t index)
{
	auto* state = globals::state;
	const auto& request = queue[index];

	auto original = queueOriginal;
	state->LoadFromJson(original);
	const auto pendingBefore = FeaturesPendingRestart();
	{
		SceneSettingsManager::SceneLayerGuard guard(*SceneSettingsManager::GetSingleton());
		request.apply();
	}
	std::string restartNeeded;
	for (const auto& name : FeaturesPendingRestart()) {
		if (!pendingBefore.contains(name))
			restartNeeded += (restartNeeded.empty() ? "" : ", ") + name;
	}
	testConfigSnapshot = nlohmann::json::object();
	state->SaveToJson(testConfigSnapshot);
	original = queueOriginal;
	state->LoadFromJson(original);

	const bool unchanged = testConfigSnapshot == queueOriginal;
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

	userConfigSnapshot = queueOriginal;
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
	const auto comparison = ComputeABComparison(aggregator.GetHeadlineIntervals());
	runResults.push_back({ queue[queueIndex].label, comparison, FrameBudgetMs(), comparison.valid ? "" : "No timing samples were collected." });
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
	testInterval = 0;
	ReleaseProfiler();

	if (finished)
		RevealResults();
	else
		logger::info("[A/B] Comparison cancelled, settings restored");
}

/** The overlay window is where the A/B results draw, so it is forced visible rather than left to its toggle. */
void ABTestingManager::RevealResults()
{
	globals::features::performanceOverlay.settings.ShowInOverlay = true;
	if (auto* menu = Menu::GetSingleton()) {
		menu->overlayVisible = true;
		if (menuClosedByRun)
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
			{ "percentOfBudget", run.budgetMs > 0.0f ? c.deltaMs / run.budgetMs * 100.0f : 0.0f },
			{ "verdict", verdict },
			{ "note", run.note } });
	}
	return { { "comparisonRunning", queueActive },
		{ "comparison", queueActive ? nlohmann::json(queue[queueIndex].label) : nlohmann::json(nullptr) },
		{ "comparisonIndex", queueActive ? queueIndex + 1 : 0 },
		{ "comparisonCount", queue.size() },
		{ "frameBudgetMs", FrameBudgetMs() },
		{ "runs", std::move(runs) } };
}

bool ABTestingManager::StartSettingComparison(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target, std::string label, std::string* error)
{
	nlohmann::json probe;
	feature.SaveSettings(probe);
	if (!Util::Settings::GetCatalogSerializedValue(probe, setting))
		return Refuse(error, "The setting is not present in the feature's saved settings.");

	std::vector<ABComparisonRequest> requests;
	requests.push_back(SettingRequest(feature, setting, target, std::move(label)));
	return StartComparisons(std::move(requests), {}, error);
}

bool ABTestingManager::StartSettingComparisonByKey(const std::string& featureShortName, const std::string& key, const std::string& path, const nlohmann::json& value, const ABRunOptions& options, std::string* error)
{
	Feature* owner = nullptr;
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->GetShortName() == featureShortName)
			owner = feature;
	}
	if (!owner)
		return Refuse(error, "Unknown feature: " + featureShortName);

	for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
		if (setting.featureShortName != featureShortName || setting.serializedKey != key || setting.serializedPath != path)
			continue;
		nlohmann::json probe;
		owner->SaveSettings(probe);
		if (!Util::Settings::GetCatalogSerializedValue(probe, setting))
			return Refuse(error, "The setting is not present in the feature's saved settings.");

		std::vector<ABComparisonRequest> requests;
		requests.push_back(SettingRequest(*owner, setting, value, Util::Settings::GetCatalogLeafDisplayName(setting) + " = " + value.dump()));
		return StartComparisons(std::move(requests), options, error);
	}
	return Refuse(error, std::format("No catalogued setting {}/{}/{}.", featureShortName, path, key));
}

bool ABTestingManager::StartProfileComparisons(const std::string& featureShortName, const ABRunOptions& options, std::string* error)
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

	const std::pair<Feature::PerfProfile, const char*> profiles[] = {
		{ Feature::PerfProfile::Performance, T("feature.perf_overlay.profile_performance", "Performance") },
		{ Feature::PerfProfile::Balanced, T("feature.perf_overlay.profile_balanced", "Balanced") },
		{ Feature::PerfProfile::Quality, T("feature.perf_overlay.profile_quality", "Quality") },
	};
	std::vector<ABComparisonRequest> requests;
	for (const auto& [profile, name] : profiles) {
		requests.push_back({ std::string(name) + " preset", [only, profile] {
								if (only)
									only->ApplyPerformanceProfile(profile);
								else
									Feature::ApplyPerformanceProfileToAll(profile);
							} });
	}
	return StartComparisons(std::move(requests), options, error);
}

void ABTestingManager::DrawSettingsUI()
{
	auto& performanceOverlay = globals::features::performanceOverlay;

	if (ImGui::SliderInt(T(TKEY("abtest_interval"), "A/B Test Interval"), reinterpret_cast<int*>(&testInterval), 0, 10)) {
		bool overlayWasEnabled = performanceOverlay.settings.ShowInOverlay;
		if (testInterval == 0) {
			Disable();
		} else if (!abTestingEnabled) {
			Enable();
		} else {
			logger::info("Setting new A/B test interval {}.", testInterval);
		}
		performanceOverlay.settings.ShowInOverlay = overlayWasEnabled;
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s",
			T(TKEY("abtest_tooltip"),
				"A/B Testing compares two configurations by automatically swapping between them.\n"
				"Workflow: Configure your test settings, then enable A/B testing.\n"
				"- Variant B (TEST) = Your current settings when you enable testing\n"
				"- Variant A (USER) = Your previously saved user configuration\n"
				"The initial Variant B interval is an unmeasured warm-up, then testing begins with Variant A.\n"
				"Set to 0 to disable and restore TEST settings."));
	}
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
	if (!ImGui::Begin("A/B Testing", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::End();
		return;
	}

	remaining = std::max(0.0f, remaining);

	// Show current variant and time
	const char* variantLabel = aggregator.IsWarmingUp() ?
	                               T(TKEY("variant_b_warmup"), "Variant B (TEST) warm-up") :
	                           usingTestConfig ? T(TKEY("variant_b_test"), "Variant B (TEST)") :
	                                             T(TKEY("variant_a_user"), "Variant A (USER)");
	ImGui::Text("%s", fmt::format(fmt::runtime(T(TKEY("variant_time_left_fmt"), "{} : {:.1f}s left")),
						  variantLabel, remaining)
						  .c_str());

	// Show what changed (for both variants)
	if (hasTestSnapshot) {
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
