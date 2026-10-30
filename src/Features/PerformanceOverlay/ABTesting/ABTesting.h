#pragma once
#include "ABCompositorStats.h"
#include "ABTestAggregator.h"
#include "Utils/FileSystem.h"
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

struct DrawCallRow;
struct Feature;
namespace SceneSettingsCatalog
{
	struct SettingMetadata;
}

/**
 * @brief One comparison of a settings change against the current settings.
 *
 * Variant A is the current settings with `prepare` run over them, variant B is that with `apply` run over it.
 * An empty `prepare` leaves variant A as the current settings.
 */
struct ABComparisonRequest
{
	std::string label;
	std::function<void()> apply;
	std::function<void()> prepare;
};

/** @brief Pacing of a run of comparisons. Each comparison alternates A and B, ending on A so every B is bracketed. */
struct ABRunOptions
{
	uint32_t intervalSeconds = 4;
	uint32_t repeats = 2;        ///< B intervals per comparison.
	uint32_t settleFrames = 45;  ///< Frames ignored at the start of every interval so a swap can settle.
	bool closeMenu = true;       ///< Close the menu for the run and reopen it with the results.
	bool together = false;       ///< Apply every queued change at once as one comparison instead of measuring each on its own.
};

/** @brief A comparison waiting for the next run; `key` identifies the subject so it is not queued twice. */
struct ABQueuedComparison
{
	std::string key;
	ABComparisonRequest request;
};

/** @brief A setting a comparison must set before its own change, and the value it must hold. */
struct ABSettingPrerequisite
{
	const SceneSettingsCatalog::SettingMetadata* setting;
	nlohmann::json requiredValue;
};

/** @brief The outcome of one comparison in a run. */
struct ABRunResult
{
	std::string label;
	ABComparison comparison;
	float budgetMs = 0.0f;  ///< Frame budget the delta is a share of.
	std::string note;       ///< Why there is no comparison, when there is none.
};

/** @brief One row of the benchmark table: a finished result, the comparison running now, or one still waiting to run. */
struct ABBenchmarkRow
{
	std::string key;  ///< Queue identity while the row can still be removed from the queue; empty otherwise.
	ABRunResult result;
	enum class Status
	{
		Queued,
		Running,
		Done,
		Skipped
	} status;
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

	/** @brief Identity of a catalogued setting in the queue, also used to outline it in its feature menu. */
	static std::string SettingKey(const SceneSettingsCatalog::SettingMetadata& setting);

	/** @brief Adds a comparison of the current settings with one catalogued setting changed to `target`; queuing the same setting again replaces it. */
	bool QueueSetting(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target, std::string label, std::string* error);

	/** @brief Adds a comparison of one catalogued setting at `from` against `to`; queuing the same setting again replaces it. */
	bool QueueSettingChange(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& from, const nlohmann::json& to, std::string label, std::string* error);

	/**
	 * @brief Reads a catalogued setting's value from the saved user configuration.
	 * @return The saved value, or nullopt when the file cannot be read or does not hold that setting.
	 */
	std::optional<nlohmann::json> SavedSettingValue(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting);

	/**
	 * @brief The settings that must be turned on for one catalogued setting to be usable, outermost first.
	 *
	 * Reads the feature's current settings, so prerequisites already at their required value are left out.
	 * @return The steps a comparison prepares first, empty when the setting is usable as it is, or nullopt when
	 *         the chain cannot be resolved (a cycle or a setting that is not catalogued).
	 */
	std::optional<std::vector<ABSettingPrerequisite>> PrerequisitesFor(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting);

	/**
	 * @brief Display names of the features that would need a restart for a change to one catalogued setting to take effect.
	 * @param target Value the setting would be changed to.
	 * @param from Value the setting is first set to, when the comparison prepares it; empty for none.
	 * @return The names, comma separated; empty when the change can be measured live.
	 */
	std::string SettingRestartReason(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, const nlohmann::json& target,
		const std::optional<nlohmann::json>& from = std::nullopt);

	/** @brief Finds a catalogued setting by feature short name, serialized key and serialized path (empty for a top-level key), then queues it. */
	bool QueueSettingByKey(const std::string& featureShortName, const std::string& key, const std::string& path, const nlohmann::json& value, std::string* error);

	/**
	 * @brief Adds a comparison with each of the Performance, Balanced and Quality presets.
	 * @param featureShortName Limit the preset to one feature; empty applies it to every feature, as the global preset cards do.
	 * @return False when nothing was queued: a run is in progress, or every preset needs a restart.
	 */
	bool QueueProfiles(const std::string& featureShortName, std::string* error);

	/** @brief Presets the last QueueProfiles left out because they need a restart, ready to show; empty when none were left out. */
	const std::string& QueueNotice() const { return queueNotice; }

	/** @brief Removes a queued comparison; false when it was not queued. */
	bool Unqueue(const std::string& key);

	bool IsQueued(const std::string& key) const;
	void ClearQueue();
	const std::vector<ABQueuedComparison>& GetQueue() const { return comparisonQueue; }

	/** @brief Every row of the benchmark table, in order: finished results, the comparison running now, those still queued for the run, then the queue. */
	std::vector<ABBenchmarkRow> GetBenchmarkRows() const;

	/** @brief Pacing for the next RunQueue. */
	ABRunOptions& RunOptions() { return runOptions; }

	/** @brief Runs every queued comparison in order and empties the queue. */
	bool RunQueue(std::string* error);

	/** @brief Draws the Run benchmark and Clear all buttons, both disabled while a run is in progress or the queue is empty. */
	void DrawQueueButtons();

	/** @brief Draws the Performance page Benchmark tab: the queue mode, the buttons, the benchmark table, pacing, progress and the live A/B controls. */
	void DrawBenchmarkTab();

	/** @brief Forces the performance overlay visible so the comparison in progress and its results can be seen. */
	void RevealOverlay();

	/** @brief Results of the comparisons run since they were last cleared. */
	const std::vector<ABRunResult>& GetRunResults() const { return runResults; }

	/** @brief Discards the comparison results. */
	void ClearRunResults() { runResults.clear(); }

	/** @brief Progress and results of the current or last run, for devbench. */
	nlohmann::json StatusJson() const;

	/** @brief Returns true if the TEST (variant B) configuration is currently active. */
	bool IsUsingTestConfig() const { return usingTestConfig; }

	/**
	 * @brief Starts the live A/B test.
	 *
	 * Snapshots the current settings as variant B (TEST), loads and snapshots
	 * the saved user settings as variant A (USER), then begins the test cycle
	 * starting with variant B.
	 * @return False when the current settings match the saved ones, so nothing changed: the
	 *         settings are restored, no test starts and LiveNotice() says why.
	 */
	bool Enable();

	/** @brief Why the last start did not begin a live A/B test; empty once a start succeeds. */
	const std::string& LiveNotice() const { return liveNotice; }

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

	/** @brief Draws the live A/B controls: the swap pace, Start/Stop and what the two variants are. */
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
	uint32_t testInterval = 4;
	uint32_t liveTestInterval = 4;
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

	/** @brief Display names of the features a trial change leaves needing a restart, comma separated; the state is restored before returning. */
	static std::string RestartNeededBy(const std::function<void()>& apply);

	/** @brief Queues `request` under the setting's key, refusing when the setting is absent from the feature or a run is active. */
	bool EnqueueSetting(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, ABComparisonRequest request, std::string* error);

	bool Enqueue(ABQueuedComparison item);

	std::vector<ABQueuedComparison> comparisonQueue;
	ABRunOptions runOptions;
	std::vector<ABComparisonRequest> queue;
	size_t queueIndex = 0;
	ABRunOptions queueOptions;
	nlohmann::json queueOriginal;
	std::vector<ABRunResult> runResults;
	std::string liveNotice;
	std::string queueNotice;
	bool queueActive = false;
	std::atomic<bool> cancelRequested = false;  ///< Set off the render thread; Update() restores the settings so a cancel cannot race a swap.
	bool menuClosedByRun = false;
	bool profilerOwned = false;
	bool profilerWasEnabled = false;
	uint32_t lastProfilerFrame = UINT32_MAX;
	std::chrono::steady_clock::time_point lastFrameTime;  ///< Previous RecordFrame, so flat frame time is the interval between calls.
};
