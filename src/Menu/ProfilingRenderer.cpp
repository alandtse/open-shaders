#include "ProfilingRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "CSEditor/Browser/BrowserWidgets.h"
#include "Feature.h"
#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Fonts.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/MenuIcons.h"
#include "Menu/ThemeManager.h"
#include "State.h"
#include "Utils/Format.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

static constexpr uint32_t kDisplayedRollingFrameCount = 60;
static constexpr float kMaxDisplayTimingSampleMs = 1000.0f;
static constexpr float kGraphHeadroomScale = 1.2f;
static constexpr float kMainGraphHeight = 180.0f;
static constexpr float kFeatureGraphHeight = 100.0f;
static constexpr float kMainGraphMinFrameTimeSec = 0.0001f;
static constexpr float kFeatureGraphMinFrameTimeSec = 0.00001f;
static constexpr float kTimingTableMetricColumnWidth = 55.0f;
static constexpr float kTimingTablePercentColumnWidth = 45.0f;
static constexpr float kStatsRefreshSeconds = 1.0f;

static ImU32 HslToImU32(float h, float s, float l)
{
	auto hue2rgb = [](float p, float q, float t) -> float {
		if (t < 0.0f)
			t += 1.0f;
		if (t > 1.0f)
			t -= 1.0f;
		if (t < 1.0f / 6.0f)
			return p + (q - p) * 6.0f * t;
		if (t < 0.5f)
			return q;
		if (t < 2.0f / 3.0f)
			return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
		return p;
	};

	float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
	float p = 2.0f * l - q;
	float r = hue2rgb(p, q, h + 1.0f / 3.0f);
	float g = hue2rgb(p, q, h);
	float b = hue2rgb(p, q, h - 1.0f / 3.0f);

	return IM_COL32(
		static_cast<uint8_t>(r * 255.0f),
		static_cast<uint8_t>(g * 255.0f),
		static_cast<uint8_t>(b * 255.0f),
		255);
}

// Comparators for a timing table's columns: Pass by name (case-insensitive), then
// Avg / P95 / P99 by value, and % by the average's share, which orders like the
// average itself. Rows are pointers into the cached entries.
template <class Row, class NameOf, class MetricsOf>
static std::vector<Util::TableRowSortFunc<const Row*>> TimingRowSorts(NameOf a_nameOf, MetricsOf a_metricsOf)
{
	auto byName = [a_nameOf](const Row* a_lhs, const Row* a_rhs, bool a_ascending) {
		auto less = [](const std::string& a_l, const std::string& a_r) {
			return std::ranges::lexicographical_compare(a_l, a_r, [](char a_x, char a_y) {
				return std::tolower(static_cast<unsigned char>(a_x)) < std::tolower(static_cast<unsigned char>(a_y));
			});
		};
		return a_ascending ? less(a_nameOf(*a_lhs), a_nameOf(*a_rhs)) : less(a_nameOf(*a_rhs), a_nameOf(*a_lhs));
	};
	auto byMetric = [a_metricsOf](size_t a_index) {
		return [a_metricsOf, a_index](const Row* a_lhs, const Row* a_rhs, bool a_ascending) {
			const float l = a_metricsOf(*a_lhs)[a_index];
			const float r = a_metricsOf(*a_rhs)[a_index];
			return a_ascending ? l < r : r < l;
		};
	};
	return { byName, byMetric(0), byMetric(1), byMetric(2), byMetric(0) };
}

// Guards rolling stats against disjoint-frame spikes and non-finite samples,
// mirroring Profiler.cpp's own IsValidProfilerSample check on the source data.
static bool IsDisplayTimingSampleValid(float value)
{
	return std::isfinite(value) && value >= 0.0f && value <= kMaxDisplayTimingSampleMs;
}

static int ScaleToUiInt(float value)
{
	return std::max(1, static_cast<int>(std::round(value * Util::GetUIScale())));
}

// FNV-1a + a MurmurHash3-style finalizer: a deterministic per-name color that
// stays stable across sessions and discovery order, unlike an insertion-order
// counter (which reshuffles colors whenever a new pass is first seen).
static uint32_t FinalizeHash(uint32_t hash)
{
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	hash *= 0x846ca68bu;
	hash ^= hash >> 16;
	return hash;
}

static uint32_t StableHash(std::string_view value)
{
	return FinalizeHash(Util::Fnv1a32(value));
}

static uint32_t MixHash(uint32_t hash, uint32_t salt)
{
	hash ^= salt + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return FinalizeHash(hash);
}

static float HashToUnitFloat(uint32_t hash)
{
	return static_cast<float>(static_cast<double>(hash) / 4294967296.0);
}

static float GetColorMarkerExtraWidth()
{
	return std::ceil(std::max(6.0f, ImGui::GetTextLineHeight() * 0.65f) + ImGui::GetStyle().ItemInnerSpacing.x);
}

// Small colored swatch ties a table row to its matching graph segment.
static void RenderColorMarker(ImU32 color)
{
	const float lineHeight = ImGui::GetTextLineHeight();
	const float markerSize = std::max(6.0f, std::floor(lineHeight * 0.65f));
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const float markerY = cursor.y + (lineHeight - markerSize) * 0.5f;
	const ImVec2 markerMin(cursor.x, markerY);
	const ImVec2 markerMax(cursor.x + markerSize, markerY + markerSize);

	auto* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(markerMin, markerMax, color, 2.0f);
	drawList->AddRect(markerMin, markerMax, ImGui::GetColorU32(ImGuiCol_Border), 2.0f);

	ImGui::Dummy(ImVec2(markerSize, lineHeight));
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
}

static void ReplaceAll(std::string& value, std::string_view from, std::string_view to)
{
	if (from.empty())
		return;

	size_t pos = 0;
	while ((pos = value.find(from.data(), pos, from.size())) != std::string::npos) {
		value.replace(pos, from.size(), to.data(), to.size());
		pos += to.size();
	}
}

// Abbreviates common long substrings and truncates so graph legends fit
// their column instead of being clipped mid-word.
static std::string BuildProfilerGraphLabel(std::string_view label)
{
	std::string result(label.data(), label.size());
	ReplaceAll(result, "ScreenSpaceShadows", "SSShadows");
	ReplaceAll(result, "ScreenSpace", "SS");
	ReplaceAll(result, "CommunityShaders", "CS");
	ReplaceAll(result, "SubsurfaceScattering", "SSS");
	ReplaceAll(result, "DynamicResolution", "DynRes");
	ReplaceAll(result, "Visualization", "Viz");
	ReplaceAll(result, "Composite", "Comp");
	ReplaceAll(result, "Dispatch", "Disp");
	ReplaceAll(result, "Foveated", "Fov");
	ReplaceAll(result, "Periphery", "Periph");
	ReplaceAll(result, "Temporal", "Temp");
	ReplaceAll(result, "Dynamic", "Dyn");
	ReplaceAll(result, "Resolution", "Res");
	ReplaceAll(result, "Upscaling", "Upscale");
	ReplaceAll(result, "Render", "Rnd");
	ReplaceAll(result, "Shader", "Shd");
	ReplaceAll(result, "::", ":");

	constexpr size_t kMaxGraphLabelLength = 34;
	if (result.size() > kMaxGraphLabelLength)
		result = result.substr(0, kMaxGraphLabelLength - 2) + "..";

	return result;
}

static int ComputeGraphLegendWidth(int totalWidth, int minGraphWidth, float widthFraction, int minLegendWidth, int maxLegendWidth)
{
	const int reservedGraphWidth = std::min(minGraphWidth, totalWidth);
	const int availableLegendWidth = std::max(0, totalWidth - reservedGraphWidth);
	if (availableLegendWidth <= 0)
		return 0;

	const int desiredLegendWidth = std::clamp(static_cast<int>(totalWidth * widthFraction), minLegendWidth, maxLegendWidth);
	return std::min(desiredLegendWidth, availableLegendWidth);
}

int ProfilingRenderer::ComputeFeatureGraphLegendWidth(const FeatureTimingData& data, int totalWidth)
{
	if (data.entries.empty())
		return 0;

	const float uiScale = Util::GetUIScale();
	constexpr float legendTextScale = 0.74f;
	const float markerAndConnectorWidth = (3.0f + 5.0f + 18.0f + 8.0f + 5.0f) * uiScale;
	const float textColumnWidth = std::max(
		48.0f * uiScale,
		ImGui::CalcTextSize("000.00ms").x * legendTextScale + 5.0f * uiScale);
	float labelWidth = 0.0f;
	for (const auto& entry : data.entries) {
		const auto label = BuildProfilerGraphLabel(entry.label);
		labelWidth = std::max(labelWidth, ImGui::CalcTextSize(label.c_str()).x * legendTextScale);
	}

	const int desiredLegendWidth = static_cast<int>(std::ceil(markerAndConnectorWidth + textColumnWidth + labelWidth + 10.0f * uiScale));
	const int minGraphWidth = ScaleToUiInt(24.0f);
	const int availableLegendWidth = std::max(0, totalWidth - std::min(minGraphWidth, totalWidth));
	return std::min(desiredLegendWidth, availableLegendWidth);
}

static float GetTextColumnWidth(const char* header, const std::vector<std::string>& labels, float extraWidth = 0.0f)
{
	float width = ImGui::CalcTextSize(header).x;
	for (const auto& label : labels)
		width = std::max(width, ImGui::CalcTextSize(label.c_str()).x);

	return std::ceil(width + ImGui::GetStyle().CellPadding.x * 2.0f + extraWidth);
}

ImU32 ProfilingRenderer::GetGroupColor(std::string_view groupName)
{
	const uint32_t hash = StableHash(groupName);
	const float hue = HashToUnitFloat(hash);
	const float saturation = 0.68f + HashToUnitFloat(MixHash(hash, 0xA511E9B3u)) * 0.12f;
	const float lightness = 0.50f + HashToUnitFloat(MixHash(hash, 0x63D83595u)) * 0.10f;
	return HslToImU32(hue, saturation, lightness);
}

uint32_t ProfilingRenderer::ToLegitColor(ImU32 imColor)
{
	uint8_t r = (imColor >> 0) & 0xFF;
	uint8_t g = (imColor >> 8) & 0xFF;
	uint8_t b = (imColor >> 16) & 0xFF;
	return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

ImVec4 ProfilingRenderer::HeatColor(float value, float maxValue)
{
	if (maxValue <= 0.0f)
		return Util::Colors::GetDefault();

	float x = std::clamp(value / maxValue, 0.0f, 1.0f);

	float x2 = x * x;
	float x3 = x2 * x;
	float x4 = x2 * x2;
	float x5 = x3 * x2;

	float r = 0.13572138f + 4.61539260f * x - 42.66032258f * x2 + 132.13108234f * x3 - 152.94239396f * x4 + 59.28637943f * x5;
	float g = 0.09140261f + 2.19418839f * x + 4.84296658f * x2 - 14.18503333f * x3 + 4.27729857f * x4 + 2.82956604f * x5;
	float b = 0.10667330f + 12.64194608f * x - 60.58204836f * x2 + 110.36276771f * x3 - 89.90310912f * x4 + 27.34824973f * x5;

	float alpha = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).w;

	return ImVec4(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), alpha);
}

void ProfilingRenderer::TextHeat(const char* fmt, float value, float maxValue)
{
	ImVec4 bg = HeatColor(value, maxValue);
	ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(bg));
	ImGui::TextColored(Util::Colors::GetDefault(), fmt, value);
}

void ProfilingRenderer::RenderTimingModeToggle()
{
	int mode = static_cast<int>(timingMode);

	ImGui::PushID("ProfilingTimingMode");
	ImGui::RadioButton(T("menu.profiling.gpu", "GPU"), &mode, static_cast<int>(TimingMode::GPU));
	ImGui::SameLine();
	ImGui::RadioButton(T("menu.profiling.cpu", "CPU"), &mode, static_cast<int>(TimingMode::CPU));
	ImGui::PopID();

	const auto newMode = static_cast<TimingMode>(mode);
	if (newMode != timingMode) {
		timingMode = newMode;
		timeSinceLastUpdate = kStatsRefreshSeconds;
	}
}

void ProfilingRenderer::SetupTimingTableColumns(float passColumnWidth, bool includePercentColumn)
{
	const float scale = Util::GetUIScale();
	// Timings sort slowest first on the first click, names A to Z.
	constexpr ImGuiTableColumnFlags metricFlags = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
	ImGui::TableSetupColumn(T("menu.profiling.pass", "Pass"), ImGuiTableColumnFlags_WidthFixed, passColumnWidth);
	ImGui::TableSetupColumn(T("menu.profiling.avg", "Avg"), metricFlags, kTimingTableMetricColumnWidth * scale);
	ImGui::TableSetupColumn(T("menu.profiling.p95", "P95"), metricFlags, kTimingTableMetricColumnWidth * scale);
	ImGui::TableSetupColumn(T("menu.profiling.p99", "P99"), metricFlags, kTimingTableMetricColumnWidth * scale);
	if (includePercentColumn)
		ImGui::TableSetupColumn(T("menu.profiling.percent", "%"), metricFlags, kTimingTablePercentColumnWidth * scale);
}

// hasGpu/hasCpu, not activeGpu/activeCpu: a pass that doesn't run every frame
// (e.g. Skylighting::ProbeUpdate) would otherwise flicker in and out of the
// view, and the zero-backfilled rolling average already accounts for a
// missed cycle on its own.
static bool HasLiveTimingMode(const Profiler::TimerResult& result, bool cpuMode)
{
	return cpuMode ? result.hasCpu : result.hasGpu;
}

static void RenderTaskGraph(ImGuiUtils::ProfilerGraph& graph, Util::GraphSampleGate& gate, const std::vector<legit::ProfilerTask>& tasks, float height, bool showLegend)
{
	if (gate.ShouldSample())
		graph.LoadFrameData(tasks.data(), tasks.size());

	float maxFrameTimeSec = graph.GetPeakFrameTime() * kGraphHeadroomScale;
	if (maxFrameTimeSec < kMainGraphMinFrameTimeSec)
		maxFrameTimeSec = kMainGraphMinFrameTimeSec;

	const float uiScale = Util::GetUIScale();
	const int totalWidth = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x));
	const int legendWidth = showLegend ? ComputeGraphLegendWidth(totalWidth, ScaleToUiInt(100.0f), 0.42f, ScaleToUiInt(280.0f), ScaleToUiInt(420.0f)) : 0;
	const int graphWidth = std::max(1, totalWidth - legendWidth);

	graph.RenderTimings(static_cast<float>(graphWidth), static_cast<float>(legendWidth), height, 0, maxFrameTimeSec, uiScale);
}

bool ProfilingRenderer::RenderGraph(float height, bool showLegend)
{
	auto& profiler = (*globals::profiler);
	const auto& results = profiler.GetResults();
	bool cpuMode = (timingMode == TimingMode::CPU);

	if (results.empty())
		return false;

	std::vector<legit::ProfilerTask> tasks;

	double accumulated = 0.0;
	for (const auto& result : results) {
		if (!result.valid || !HasLiveTimingMode(result, cpuMode))
			continue;

		float timeMs = cpuMode ? result.cpuTimeMs : result.gpuTimeMs;

		const std::string groupName = GetGroupName(result.name);

		legit::ProfilerTask task;
		task.startTime = accumulated / 1000.0;
		task.endTime = (accumulated + timeMs) / 1000.0;
		task.name = result.name;
		task.displayName = BuildProfilerGraphLabel(result.name);
		task.color = ToLegitColor(GetGroupColor(groupName));
		tasks.push_back(task);
		accumulated += timeMs;
	}

	if (tasks.empty())
		return false;

	RenderTaskGraph(gpuGraph, gpuGraphGate, tasks, height, showLegend);
	return true;
}

void ProfilingRenderer::RenderCompactGraph(float height)
{
	auto* profiler = globals::profiler;
	if (!profiler || !profiler->IsUserEnabled()) {
		ImGui::TextDisabled("%s", T("menu.profiling.disabled", "Profiling is disabled"));
		return;
	}
	profiler->RequestCapture(timingMode == TimingMode::CPU ? Profiler::CaptureMode::CPU : Profiler::CaptureMode::GPU);
	if (!RenderGraph(height, false))
		ImGui::TextDisabled("%s", T("menu.profiling.no_timing_data_world", "No timing data available (enter game world)"));
}

void ProfilingRenderer::RenderStatistics(bool showTable, bool showModeToggle)
{
	auto& profiler = (*globals::profiler);

	// Only the full profiling page offers the toggle; the overlay-embedded
	// row (showModeToggle=false) just shows the off-state below rather than
	// silently re-enabling what the user turned off.
	if (showModeToggle) {
		bool enabled = profiler.IsUserEnabled();
		if (ImGui::Checkbox(T("menu.profiling.enable_profiling", "Enable Profiling"), &enabled))
			profiler.SetUserEnabled(enabled);
		ImGui::SameLine();
		if (Util::Button(T("menu.profiling.clear_timers", "Clear Timers"))) {
			profiler.ClearTimers();
			cachedGroups.clear();
			cachedTotalAvgMs = 0.0f;
			cachedMaxAvgMs = 0.0f;
			cachedMaxP95Ms = 0.0f;
			cachedMaxP99Ms = 0.0f;
			timeSinceLastUpdate = 0.0f;
		}
	}

	if (!profiler.IsUserEnabled()) {
		ImGui::TextDisabled("%s", T("menu.profiling.disabled", "Profiling is disabled"));
		return;
	}

	bool cpuMode = (timingMode == TimingMode::CPU);
	if (showModeToggle) {
		RenderTimingModeToggle();
		cpuMode = (timingMode == TimingMode::CPU);
		ImGui::Separator();
	}
	profiler.RequestCapture(cpuMode ? Profiler::CaptureMode::CPU : Profiler::CaptureMode::GPU);

	float currentTime = static_cast<float>(ImGui::GetTime());
	float deltaTime = currentTime - lastFrameTime;
	lastFrameTime = currentTime;
	timeSinceLastUpdate += deltaTime;

	if (timeSinceLastUpdate >= kStatsRefreshSeconds) {
		timeSinceLastUpdate = 0.0f;

		UpdateStatistics(cpuMode);
	}

	if (cachedGroups.empty()) {
		ImGui::TextDisabled("%s", T("menu.profiling.no_timing_data_world", "No timing data available (enter game world)"));
		return;
	}

	RenderGraph(kMainGraphHeight * Util::GetUIScale(), true);
	ImGui::Spacing();

	if (showTable) {
		std::vector<std::string> passLabels;
		passLabels.reserve(cachedGroups.size());
		for (const auto& group : cachedGroups) {
			passLabels.push_back(group.name);
			for (const auto& pass : group.passes)
				passLabels.push_back(pass.label);
		}
		const float passColumnWidth = GetTextColumnWidth(T("menu.profiling.pass", "Pass"), passLabels,
			GetColorMarkerExtraWidth() + ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetStyle().IndentSpacing);

		if (ImGui::BeginTable("##Profiler", 5,
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX |
					ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate)) {
			SetupTimingTableColumns(passColumnWidth, true);
			ImGui::TableHeadersRow();

			// Groups sort among themselves and each group's passes within it. No
			// active sort (the tristate's third click) keeps the order the passes ran in.
			const Util::TableSortSpec sort = Util::ReadTableSortSpec();
			const auto groupSorts = TimingRowSorts<GroupEntry>([](const GroupEntry& a_group) -> const std::string& { return a_group.name; },
				[](const GroupEntry& a_group) { return std::array{ a_group.totalAvgMs, a_group.totalP95Ms, a_group.totalP99Ms }; });
			const auto passSorts = TimingRowSorts<PassEntry>([](const PassEntry& a_pass) -> const std::string& { return a_pass.label; },
				[](const PassEntry& a_pass) { return std::array{ a_pass.avgMs, a_pass.p95Ms, a_pass.p99Ms }; });
			std::vector<const GroupEntry*> groups;
			groups.reserve(cachedGroups.size());
			for (const auto& group : cachedGroups)
				groups.push_back(&group);
			Util::SortTableRows(groups, sort, groupSorts);

			// Avg / P95 / P99 cells heat-coloured against the column maxima, then the share of the total.
			const auto metricCells = [](float avg, float p95, float p99) {
				ImGui::TableNextColumn();
				TextHeat("%.3f", avg, cachedMaxAvgMs);
				ImGui::TableNextColumn();
				TextHeat("%.3f", p95, cachedMaxP95Ms);
				ImGui::TableNextColumn();
				TextHeat("%.3f", p99, cachedMaxP99Ms);
				ImGui::TableNextColumn();
				if (cachedTotalAvgMs > 0.0f)
					TextHeat("%5.1f", (avg / cachedTotalAvgMs) * 100.0f, 100.0f);
			};

			// Consumed on the first draw, so a group that is not reporting does not grab the view later.
			const std::string focus = std::exchange(focusGroup, {});

			// Right-click a group to pick it in or out of the Performance Overlay's graph.
			// Keyed on the tree node just drawn, so each group gets its own menu.
			const auto groupContextMenu = [](const std::string& a_group) {
				if (ImGui::BeginPopupContextItem()) {
					RenderOverlayGroupToggle(a_group);
					ImGui::EndPopup();
				}
			};

			for (const GroupEntry* groupRow : groups) {
				const auto& group = *groupRow;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				// A feature's popover asked for this group: open it and scroll to it.
				if (!focus.empty() && group.name == focus) {
					ImGui::SetScrollHereY(0.25f);
					ImGui::SetNextItemOpen(true);
				}

				const ImU32 groupColor = GetGroupColor(group.name);
				if (group.passes.empty()) {
					RenderColorMarker(groupColor);
					ImGui::TreeNodeEx(group.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
					groupContextMenu(group.name);
					metricCells(group.totalAvgMs, group.totalP95Ms, group.totalP99Ms);
					continue;
				}

				RenderColorMarker(groupColor);
				const bool open = ImGui::TreeNodeEx(group.name.c_str(), 0);
				groupContextMenu(group.name);
				metricCells(group.totalAvgMs, group.totalP95Ms, group.totalP99Ms);
				if (!open)
					continue;

				std::vector<const PassEntry*> passes;
				passes.reserve(group.passes.size());
				for (const auto& pass : group.passes)
					passes.push_back(&pass);
				Util::SortTableRows(passes, sort, passSorts);
				for (const PassEntry* passRow : passes) {
					const auto& pass = *passRow;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					RenderColorMarker(groupColor);
					ImGui::TreeNodeEx(pass.label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
					metricCells(pass.avgMs, pass.p95Ms, pass.p99Ms);
				}
				ImGui::TreePop();
			}
			ImGui::EndTable();
		}
	}
}

// Rebuilds a TimerResult's rolling history into a compact, oldest-first
// sample array so a Total row can sum actual per-frame samples instead of
// summing each pass's own percentile (percentile(A)+percentile(B) overstates
// percentile(A+B)).
template <size_t SampleCount>
static uint32_t CollectDisplayTimingSamples(const Profiler::TimerResult& result, bool cpuMode, std::array<float, SampleCount>& samples)
{
	samples.fill(0.0f);

	const uint32_t historyCount = cpuMode ? result.cpuHistoryCount : result.historyCount;
	if (historyCount == 0)
		return 0;

	std::array<float, SampleCount> collectedSamples{};
	uint32_t sampleCount = 0;
	for (uint32_t offset = 0; offset < historyCount && sampleCount < SampleCount; ++offset) {
		const uint32_t historyIndex = historyCount - 1 - offset;
		const float sample = cpuMode ?
		                         result.GetCpuHistorySample(historyIndex) :
		                         result.GetHistorySample(historyIndex);
		if (!IsDisplayTimingSampleValid(sample))
			continue;

		collectedSamples[SampleCount - 1 - sampleCount] = sample;
		sampleCount++;
	}

	const auto sourceOffset = SampleCount - sampleCount;
	for (uint32_t i = 0; i < sampleCount; ++i)
		samples[i] = collectedSamples[sourceOffset + i];

	return sampleCount;
}

template <size_t SampleCount>
static float GetSortedPercentile(const std::array<float, SampleCount>& samples, uint32_t sampleCount, float percentile)
{
	if (sampleCount == 0)
		return 0.0f;

	const float idx = (percentile / 100.0f) * static_cast<float>(sampleCount - 1);
	const uint32_t lo = static_cast<uint32_t>(idx);
	const uint32_t hi = std::min(lo + 1, sampleCount - 1);
	const float frac = idx - static_cast<float>(lo);
	return samples[lo] * (1.0f - frac) + samples[hi] * frac;
}

struct DisplayTimingStats
{
	float avgMs = 0.0f;
	float p95Ms = 0.0f;
	float p99Ms = 0.0f;
};

template <size_t SampleCount>
static DisplayTimingStats ComputeDisplayTimingStats(std::array<float, SampleCount> samples, uint32_t sampleCount)
{
	DisplayTimingStats stats;
	if (sampleCount == 0)
		return stats;

	float sum = 0.0f;
	for (uint32_t i = 0; i < sampleCount; ++i)
		sum += samples[i];

	stats.avgMs = sum / static_cast<float>(sampleCount);
	std::sort(samples.begin(), samples.begin() + sampleCount);
	stats.p95Ms = GetSortedPercentile(samples, sampleCount, 95.0f);
	stats.p99Ms = GetSortedPercentile(samples, sampleCount, 99.0f);
	return stats;
}

// Accumulates several passes' per-frame samples (aligned to the same ring
// position -- relies on each Profiler source pushing exactly one sample per
// timer per cycle) so a feature's Total row can compute avg/P95/P99 from
// the actual summed-per-frame series, not from summing each pass's own stats.
template <size_t SampleCount>
struct DisplayTimingSampleAccumulator
{
	void Add(const std::array<float, SampleCount>& sourceSamples, uint32_t sourceSampleCount)
	{
		if (sourceSampleCount == 0)
			return;

		sampleCount = std::max(sampleCount, sourceSampleCount);
		const auto sampleOffset = SampleCount - sourceSampleCount;
		for (uint32_t i = 0; i < sourceSampleCount; ++i)
			samples[sampleOffset + i] += sourceSamples[i];
	}

	[[nodiscard]] DisplayTimingStats GetStats() const
	{
		if (sampleCount == 0)
			return {};

		std::array<float, SampleCount> compactSamples{};
		const auto sampleOffset = SampleCount - sampleCount;
		for (uint32_t i = 0; i < sampleCount; ++i)
			compactSamples[i] = samples[sampleOffset + i];

		return ComputeDisplayTimingStats(compactSamples, sampleCount);
	}

	std::array<float, SampleCount> samples{};
	uint32_t sampleCount = 0;
};

void ProfilingRenderer::UpdateStatistics(bool cpuMode)
{
	cachedGroups.clear();
	cachedTotalAvgMs = 0.0f;
	cachedMaxAvgMs = 0.0f;
	cachedMaxP95Ms = 0.0f;
	cachedMaxP99Ms = 0.0f;
	std::unordered_map<std::string, size_t> groupIndex;
	std::unordered_map<std::string, DisplayTimingSampleAccumulator<Profiler::kHistorySize>> groupSamples;

	for (const auto& result : globals::profiler->GetStatistics()) {
		if (!result.valid || !HasLiveTimingMode(result, cpuMode))
			continue;

		std::array<float, Profiler::kHistorySize> samples{};
		const uint32_t sampleCount = CollectDisplayTimingSamples(result, cpuMode, samples);
		const auto separator = result.name.find("::");
		const auto groupName = result.name.substr(0, separator);
		const auto [it, inserted] = groupIndex.try_emplace(groupName, cachedGroups.size());
		if (inserted)
			cachedGroups.push_back({ groupName, 0.0f, 0.0f, 0.0f, {} });
		if (separator != std::string::npos) {
			cachedGroups[it->second].passes.push_back({ result.name.substr(separator + 2),
				cpuMode ? result.cpuAvgMs : result.avgMs,
				cpuMode ? result.cpuP95Ms : result.p95Ms,
				cpuMode ? result.cpuP99Ms : result.p99Ms });
		}
		groupSamples[groupName].Add(samples, sampleCount);
	}

	for (auto& group : cachedGroups) {
		const auto stats = groupSamples.at(group.name).GetStats();
		group.totalAvgMs = stats.avgMs;
		group.totalP95Ms = stats.p95Ms;
		group.totalP99Ms = stats.p99Ms;
		cachedTotalAvgMs += stats.avgMs;
		cachedMaxAvgMs = std::max(cachedMaxAvgMs, stats.avgMs);
		cachedMaxP95Ms = std::max(cachedMaxP95Ms, stats.p95Ms);
		cachedMaxP99Ms = std::max(cachedMaxP99Ms, stats.p99Ms);
	}
}

ProfilingRenderer::FeatureTimingData ProfilingRenderer::CollectFeatureTimingData(const std::string& featurePrefix, bool cpuMode)
{
	FeatureTimingData data;
	DisplayTimingSampleAccumulator<kDisplayedRollingFrameCount> totalSamples;
	const auto prefix = GetFeatureTimerPrefix(featurePrefix);
	for (const auto& r : globals::profiler->GetResults()) {
		if (!IsFeatureTimerResult(r, prefix) || !HasLiveTimingMode(r, cpuMode))
			continue;

		std::array<float, kDisplayedRollingFrameCount> samples{};
		const uint32_t sampleCount = CollectDisplayTimingSamples(r, cpuMode, samples);
		if (sampleCount == 0)
			continue;

		const auto stats = ComputeDisplayTimingStats(samples, sampleCount);
		std::string label = r.name.substr(prefix.size());
		float timeMs = cpuMode ? r.cpuTimeMs : r.gpuTimeMs;
		data.entries.push_back({ label, r.name, timeMs, stats.avgMs, stats.p95Ms, stats.p99Ms });
		data.maxAvg = std::max(data.maxAvg, stats.avgMs);
		data.maxP95 = std::max(data.maxP95, stats.p95Ms);
		data.maxP99 = std::max(data.maxP99, stats.p99Ms);
		totalSamples.Add(samples, sampleCount);
	}

	const auto totalStats = totalSamples.GetStats();
	data.totalAvg = totalStats.avgMs;
	data.totalP95 = totalStats.p95Ms;
	data.totalP99 = totalStats.p99Ms;
	data.maxAvg = std::max(data.maxAvg, data.totalAvg);
	data.maxP95 = std::max(data.maxP95, data.totalP95);
	data.maxP99 = std::max(data.maxP99, data.totalP99);

	return data;
}

const ProfilingRenderer::FeatureTimingData& ProfilingRenderer::GetFeatureTimings(const std::string& featurePrefix)
{
	auto& state = featureGraphs[featurePrefix];
	const int frame = ImGui::GetFrameCount();
	if (state.dataFrame != frame) {
		const bool cpuMode = (timingMode == TimingMode::CPU);
		globals::profiler->RequestCapture(cpuMode ? Profiler::CaptureMode::CPU : Profiler::CaptureMode::GPU, GetFeatureTimerPrefix(featurePrefix));
		state.data = CollectFeatureTimingData(featurePrefix, cpuMode);
		state.dataFrame = frame;
	}
	return state.data;
}

void ProfilingRenderer::RecordFeatureTimingGraph(const FeatureTimingData& data, ImGuiUtils::ProfilerGraph& graph, Util::GraphSampleGate& gate)
{
	std::vector<legit::ProfilerTask> tasks;
	tasks.reserve(data.entries.size());
	double accumulated = 0.0;
	for (const auto& e : data.entries) {
		legit::ProfilerTask task;
		task.startTime = accumulated / 1000.0;
		task.endTime = (accumulated + e.timeMs) / 1000.0;
		task.name = e.label;
		task.displayName = BuildProfilerGraphLabel(e.label);
		task.color = ToLegitColor(GetGroupColor(e.colorKey));
		tasks.push_back(task);
		accumulated += e.timeMs;
	}
	if (!tasks.empty() && gate.ShouldSample())
		graph.LoadFrameData(tasks.data(), tasks.size());
}

void ProfilingRenderer::DrawFeatureTimingGraph(const FeatureTimingData& data, ImGuiUtils::ProfilerGraph& graph, float graphHeight)
{
	float maxFrameTimeSec = graph.GetPeakFrameTime() * kGraphHeadroomScale;
	if (maxFrameTimeSec < kFeatureGraphMinFrameTimeSec)
		maxFrameTimeSec = kFeatureGraphMinFrameTimeSec;

	const float uiScale = Util::GetUIScale();
	const int totalWidth = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x));
	const int legendWidth = ComputeFeatureGraphLegendWidth(data, totalWidth);
	const int graphWidth = std::max(1, totalWidth - legendWidth);

	graph.RenderTimings(static_cast<float>(graphWidth), static_cast<float>(legendWidth), graphHeight * uiScale, 0, maxFrameTimeSec, uiScale);
}

std::string ProfilingRenderer::FeatureCostChipLabel(const FeatureTimingData& data)
{
	const bool cpuMode = (timingMode == TimingMode::CPU);
	return std::format("{} {}", cpuMode ? T("menu.profiling.cpu", "CPU") : T("menu.profiling.gpu", "GPU"),
		Util::FormatMilliseconds(data.totalAvg));
}

float ProfilingRenderer::MeasureFeatureCostChip(const std::string& featurePrefix)
{
	if (!globals::profiler || !globals::profiler->IsUserEnabled() || !IsFeatureProfilingAvailable(featurePrefix))
		return 0.0f;

	const auto& data = GetFeatureTimings(featurePrefix);
	if (data.entries.empty())
		return 0.0f;

	MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
	return BrowserUI::MeasureChip(FeatureCostChipLabel(data).c_str(), true);
}

bool ProfilingRenderer::RenderFeatureCostChip(const std::string& featurePrefix, float rightLimit)
{
	if (!globals::profiler || !globals::profiler->IsUserEnabled() || !IsFeatureProfilingAvailable(featurePrefix))
		return false;

	auto& state = featureGraphs[featurePrefix];
	const auto& data = GetFeatureTimings(featurePrefix);
	if (data.entries.empty())
		return false;
	// Recorded whether or not the popover is open, so it opens on a filled history.
	const bool cpuMode = (timingMode == TimingMode::CPU);
	RecordFeatureTimingGraph(data, cpuMode ? state.cpuGraph : state.gpuGraph, cpuMode ? state.cpuGate : state.gpuGate);

	const auto& theme = globals::menu->GetTheme();
	const std::string label = FeatureCostChipLabel(data);
	const ImVec4 costColor = Util::GetThresholdColor(data.totalAvg, ThemeManager::Constants::FEATURE_COST_GOOD_MS,
		ThemeManager::Constants::FEATURE_COST_WARNING_MS, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error);

	const char* popupId = "##FeatureProfilingPopover";
	{
		MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
		if (ImGui::GetCursorScreenPos().x + BrowserUI::MeasureChip(label.c_str(), true) > rightLimit)
			return false;
		const ImVec4 secondary = Util::Colors::GetSecondary();
		ImGui::PushStyleColor(ImGuiCol_Text, secondary);
		if (BrowserUI::Chip("##FeatureCost", label.c_str(), Icons::TI(MenuIcons::kProfiling), &costColor))
			ImGui::OpenPopup(popupId);
		ImGui::PopStyleColor();
	}
	// The popover already shows everything the tooltip would.
	if (!ImGui::IsPopupOpen(popupId)) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T("menu.profiling.cost_chip_tooltip", "Average time this feature's passes take each frame. Click for the breakdown."));
			if (ImGui::BeginTable("##FeatureCostTooltip", 2, ImGuiTableFlags_SizingFixedFit)) {
				for (const auto& e : data.entries) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(e.label.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Util::FormatMilliseconds(e.avgMs).c_str());
				}
				ImGui::EndTable();
			}
		}
	}

	const float popoverWidth = ThemeManager::Constants::FEATURE_PROFILING_POPOVER_WIDTH * Util::GetUIScale();
	ImGui::SetNextWindowSizeConstraints(ImVec2(popoverWidth, 0.0f), ImVec2(popoverWidth, FLT_MAX));
	if (ImGui::BeginPopup(popupId)) {
		{
			MenuFonts::FontRoleGuard body(Menu::FontRole::Body);
			RenderFeatureTimersPopover(featurePrefix, state, data);
		}
		ImGui::EndPopup();
	}
	return true;
}

void ProfilingRenderer::RenderFeatureTimersPopover(const std::string& featurePrefix, FeatureGraphState& state, const FeatureTimingData& data)
{
	Util::SeparatorTextWithFont(GetGroupDisplayName(featurePrefix).c_str(), Menu::FontRole::Subheading);
	RenderTimingModeToggle();

	const bool cpuMode = (timingMode == TimingMode::CPU);
	DrawFeatureTimingGraph(data, cpuMode ? state.cpuGraph : state.gpuGraph, kFeatureGraphHeight);
	ImGui::Spacing();

	if (ImGui::BeginTable("##FeatureTimers", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate)) {
		std::vector<std::string> passLabels;
		passLabels.reserve(data.entries.size() + 1);
		for (const auto& e : data.entries)
			passLabels.push_back(e.label);
		passLabels.emplace_back(T("menu.profiling.total", "Total"));
		SetupTimingTableColumns(GetTextColumnWidth(T("menu.profiling.pass", "Pass"), passLabels, GetColorMarkerExtraWidth()), false);
		ImGui::TableHeadersRow();

		// The Total row is rendered separately below and stays last regardless of sort order.
		std::vector<const FeatureTimingEntry*> rows;
		rows.reserve(data.entries.size());
		for (const auto& e : data.entries)
			rows.push_back(&e);
		Util::SortTableRows(rows, Util::ReadTableSortSpec(),
			TimingRowSorts<FeatureTimingEntry>([](const FeatureTimingEntry& a_entry) -> const std::string& { return a_entry.label; },
				[](const FeatureTimingEntry& a_entry) { return std::array{ a_entry.avgMs, a_entry.p95Ms, a_entry.p99Ms }; }));

		for (const FeatureTimingEntry* row : rows) {
			const auto& e = *row;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			RenderColorMarker(GetGroupColor(e.colorKey));
			ImGui::TextUnformatted(e.label.c_str());
			ImGui::TableNextColumn();
			TextHeat("%.3f", e.avgMs, data.maxAvg);
			ImGui::TableNextColumn();
			TextHeat("%.3f", e.p95Ms, data.maxP95);
			ImGui::TableNextColumn();
			TextHeat("%.3f", e.p99Ms, data.maxP99);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const auto totalColor = globals::menu->GetTheme().StatusPalette.InfoColor;
		ImGui::TextColored(totalColor, "%s", T("menu.profiling.total", "Total"));
		ImGui::TableNextColumn();
		ImGui::TextColored(totalColor, "%.3f", data.totalAvg);
		ImGui::TableNextColumn();
		ImGui::TextColored(totalColor, "%.3f", data.totalP95);
		ImGui::TableNextColumn();
		ImGui::TextColored(totalColor, "%.3f", data.totalP99);

		ImGui::EndTable();
	}

	ImGui::Spacing();
	RenderOverlayGroupToggle(featurePrefix);
	if (globals::features::performanceOverlay.loaded)
		ImGui::SameLine();
	// Right-aligned so it reads as the way out rather than another setting
	const char* openLabel = T("menu.profiling.open_page", "Open Profiling Page");
	const float openWidth = ImGui::CalcTextSize(openLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - openWidth));
	if (Util::Button(openLabel)) {
		OpenProfilingPage(featurePrefix);
		ImGui::CloseCurrentPopup();
	}
	Util::HoverTip(T("menu.profiling.open_page_tooltip", "Every feature's passes side by side, with this feature expanded"));
}

void ProfilingRenderer::RenderOverlayGroupToggle(const std::string& group)
{
	auto& overlay = globals::features::performanceOverlay;
	if (!overlay.loaded)
		return;
	bool shown = overlay.IsProfilerGroupShown(group);
	ImGui::PushID(group.c_str());
	if (ImGui::Checkbox(T("menu.profiling.show_in_overlay", "Show in Performance Overlay"), &shown))
		overlay.SetProfilerGroupShown(group, shown);
	ImGui::PopID();
	Util::HoverTip(T("menu.profiling.show_in_overlay_tooltip", "Include these passes in the Performance Overlay's render pass graph"));
}

void ProfilingRenderer::OpenProfilingPage(const std::string& group)
{
	focusGroup = group;
	globals::menu->SelectFeatureMenu(std::string(MenuPageIds::kProfiling));
}

void ProfilingRenderer::RenderOverlayGraph()
{
	auto& profiler = (*globals::profiler);
	if (!profiler.IsUserEnabled()) {
		ImGui::TextDisabled("%s", T("menu.profiling.disabled", "Profiling is disabled"));
		return;
	}
	const bool cpuMode = (timingMode == TimingMode::CPU);
	profiler.RequestCapture(cpuMode ? Profiler::CaptureMode::CPU : Profiler::CaptureMode::GPU);
	const auto& overlay = globals::features::performanceOverlay;

	bool anyTimers = false;
	std::vector<legit::ProfilerTask> tasks;
	double accumulated = 0.0;
	for (const auto& result : profiler.GetResults()) {
		if (!result.valid || !HasLiveTimingMode(result, cpuMode))
			continue;
		anyTimers = true;
		const std::string groupName = GetGroupName(result.name);
		if (!overlay.IsProfilerGroupShown(groupName))
			continue;

		const float timeMs = cpuMode ? result.cpuTimeMs : result.gpuTimeMs;
		legit::ProfilerTask task;
		task.startTime = accumulated / 1000.0;
		task.endTime = (accumulated + timeMs) / 1000.0;
		task.name = result.name;
		task.displayName = BuildProfilerGraphLabel(result.name);
		task.color = ToLegitColor(GetGroupColor(groupName));
		tasks.push_back(task);
		accumulated += timeMs;
	}

	if (!anyTimers) {
		ImGui::TextDisabled("%s", T("menu.profiling.no_timing_data_world", "No timing data available (enter game world)"));
		return;
	}
	if (tasks.empty()) {
		ImGui::TextDisabled("%s", T("menu.profiling.all_groups_hidden", "Every render pass is hidden. Pick some in the Performance Overlay settings."));
		return;
	}
	RenderTaskGraph(overlayGraph, overlayGraphGate, tasks, kMainGraphHeight * Util::GetUIScale(), true);
}

std::vector<std::string> ProfilingRenderer::GetActiveGroups()
{
	auto& profiler = (*globals::profiler);
	profiler.RequestCapture(timingMode == TimingMode::CPU ? Profiler::CaptureMode::CPU : Profiler::CaptureMode::GPU);
	std::vector<std::string> groups;
	for (const auto& result : profiler.GetResults()) {
		if (!result.valid)
			continue;
		auto group = GetGroupName(result.name);
		if (std::ranges::find(groups, group) == groups.end())
			groups.push_back(std::move(group));
	}
	return groups;
}

std::string ProfilingRenderer::GetGroupDisplayName(const std::string& group)
{
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->GetShortName() == group)
			return feature->GetDisplayName();
	}
	return group;
}

std::string ProfilingRenderer::GetGroupName(std::string_view timerName)
{
	const auto pos = timerName.find("::");
	return std::string(pos != std::string_view::npos ? timerName.substr(0, pos) : timerName);
}

bool ProfilingRenderer::IsFeatureProfilingAvailable(const std::string& featurePrefix)
{
	if (!globals::profiler)
		return false;
	if (GpuPassCapabilities::Contains(featurePrefix))
		return true;

	const auto prefix = GetFeatureTimerPrefix(featurePrefix);
	const auto& results = globals::profiler->GetResults();
	return std::any_of(results.begin(), results.end(), [&](const auto& result) {
		return IsFeatureTimerResult(result, prefix);
	});
}

std::string ProfilingRenderer::GetFeatureTimerPrefix(const std::string& featurePrefix)
{
	return featurePrefix + "::";
}

bool ProfilingRenderer::IsFeatureTimerResult(const Profiler::TimerResult& result, std::string_view prefix)
{
	return result.valid && result.name.starts_with(prefix);
}
