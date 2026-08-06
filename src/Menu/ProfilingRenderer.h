#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "Profiler.h"
#include "Utils/LegitProfiler.h"

/**
 * @brief Renders GPU and CPU profiling statistics, timing graphs, and per-feature timer breakdowns.
 *
 * Provides the Profiling page in the menu with frame time graphs, sortable
 * timing tables grouped by rendering pass, and per-feature timer views that
 * can be embedded in individual feature settings panels.
 */
class ProfilingRenderer
{
public:
	/** @brief Selects whether profiling displays GPU or CPU timing data. */
	enum class TimingMode
	{
		GPU,
		CPU
	};

	/** @brief Per-feature profiling state: disabled, or displaying GPU/CPU timings. */
	enum class FeatureTimingMode
	{
		Off,
		GPU,
		CPU
	};

	/**
	 * @brief Renders the main profiling statistics view with timing graph and pass table.
	 *
	 * Draws a frame time graph, a GPU/CPU mode toggle, and a grouped table of
	 * per-pass timing statistics with average, P95, and P99 columns.
	 *
	 * @param showTable If true, renders the detailed timing table below the graph.
	 * @param showModeToggle If true, renders the GPU/CPU timing mode toggle.
	 */
	static void RenderStatistics(bool showTable = true, bool showModeToggle = true);

	/**
	 * @brief Renders the per-feature profiling section with Off/GPU/CPU controls.
	 *
	 * Selecting GPU or CPU requests a profiler capture and displays the
	 * feature's timers as an inline graph and table. Off requests no
	 * capture from this panel; capture is global, so overhead stops only
	 * once no other visible view is requesting it either.
	 *
	 * @param featurePrefix The profiler timer name prefix identifying the feature
	 *        (e.g. "ScreenSpaceGI").
	 */
	static void RenderFeatureTimers(const std::string& featurePrefix);

	/**
	 * @brief Checks whether the per-feature profiling section should be shown.
	 *
	 * Always true while the profiler exists, so the Off/GPU/CPU controls stay
	 * reachable even before any capture has produced timer results.
	 */
	static bool HasFeatureTimers(const std::string& featurePrefix);

private:
	static inline TimingMode timingMode = TimingMode::GPU;
	static inline float timeSinceLastUpdate = 0.0f;
	static inline float lastFrameTime = 0.0f;

	struct PassEntry
	{
		std::string label;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};
	struct GroupEntry
	{
		std::string name;
		float totalAvgMs = 0.0f;
		float totalP95Ms = 0.0f;
		float totalP99Ms = 0.0f;
		std::vector<PassEntry> passes;
	};
	static inline float cachedTotalAvgMs = 0.0f;
	static inline float cachedTotalP95Ms = 0.0f;
	static inline float cachedTotalP99Ms = 0.0f;
	static inline float cachedMaxAvgMs = 0.0f;
	static inline float cachedMaxP95Ms = 0.0f;
	static inline float cachedMaxP99Ms = 0.0f;
	static inline std::vector<GroupEntry> cachedGroups;

	static inline ImGuiUtils::ProfilerGraph gpuGraph{ Profiler::kHistorySize };

	struct FeatureGraphState
	{
		ImGuiUtils::ProfilerGraph gpuGraph{ Profiler::kHistorySize };
		ImGuiUtils::ProfilerGraph cpuGraph{ Profiler::kHistorySize };
	};
	static inline std::unordered_map<std::string, FeatureGraphState> featureGraphs;
	static inline std::unordered_map<std::string, FeatureTimingMode> featureTimingModes;

	struct FeatureTimingEntry
	{
		std::string label;
		/// Feature-qualified name, so two features' same-named passes (e.g.
		/// both having a "Downsample") don't collide onto the same color.
		std::string colorKey;
		float timeMs;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};
	struct FeatureTimingData
	{
		std::vector<FeatureTimingEntry> entries;
		float totalAvg = 0.0f;
		float totalP95 = 0.0f;
		float totalP99 = 0.0f;
		float maxAvg = 0.0f;
		float maxP95 = 0.0f;
		float maxP99 = 0.0f;
	};

	static ImU32 GetGroupColor(std::string_view groupName);
	static uint32_t ToLegitColor(ImU32 imColor);
	static ImVec4 HeatColor(float value, float maxValue);
	static void TextHeat(const char* fmt, float value, float maxValue);
	static void RenderTimingModeToggle();
	static void SetupTimingTableColumns(float passColumnWidth, bool includePercentColumn);
	static void RenderGraph();
	static std::string GetFeatureTimerPrefix(const std::string& featurePrefix);
	static bool IsFeatureTimerResult(const Profiler::TimerResult& result, std::string_view prefix);

	static FeatureTimingData CollectFeatureTimingData(const std::string& featurePrefix, bool cpuMode);
	static int ComputeFeatureGraphLegendWidth(const FeatureTimingData& data, int totalWidth);
	static bool RenderFeatureTimingGraph(const FeatureTimingData& data, ImGuiUtils::ProfilerGraph& graph, float graphHeight);
	static bool RenderFeatureTimingData(const std::string& featurePrefix, FeatureTimingMode featureMode, bool showTable);
	static bool RenderFeatureOverview();
};
