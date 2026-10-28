#pragma once
#include "ABCompositorStats.h"
#include "ABTestAggregator.h"
#include "Utils/FileSystem.h"
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct DrawCallRow;
struct Feature;
namespace SceneSettingsCatalog
{
	struct SettingMetadata;
}

/** @brief One comparison of a settings change against the current settings: variant B is the original settings with `apply` run over them. */
struct ABComparisonRequest
{
	std::string label;
	std::function<void()> apply;
};

/** @brief Pacing of a run of comparisons. Each comparison alternates A and B, ending on A so every B is bracketed. */
struct ABRunOptions
{
	uint32_t intervalSeconds = 4;
	uint32_t repeats = 2;        ///< B intervals per comparison.
	uint32_t settleFrames = 45;  ///< Frames ignored at the start of every interval so a swap can settle.
	bool closeMenu = true;       ///< Close the menu for the run and reopen it with the results.
};

/** @brief The outcome of one comparison in a run. */
struct ABRunResult
{
	std::string label;
	ABComparison comparison;
	float budgetMs = 0.0f;  ///< Frame budget the delta is a share of.
	std::string note;       ///< Why there is no comparison, when there is none.
};

/**
 * @brief Manages A/B testing of feature configurations for performance comparison.
 *
 * Automatically swaps between two configuration snapshots (USER and TEST) at a
 * configurable interval, collecting per-frame draw call timing data through the
 * ABTestAggregator. Both snapshots are held in memory to avoid disk I/O during swaps.
 */
/**
 * @brief Reads the newest completed SteamVR compositor frame.
 *
 * @param sample Receives the frame when one is new.
 * @param lastFrameIndex Compositor frame index already consumed; updated on success.
 * @return True when a frame newer than lastFrameIndex was read. Always false outside VR.
 */
bool ReadLatestCompositorSample(CompositorSample& sample, uint32_t& lastFrameIndex, bool* timingAvailable = nullptr);

class ABTestingManager
{
public:
	/** @brief Returns the global singleton instance. */
	static ABTestingManager* GetSingleton();

	/**
	 * @brief Sets the swap interval for A/B testing.
	 * @param interval Number of seconds between variant swaps.
	 */
	void SetTestInterval(uint32_t interval);

	/** @brief Returns the current swap interval in seconds. */
	uint32_t GetTestInterval() const { return testInterval; }

	/** @brief Returns true if A/B testing is currently running. */
	bool IsEnabled() const { return abTestingEnabled; }

	/** @brief True while a run of comparisons is in progress (a subset of IsEnabled). */
	bool IsComparisonRunning() const { return queueActive; }

	/**
	 * @brief Runs each request as an A/B comparison against the current settings, then restores them.
	 *
	 * Results accumulate in GetRunResults and are revealed in the Performance overlay when the run ends.
	 * @param error Receives the reason when the run cannot start.
	 */
	bool StartComparisons(std::vector<ABComparisonRequest> requests, const ABRunOptions& options, std::string* error);

	/** @brief Compares the current settings with one catalogued setting changed to `target`. */
	bool StartSettingComparison(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target, std::string label, std::string* error);

	/** @brief Finds a catalogued setting by feature short name, serialized key and serialized path (empty for a top-level key), then runs StartSettingComparison. */
	bool StartSettingComparisonByKey(const std::string& featureShortName, const std::string& key, const std::string& path, const nlohmann::json& value, const ABRunOptions& options, std::string* error);

	/**
	 * @brief Compares the current settings with each Performance, Balanced and Quality profile that is not already in effect.
	 * @param featureShortName Limit the profile to one feature; empty applies it to every feature, as the global preset cards do.
	 */
	bool StartProfileComparisons(const std::string& featureShortName, const ABRunOptions& options, std::string* error);

	/** @brief Results of the comparisons run since they were last cleared. */
	const std::vector<ABRunResult>& GetRunResults() const { return runResults; }

	/** @brief Discards the comparison results. */
	void ClearRunResults() { runResults.clear(); }

	/** @brief Progress and results of the current or last run, for devbench. */
	nlohmann::json StatusJson() const;

	/** @brief Returns true if the TEST (variant B) configuration is currently active. */
	bool IsUsingTestConfig() const { return usingTestConfig; }

	/**
	 * @brief Starts A/B testing.
	 *
	 * Snapshots the current settings as variant B (TEST), loads and snapshots
	 * the saved user settings as variant A (USER), then begins the test cycle
	 * starting with variant B.
	 */
	void Enable();

	/**
	 * @brief Stops A/B testing and restores the TEST configuration.
	 *
	 * Loads the variant B (TEST) snapshot back into the active state so the
	 * user returns to the settings they had when testing was enabled.
	 */
	void Disable();

	/**
	 * @brief Per-frame update that handles timed variant swapping.
	 *
	 * Checks elapsed time since the last swap and, when the interval expires,
	 * toggles between USER and TEST configuration snapshots (in-memory, no disk I/O).
	 */
	void Update();

	/** @brief Draws the ImGui A/B test interval slider in the settings panel. */
	void DrawSettingsUI();

	/** @brief Draws the in-game overlay showing the active variant and remaining swap time. */
	void DrawOverlayUI();

	/** @brief Returns a mutable reference to the underlying test data aggregator. */
	ABTestAggregator& GetAggregator() { return aggregator; }

	/** @brief Per-frame data collection for the active run; call once per frame while IsEnabled. */
	void RecordFrame(const std::vector<DrawCallRow>& rows);

	/**
	 * @brief Records the newest completed SteamVR compositor frame for the active variant.
	 *
	 * Does nothing outside VR, while disabled, during the unmeasured warm-up interval, or when the
	 * runtime reports no frame timing. Call once per frame.
	 */
	void SampleCompositor();

	/** @brief Returns the SteamVR compositor timing gathered for variants A and B. */
	const CompositorStatsAccumulator& GetCompositorStats() const { return compositorStats; }

	/** @brief Returns true once the runtime has returned at least one compositor frame timing. */
	bool IsCompositorTimingAvailable() const { return compositorTimingAvailable; }

	/** @brief Returns true if both USER and TEST configuration snapshots are available. */
	bool HasCachedSnapshots() const { return hasTestSnapshot && hasUserSnapshot; }

	/** @brief Returns the cached USER (variant A) configuration snapshot. */
	const nlohmann::json& GetUserSnapshot() const { return userConfigSnapshot; }

	/** @brief Returns the cached TEST (variant B) configuration snapshot. */
	const nlohmann::json& GetTestSnapshot() const { return testConfigSnapshot; }

	/**
	 * @brief Returns a human-readable list of setting differences between USER and TEST.
	 * @return Vector of formatted strings in "path: oldValue -> newValue" form.
	 */
	std::vector<std::string> GetConfigDifferencesForDisplay() const;

	/**
	 * @brief Returns structured diff entries between the USER and TEST snapshots.
	 * @param epsilon Floating-point comparison tolerance for detecting changes.
	 * @return Vector of SettingsDiffEntry describing each changed setting.
	 */
	std::vector<SettingsDiffEntry> GetConfigDiffEntries(float epsilon = 0.0001f) const;

	/** @brief Replaces persisted internal names with their UI display names. */
	static std::string GetSettingsPathDisplayName(std::string path);

	/** @brief Discards both cached configuration snapshots. Called when overlay results are cleared. */
	void ClearCachedSnapshots();

private:
	uint32_t testInterval = 0;
	bool abTestingEnabled = false;
	bool usingTestConfig = false;
	LARGE_INTEGER timingFrequency{};
	LARGE_INTEGER lastTestSwitch{};
	ABTestAggregator aggregator;
	CompositorStatsAccumulator compositorStats;
	uint32_t lastCompositorFrameIndex = UINT32_MAX;
	bool compositorTimingAvailable = false;

	// In-memory storage for both variants to avoid disk I/O during swapping
	nlohmann::json testConfigSnapshot;
	nlohmann::json userConfigSnapshot;
	bool hasTestSnapshot = false;
	bool hasUserSnapshot = false;

	// Track what changed between USER and TEST configs
	std::vector<std::string> GetConfigDifferences() const;

	/** @brief Loads the TEST snapshot, clears the collected data and starts the first (warm-up) interval; both snapshots must be set. */
	void BeginRun();
	void SampleProfilerHeadline();
	void AcquireProfiler();
	void ReleaseProfiler();
	void BeginComparison(size_t index);
	void FinishComparison();
	void EndComparisons(bool finished);
	void RevealResults();

	std::vector<ABComparisonRequest> queue;
	size_t queueIndex = 0;
	ABRunOptions queueOptions;
	nlohmann::json queueOriginal;
	std::vector<ABRunResult> runResults;
	bool queueActive = false;
	std::atomic<bool> cancelRequested = false;  ///< Set off the render thread; Update() restores the settings so a cancel cannot race a swap.
	bool menuClosedByRun = false;
	bool profilerOwned = false;
	bool profilerWasEnabled = false;
	uint32_t lastProfilerFrame = UINT32_MAX;
};
