#include "Profiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
	constexpr float kMaxSaneProfilerSampleMs = 1000.0f;

	// Guards rolling stats against disjoint-frame spikes and non-finite samples.
	bool IsValidProfilerSample(float ms)
	{
		return std::isfinite(ms) && ms >= 0.0f && ms <= kMaxSaneProfilerSampleMs;
	}
}

float Profiler::RollingHistory::GetAverage() const
{
	if (count == 0)
		return lastMs;
	float sum = 0.0f;
	for (uint32_t i = 0; i < count; i++)
		sum += history[i];
	return sum / static_cast<float>(count);
}

float Profiler::RollingHistory::GetPercentile(float p) const
{
	if (count == 0)
		return lastMs;

	thread_local std::vector<float> sorted;
	sorted.resize(count);
	for (uint32_t i = 0; i < count; i++)
		sorted[i] = history[i];
	std::sort(sorted.begin(), sorted.end());

	float idx = (p / 100.0f) * static_cast<float>(count - 1);
	uint32_t lo = static_cast<uint32_t>(idx);
	uint32_t hi = std::min(lo + 1, count - 1);
	float frac = idx - static_cast<float>(lo);
	return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

void Profiler::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
{
	Release();

	device = a_device;
	context = a_context;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	cpuTicksToMs = 1000.0 / static_cast<double>(freq.QuadPart);

	for (auto& frame : frames) {
		frame.batch.Configure(kMaxTimers, "Profiler::Frame");
		frame.batch.Preallocate(a_device);
		frame.timers.resize(kMaxTimers);
		frame.activeStack.clear();
		frame.captureCycle = false;
		frame.acquiredTimerCount = 0;
		frame.inFlight = false;
		frame.capturedCycle = 0;
	}

	writeFrame = 0;
	readFrame = 0;
	frameActive = false;
	initialized = true;
	// userEnabled is left as-is: a device teardown/re-init (e.g. display
	// mode change) must not silently re-enable a user's "off" preference.
	captureRequested.store(false, std::memory_order_release);
	captureActive.store(false, std::memory_order_release);
	activeCpuTimers.clear();
	completedCpuTimers.clear();
	activePassUsesGpu.clear();
	requestedCaptureMode = CaptureMode::None;
	requestedNamePrefix.clear();
	activeCaptureMode = CaptureMode::None;
	activeNamePrefix.clear();
	resolvedTotalMs = 0.0f;
	resolvedCpuTotalMs = 0.0f;
	capturedFrameCount = 0;
	capturedGpuFrameCount = 0;
	capturedCpuFrameCount = 0;
	acquiredSlotsThisFrame = 0;
	acquiredSlots = 0;
	peakAcquiredSlots = 0;
	slotRefusals = 0;
}

void Profiler::Release()
{
	for (auto& frame : frames) {
		frame.batch.ReleaseQueries();
		frame.timers.clear();
		frame.activeStack.clear();
		frame.acquiredTimerCount = 0;
		frame.captureCycle = false;
		frame.inFlight = false;
	}
	results.clear();
	knownTimers.clear();
	knownTimerIndex.clear();
	captureCycleCount = 0;
	totalTimeMs = 0.0f;
	cpuTotalTimeMs = 0.0f;
	resolvedTotalMs = 0.0f;
	resolvedCpuTotalMs = 0.0f;
	capturedFrameCount = 0;
	capturedGpuFrameCount = 0;
	capturedCpuFrameCount = 0;
	writeFrame = 0;
	readFrame = 0;
	acquiredSlotsThisFrame = 0;
	acquiredSlots = 0;
	peakAcquiredSlots = 0;
	slotRefusals = 0;
	activeCpuTimers.clear();
	completedCpuTimers.clear();
	activePassUsesGpu.clear();
	frameActive = false;
	initialized = false;
	device = nullptr;
	context = nullptr;
	// userEnabled is left as-is; see the matching note in Initialize().
	captureRequested.store(false, std::memory_order_release);
	captureActive.store(false, std::memory_order_release);
	requestedCaptureMode = CaptureMode::None;
	requestedNamePrefix.clear();
	activeCaptureMode = CaptureMode::None;
	activeNamePrefix.clear();
}

void Profiler::SetUserEnabled(bool a_enabled)
{
	userEnabled.store(a_enabled, std::memory_order_release);
	if (!a_enabled) {
		captureActive.store(false, std::memory_order_release);
		std::scoped_lock lock(captureRequestLock);
		captureRequested.store(false, std::memory_order_release);
		requestedCaptureMode = CaptureMode::None;
		requestedNamePrefix.clear();
	}
}

void Profiler::RequestCapture(CaptureMode a_mode, std::string_view a_namePrefix)
{
	if (!IsUserEnabled() || a_mode == CaptureMode::None)
		return;

	std::scoped_lock lock(captureRequestLock);
	if (!captureRequested.load(std::memory_order_relaxed)) {
		requestedCaptureMode = a_mode;
		requestedNamePrefix = a_namePrefix;
	} else {
		requestedCaptureMode = static_cast<CaptureMode>(static_cast<uint8_t>(requestedCaptureMode) | static_cast<uint8_t>(a_mode));
		if (requestedNamePrefix != a_namePrefix)
			requestedNamePrefix.clear();
	}
	captureRequested.store(true, std::memory_order_release);
}

void Profiler::LatchCaptureRequest()
{
	std::scoped_lock lock(captureRequestLock);
	const bool requested = captureRequested.exchange(false, std::memory_order_acq_rel) && IsUserEnabled();
	activeCaptureMode = requested ? requestedCaptureMode : CaptureMode::None;
	activeNamePrefix = requested ? std::move(requestedNamePrefix) : std::string{};
	requestedCaptureMode = CaptureMode::None;
	requestedNamePrefix.clear();
	captureActive.store(requested && activeCaptureMode != CaptureMode::None, std::memory_order_release);
}

void Profiler::BeginFrame()
{
	if (!initialized || !context || frameActive || !IsEnabled())
		return;

	if (!CollectResults())
		return;

	auto& frame = frames[writeFrame];
	frame.batch.Reset();
	frame.activeStack.clear();
	frame.captureCycle = true;
	frame.inFlight = frame.batch.BeginBatch(device, context);
	frameActive = frame.inFlight;
	frame.acquiredTimerCount = 0;
	acquiredSlotsThisFrame = 0;
	if (!frameActive)
		frame.captureCycle = false;
}

bool Profiler::BeginPass(std::string_view name, bool fireCallbacks)
{
	if (!initialized || !context || !IsEnabled() || !MatchesActiveFilter(name))
		return false;

	if (!HasCaptureMode(activeCaptureMode, CaptureMode::GPU)) {
		if (!BeginCpuPass(name))
			return false;
		activePassUsesGpu.push_back(false);
		if (fireCallbacks && beginPerfEvent)
			beginPerfEvent(name);
		return true;
	}

	if (!frameActive)
		BeginFrame();
	if (!frameActive) {
		if (HasCaptureMode(activeCaptureMode, CaptureMode::CPU) && BeginCpuPass(name)) {
			activePassUsesGpu.push_back(false);
			if (fireCallbacks && beginPerfEvent)
				beginPerfEvent(name);
			return true;
		}
		return false;
	}

	auto& frame = frames[writeFrame];
	const int slot = frame.batch.AcquireInterval(device, context);
	if (slot < 0) {
		slotRefusals++;
		if (HasCaptureMode(activeCaptureMode, CaptureMode::CPU) && BeginCpuPass(name)) {
			activePassUsesGpu.push_back(false);
			if (fireCallbacks && beginPerfEvent)
				beginPerfEvent(name);
			return true;
		}
		return false;
	}
	acquiredSlotsThisFrame++;

	auto& timer = frame.timers[slot];
	timer.name = name;
	timer.depth = static_cast<uint32_t>(frame.activeStack.size());
	timer.cpuDepth = timer.depth + static_cast<uint32_t>(activeCpuTimers.size());
	timer.parentSlot = frame.activeStack.empty() ? -1 : frame.activeStack.back();
	if (HasCaptureMode(activeCaptureMode, CaptureMode::CPU))
		QueryPerformanceCounter(&timer.cpuBegin);
	frame.activeStack.push_back(slot);
	activePassUsesGpu.push_back(true);
	frame.acquiredTimerCount = std::max(frame.acquiredTimerCount, static_cast<uint32_t>(slot + 1));

	if (fireCallbacks && beginPerfEvent)
		beginPerfEvent(name);
	return true;
}

void Profiler::EndPass(bool fireCallbacks)
{
	if (!initialized || !context || activePassUsesGpu.empty())
		return;

	const bool usesGpu = activePassUsesGpu.back();
	activePassUsesGpu.pop_back();
	if (!usesGpu) {
		EndCpuPass();
		if (fireCallbacks && endPerfEvent)
			endPerfEvent({});
		return;
	}
	if (!frameActive)
		return;

	auto& frame = frames[writeFrame];
	if (frame.activeStack.empty())
		return;

	const int slot = frame.activeStack.back();
	frame.activeStack.pop_back();

	auto& timer = frame.timers[slot];

	if (HasCaptureMode(activeCaptureMode, CaptureMode::CPU)) {
		LARGE_INTEGER cpuEnd;
		QueryPerformanceCounter(&cpuEnd);
		const float cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);
		if (IsValidProfilerSample(cpuMs))
			completedCpuTimers.push_back({ timer.name, cpuMs, timer.cpuDepth });
	}

	frame.batch.CloseInterval(context, static_cast<uint32_t>(slot));

	if (fireCallbacks && endPerfEvent)
		endPerfEvent({});
}

void Profiler::EndFrame(uint32_t a_frameCount)
{
	if (!initialized || !context) {
		LatchCaptureRequest();
		activeCpuTimers.clear();
		completedCpuTimers.clear();
		return;
	}

	const bool captureCycleActive = activeCaptureMode != CaptureMode::None;
	const uint64_t captureCycle = captureCycleActive ? ++captureCycleCount : captureCycleCount;
	if (HasCaptureMode(activeCaptureMode, CaptureMode::CPU)) {
		PublishCpuResults(activeNamePrefix, a_frameCount, captureCycle);
	} else {
		completedCpuTimers.clear();
		cpuTotalTimeMs = 0.0f;
	}

	if (!frameActive) {
		const bool gpuCycleActive = IsEnabled() && HasCaptureMode(activeCaptureMode, CaptureMode::GPU);
		if (!gpuCycleActive)
			totalTimeMs = 0.0f;
		// No GPU frame open this cycle (capture was off, or nothing called
		// BeginPass), but the ring may still hold an older frame's results
		// pending resolution -- keep draining it even while otherwise idle.
		if (!CollectResults()) {
			LatchCaptureRequest();
			return;
		}

		if (!gpuCycleActive) {
			totalTimeMs = 0.0f;
			// Advance past drained slots so the next capture cannot replay them.
			if (std::ranges::any_of(frames, [](const FrameQueries& f) { return f.captureCycle; }))
				writeFrame = (writeFrame + 1) % kFrameLatency;
			LatchCaptureRequest();
			return;
		}

		auto& frame = frames[writeFrame];
		frame.batch.Reset();
		frame.activeStack.clear();
		frame.acquiredTimerCount = 0;
		frame.captureCycle = false;
		frame.inFlight = false;
		if (HasCaptureMode(activeCaptureMode, CaptureMode::GPU)) {
			frame.inFlight = frame.batch.BeginBatch(device, context);
			if (frame.inFlight) {
				frame.batch.EndBatch(context);
				frame.captureCycle = true;
			}
		}
		acquiredSlots = 0;
		if (!frame.captureCycle) {
			LatchCaptureRequest();
			return;
		}
	} else {
		frameActive = false;
		frames[writeFrame].batch.EndBatch(context);
		frames[writeFrame].captureCycle = true;
		acquiredSlots = acquiredSlotsThisFrame;
		peakAcquiredSlots = std::max(peakAcquiredSlots, acquiredSlotsThisFrame);
	}

	frames[writeFrame].namePrefix = activeNamePrefix;
	frames[writeFrame].capturedFrame = a_frameCount;
	frames[writeFrame].capturedCycle = captureCycle;
	writeFrame = (writeFrame + 1) % kFrameLatency;
	LatchCaptureRequest();
}

Profiler::KnownTimer& Profiler::GetOrCreateTimer(const std::string& name)
{
	auto [it, inserted] = knownTimerIndex.try_emplace(name, knownTimers.size());
	if (inserted) {
		KnownTimer kt;
		kt.name = name;
		knownTimers.push_back(std::move(kt));
	}
	return knownTimers[it->second];
}

bool Profiler::BeginCpuPass(std::string_view name)
{
	if (!IsEnabled() || !HasCaptureMode(activeCaptureMode, CaptureMode::CPU) || !MatchesActiveFilter(name))
		return false;
	CpuTimer timer;
	timer.name = name;
	QueryPerformanceCounter(&timer.cpuBegin);
	const auto gpuDepth = frameActive ? static_cast<uint32_t>(frames[writeFrame].activeStack.size()) : 0u;
	timer.depth = static_cast<uint32_t>(activeCpuTimers.size()) + gpuDepth;
	activeCpuTimers.push_back(std::move(timer));
	return true;
}

void Profiler::EndCpuPass()
{
	if (activeCpuTimers.empty())
		return;

	CpuTimer timer = std::move(activeCpuTimers.back());
	activeCpuTimers.pop_back();

	LARGE_INTEGER cpuEnd;
	QueryPerformanceCounter(&cpuEnd);
	const float cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);
	if (!IsValidProfilerSample(cpuMs))
		return;

	completedCpuTimers.push_back({ std::move(timer.name), cpuMs, timer.depth });
}

bool Profiler::CollectResults()
{
	readFrame = writeFrame;
	auto& frame = frames[readFrame];
	if (!frame.captureCycle)
		return true;

	struct ActiveTimerData
	{
		/// Sum of interval self times after direct nested passes are removed.
		float gpuMs = 0.0f;
		/// Inclusive depth-0 duration for the exact frame-total check.
		float topLevelMs = 0.0f;
		bool hasGpu = false;
	};
	std::unordered_map<std::string, ActiveTimerData> activeTimers;
	float activeTotalMs = 0.0f;
	bool gpuFrameResolved = false;

	if (frame.inFlight) {
		struct IntervalTiming
		{
			float gpuMs = 0.0f;
			bool valid = false;
		};
		std::vector<IntervalTiming> intervals(frame.acquiredTimerCount);
		const auto status = frame.batch.TryResolve(context,
			[&](uint32_t i, uint64_t deltaTicks, uint64_t frequency) {
				if (i >= intervals.size())
					return;
				auto& timer = frame.timers[i];
				float ms = static_cast<float>(static_cast<double>(deltaTicks) * 1000.0 / static_cast<double>(frequency));
				const bool gpuValid = IsValidProfilerSample(ms);
				if (!gpuValid)
					return;

				GetOrCreateTimer(timer.name);
				intervals[i] = { ms, true };
			});
		if (status == Util::TimestampQueryBatch::Status::NotReady)
			return false;

		std::vector<float> nestedGpuMs(intervals.size(), 0.0f);
		for (uint32_t i = 0; i < intervals.size(); ++i) {
			if (!intervals[i].valid)
				continue;
			int32_t parentSlot = frame.timers[i].parentSlot;
			while (parentSlot >= 0 && static_cast<uint32_t>(parentSlot) < intervals.size() && !intervals[parentSlot].valid)
				parentSlot = frame.timers[parentSlot].parentSlot;
			if (parentSlot >= 0 && static_cast<uint32_t>(parentSlot) < intervals.size())
				nestedGpuMs[parentSlot] += intervals[i].gpuMs;
		}

		for (uint32_t i = 0; i < intervals.size(); ++i) {
			auto& timer = frame.timers[i];
			if (!intervals[i].valid)
				continue;

			// Aggregate same-name call sites only after interval self time is known.
			auto& entry = activeTimers[timer.name];
			entry.gpuMs += std::max(intervals[i].gpuMs - nestedGpuMs[i], 0.0f);
			entry.hasGpu = true;
			if (timer.depth == 0) {
				activeTotalMs += intervals[i].gpuMs;
				entry.topLevelMs += intervals[i].gpuMs;
			}
		}

		frame.inFlight = false;
		gpuFrameResolved = (status == Util::TimestampQueryBatch::Status::Ok);
	}

	totalTimeMs = activeTotalMs;
	resolvedTotalMs = activeTotalMs;
	capturedGpuFrameCount = frame.capturedFrame;
	capturedFrameCount = std::max(capturedFrameCount, frame.capturedFrame);

	// Each resolved source cycle contributes exactly one aligned history sample.
	for (auto& known : knownTimers) {
		known.activeGpu = false;
		known.topLevelMs = 0.0f;
		auto it = activeTimers.find(known.name);
		const bool includedByFilter = frame.namePrefix.empty() || known.name.starts_with(frame.namePrefix);
		const bool freshGpu = it != activeTimers.end() && it->second.hasGpu;
		if (freshGpu) {
			known.hasGpu = true;
			known.gpu.PushSample(it->second.gpuMs);
			known.activeGpu = true;
			known.topLevelMs = it->second.topLevelMs;
		} else if (gpuFrameResolved && includedByFilter && known.hasGpu) {
			known.gpu.PushSample(0.0f);
		}
		if (freshGpu)
			known.lastSampleCycle = std::max(known.lastSampleCycle, frame.capturedCycle);
	}

	RetireStaleTimers();
	RebuildResults();
	frame.captureCycle = false;
	return true;
}

void Profiler::PublishCpuResults(std::string_view a_namePrefix, uint32_t a_frameCount, uint64_t a_captureCycle)
{
	struct CpuTimerData
	{
		float ms = 0.0f;
	};
	std::unordered_map<std::string, CpuTimerData> activeTimers;
	float activeCpuTotalMs = 0.0f;
	for (const auto& timer : completedCpuTimers) {
		activeTimers[timer.name].ms += timer.cpuMs;
		if (timer.depth == 0)
			activeCpuTotalMs += timer.cpuMs;
		GetOrCreateTimer(timer.name);
	}
	completedCpuTimers.clear();

	cpuTotalTimeMs = activeCpuTotalMs;
	resolvedCpuTotalMs = activeCpuTotalMs;
	capturedCpuFrameCount = a_frameCount;
	capturedFrameCount = std::max(capturedFrameCount, a_frameCount);

	for (auto& known : knownTimers) {
		known.activeCpu = false;
		const auto it = activeTimers.find(known.name);
		const bool includedByFilter = a_namePrefix.empty() || known.name.starts_with(a_namePrefix);
		if (it != activeTimers.end()) {
			known.hasCpu = true;
			known.activeCpu = true;
			known.cpu.PushSample(it->second.ms);
			known.lastSampleCycle = std::max(known.lastSampleCycle, a_captureCycle);
		} else if (includedByFilter && known.hasCpu) {
			known.cpu.PushSample(0.0f);
		}
	}
	RetireStaleTimers();
	RebuildResults();
}

void Profiler::RebuildResults()
{
	results.clear();
	results.reserve(knownTimers.size());
	for (const auto& known : knownTimers) {
		TimerResult result;
		result.name = known.name;
		result.hasGpu = known.hasGpu;
		result.hasCpu = known.hasCpu;
		result.activeGpu = known.activeGpu;
		result.activeCpu = known.activeCpu;
		result.gpuTimeMs = known.gpu.lastMs;
		result.topLevelMs = known.activeGpu ? known.topLevelMs : 0.0f;
		result.cpuTimeMs = known.cpu.lastMs;
		result.avgMs = known.gpu.GetAverage();
		result.p95Ms = known.gpu.GetPercentile(95.0f);
		result.p99Ms = known.gpu.GetPercentile(99.0f);
		result.cpuAvgMs = known.cpu.GetAverage();
		result.cpuP95Ms = known.cpu.GetPercentile(95.0f);
		result.cpuP99Ms = known.cpu.GetPercentile(99.0f);
		result.valid = true;
		result.historyBuffer = known.gpu.history;
		result.historyHead = known.gpu.head;
		result.historyCount = known.gpu.count;
		result.cpuHistoryBuffer = known.cpu.history;
		result.cpuHistoryHead = known.cpu.head;
		result.cpuHistoryCount = known.cpu.count;
		results.push_back(std::move(result));
	}
}

void Profiler::RetireStaleTimers()
{
	const size_t before = knownTimers.size();
	std::erase_if(knownTimers, [this](const KnownTimer& known) {
		return !known.activeGpu && !known.activeCpu &&
		       captureCycleCount - known.lastSampleCycle >= kTimerRetireFrames;
	});
	if (knownTimers.size() != before)
		RebuildTimerIndex();
}

void Profiler::RebuildTimerIndex()
{
	knownTimerIndex.clear();
	for (size_t i = 0; i < knownTimers.size(); i++)
		knownTimerIndex[knownTimers[i].name] = i;
}
