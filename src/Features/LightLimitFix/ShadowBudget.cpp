// ShadowBudget.cpp
// Per-light cost tracking (BudgetTracker): max(CPU submit, GPU raster) via D3D11 timestamp queries, and the frame-time percentile helper.

#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/GpuTimestamps.h"
#include "ShadowCasterInternal.h"

#include <Tracy/Tracy.hpp>  // ZoneScopedN

namespace ShadowCasterManager
{
	float ComputeFrameTimePercentile90()
	{
		return FrameTimePercentile(s_ftRing, s_ftCount);
	}

	static int64_t GetPerfCounter()
	{
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);

		int64_t t = (int64_t)counter.QuadPart;

		static int64_t freq = 0;
		if (freq == 0) {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			// Clamp to 1: a sub-1 MHz (or unsupported, 0 Hz) counter would
			// otherwise divide by zero on every call.
			freq = std::max<int64_t>(f.QuadPart / 1000000, 1);
		}

		return t / freq;
	}

	// ---------------------------------------------------------------------
	// GPU timestamp batches
	// ---------------------------------------------------------------------

	struct BudgetGpuTimer
	{
		// One batch per frame; results resolve a few frames later, so keep a
		// small ring in flight. 8 covers any sane GPU queue depth.
		static constexpr int kBatchCount = 8;
		// Redraws per frame are budget-capped far below this; overflow lights
		// silently keep the CPU fallback.
		static constexpr uint32_t kMaxSamplesPerBatch = 64;

		struct Sample
		{
			uint64_t key = 0;
			uint32_t fallbackUs = 0;  ///< CPU-measured render interval (QPC)
			uint32_t progressUs = 0;  ///< step-0 accumulate cost, added to either path
		};

		struct Batch
		{
			Util::TimestampQueryBatch queries;
			std::vector<Sample> samples;
			bool inFlight = false;

			Batch() { samples.resize(kMaxSamplesPerBatch); }
		};

		Batch batches[kBatchCount];
		int recording = -1;  ///< batch index being recorded, -1 outside a frame
		int cursor = 0;      ///< next batch slot to record into

		BudgetGpuTimer()
		{
			for (auto& batch : batches)
				batch.queries.Configure(kMaxSamplesPerBatch, "SCM::Budget");
		}

		bool Unavailable() const
		{
			for (const auto& batch : batches)
				if (batch.queries.CreationFailed())
					return true;
			return false;
		}

		static uint32_t ClampUs(int64_t us)
		{
			return static_cast<uint32_t>(std::clamp<int64_t>(us, 0, 0xFFFFFFFF));
		}

		static uint32_t FinalCostUs(const Sample& sample, uint32_t costUs)
		{
			// Keep the step-0 CPU accumulate cost in the budget on both paths
			// so CPU-heavy lights aren't under-counted, and floor at 1us so a
			// sub-tick sample can't read as free forever.
			return std::max(1u, ClampUs(static_cast<int64_t>(costUs) + sample.progressUs));
		}

		void RetireBatchCpu(BudgetTracker& tracker, Batch& batch)
		{
			for (uint32_t i = 0; i < batch.queries.Used(); i++)
				tracker.CommitResolved(batch.samples[i].key, FinalCostUs(batch.samples[i], batch.samples[i].fallbackUs));
			batch.queries.Reset();
			batch.inFlight = false;
		}

		/// Commits every resolved batch's samples through tracker.CommitResolved;
		/// leaves unresolved batches in flight. Intervals whose timestamps did
		/// not resolve (or whose bracket landed disjoint) commit the CPU value.
		void Drain(BudgetTracker& tracker)
		{
			auto* context = globals::d3d::context;
			for (auto& batch : batches) {
				if (!batch.inFlight)
					continue;
				// Track which intervals resolved on the GPU; the rest fall back.
				bool resolved[kMaxSamplesPerBatch]{};
				const auto status = batch.queries.TryResolve(context,
					[&](uint32_t i, uint64_t deltaTicks, uint64_t frequency) {
						resolved[i] = true;
						const uint32_t gpuUs = ClampUs(static_cast<int64_t>(deltaTicks * 1000000ull / frequency));
						// Bound by whichever side actually dominates: CPU submit
						// (draw calls, state changes) for many small lights, GPU
						// raster for large/expensive ones. GPU-only measurement
						// made the budget blind to CPU cost, which VR's many-small
						// -light workload is bottlenecked on.
						tracker.CommitResolved(batch.samples[i].key,
							FinalCostUs(batch.samples[i], std::max(gpuUs, batch.samples[i].fallbackUs)));
					});
				if (status == Util::TimestampQueryBatch::Status::NotReady)
					continue;  // still in flight; try again next frame
				for (uint32_t i = 0; i < batch.queries.Used(); i++) {
					if (!resolved[i])
						tracker.CommitResolved(batch.samples[i].key, FinalCostUs(batch.samples[i], batch.samples[i].fallbackUs));
				}
				batch.queries.Reset();
				batch.inFlight = false;
			}
		}
	};

	BudgetTracker::BudgetTracker() = default;
	BudgetTracker::~BudgetTracker() = default;

	void BudgetTracker::BeginRenderBatch()
	{
		if (!_gpu)
			_gpu = std::make_unique<BudgetGpuTimer>();
		if (_gpu->Unavailable() || !globals::d3d::device || !globals::d3d::context)
			return;

		_gpu->Drain(*this);

		auto& batch = _gpu->batches[_gpu->cursor];
		if (batch.inFlight) {
			// GPU is a full ring behind (or results never resolved); commit
			// the CPU fallbacks rather than stalling on GetData.
			_gpu->RetireBatchCpu(*this, batch);
		}
		if (!batch.queries.BeginBatch(globals::d3d::device, globals::d3d::context))
			return;

		_gpu->recording = _gpu->cursor;
		_gpu->cursor = (_gpu->cursor + 1) % BudgetGpuTimer::kBatchCount;
	}

	void BudgetTracker::EndRenderBatch()
	{
		if (!_gpu || _gpu->recording < 0)
			return;
		auto& batch = _gpu->batches[_gpu->recording];
		batch.queries.EndBatch(globals::d3d::context);
		batch.inFlight = batch.queries.Used() > 0;
		_gpu->recording = -1;
	}

	void BudgetTracker::CommitResolved(uint64_t key, uint32_t costUs)
	{
		// Deferred results can outlive their light (freed or expired between
		// issue and resolve); dropping them avoids resurrecting dead keys
		// whose recycled address would seed a new light with stale costs.
		auto it = _map.find(key);
		if (it != _map.end())
			it->second->CommitCost(costUs, _counter);
	}

	void BudgetEntry::BeginStep(int32_t /*step*/)
	{
		_startTime = GetPerfCounter();
	}

	uint32_t BudgetEntry::ElapsedSinceBeginUs() const
	{
		return static_cast<uint32_t>(std::clamp<int64_t>(GetPerfCounter() - _startTime, 0, 0xFFFFFFFF));
	}

	void BudgetEntry::EndStep(int32_t step, int32_t helperCounter)
	{
		if (step == 0) {
			Progress = ElapsedSinceBeginUs();
		} else if (step == 1) {
			CommitCost(static_cast<uint32_t>(std::min<int64_t>(static_cast<int64_t>(ElapsedSinceBeginUs()) + Progress, 0xFFFFFFFF)), helperCounter);
			Progress = 0;
		}
	}

	void BudgetEntry::CommitCost(uint32_t costUs, int32_t helperCounter)
	{
		int32_t ix = TrackedCount % kBudgetWindowSize;
		Current -= Tracked[ix];
		Tracked[ix] = costUs;
		Current += Tracked[ix];
		TrackedCount++;
		LastTrackedHelper = helperCounter;
	}

	bool BudgetEntry::IsExpired(int32_t helperCounter) const
	{
		return LastTrackedHelper < 0 || (helperCounter - LastTrackedHelper) >= 600;
	}

	void BudgetTracker::Begin(int32_t step)
	{
		if (step == 0) {
			_counter++;
			// Amortise the GC: a periodic full-map walk that freed every
			// expired BudgetEntry in one frame caused ~10s-cadence stutters
			// (300 frames at 30 fps) because the heap freed dozens of
			// unique_ptr<BudgetEntry> back to back, taking a heap lock for
			// each. Run incrementally every 30 frames (~0.5s at 60fps) and
			// cap erasures per call so the cost spreads across many frames
			// instead of spiking once.
			if ((_counter % 30) == 0)
				CleanupExpired();
		}
	}

	void BudgetTracker::BeginLight(RE::BSShadowLight* light, int32_t step)
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto& e = _map[key];
		if (!e) {
			e = std::make_unique<BudgetEntry>();
			e->Key = key;
		}
		e->BeginStep(step);

		if (step == 1 && _gpu && _gpu->recording >= 0) {
			auto& batch = _gpu->batches[_gpu->recording];
			const int slot = batch.queries.BeginInterval(globals::d3d::device, globals::d3d::context);
			if (slot >= 0)
				batch.samples[slot].key = key;
			else if (batch.queries.Used() < BudgetGpuTimer::kMaxSamplesPerBatch)
				batch.samples[batch.queries.Used()].key = 0;  // stale key must not match in EndLight
		}
	}

	void BudgetTracker::EndLight(RE::BSShadowLight* light, int32_t step)
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto it = _map.find(key);
		if (it == _map.end())
			return;

		if (step == 1 && _gpu && _gpu->recording >= 0) {
			auto& batch = _gpu->batches[_gpu->recording];
			// The current (uncommitted) interval belongs to this light:
			// BeginLight/EndLight pairs never nest in the render loop.
			const uint32_t slot = batch.queries.Used();
			if (slot < BudgetGpuTimer::kMaxSamplesPerBatch && batch.samples[slot].key == key) {
				auto& sample = batch.samples[slot];
				batch.queries.CommitInterval(globals::d3d::context);
				sample.fallbackUs = it->second->ElapsedSinceBeginUs();
				sample.progressUs = it->second->Progress;
				it->second->Progress = 0;
				return;  // ring commit happens when the batch resolves
			}
		}
		it->second->EndStep(step, _counter);
	}

	int32_t BudgetTracker::GetCost(RE::BSShadowLight* light) const
	{
		uint64_t key = reinterpret_cast<uint64_t>(light);
		auto it = _map.find(key);
		if (it == _map.end() || it->second->TrackedCount == 0)
			return GetAverageCostUs();  // unknown light: fall back to fleet average
		int32_t n = std::min(kBudgetWindowSize, it->second->TrackedCount);
		return it->second->Current / std::max(1, n);
	}

	void BudgetTracker::CleanupExpired()
	{
		ZoneScopedN("SCM::BudgetTracker::CleanupExpired");
		// Hard cap on erasures per call so a wave of expirations (e.g. the
		// player crossed a cell boundary 600 frames ago and dozens of
		// shadow lights all expire on the same tick) spreads its heap-free
		// cost across many frames instead of stalling one frame. With
		// kMaxErasePerCall=4 and Begin() calling this every 30 frames, the
		// tracker can drain ~8 expired entries per second steady-state and
		// up to 4 per call worst-case -- enough to keep the map bounded
		// in practice without the periodic stutter.
		constexpr size_t kMaxErasePerCall = 4;
		size_t erased = 0;
		for (auto it = _map.begin(); it != _map.end() && erased < kMaxErasePerCall;) {
			if (it->second->IsExpired(_counter)) {
				it = _map.erase(it);
				++erased;
			} else {
				++it;
			}
		}
	}

	int32_t BudgetTracker::GetAverageCostUs() const
	{
		int64_t sum = 0;
		int32_t count = 0;
		for (auto& [k, entry] : _map) {
			int32_t n = std::min(kBudgetWindowSize, entry->TrackedCount);
			if (n == 0)
				continue;
			sum += entry->Current / std::max(1, n);
			count++;
		}
		return count > 0 ? static_cast<int32_t>(sum / count) : 0;
	}
}
