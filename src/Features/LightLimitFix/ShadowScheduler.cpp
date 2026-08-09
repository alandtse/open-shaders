// ShadowScheduler.cpp
// The shadow caster scheduling core: geometry hashing, light transitions, ScheduleShadowCasters, and render dispatch.

#include <bit>
#include <cassert>
#include <cmath>

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/PerfUtils.h"
#include "../../Utils/UI.h"
#include "../LightLimitFix.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

#include <Windows.h>  // SEH (__try) for the shadow-light usability backstop

#define I18N_KEY_PREFIX "feature.light_limit_fix."

namespace ShadowCasterManager
{
	// Shadow map content hash for cached-shadow-map detection.

	/// EMA-anchored per-light radius, shared by the redraw and static-cache
	/// hashes so flicker-jitter alone can't defeat either.
	static std::unordered_map<const RE::NiLight*, float> s_hashRadiusAnchor;

	static float AnchoredRadiusForHash(const RE::NiLight* ni, float liveRadius)
	{
		PruneIfOversized(s_hashRadiusAnchor, 1024);
		auto [it, isNew] = s_hashRadiusAnchor.try_emplace(ni, liveRadius);
		if (!isNew)
			it->second += 0.15f * (liveRadius - it->second);
		return it->second;
	}

	/// Shared by the redraw hash and static-cache hash so both agree on what
	/// "moved" means.
	static std::uint64_t FoldLightPose(std::uint64_t h, RE::NiLight* ni, float posStep)
	{
		const float kPosStep = std::max(posStep, 1.0f);
		constexpr float kRotStep = 0.01f;
		constexpr float kRadiusStep = 1.0f;

		const auto& t = ni->world.translate;
		h = HashCombineFloat(h, QuantizeFloat(t.x, kPosStep));
		h = HashCombineFloat(h, QuantizeFloat(t.y, kPosStep));
		h = HashCombineFloat(h, QuantizeFloat(t.z, kPosStep));
		// Spot direction lives in the rotation matrix, so this covers a spot
		// re-aiming without translating.
		const auto& r = ni->world.rotate;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				h = HashCombineFloat(h, QuantizeFloat(r.entry[i][j], kRotStep));
		// NiPointLight uses .x. Hashed on the EMA anchor, not the live flicker-
		// jittered value -- see AnchoredRadiusForHash above.
		h = HashCombineFloat(h,
			QuantizeFloat(AnchoredRadiusForHash(ni, ni->GetLightRuntimeData().radius.x), kRadiusStep));
		return h;
	}

	/// Hash of a shadow map's content-determining inputs (light pose+radius,
	/// each caster's worldBound+identity); unchanged hash means the cached slot is current.
	static std::uint64_t ComputeShadowGeomHash(RE::BSShadowLight* light, float posStep)
	{
		if (!light)
			return 0;
		auto* ni = light->light.get();
		if (!ni)
			return 0;
		std::uint64_t h = 0x9e3779b97f4a7c15ull;  // arbitrary nonzero seed
		// Coarse radius fold: a permanent radius change must retire the baked
		// depth + snapshot. Truncated (not quantized-with-hysteresis) and anchored
		// like FoldLightPose's radius fold, so flicker alone can't flip it.
		h = h * 31 + static_cast<std::uint64_t>(
						 AnchoredRadiusForHash(ni, ni->GetLightRuntimeData().radius.x) / 64.0f);

		h = FoldLightPose(h, ni, posStep);

		// Caster set + each caster's worldBound (engine-updated). Same steps
		// the pose fold uses, so caster motion and light motion agree on what
		// counts as "moved".
		const float kPosStep = std::max(posStep, 1.0f);
		constexpr float kRadiusStep = 1.0f;
		for (auto& nip : light->geomList) {
			auto* ts = nip.get();
			if (!ts)
				continue;
			const auto raw = reinterpret_cast<std::uintptr_t>(ts);
			h = HashCombine(h, static_cast<std::uint32_t>(raw));
			h = HashCombine(h, static_cast<std::uint32_t>(raw >> 32));
			const auto& wb = ts->worldBound;
			h = HashCombineFloat(h, QuantizeFloat(wb.center.x, kPosStep));
			h = HashCombineFloat(h, QuantizeFloat(wb.center.y, kPosStep));
			h = HashCombineFloat(h, QuantizeFloat(wb.center.z, kPosStep));
			h = HashCombineFloat(h, QuantizeFloat(wb.radius, kRadiusStep));
		}

		return h;
	}

	/// Player-only dynamic-caster proxy, applied every frame on top of the
	/// (possibly stale) cached geomList hash -- see ComputeShadowGeomHash's
	/// walk cache below. Must never be folded into the cached hash itself: a
	/// stationary light the player walks through would then hash constant for
	/// up to kGeomHashRehashInterval frames, freezing their shadow and, with
	/// RedrawDueGateEnabled, starving its redraw admission -- reads as flicker
	/// (the actual reported bug this fixes). All actors would starve the
	/// budget, so this stays player-only; O(1), safe to run uncached.
	static std::uint64_t FoldPlayerCasterProxy(std::uint64_t h, RE::NiLight* ni, float posStep)
	{
		auto* plr = RE::PlayerCharacter::GetSingleton();
		if (!plr || !ni)
			return h;
		const auto pp = plr->GetPosition();
		const auto& lp = ni->world.translate;
		const float dx = pp.x - lp.x, dy = pp.y - lp.y, dz = pp.z - lp.z;
		const float r = ni->GetLightRuntimeData().radius.x;
		if (dx * dx + dy * dy + dz * dz >= r * r)
			return h;
		const float actorStep = std::clamp(posStep, 1.0f, 8.0f);
		h = HashCombineFloat(h, QuantizeFloat(pp.x, actorStep));
		h = HashCombineFloat(h, QuantizeFloat(pp.y, actorStep));
		h = HashCombineFloat(h, QuantizeFloat(pp.z, actorStep));
		return h;
	}

	/// Frame of a light's most recent UpdateCamera pass (exit hysteresis, keyed by
	/// recency not a resettable counter); valid for kCameraExitStreak frames even
	/// for a never-yet-slotted candidate.
	std::unordered_map<RE::BSShadowLight*, int32_t> s_lastValidFrame;

	/// Lights that have already rendered shadow content once. Keyed by NiLight,
	/// like the atlas slot owner: it outlives both pool churn and the wrapper
	/// recreation a promotion does, which is exactly what the fade-in must not
	/// restart on. Cleared on the scene/cell resets that recycle NiLights.
	std::unordered_set<const RE::NiLight*> s_everHadShadow;

	// UpdateCamera's inputs are flicker-jittered every frame, so a light near
	// the sphere-vs-frustum boundary flaps at flicker frequency without this.
	constexpr uint32_t kCameraExitStreak = 60;

	/// Consecutive-desire frames before a light may grow into a larger tile class.
	/// Leaky and geometry-only, so unrelated pool churn no longer resets it.
	constexpr uint16_t kPromoteStreakFrames = 60;

	/// Lights held this frame (UpdateCamera failed, exit streak immature, but
	/// already held a slot). Excluded from `pending` before budget accounting --
	/// rejected camera state means they keep their cached tile, not redraw.
	std::unordered_set<RE::BSShadowLight*> s_cameraHold;

	/// Consecutive frames a light scored below ShadowImpactFloor (exit hysteresis
	/// mirroring s_lastValidFrame); without it a light hovering near the floor
	/// re-bakes every dip and can latch splitExcluded into permanent full renders.
	std::unordered_map<RE::BSShadowLight*, uint32_t> s_belowFloorStreak;

	// CPU-only meters (steady_clock). The budget tracker's per-light cost is a
	// GPU timestamp interval; these answer the walk-vs-submission CPU question
	// it cannot. Accum = the engine Accumulate (cull walk + appends);
	// Submit = Render() (pass setup + draw submission).
	std::atomic<uint64_t> s_cpuAccumUs{ 0 };
	std::atomic<uint64_t> s_cpuSubmitUs{ 0 };
	std::atomic<uint32_t> s_cpuAccumN{ 0 };
	std::atomic<uint32_t> s_cpuSubmitN{ 0 };
	// EnableLight's own cost (setup + accumulate), isolated from Render's
	// GPU-submit cost above -- diagnostic for array-vs-atlas CPU comparisons.
	std::atomic<uint64_t> s_cpuEnableUs{ 0 };
	std::atomic<uint32_t> s_cpuEnableN{ 0 };

	template <class Fn>
	static uint64_t TimeUs(Fn&& fn)
	{
		const auto t0 = std::chrono::steady_clock::now();
		fn();
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
	}

	/// Frame + slot of each light's most recent Accumulate (render thread only).
	/// Prevents duplicate Accumulate registrations per light per frame.
	std::unordered_map<RE::BSShadowLight*, std::pair<uint32_t, uint32_t>> s_lightAccumFrame;

	/// Camera position when the current light's accumulate begins. The contribution
	/// cull measures caster size from here (viewer footprint), not the light.
	RE::NiPoint3 s_cullCameraPos{};

	/// StaticOnly re-bakes issued this frame. A bake re-rasterizes a light's
	/// whole static caster set into its cache tile, so this is the cost the
	/// cache trades against: sustained non-zero means the static set keeps
	/// churning and the cache is paying for itself repeatedly.
	std::atomic<uint32_t> s_staticBakeCount{ 0 };
	/// Cumulative bakes since load, published in the snapshot so a headless A/B
	/// can difference it across a run without attaching a profiler.
	std::atomic<uint64_t> s_staticBakeTotal{ 0 };

	/// Cumulative s_pendingCellReset drains since load -- diagnostic for
	/// whether Hook_ResetScene fires only on real zone transitions or also
	/// on ordinary exterior cell-grid streaming during normal movement.
	std::atomic<uint64_t> s_cellResetTotal{ 0 };

	// Each light gets exactly ONE filtered accumulate per frame -- normally
	// DynamicOnly, occasionally StaticOnly to rebake a changed caster set. The
	// atlas slot owns what's actually baked, so a realloc can't leave this stale.
	struct SplitState
	{
		uint64_t pendingHash = 0;      ///< static hash observed on the latest accumulate
		bool bakeQueued = true;        ///< a rebake is due -- next accumulate is StaticOnly
		bool bakeThisFrame = false;    ///< this frame's accumulate was StaticOnly (render to cache)
		uint8_t mismatchStreak = 0;    ///< consecutive accumulates whose hash differed from the bake
		RE::NiPoint3 bakePos{};        ///< light position the static tile was baked at
		RE::NiMatrix3 bakeRot{};       ///< light rotation the static tile was baked at (frustum-light drift check)
		uint8_t poseRebakes = 0;       ///< pose-drift rebakes inside the current window
		uint32_t poseWindowStart = 0;  ///< frame the pose-rebake window opened
		bool splitExcluded = false;    ///< jitter outruns the bake's validity: render full, no split
		bool fullThisFrame = false;    ///< this frame's accumulate was All (cap/exclusion fallback)
		/// Last completed accumulate appended >= 1 dynamic caster. Defaults
		/// true (never sleep a light until an accumulate proves it moverless).
		bool sawDynamicLastAccum = true;
		/// The latest StaticOnly bake appended >= 1 static caster. False for a
		/// bake taken before any caster settled: its cache tile is blank.
		bool bakeSawStatic = false;
	};
	std::unordered_map<RE::BSShadowLight*, SplitState> s_splitState;

	// --- Empty-dynamic sleep: schedule-time skip of moverless redraws --------

	// A composited bake stays valid only while the light sits within this
	// drift of the pose it was baked at (world units). Carried torches move
	// past this every frame, which is what keeps their shadows live.
	constexpr float kSplitPoseDriftMax = 4.0f;

	// Staleness backstop for sleeping lights: a real redraw at least this
	// often bounds every change the sleep predicate cannot observe (player,
	// off-screen movers, static-set edits) to under a second at 60 fps.
	constexpr int32_t kSleepRedrawIntervalFrames = 45;
	// Slot-phase stagger so lights that fell asleep together (scene load)
	// don't all take their backstop redraw on the same frame.
	constexpr int32_t kSleepStaggerStride = 7;

	// Stop-motion reporting floor: ~133ms at 60fps, roughly where a held shadow
	// reads as visible judder. Well inside kSleepRedrawIntervalFrames so a real
	// stall is flagged before the sleep backstop's own redraw masks it.
	constexpr int32_t kStallReportThreshold = 8;

	// Far longer than the sleep backstop: that predicate proves the tile is
	// unchanged, this one only claims nothing samples it, so it's the only
	// bound on a permanent false-negative (a caster thinner than tap pitch).
	constexpr int32_t kZeroDemandRedrawIntervalFrames = 240;

	// Cumulative schedule-time sleep skips since load (snapshot metric).
	std::atomic<uint64_t> s_sleepSkipTotal{ 0 };
	// Cumulative schedule-time zero-demand skips since load (snapshot metric).
	std::atomic<uint64_t> s_demandSkipTotal{ 0 };

	// Zero-demand-skip audit (measurement only): an oracle cross-checking
	// the engine's frustum cull, plus a counterfactual budget-admission
	// re-run with hard-skipped lights removed.

	// Outside by a tenth of the radius rather than by a texel: the engine's own
	// UpdateCamera runs at a different point in the frame than this read, so a
	// boundary-width disagreement carries no information.
	constexpr float kFrustumAuditMargin = 1.10f;
	// Consecutive frames a light must stay in the suspect quadrant. A moving
	// camera makes any single-frame disagreement meaningless.
	constexpr uint32_t kFrustumAuditStreak = 30;

	/// Consecutive frames a candidate sat in the "engine kept it, sphere is out"
	/// quadrant. Pointer-keyed like s_lastValidFrame; keys are never dereferenced,
	/// so address recycling can only misreport a diagnostic, never a decision.
	std::unordered_map<RE::BSShadowLight*, uint32_t> s_frustumSuspectStreak;

	// Debugging aid: lights already logged once for a "high-priority light is
	// demand-skipped" contradiction, so the warning doesn't repeat every
	// frame. Cleared once the light stops being skip-eligible.
	std::unordered_set<RE::BSShadowLight*> s_highPrioritySkipLogged;

	std::atomic<uint64_t> s_demandSkipEligibleTotal{ 0 };
	std::atomic<uint64_t> s_demandSwapInTotal{ 0 };
	std::atomic<uint64_t> s_demandRedrawsSavedTotal{ 0 };

	int32_t s_demandAuditLastLogFrame = 0;
	constexpr int32_t kDemandAuditLogInterval = 300;

	/// Whether a streak consumer was live last frame. Streaks freeze while none
	/// is, and a frozen count says nothing about the sampling since.
	bool s_demandStreaksActive = false;

	/// Audit totals accumulated across the log interval. s_schedDiag resets every
	/// frame, so logging it directly would report one arbitrary sample rather
	/// than a rate.
	struct DemandAuditWindow
	{
		uint64_t frames = 0;
		uint64_t saturatedFrames = 0;
		uint64_t budgetSaturatedFrames = 0;
		uint64_t candidates = 0;
		uint64_t keptOut = 0;
		uint64_t suspects = 0;
		uint64_t slotted = 0;
		uint64_t zero = 0;
		uint64_t subTap = 0;
		uint64_t skipEligible = 0;
		uint64_t swapIn = 0;
		uint64_t swapInAboveEps = 0;
		uint64_t skips = 0;
		int64_t redrawsSaved = 0;
		// Streak-length histograms: bucket i covers [2^(i-1), 2^i - 1], bucket 0
		// is exactly 0. resetHistogram buckets on streak end (misses in-progress
		// streaks); liveSnapshotHistogram recovers those each interval.
		std::array<uint64_t, 9> resetHistogram{};
		std::array<uint64_t, 9> liveSnapshotHistogram{};

		// Stop-motion window, kept separate from the demand-streak histograms: a
		// stall is a scheduler-starvation signal, a demand streak is occlusion
		// duration, and conflating them drowns the former under the latter.
		uint32_t stallMaxOfMax = 0;
		int32_t stallWorstSlot = -1;
		uint64_t stallMaxSum = 0;
		uint64_t stallOver = 0;
		double demandRatioSum = 0.0;
		std::array<uint64_t, 6> stallHistogram{};  // 0, 1-2, 3-7, 8-15, 16-44, 45+
	};
	DemandAuditWindow s_demandAuditWindow;

	static uint32_t EffectiveZeroDemandStreak()
	{
		return kZeroDemandSkipStreak;
	}

	// Mirrors EffectiveZeroDemandStreak() for the producer's tap count, purely
	// for audit-log visibility here -- the actual dispatch reads
	// LightLimitFix::settings directly (LightLimitFix.cpp).
	static uint32_t EffectiveDemandTapCount()
	{
		return LightLimitFix::kDemandTapCount;
	}

	/// True when the published sample supports a per-slot absence verdict. Fails
	/// open on every axis: unmeasured, stale or cluster-saturated frames all
	/// read as "fully visible".
	static bool DemandSampleUsable()
	{
		if (!s_shadowDemand.initialized)
			return false;
		if (s_shadowDemand.frameCounter - s_shadowDemand.lastDrainFrame >= kDemandStaleFrames)
			return false;
		return !s_shadowDemand.clusterSaturated;
	}

	/// Demand array index for a pool entry, or -1 when it has no reading: the
	/// sun's bookkeeping slot and anything past the fixed demand array (which
	/// lands in the producer's overflow counter and always reads 0).
	static int32_t DemandSlotFor(const LightEntry& e)
	{
		if (e.Index < 0 || (s_lights.Sun && e.Index == 0))
			return -1;
		if (static_cast<uint32_t>(e.Index) >= kMaxShadowDemandSlots)
			return -1;
		return e.Index;
	}

	/// Advances each slot's consecutive-samples-below-epsilon streak. Only a
	/// distinct sample counts, so a stalled readback can't re-count one reading
	/// and complete a streak without ever exercising a second jitter offset.
	static void AdvanceDemandStreaks()
	{
		// A streak measured under a different tap pattern is not evidence about
		// this one, so restart every count when the consumers come back on.
		if (!s_demandStreaksActive) {
			s_demandStreaksActive = true;
			for (int i = 0; i < s_lights.Size; i++)
				s_lights.Lights[i].untouchedSamples = 0;
		}
		const bool usable = DemandSampleUsable();
		for (int i = 0; i < s_lights.Size; i++) {
			auto& e = s_lights.Lights[i];
			// An empty slot's reading belongs to nobody. Zeroing (not skipping) stops
			// a light reclaiming its own free slot from resuming a streak measured
			// before it left -- reclaim is the one acquire path that preserves the entry.
			if (!e.Light) {
				e.untouchedSamples = 0;
				continue;
			}
			if (!usable || e.lastDemandSerial == s_shadowDemand.sampleSerial)
				continue;
			const int32_t slot = DemandSlotFor(e);
			if (slot < 0)
				continue;
			// The demand array is indexed by pool slot, so any divergence between
			// the two attributes one light's visibility to another. Debug-only:
			// GetShadowSlot is a linear scan.
			assert(e.Index == GetShadowSlot(e.Light));
			e.lastDemandSerial = s_shadowDemand.sampleSerial;
			// Hard reset, not a decay: one above-floor tap is strong evidence of
			// presence (the sparse sampler rarely hits a small lit footprint), so it
			// erases the whole streak. Sampling-gap tolerance lives in streak length.
			if (s_shadowDemand.maxLatest[slot] <= kDemandUntouchedMaxRaw)
				e.untouchedSamples++;
			else {
				// Bucket the completed streak's length before erasing it -- the
				// only point a finished occlusion event's duration is observable.
				s_demandAuditWindow.resetHistogram[DemandStreakBucket(e.untouchedSamples)]++;
				e.untouchedSamples = 0;
			}
		}
	}

	/// Accumulates this frame's audit counters and, on the log cadence, emits a
	/// correctness finding expected near zero (else the oracle is mis-modelled)
	/// and a capacity finding expected large (the normal frustum-vs-visibility gap).
	static void EmitDemandAuditLog(int32_t now)
	{
		auto& w = s_demandAuditWindow;
		w.frames++;
		w.saturatedFrames += s_shadowDemand.clusterSaturated ? 1 : 0;
		w.budgetSaturatedFrames += s_schedDiag.demand_budget_saturated ? 1 : 0;
		w.candidates += static_cast<uint64_t>(s_schedDiag.frustum_audit_candidates);
		w.keptOut += static_cast<uint64_t>(s_schedDiag.frustum_audit_kept_out);
		w.suspects += static_cast<uint64_t>(s_schedDiag.frustum_audit_suspects);
		w.slotted += static_cast<uint64_t>(s_schedDiag.demand_slotted);
		w.zero += static_cast<uint64_t>(s_schedDiag.demand_zero);
		w.subTap += static_cast<uint64_t>(s_schedDiag.demand_sub_tap);
		w.skipEligible += static_cast<uint64_t>(s_schedDiag.demand_skip_eligible);
		w.swapIn += static_cast<uint64_t>(s_schedDiag.demand_swap_in);
		w.swapInAboveEps += static_cast<uint64_t>(s_schedDiag.demand_swap_in_above_eps);
		w.skips += static_cast<uint64_t>(s_schedDiag.demand_skips);
		w.redrawsSaved += s_schedDiag.demand_redraws_saved;

		if (static_cast<uint32_t>(s_schedDiag.stall_max) > w.stallMaxOfMax) {
			w.stallMaxOfMax = static_cast<uint32_t>(s_schedDiag.stall_max);
			w.stallWorstSlot = s_schedDiag.stall_worst_slot;
		}
		w.stallMaxSum += static_cast<uint64_t>(s_schedDiag.stall_max);
		w.stallOver += static_cast<uint64_t>(s_schedDiag.stall_over_threshold);
		w.demandRatioSum += s_schedDiag.demand_ratio;

		if (now - s_demandAuditLastLogFrame < kDemandAuditLogInterval)
			return;
		s_demandAuditLastLogFrame = now;
		const double frames = static_cast<double>(std::max<uint64_t>(w.frames, 1));

		// Snapshot every live entry's CURRENT (possibly in-progress/right-censored)
		// streak once per interval -- the population resetHistogram can't see,
		// since an occlusion outliving the interval never triggers a reset.
		for (int i = 0; i < s_lights.Size; i++) {
			const auto& e = s_lights.Lights[i];
			if (e.Light) {
				w.liveSnapshotHistogram[DemandStreakBucket(e.untouchedSamples)]++;
				w.stallHistogram[StallBucket(e.dirtyStallFrames)]++;
			}
		}

		logger::info(
			"[SCM] frustum audit: cand={:.1f} kept_sphere_out={:.1f} suspect={:.1f} per frame "
			"(margin {:.2f}, >={}f, non-VR, no sun)",
			w.candidates / frames, w.keptOut / frames, w.suspects / frames,
			kFrustumAuditMargin, kFrustumAuditStreak);
		logger::info(
			"[SCM] visibility headroom: slotted={:.1f} zero={:.1f} (subTap={:.1f} occludedOrHidden={:.1f}) "
			"skipEligible={:.1f} skips={:.1f} swapIn={:.1f} swapInAboveEps={:.1f} redrawsSaved={} "
			"budgetSaturated={}/{} saturatedFrames={} eps={} streak={} taps={} phase1={} skipActive={}",
			w.slotted / frames, w.zero / frames, w.subTap / frames,
			static_cast<double>(w.zero - w.subTap) / frames,
			w.skipEligible / frames, w.skips / frames, w.swapIn / frames, w.swapInAboveEps / frames,
			w.redrawsSaved, w.budgetSaturatedFrames, w.frames, w.saturatedFrames,
			kDemandUntouchedMaxRaw, EffectiveZeroDemandStreak(), EffectiveDemandTapCount(),
			true, s_settings.SkipZeroDemandRedraw);
		logger::info(
			"[SCM] demand streak histogram [0,1-2,3-7,8-15,16-31,32-63,64-127,128-255,256+]: "
			"reset=[{},{},{},{},{},{},{},{},{}] live=[{},{},{},{},{},{},{},{},{}]",
			w.resetHistogram[0], w.resetHistogram[1], w.resetHistogram[2], w.resetHistogram[3],
			w.resetHistogram[4], w.resetHistogram[5], w.resetHistogram[6], w.resetHistogram[7], w.resetHistogram[8],
			w.liveSnapshotHistogram[0], w.liveSnapshotHistogram[1], w.liveSnapshotHistogram[2],
			w.liveSnapshotHistogram[3], w.liveSnapshotHistogram[4], w.liveSnapshotHistogram[5],
			w.liveSnapshotHistogram[6], w.liveSnapshotHistogram[7], w.liveSnapshotHistogram[8]);
		logger::info(
			"[SCM] redraw stall: maxOfMax={} (slot={}) meanMax={:.1f} over{}={:.1f}/frame "
			"hist[0,1-2,3-7,8-15,16-44,45+]=[{},{},{},{},{},{}] demandRatio={:.2f} dueGate={}",
			w.stallMaxOfMax, w.stallWorstSlot, static_cast<double>(w.stallMaxSum) / frames,
			kStallReportThreshold, static_cast<double>(w.stallOver) / frames,
			w.stallHistogram[0], w.stallHistogram[1], w.stallHistogram[2],
			w.stallHistogram[3], w.stallHistogram[4], w.stallHistogram[5],
			w.demandRatioSum / frames, s_shadowDemand.redrawDueGate);
		w = DemandAuditWindow{};
	}

	/// What SkipZeroDemandRedraw would skip. The ceiling on any win this feature
	/// can produce, and the input the Q2 counterfactual removes.
	static bool DemandSkipCandidate(const LightEntry& e)
	{
		if (!DemandSampleUsable())
			return false;
		const int32_t slot = DemandSlotFor(e);
		if (slot < 0 || e.LastDrawnFrame < 0)
			return false;
		return s_shadowDemand.maxLatest[slot] <= kDemandUntouchedMaxRaw &&
		       e.untouchedSamples >= EffectiveZeroDemandStreak();
	}

	/// True when this light's single accumulate can run DynamicOnly: split
	/// cache on and usable for it, slot bake valid, pose within bake drift.
	/// EnableLight picks its filter mode through this and the schedule-time
	/// sleep skip reuses it, so the two can never drift apart.
	static bool SplitDynamicOnlyEligible(RE::BSShadowLight* light, const SplitState& st, bool staticValid,
		float pendingScale)
	{
		if (!StaticAtlasReady())
			return false;
		if (st.splitExcluded || st.bakeQueued || !staticValid)
			return false;
		if (auto* ni = light->light.get()) {
			// Pose freshness: compositing movers over a bake taken at a
			// drifted pose shows two misaligned shadows at once (reads as
			// extra darkness).
			const float px = ni->world.translate.x - st.bakePos.x;
			const float py = ni->world.translate.y - st.bakePos.y;
			const float pz = ni->world.translate.z - st.bakePos.z;
			if (px * px + py * py + pz * pz > kSplitPoseDriftMax * kSplitPoseDriftMax)
				return false;
			// Rotation freshness (frustum lights only): reject once the forward
			// axis drifts past the bake's own texel resolution. semiWidth<=0
			// (degenerate field) skips rather than fails closed.
			if (const auto* frustumLight = skyrim_cast<const RE::BSShadowFrustumLight*>(light)) {
				const auto& frustumRtd = frustumLight->GetShadowFrustumLightRuntimeData();
				if (frustumRtd.semiWidth > 0.0f) {
					const float baseTileTexels = s_initialShadowMapResolution > 0 ?
					                                 static_cast<float>(s_initialShadowMapResolution) :
					                                 2048.0f;
					const float texels = baseTileTexels * std::max(pendingScale, kTileScaleFloor);
					const float halfAngle = frustumRtd.semiWidth / texels;
					const float maxCosDrift = std::clamp(1.0f - 2.0f * halfAngle * halfAngle, -1.0f, 1.0f);
					const RE::NiPoint3 fwd = ni->world.rotate.GetVectorY();
					const RE::NiPoint3 bakeFwd = st.bakeRot.GetVectorY();
					const float dot = fwd.x * bakeFwd.x + fwd.y * bakeFwd.y + fwd.z * bakeFwd.z;
					if (dot < maxCosDrift)
						return false;
				}
			}
		}
		return true;
	}

	/// Schedule-time sleep predicate: every condition proving this light's
	/// redraw would reproduce the tile it already holds, so the scheduler
	/// skips it outright (no accumulate, no budget, no render). Slot state is
	/// re-read every frame so an atlas reclaim or realloc wakes the light.
	static bool SleepSkipEligible(const LightEntry& e, int32_t slot, int32_t now)
	{
		if (e.LastDrawnFrame < 0)
			return false;
		// A staged class change must rerender before the light may sleep.
		if (e.pendingScale != e.renderedScale)
			return false;
		const auto it = s_splitState.find(e.Light);
		if (it == s_splitState.end())
			return false;
		const SplitState& st = it->second;
		// A pending static-set divergence or an observed mover means the
		// next accumulate would change the tile.
		if (st.mismatchStreak != 0 || st.sawDynamicLastAccum)
			return false;
		uint64_t bakedHash = 0;
		bool staticValid = false;
		if (!GetSlotStaticState(slot, bakedHash, staticValid))
			return false;
		AtlasTileTexels tile{};
		if (!GetSlotTileTexels(slot, tile) || !tile.contentValid)
			return false;
		if (!SplitDynamicOnlyEligible(e.Light, st, staticValid, e.pendingScale))
			return false;
		// Staleness backstop: never skip once the backstop redraw is due,
		// and keep pressing for it every frame until the budget grants it.
		if (now - e.LastDrawnFrame >= kSleepRedrawIntervalFrames)
			return false;
		if (((now + slot * kSleepStaggerStride) % kSleepRedrawIntervalFrames) == 0)
			return false;
		return true;
	}

	/// Schedule-time zero-demand predicate: unlike SleepSkipEligible (tile
	/// reproduces exactly), this only asserts nothing samples it. Reuses only
	/// sleep's tile-existence checks, not its static-bake/mover checks.
	static bool DemandSkipEligible(const LightEntry& e, int32_t slot, int32_t now)
	{
		if (!s_settings.SkipZeroDemandRedraw)
			return false;
		// Unmeasured, stale or cluster-saturated all read as fully visible.
		if (!DemandSampleUsable())
			return false;
		const int32_t demandSlot = DemandSlotFor(e);
		if (demandSlot < 0)
			return false;
		// The atlas tile is keyed by the pool slot, the demand array by e.Index;
		// divergence between the two suppresses one light's redraw on another's
		// visibility. Debug-only: GetShadowSlot is a linear scan.
		assert(slot == e.Index && e.Index == GetShadowSlot(e.Light));
		if (e.LastDrawnFrame < 0)
			return false;
		// A staged class change must rerender before the light may be skipped.
		if (e.pendingScale != e.renderedScale)
			return false;
		AtlasTileTexels tile{};
		if (!GetSlotTileTexels(slot, tile) || !tile.contentValid)
			return false;
		// "Never measured" is structurally distinct from "measured absent": new
		// lights and newly installed slots start the streak at zero.
		if (e.untouchedSamples < EffectiveZeroDemandStreak())
			return false;
		if (s_shadowDemand.maxLatest[demandSlot] > kDemandUntouchedMaxRaw)
			return false;
		// Staleness backstop: never skip once the backstop redraw is due, and
		// keep pressing for it every frame until the budget grants it.
		if (now - e.LastDrawnFrame >= kZeroDemandRedrawIntervalFrames)
			return false;
		if (((now + slot * kSleepStaggerStride) % kZeroDemandRedrawIntervalFrames) == 0)
			return false;
		return true;
	}

	// Light enable / disable helpers.

	static void EraseFromConvertList(RE::BSShadowLight* light)
	{
		for (auto it = s_normalConvert.begin(); it != s_normalConvert.end(); ++it) {
			if (it->light == light) {
				GameClearGeometryList(light);
				s_normalConvert.erase(it);
				return;
			}
		}
	}

	void DisableLight(RE::BSShadowLight* light)
	{
		EraseFromConvertList(light);
		auto* cull = light->cullingProcess;
		if (cull && cull->portalGraphEntry)
			GameClearPortalVisibility(reinterpret_cast<RE::BSPortalGraphEntry*>(cull->portalGraphEntry));
		light->ReturnShadowmaps();
	}

	// Activates a light as a normal (non-shadow) light without allocating a shadow
	// slot. Separate Tracy sub-zones for re-enable vs. first-conversion paths so a
	// capture distinguishes steady-state cost from fresh-conversion cost.
	void ConvertLight(RE::BSShadowLight* light, RE::ShadowSceneNode* ssn, bool isNS)
	{
		// Already converted: just re-enable so geometry picks it up this frame.
		for (auto& c : s_normalConvert) {
			if (c.light == light) {
				ZoneNamedN(zReEnable, "SCM::Engine::ConvertLight::ReEnable", true);
				GameEnableLight(ssn, light);
				return;
			}
		}

		// First conversion this session: release shadow resources, register.
		ZoneNamedN(zFirstConv, "SCM::Engine::ConvertLight::FirstConvert", true);
		auto* cull = GetLightCullingProcess(light);
		if (cull && cull->portalGraphEntry)
			GameClearPortalVisibility(reinterpret_cast<RE::BSPortalGraphEntry*>(cull->portalGraphEntry));
		light->ReturnShadowmaps();

		s_normalConvert.push_back({ light, isNS });
		GameEnableLight(ssn, light);
	}

	// Reset a promoted light's descriptor pool-slot indices to kNONE. BSShadowParabolicLight is
	// allocated non-zeroed and no ctor writes them; the VR engine indexes the depth-stencil pool
	// by the garbage -> nvwgf2umx OOB walk on teardown. kNONE forces re-allocation. Must run for
	// EVERY promoted light, incl. ones never EnableLight'd (else teardown reads the garbage).
	static void InitPromotedDescriptorSlots(RE::BSShadowLight* light)
	{
		if (!light)
			return;
		int32_t idx = s_lights.FindLight(light, s_settings.ShadowLightCount);
		if (idx < 0)
			idx = 0;
		if (globals::game::isVR) {
			auto& vrData = light->GetVRRuntimeData();
			for (auto& desc : vrData.shadowmapDescriptors) {
				desc.renderTarget = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.vrRenderTarget[0] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.vrRenderTarget[1] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.shadowmapIndex = static_cast<uint32_t>(idx);
			}
			for (auto& desc : vrData.focusShadowmapDescriptors) {
				desc.vrRenderTarget[0] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.vrRenderTarget[1] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
			}
		} else {
			for (auto& desc : light->GetRuntimeData().shadowmapDescriptors) {
				desc.renderTarget = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
				desc.shadowmapIndex = static_cast<uint32_t>(idx);
			}
		}
	}

	static void EnableLight(RE::BSShadowLight* light, RE::NiCamera* camera,
		RE::ShadowSceneNode* ssn, int slotIndex)
	{
		EraseFromConvertList(light);

		// Gated on s_focusShadowSlots so the engine's focus accumulate only runs when
		// ScheduleShadowCasters reserved the focus slot range this frame -- otherwise
		// it would write focus depth into slices point lights hold.
		if (s_focusShadowSlots > 0) {
			bool drawFocus = ShadowField(light, drawFocusShadows);
			if (drawFocus || (!*GetFocusShadowSelected() && light->GetIsFrustumOrDirectionalLight())) {
				GameSetupFocusShadowMaps(light, camera);
				GameSetupFocusShadowAccumulators(light);
				if (globals::game::isVR) {
					for (auto& desc : light->GetVRRuntimeData().focusShadowmapDescriptors) {
						desc.vrRenderTarget[0] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
						desc.vrRenderTarget[1] = RE::RENDER_TARGET_DEPTHSTENCIL::kNONE;
					}
				}
				ShadowField(light, drawFocusShadows) = true;
				*GetFocusShadowSelected() = true;
				*GetSunPtr() = reinterpret_cast<uint64_t>(light);
			}
		}

		GameEnableLight(ssn, light);
		GameSetShadowCasterSlot(ssn, light, *GetAccumLightSlot(), 1);

		{
			uint32_t mi = *GetMaskIndex();
			ShadowField(light, maskIndex) = mi;
			*GetMaskIndex() = mi + 1;
		}

		auto* nilight = light->light.get();
		if (nilight) {
			auto lpos = nilight->world.translate;
			auto cpos = camera->world.translate;
			auto delta = lpos - cpos;
			float dx = delta.x, dy = delta.y, dz = delta.z;
			float dist = lpos.GetDistance(cpos);
			float radius = nilight->GetLightRuntimeData().radius.x;

			float left, right, top, bottom;

			if (dist >= radius + camera->GetNearPlane()) {
				float inv = 1.0f / dist;
				float coord[4] = {
					lpos.x - dx * radius * inv,
					lpos.y - dy * radius * inv,
					lpos.z - dz * radius * inv,
					radius
				};
				float r1[2], r2[2];
				GameFrustumOverlap(camera, coord, r1, r2, 0.00001f);

				float vw = (float)*globals::game::viewWidth;
				float vh = (float)*globals::game::viewHeight;
				if (globals::game::isVR) {
					vw *= GetVRDRSWidthRatio();
					vh *= GetVRDRSHeightRatio();
				}

				left = (r1[0] + 1.0f) * 0.5f * vw;
				right = (r2[0] + 1.0f) * 0.5f * vw;
				top = (1.0f - (r1[1] + 1.0f) * 0.5f) * vh;
				bottom = (1.0f - (r2[1] + 1.0f) * 0.5f) * vh;
			} else {
				// Light contains the camera: use full screen.
				if (const uint32_t slot = *GetAccumLightSlot(); slot < kShadowMaskBits)
					*GetShadowMask() |= 1u << slot;
				left = right = top = bottom = -1.0f;
			}

			ShadowField(light, projectedBoundingBox) =
				RE::NiRect<uint32_t>((uint32_t)left, (uint32_t)right, (uint32_t)top, (uint32_t)bottom);
		}

		// Publish the light so the AppendVirtual hook can contribution-cull its
		// casters (RAII-cleared after); pick this accumulate's filter mode
		// BEFORE it runs -- StaticOnly on a queued rebake, else DynamicOnly.
		{
			bool split = StaticAtlasReady();
			SplitState* st = nullptr;
			CasterPass mode = CasterPass::All;
			uint64_t bakedHash = 0;
			bool staticValid = false;
			bool staticEmpty = false;
			if (split) {
				st = &s_splitState[light];
				st->fullThisFrame = false;
				if (st->splitExcluded) {
					// Latched jitter light: the cache can never stay fresh for
					// it, so it renders full every redraw (no bake, no copy).
					st->bakeThisFrame = false;
					st->fullThisFrame = true;
					split = false;
				}
			}
			if (split) {
				// The atlas slot owns what's baked, so a tile realloc (class
				// change) that drops the cache reads back as invalid here and
				// forces a rebake -- state keyed on the light alone would miss it.
				GetSlotStaticState(slotIndex, bakedHash, staticValid, &staticEmpty);
				// Do not inline this read into SplitDynamicOnlyEligible's call as a 4th
				// argument: unspecified argument-evaluation order let the inlined callee's
				// pose-drift float reuse slotIndex's spilled stack slot before this read, corrupting it.
				const bool slotInRange = slotIndex >= 0 && slotIndex < s_lights.Size;
				const float pendingScale = slotInRange ? s_lights.Lights[slotIndex].pendingScale : 1.0f;
				mode = (slotInRange && SplitDynamicOnlyEligible(light, *st, staticValid, pendingScale)) ?
				           CasterPass::DynamicOnly :
				           CasterPass::StaticOnly;
				// Bake budget: a hash-upset wave (scene entry, cell attach)
				// otherwise bakes every light in the same few frames and
				// starves the redraw budget. Deferred bakes stay queued; with
				// no valid seed the light renders full instead.
				if (mode == CasterPass::StaticOnly &&
					s_staticBakeCount.load(std::memory_order_relaxed) >= 2u) {
					st->bakeQueued = true;
					if (staticValid) {
						mode = CasterPass::DynamicOnly;
					} else {
						mode = CasterPass::All;
						st->fullThisFrame = true;
					}
				}
				if (mode == CasterPass::StaticOnly) {
					if (auto* ni = light->light.get()) {
						st->bakePos = ni->world.translate;
						st->bakeRot = ni->world.rotate;
					}
					st->bakeQueued = false;
				}
				st->bakeThisFrame = (mode == CasterPass::StaticOnly);
				if (st->bakeThisFrame) {
					s_staticBakeCount.fetch_add(1, std::memory_order_relaxed);
					// A pose-stable light bakes once per window; >=4 bakes in 300
					// frames means the cache never holds -- render full and stop it.
					const uint32_t nowFrame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
					if (nowFrame < st->poseWindowStart || nowFrame - st->poseWindowStart > 300u) {
						st->poseWindowStart = nowFrame;
						st->poseRebakes = 0;
					}
					if (++st->poseRebakes >= 4)
						st->splitExcluded = true;
				}
				// posStep is coarse (16 units) on purpose: a 1-unit step would
				// re-hash on every flicker jitter, queueing a rebake per flicker.
				s_visitStaticHash = 0x9e3779b97f4a7c15ull;
				if (auto* ni = light->light.get())
					s_visitStaticHash = FoldLightPose(s_visitStaticHash, ni, 16.0f);
				s_visitDynamicCount.store(0, std::memory_order_relaxed);
				s_visitStaticCount.store(0, std::memory_order_relaxed);
				s_cullPassMode.store(static_cast<int>(mode), std::memory_order_relaxed);
			}

			if (camera)
				s_cullCameraPos = camera->world.translate;  // viewer, for the caster cull
			SetCurrentCullLight(light);
			struct ClearCullLight
			{
				~ClearCullLight()
				{
					SetCurrentCullLight(nullptr);
					// Unconditional reset: the split-path reset below is skipped
					// when `split` is false, which would otherwise leave a stale
					// DynamicOnly filtering the next walk.
					s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
				}
			} clearGuard;

			uint32_t idx = static_cast<uint32_t>(slotIndex);
			// One Accumulate per light per frame (see s_lightAccumFrame): dedup
			// the ring-forming double and log which two slots collided.
			const uint32_t accumFrame =
				globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
			bool duplicateAccum = false;
			if (auto [it, inserted] = s_lightAccumFrame.try_emplace(light, accumFrame, idx); !inserted) {
				duplicateAccum = it->second.first == accumFrame;
				if (duplicateAccum) {
					static std::atomic<uint32_t> s_dupAccumCount{ 0 };
					const uint32_t n = s_dupAccumCount.fetch_add(1, std::memory_order_relaxed) + 1;
					if (n <= 8u || (n % 1000u) == 0u)
						logger::warn("[SCM] Skipped duplicate same-frame Accumulate (light={}, firstSlot={}, thisSlot={}, frame={}, n={})",
							(void*)light, it->second.second, idx, accumFrame, n);
				} else {
					it->second = { accumFrame, idx };
				}
			}
			if (s_lightAccumFrame.size() > 512)
				std::erase_if(s_lightAccumFrame, [&](const auto& kv) { return kv.second.first != accumFrame; });
			if (!duplicateAccum) {
				// Rebuild missed attachments: the engine attaches geometry once
				// per geometry (kRenderUse latch), so a light created after scene
				// attach otherwise casts nothing forever.
				s_healAttached.clear();
				s_accumRebuildAttach.store(light->geomList.empty(), std::memory_order_relaxed);
				s_cpuAccumUs.fetch_add(TimeUs([&] { light->Accumulate(idx, idx, nullptr); }), std::memory_order_relaxed);
				s_accumRebuildAttach.store(false, std::memory_order_relaxed);
				s_cpuAccumN.fetch_add(1, std::memory_order_relaxed);
				*GetAccumLightSlot() += light->shadowMapCount;
			}

			if (split) {
				st->pendingHash = s_visitStaticHash;
				// Latch mover presence for the sleep skip; only passes that could
				// see movers may clear it (StaticOnly filters them out).
				if (mode != CasterPass::StaticOnly && !duplicateAccum)
					st->sawDynamicLastAccum = s_visitDynamicCount.load(std::memory_order_relaxed) != 0;
				if (mode == CasterPass::StaticOnly)
					st->bakeSawStatic = !duplicateAccum && s_visitStaticCount.load(std::memory_order_relaxed) != 0;
				// Queue a rebake only once the hash divergence PERSISTS (3
				// accumulates), so a flickering hash that oscillates back to the
				// baked value doesn't rebake, but a genuine set change does.
				if (mode == CasterPass::DynamicOnly && st->pendingHash != bakedHash) {
					if (st->mismatchStreak < 0xFF)
						st->mismatchStreak++;
					// staticEmpty bypasses the hysteresis: it protects a GOOD
					// cache, not one that was never good.
					if (staticEmpty || st->mismatchStreak >= 3)
						st->bakeQueued = true;
				} else {
					st->mismatchStreak = 0;
				}
				s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
			}
		}

		// Extended mode: pre-set kNONE renderTarget so RenderCascade re-runs its
		// slot-allocation block (Hook_OverwriteShadowMapIndex); otherwise it keeps a
		// prior-frame slot and a light not redrawn this frame corrupts another's map.
		if (s_settings.ShadowLightCount > 4)
			InitPromotedDescriptorSlots(light);

		// Only apply lens flare when lensFlareData is non-null; calling it on parabolic lights
		// (null lensFlareData) registers them into the lens flare system, causing a crash
		// in the lens flare pass when it tries to dereference the null sprite data.
		if (light->lensFlareData)
			GameApplyLensFlare(light);
	}

	// Main shadow caster manager: replaces the game's
	// CalculateActiveShadowCasterLights entirely. Runs via stl::detour_thunk;
	// obtains all inputs from game globals.

	// Lightweight per-frame candidate entry used during scheduling.
	//
	// After the validation pass, exactly one of {chosen, excess, invalid}
	// is true (or none if it's the sun, which is processed separately).
	struct CandidateLight
	{
		RE::BSShadowLight* light{ nullptr };
		double score{ 0.0 };
		bool sun{ false };
		bool chosen{ false };         // valid + within ShadowLightCount budget
		bool excess{ false };         // valid but over budget (convert or disable)
		bool belowFloor{ false };     // on-screen impact below ShadowImpactFloor
		bool invalid{ false };        // shorthand: invalidCamera || invalidPortal
		bool invalidCamera{ false };  // UpdateCamera returned false -- shorthand for
									  // branches that don't care which sub-reason
		bool invalidPortal{ false };  // portal cull: light's cell not visible from
									  // camera's cell. Must DisableLight; converting
									  // routes through cluster lighting which has no
									  // portal awareness and would bleed through walls.

		// Sub-reasons for invalidCamera, recovered from engine side-band flags:
		//   frustrumCull == 0xff -> off-screen, ConvertLight wasted -> drop
		//   lodDimmer == 0.0f    -> past LOD fade end, still visible -> ConvertLight
		//                           (resets lodDimmer so cluster lighting picks it up)
		// Both can fire together; frustum-out wins (contribution is zero either way).
		bool invalidFrustum{ false };  // BSMultiBoundSphere::WithinFrustum / cone-frustum cull
		bool invalidLod{ false };      // engine's LOD-fade zeroed lodDimmer

		// UpdateCamera failed but the exit streak hasn't matured and the light
		// held a slot last frame: keep slot/tile, skip redraw, stay `chosen`.
		bool cameraHold{ false };
	};

	/// Q1: independent sphere-vs-frustum test, compared against the engine's
	/// frustrumCull flag, not SCM's own verdict (lags by kCameraExitStreak
	/// frames on purpose). Only "engine kept a sphere-out light" is a defect.
	static void AuditFrustumCull(const CandidateLight& c, RE::NiCamera* camera)
	{
		auto* ni = c.light->light.get();
		if (!ni)
			return;
		// Scaled world radius: testing the unscaled radius manufactures
		// suspects out of nothing for a light parented to a scaled NiNode.
		const float scale = ni->world.scale;
		const float radius = ni->GetLightRuntimeData().radius.x * scale;
		if (!(radius > 0.0f))
			return;  // directional or degenerate: a sphere test on it is meaningless
		s_schedDiag.frustum_audit_candidates++;

		if (camera->PointInFrustum(ni->world.translate, radius * kFrustumAuditMargin) ||
			c.light->frustrumCull != 0) {
			s_frustumSuspectStreak.erase(c.light);
			return;
		}
		s_schedDiag.frustum_audit_kept_out++;
		PruneIfOversized(s_frustumSuspectStreak, 512);
		const uint32_t frames = ++s_frustumSuspectStreak[c.light];
		if (frames < kFrustumAuditStreak)
			return;
		s_schedDiag.frustum_audit_suspects++;
		if (frames != kFrustumAuditStreak)
			return;  // one line per suspect episode, not per frame

		const int32_t slot = GetShadowSlot(c.light);
		const uint32_t demandMax = (slot >= 0 && static_cast<uint32_t>(slot) < kMaxShadowDemandSlots) ?
		                               s_shadowDemand.maxLatest[slot] :
		                               0u;
		const auto cp = camera->world.translate;
		const auto lp = ni->world.translate;
		const float dist = std::sqrt((lp.x - cp.x) * (lp.x - cp.x) + (lp.y - cp.y) * (lp.y - cp.y) +
									 (lp.z - cp.z) * (lp.z - cp.z));
		logger::info("[SCM]       [suspect] light={:#x} name={} r={:.1f} scale={:.3f} centerDist={:.1f} frames={} demandMax={}",
			reinterpret_cast<uintptr_t>(c.light), ni->name.c_str(), radius, scale, dist, frames, demandMax);
	}

	// Why a candidate was demoted/disabled this frame, captured from the validation
	// flags so the shadow table can explain each "Conv" row. Populated in the
	// candidate tally loop (the flags are computed regardless); read only when the
	// debug table is open, so no runtime cost in normal play.
	enum class ConvertReason : uint8_t
	{
		None,
		Portal,           // not reachable through the visible portal graph
		FrustumDistance,  // off-screen or beyond the shadow-cull distance (frustrumCull)
		LodFaded,         // past the light's LOD fade distance (lodDimmer == 0)
		Excess,           // ranked below the shadow-caster budget
		CameraOther,      // UpdateCamera rejected it for some other reason
	};
	static std::unordered_map<uintptr_t, ConvertReason> s_convertReason;

	// Headless scheduling-diagnostics snapshot (devbench `inspect kind=llfshadows`).
	// The scheduler fills s_schedSnapshot under the mutex at pass end; RequestSchedSnapshot
	// reads it under the same mutex. s_schedDumpFrames latches a short window of passes
	// so polling returns fresh data even while the menu is closed.
	static std::mutex s_schedSnapshotMutex;
	static SchedSnapshot s_schedSnapshot;
	static std::atomic<int> s_schedDumpFrames{ 0 };

	const char* SchedReasonName(uint8_t a_reason)
	{
		switch (static_cast<ConvertReason>(a_reason)) {
		case ConvertReason::Portal:
			return "portal";
		case ConvertReason::FrustumDistance:
			return "frustum";
		case ConvertReason::LodFaded:
			return "lod";
		case ConvertReason::Excess:
			return "excess";
		case ConvertReason::CameraOther:
			return "other";
		default:
			return "none";
		}
	}

	SchedSnapshot RequestSchedSnapshot()
	{
		// Prime ~2s of scheduling passes so repeated polls return fresh data even with
		// the menu closed; hand back the latest snapshot under the lock.
		s_schedDumpFrames.store(120, std::memory_order_relaxed);
		std::scoped_lock lock(s_schedSnapshotMutex);
		return s_schedSnapshot;
	}

	const char* ConvertReasonText(uintptr_t a_key)
	{
		auto it = s_convertReason.find(a_key);
		if (it == s_convertReason.end())
			return nullptr;
		switch (it->second) {
		case ConvertReason::Portal:
			return T(TKEY("conv_reason_portal"), "Reason: portal-culled -- the light's room isn't reachable through the visible portal graph.");
		case ConvertReason::FrustumDistance:
			return T(TKEY("conv_reason_frustum"), "Reason: frustum/distance-culled -- off-screen, or beyond the shadow-cull distance.");
		case ConvertReason::LodFaded:
			return T(TKEY("conv_reason_lod"), "Reason: LOD-faded -- past the light's LOD fade-out distance.");
		case ConvertReason::Excess:
			return T(TKEY("conv_reason_excess"), "Reason: excess -- ranked below the shadow-caster budget.");
		case ConvertReason::CameraOther:
			return T(TKEY("conv_reason_other"), "Reason: rejected by the engine visibility test.");
		default:
			return nullptr;
		}
	}

	// Explicitly resets s_currentCullLight/s_cullPassMode rather than trusting the
	// RAII guard: MSVC doesn't guarantee destructors run during SEH-caught
	// propagation without /EHa; a left-unreset cull light filters every walk after.
	static void LogShadowSehCatch(RE::BSShadowLight* a_light = nullptr)
	{
		SetCurrentCullLight(nullptr);
		s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
		static std::atomic<int> n{ 0 };
		if (n.fetch_add(1, std::memory_order_relaxed) < 20)
			logger::warn("[SCM] SEH caught AV in shadow-light usability scan (probe missed); skipping light. light={:#x}",
				reinterpret_cast<uintptr_t>(a_light));
	}

	// SEH backstop in its own function (no C++ unwinding objects) so MSVC accepts
	// __try. Returns false (unusable) on any access violation.
	template <class Fn>
	static bool SafeUsable(Fn&& a_fn, RE::BSShadowLight* a_light)
	{
		__try {
			return a_fn(a_light);
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			LogShadowSehCatch(a_light);
			return false;
		}
	}

	// Run UpdateCamera + EnableLight + the validity scan in one SEH region: EnableLight can read a
	// freed light and corrupt accumLightSlot/bounds, AV'ing here; catching it skips the light (the
	// per-frame accumLightSlot reset recovers). __declspec(noinline) is load-bearing -- inlining
	// dissolves the __except. No C++ unwinding objects in this frame (MSVC __try).
	template <class UsableFn>
	__declspec(noinline) static bool SafeEnableAndValidate(LightEntry& e, RE::NiCamera* a_camera,
		RE::ShadowSceneNode* a_ssn, std::uint32_t a_slot, UsableFn&& a_isUsable)
	{
		__try {
			e.Light->UpdateCamera(a_camera);
			EnableLight(e.Light, a_camera, a_ssn, a_slot);
			return e.Light && a_isUsable(e.Light);
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			LogShadowSehCatch(e.Light);
			return false;
		}
	}

	static void ReserveFocusShadowSlots()
	{
		// Reserve pool slots matching the engine's focus-shadow actor count;
		// eject any point light occupying a slot the engine now claims for
		// focus rendering (reassigned to a free slot, or falls through to excess).
		s_focusShadowSlots = std::clamp(GetFocusShadowActorCount(), 0, kFocusShadowMaxSlots);
		for (int i = kFocusShadowBaseSlotIndex; i < kFocusShadowBaseSlotIndex + s_focusShadowSlots && i < s_lights.Size; ++i) {
			if (s_lights.Lights[i].Light)
				s_lights.Lights[i].Clear();
		}
	}

	static RE::BSShadowLight* SetupSunLight(RE::ShadowSceneNode* ssn)
	{
		RE::BSShadowLight* sunLight = nullptr;
		// ---- Sun / directional light ----
		if (!GetSunBool2()) {
			auto* sun = ssn->GetRuntimeData().sunShadowDirLight;
			if (sun) {
				static REL::Relocation<bool*> vrUpdateFlag{ REL::Offset(0x1ed62f8) };
				uint8_t vrFlag = globals::game::isVR ? static_cast<uint8_t>(*vrUpdateFlag) + 1 : 0;
				sun->Accumulate(*GetAccumLightSlot(), 0, nullptr, vrFlag);

				if (sun->lensFlareData && !globals::game::isVR)
					GameApplyLensFlare(sun);

				if (globals::game::isVR && !GetVRAccumFirst()) {
					GameVRPrepareShadowMaps(sun);
					GameVRAccumulateShadowMaps(sun);
				}

				sunLight = sun;
			}
		}
		return sunLight;
	}

	static void ScrubFocusShadowFlags(RE::ShadowSceneNode* ssn)
	{
		// Extended mode: scrub drawFocusShadows on every active light and the sun --
		// a stale flag on a parabolic light in slot [4..7] sends
		// BSShadowParabolicLight::Render into its focus loop on a non-directional
		// light and CTDs. Belt-and-braces alongside the SetupResources patches.
		if (s_settings.ShadowLightCount > 4) {
			for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
				if (auto* l = sp.get())
					ShadowField(l, drawFocusShadows) = false;
			}
			if (auto* sun2 = ssn->GetRuntimeData().sunShadowDirLight)
				ShadowField(sun2, drawFocusShadows) = false;
		}
	}

	static void ScoreShadowCandidates(RE::ShadowSceneNode* ssn, RE::NiCamera* camera,
		RE::BSShadowLight* sunLight, std::vector<CandidateLight>& candidates)
	{
		{
			ZoneScopedN("SCM::ScoreCandidates");
			SetupSceneFormula(camera);

			candidates.clear();
			s_cameraHold.clear();
			ClearBelowFloor();
			candidates.reserve(ssn->GetRuntimeData().activeShadowLights.size());

			int32_t tmpIndex = 0;
			for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
				auto* l = sp.get();
				if (!l || l == sunLight)
					continue;
				// Promoted lights are allocated non-zeroed; their descriptor pool-slots stay
				// garbage until init. EnableLight only inits lights that win a render slot, so
				// init here (once) -- this is the type-safe BSShadowLight source -- to cover a
				// promoted light added but never enabled before teardown reads it.
				if (s_settings.ShadowLightCount > 4) {
					if (auto* ni = l->light.get(); ni) {
						std::scoped_lock lk(s_shadowConvertMutex);
						if (s_shadowConvert.contains(ni) && !s_shadowConvertDescriptorInited.contains(ni)) {
							InitPromotedDescriptorSlots(l);
							s_shadowConvertDescriptorInited.insert(ni);
						}
					}
				}
				auto& c = candidates.emplace_back();
				c.light = l;
				c.sun = false;
				float impact = 1.0f;
				c.score = CalculateLightScore(l, camera, tmpIndex++,
					s_settings.ShadowImpactFloor > 0.0f ? &impact : nullptr);
				if (s_settings.ShadowImpactFloor > 0.0f && impact < s_settings.ShadowImpactFloor) {
					// Exit hysteresis, own consecutive-count gate (unlike the recency-based
					// s_lastValidFrame above): a light must fail 15 consecutive frames
					// before dropping, or it flaps its atlas slot on every dip.
					PruneIfOversized(s_belowFloorStreak, 512);
					if (++s_belowFloorStreak[l] >= 15) {
						c.belowFloor = true;
						AddBelowFloor(reinterpret_cast<uintptr_t>(l));
					}
				} else {
					s_belowFloorStreak.erase(l);
				}
			}
#ifdef TRACY_ENABLE
			char buf[32];
			const int n = snprintf(buf, sizeof(buf), "candidates=%zu", candidates.size());
			if (n > 0)
				ZoneText(buf, static_cast<size_t>(n));
#endif
		}

		// Drop tracking entries whose NiLight left activeShadowLights -- pointer membership
		// only, never deref a freed light. This is the only safe cleanup: ResetSession can't
		// wipe (deferred, races the new cell's promotions) and bulk ClearLightArrays bypasses
		// the per-light erase, so without it promoted lights leak in and read as native.
		{
			std::scoped_lock lk(s_shadowConvertMutex);
			if (!s_shadowConvert.empty() || !s_shadowConvertDescriptorInited.empty()) {
				std::set<RE::NiLight*> liveNi;
				for (auto& c : candidates)
					if (auto* ni = c.light->light.get())
						liveNi.insert(ni);
				std::erase_if(s_shadowConvert, [&](RE::NiLight* ni) { return !liveNi.contains(ni); });
				std::erase_if(s_shadowConvertDescriptorInited, [&](RE::NiLight* ni) { return !liveNi.contains(ni); });
			}
		}

		// Prune split state for departed lights: pointer keys recycle, and a new
		// light inheriting a stale splitExcluded latch renders full forever.
		// Threshold avoids churning state for gate-flapping lights in small scenes.
		if (s_splitState.size() > 128) {
			std::set<RE::BSShadowLight*> liveLights;
			for (auto& c : candidates)
				liveLights.insert(c.light);
			std::erase_if(s_splitState, [&](const auto& kv) { return !liveLights.contains(kv.first); });
		}

		// Apply debug pins: bias scoring so pinned-shadow lights sort to the top
		// (forced chosen) and pinned-convert lights to the bottom (forced excess).
		// Pin sets are exclusive; a stale entry in both defers to pin-shadow.
		for (auto& c : candidates) {
			auto key = reinterpret_cast<uintptr_t>(c.light);
			if (s_pinShadow.count(key))
				c.score += 1e15;
			else if (s_pinConvert.count(key))
				c.score -= 1e15;
		}

		// Sort descending by score; sun always first. Deterministic tiebreak on exact
		// ties (e.g. symmetric braziers): std::sort isn't stable, so an unstable tie
		// flips which fixture claims a free slot first, producing visible flicker.
		std::sort(candidates.begin(), candidates.end(),
			[](const CandidateLight& a, const CandidateLight& b) {
				if (a.sun != b.sun)
					return a.sun;
				if (a.score != b.score)
					return a.score > b.score;
				return reinterpret_cast<uintptr_t>(a.light) < reinterpret_cast<uintptr_t>(b.light);
			});
	}

	static void ValidateCandidates(RE::NiCamera* camera, bool wantDiag, std::vector<CandidateLight>& candidates)
	{
		// ---- Validation pass (no game mutations) ----
		// Splitting validation from mutation defers state changes to a single
		// atomic loop later, avoiding a dangling-pointer window where an earlier
		// phase's mutation invalidates raw pointers held in s_lights[].
		auto* globalCull = *reinterpret_cast<RE::BSCullingProcess**>(
			*reinterpret_cast<uintptr_t**>(
				REL::RelocationID(528077, 415022).address()));

		int wantCount = 0;

		// Per-candidate UpdateCamera vfunc + portal-graph visibility walk
		// + chosen/excess tagging. Captured separately so memoization or
		// caching of UpdateCamera/portal verdicts can be measured.
		{
			ZoneNamedN(zoneCandVal, "SCM::CandidateValidation", true);
			// Non-VR only: the engine culls against two frustums there, so a
			// single-sphere-vs-frustum verdict there is uninterpretable.
			const bool auditFrustum = s_shadowDemand.instrumentation && !globals::game::isVR;
			for (auto& c : candidates) {
				auto* l = c.light;
				// Unconditional, ahead of every gate: run it inside the UpdateCamera
				// failure branch and a light the engine KEPT never reaches it,
				// making the one informative quadrant unreachable.
				if (auditFrustum)
					AuditFrustumCull(c, camera);
				// frustrumCull != 0 does NOT mean geometrically off-screen --
				// BSShadowParabolicLight also sets it for a shadow-distance LOD test.
				if (!l->UpdateCamera(camera)) {
					// Exit hysteresis: honor invalid only after kCameraExitStreak frames.
					// Age-prune (not PruneIfOversized): a missing entry means
					// never-valid, so only drop entries past the exit-streak window.
					if (s_lastValidFrame.size() > 512) {
						const int32_t pruneNow = *globals::game::frameCounter;
						std::erase_if(s_lastValidFrame, [pruneNow](const auto& kv) {
							return (pruneNow - kv.second) >= static_cast<int32_t>(kCameraExitStreak);
						});
					}
					// Capture BEFORE the restore below overwrites lodDimmer, else
					// invalidLod always reads false and its UI reason goes dead.
					const bool lodFadedNow = (l->lodDimmer == 0.0f);
					// UpdateCamera's LOD sub-test can zero lodDimmer even on a held frame;
					// without restoring it, addShadowLight's fade*=lodDimmer would render
					// a held light at zero intensity despite keeping its slot.
					RestoreZeroedLodDimmer(l);
					const auto lastValidIt = s_lastValidFrame.find(l);
					const int32_t nowFrame = *globals::game::frameCounter;
					// No entry at all means this light has NEVER passed UpdateCamera --
					// must not claim chosen/budget status on its very first candidate frame.
					const bool recentlyValid = lastValidIt != s_lastValidFrame.end() &&
					                           (nowFrame - lastValidIt->second) <
					                               static_cast<int32_t>(kCameraExitStreak);
					if (recentlyValid) {
						c.cameraHold = true;
						s_cameraHold.insert(l);
					} else {
						c.invalidCamera = true;
						c.invalid = true;
						// Both can be true (off-screen AND LOD-faded); recorded as
						// independent bits, since frustum-out is terminal but LOD-faded is a convert.
						c.invalidFrustum = (l->frustrumCull != 0);
						c.invalidLod = lodFadedNow;
						continue;
					}
				} else {
					s_lastValidFrame[l] = *globals::game::frameCounter;
				}
				// No culling process/portal means unconditionally visible. Skip for
				// promoted lights (rebuilt process never room-associates) and held lights.
				auto* cull = (c.cameraHold || IsPromotedLight(l->light.get())) ? nullptr : GetLightCullingProcess(l);
				if (cull) {
					auto* portal = reinterpret_cast<RE::BSPortalGraphEntry*>(cull->portalGraphEntry);
					if (portal) {
						auto* gPortal = globalCull ? reinterpret_cast<RE::BSPortalGraphEntry*>(globalCull->portalGraphEntry) : nullptr;
						if (gPortal && !GamePortalHasSharedVisibility(gPortal, portal)) {
							c.invalidPortal = true;
							c.invalid = true;
							continue;
						}
					}
				}

				// Impact floor: a below-floor light converts to a non-shadow light
				// (keeps diffuse via clusters, drops redraw), same path as over-budget.
				if (c.belowFloor) {
					c.excess = true;
				}
				// Effective point-light capacity excludes engine-claimed focus shadow slots.
				else if (wantCount < s_settings.ShadowLightCount - s_focusShadowSlots) {
					c.chosen = true;
					wantCount++;
				} else {
					c.excess = true;
				}
			}

			// Tracy candidate breakdown, per-frame, to verify chosen + excess +
			// invalid_camera + invalid_portal == total. Demotion map only fills
			// when wantDiag (menu open / devbench dump) to skip per-frame hash churn.
			if (wantDiag)
				s_convertReason.clear();
			for (auto& c : candidates) {
				s_schedDiag.candidates_total++;
				if (c.chosen)
					s_schedDiag.candidates_chosen++;
				if (c.excess)
					s_schedDiag.candidates_excess++;

				// Capture why a non-chosen light is demoted, for the shadow table. Portal
				// wins, then frustum/distance, LOD, excess -- matching branch precedence.
				if (wantDiag && !c.chosen) {
					ConvertReason r = ConvertReason::None;
					if (c.invalidPortal)
						r = ConvertReason::Portal;
					else if (c.invalidFrustum)
						r = ConvertReason::FrustumDistance;
					else if (c.invalidLod)
						r = ConvertReason::LodFaded;
					else if (c.excess)
						r = ConvertReason::Excess;
					else if (c.invalidCamera)
						r = ConvertReason::CameraOther;
					if (r != ConvertReason::None)
						s_convertReason[reinterpret_cast<uintptr_t>(c.light)] = r;
				}
				if (c.invalidCamera)
					s_schedDiag.candidates_invalid_camera++;
				if (c.invalidPortal)
					s_schedDiag.candidates_invalid_portal++;
				// Sub-reason breakdown: a light may be both frustum-out AND LOD-faded,
				// so the sum can exceed candidates_invalid_camera. "other" catches
				// UpdateCamera failures with frustrumCull cleared and lodDimmer > 0.
				if (c.invalidCamera) {
					if (c.invalidFrustum)
						s_schedDiag.candidates_invalid_frustum++;
					if (c.invalidLod)
						s_schedDiag.candidates_invalid_lod++;
					if (!c.invalidFrustum && !c.invalidLod)
						s_schedDiag.candidates_invalid_other++;
				}
			}
		}  // end SCM::CandidateValidation
	}

	static void UpdatePoolMembership(RE::ShadowSceneNode* ssn, RE::BSShadowLight* sunLight,
		std::vector<CandidateLight>& candidates)
	{
		// Pool membership update: drop expired pointers, drop unchosen,
		// add newly chosen, sync sun slot.
		{
			ZoneNamedN(zonePoolMem, "SCM::UpdatePoolMembership", true);
			// ---- Sync s_lights (our active pool) ----
			// Drop entries whose pointers are no longer in activeShadowLights (engine
			// may have freed them since last frame), protecting later lookups from
			// dereferencing dangling pointers.
			std::unordered_set<RE::BSShadowLight*> aliveSet;
			{
				auto& alive = ssn->GetRuntimeData().activeShadowLights;
				aliveSet.reserve(alive.size() + 1);
				if (sunLight)
					aliveSet.insert(sunLight);
				for (auto& sp : alive)
					if (auto* l = sp.get())
						aliveSet.insert(l);
			}
			for (int i = 0; i < s_lights.Size; i++) {
				if (!s_lights.Lights[i].Light)
					continue;
				if (aliveSet.find(s_lights.Lights[i].Light) == aliveSet.end()) {
					s_schedDiag.reconciliation_clears++;
					s_lights.Lights[i].Clear();
				}
			}

			// Sync s_normalConvert: drop entries the engine removed from both active
			// lists (bulk cell-teardown bypasses Hook_ConvertLights_Remove), plus
			// functionally-dead ones (fade=0/lodDimmer=0/null) GameEnableLight never deactivates.
			if (!s_normalConvert.empty()) {
				std::unordered_set<RE::BSLight*> normalAlive;
				normalAlive.reserve(aliveSet.size() + ssn->GetRuntimeData().activeLights.size());
				for (auto* p : aliveSet)
					normalAlive.insert(static_cast<RE::BSLight*>(p));
				for (auto& sp : ssn->GetRuntimeData().activeLights)
					if (auto* l = sp.get())
						normalAlive.insert(l);

				const std::size_t before = s_normalConvert.size();
				std::erase_if(s_normalConvert, [&](const ConvertedLight& c) {
					// Tier 1: dangling / engine-removed.
					if (!c.light || normalAlive.find(static_cast<RE::BSLight*>(c.light)) == normalAlive.end())
						return true;
					// Tier 2: functionally dead. Cheap derefs only -- no
					// virtual calls or extra hash lookups.
					auto* niLight = c.light->light.get();
					if (!niLight)
						return true;
					const auto& rt = niLight->GetLightRuntimeData();
					const float colorSum = rt.diffuse.red + rt.diffuse.green + rt.diffuse.blue;
					if (colorSum * rt.fade <= 1e-4f)
						return true;
					if (rt.radius.x <= 1e-4f)
						return true;
					return false;
				});
				const std::size_t after = s_normalConvert.size();
				if (before != after) {
					static int loggedShrink = 0;
					if (loggedShrink++ < 20 || (before - after) > 32) {
						logger::debug("[SCM] s_normalConvert reconcile: {} -> {} ({} dropped)",
							before, after, before - after);
					}
				}
			}

			// Drop entries no longer chosen. Rank-drift suppression lives in
			// CalculateLightScore's decay term; the slot pool is a dumb container
			// that follows the chosen set. The atomic loop handles ConvertLight/
			// DisableLight for dropped occupants the same frame.
			for (int i = 0; i < s_lights.Size; i++) {
				if (!s_lights.Lights[i].Light)
					continue;
				bool stillChosen = (i == 0 && s_lights.Sun);  // sun slot
				if (!stillChosen) {
					for (auto& c : candidates) {
						if (c.light == s_lights.Lights[i].Light && c.chosen) {
							stillChosen = true;
							break;
						}
					}
				}
				if (!stillChosen)
					s_lights.Lights[i].Clear();
			}

			// Two passes: reclaim-by-owner for every chosen light first, then hand
			// fresh slots to whoever's left -- one pass let a higher-ranked light
			// steal a peer's still-orphaned slot before it reclaimed it.
			static std::vector<CandidateLight*> unplaced;
			unplaced.clear();
			for (auto& c : candidates) {
				if (!c.chosen)
					continue;
				bool alreadyIn = false;
				for (int i = 0; i < s_lights.Size && !alreadyIn; i++)
					if (s_lights.Lights[i].Light == c.light)
						alreadyIn = true;
				if (alreadyIn)
					continue;

				// A light returning after a brief eviction (gate flap) reclaims the free
				// slot still holding ITS OWN tile, so content resumes sampling immediately.
				bool reclaimed = false;
				if (auto* ni = c.light->light.get()) {
					const int ownerIdx = FindFreeSlotByOwner(ni);
					if (ownerIdx >= 0 && !s_lights.Lights[ownerIdx].Light) {
						s_lights.Lights[ownerIdx].Light = c.light;
						reclaimed = true;
					}
				}
				if (!reclaimed)
					unplaced.push_back(&c);
			}
			for (auto* cp : unplaced) {
				const int idx = s_lights.FindFreeIndex(true, s_settings.ShadowLightCount, s_settings.ConvertedShadowSlots);
				if (idx < 0)
					continue;
				// Eviction nulls Light* but leaves LightEntry intact as a cache key.
				// Clear at acquire so the new occupant doesn't inherit LastDrawnFrame/
				// lastGeomHash and skip its first render, sampling stale content.
				s_lights.Lights[idx].Clear();
				// Drop the previous occupant's tile: its depth must not be
				// advertised under the new light's projection.
				FreeSlotTile(idx);
				// A fresh occupant may be a recycled pointer; the pointer-keyed
				// EMA/streak/split-state caches aren't cleared by Clear() above, so
				// erase them explicitly or it inherits the previous occupant's state.
				s_lastValidFrame.erase(cp->light);
				s_belowFloorStreak.erase(cp->light);
				s_splitState.erase(cp->light);
				if (auto* ni = cp->light->light.get()) {
					ResetScoreAnchor(ni);
					s_hashRadiusAnchor.erase(ni);
				}
				s_lights.Lights[idx].Light = cp->light;
			}

			// Update sun slot (slot 0).
			if (sunLight) {
				if (s_lights.Lights[0].Light != sunLight) {
					s_lights.Lights[0].Clear();
					s_lights.Lights[0].Light = sunLight;
				}
				s_lights.Sun = true;
			} else {
				// Sun is gone: clear slot 0 only if it was tracking the sun. If
				// Sun was already false, slot 0 holds a regular point light
				// (FindFreeIndex allocates there when Sun=false) -- do not wipe it.
				if (s_lights.Sun)
					s_lights.Lights[0].Clear();
				s_lights.Sun = false;
			}

			// Publish each occupant's score: it also orders the atlas cell budget
			// (within a class band) and drives the redraw curve percentile.
			{
				std::unordered_map<const RE::BSShadowLight*, double> scoreByLight;
				scoreByLight.reserve(candidates.size());
				for (const auto& c : candidates)
					scoreByLight.emplace(c.light, c.score);
				for (int i = 0; i < s_lights.Size; i++) {
					auto& e = s_lights.Lights[i];
					if (!e.Light)
						continue;
					if (auto it = scoreByLight.find(e.Light); it != scoreByLight.end())
						e.lastScore = it->second;
				}
			}
		}  // end SCM::UpdatePoolMembership
	}

	static double ComputeRedrawBudget()
	{
		double budget = s_settings.RedrawBudgetMs;
		// Frame-time EMA + budget formula evaluation, scoped separately from
		// ScheduleLoop so the once-per-frame cost is visible distinct from per-light cost.
		{
			ZoneNamedN(zoneCompBud, "SCM::ComputeBudget", true);
			// Update frame-time EMA and ring buffer (always, for formula params and UI).
			const float dtMs = *globals::game::deltaTime * 1000.0f;
			s_ftRing[s_ftHead] = dtMs;
			s_ftHead = (s_ftHead + 1) % kFrameWindow;
			if (s_ftCount < kFrameWindow)
				++s_ftCount;
			s_ftEMA = (s_ftCount == 1) ? dtMs : 0.1f * dtMs + 0.9f * s_ftEMA;

			const float target_ms = ComputeFrameTimePercentile90();
			if (s_ftEMA < target_ms)
				s_stableFrames = std::min(s_stableFrames + 1, 45);
			else
				s_stableFrames = 0;

			FormulaHelper::SetParam(kFormulaParam_FrameTime, static_cast<double>(s_ftEMA));
			FormulaHelper::SetParam(kFormulaParam_FrameTarget, static_cast<double>(target_ms));
			FormulaHelper::SetParam(kFormulaParam_StableFrames, static_cast<double>(s_stableFrames));

			// Evaluate the budget for the whole frame.
			//   Manual:  fixed slider value (RedrawBudgetMs).
			//   Formula: user-editable exprtk expression.
			if (s_settings.BudgetMode == BudgetModeEnum::Formula && s_formulaRedrawBudget) {
				// A user formula can divide by zero or take log/sqrt of a negative;
				// static_cast<int32_t> of NaN/inf below is UB, so sanitize here like
				// ShadowFormula.cpp's own score evaluator does.
				const double v = s_formulaRedrawBudget->Calculate();
				budget = std::isfinite(v) ? v : 0.0;
			}
			s_autoBudgetMs = static_cast<float>(budget);
		}  // end SCM::ComputeBudget
		return budget;
	}

	/// Per-frame record of a light demand-skipped ahead of the pending loop,
	/// so its percentile can still be checked against the live pool.
	struct HighPrioritySkip
	{
		LightEntry* entry;
		RE::BSShadowLight* light;
	};

	/// Priority percentile of `score` within `scoreRank` (ascending-sorted
	/// snapshot of every live light's lastScore); 1.0 == highest score.
	static float ScorePercentile(const std::vector<double>& scoreRank, double score)
	{
		if (scoreRank.size() < 2)
			return 1.0f;
		const auto it = std::lower_bound(scoreRank.begin(), scoreRank.end(), score);
		return static_cast<float>(it - scoreRank.begin()) / static_cast<float>(scoreRank.size() - 1);
	}

	/// Dirty-first, RedrawScore-ascending order shared by the real admission pass
	/// and the Q2 counterfactual replay so both traverse candidates identically.
	static bool ScheduleOrderLess(uint32_t starvationFloorFrames, const LightEntry* a, const LightEntry* b)
	{
		if (a->schedDirty != b->schedDirty)
			return a->schedDirty && !b->schedDirty;
		const bool aStarved = a->dirtyStallFrames >= starvationFloorFrames;
		const bool bStarved = b->dirtyStallFrames >= starvationFloorFrames;
		if (aStarved != bStarved)
			return aStarved && !bStarved;
		if (aStarved && a->dirtyStallFrames != b->dirtyStallFrames)
			return a->dirtyStallFrames > b->dirtyStallFrames;
		if (a->RedrawScore != b->RedrawScore)
			return a->RedrawScore < b->RedrawScore;
		if (a->LastDrawnFrame != b->LastDrawnFrame)
			return a->LastDrawnFrame < b->LastDrawnFrame;
		return a->Index < b->Index;
	}

	/// Sorts each live light (excluding the sun) into `pending` or one of
	/// the four skip buckets (held / new / sleep / demand).
	static void ClassifyPendingLights(int32_t now, std::vector<LightEntry*>& pending,
		std::vector<HighPrioritySkip>& highPrioritySkips, std::vector<LightEntry*>& sleepSkips,
		std::vector<LightEntry*>& cameraHoldSkips, std::vector<LightEntry*>& newLightSkips)
	{
		for (int i = 0; i < s_lights.Size; i++) {
			auto& e = s_lights.Lights[i];
			if (!e.Light || e.RedrawFrame)
				continue;
			// Cleared here (not just at the skip sites below) so a light falling
			// through to `pending` reads correctly as "not skipped" for the stall sweep.
			e.skippedThisFrame = false;
			// A held light keeps its tile but must not redraw; skippedThisFrame keeps
			// the stall sweep from accruing against a light that can't act.
			if (s_cameraHold.count(e.Light)) {
				e.skippedThisFrame = true;
				cameraHoldSkips.push_back(&e);
				continue;
			}
			// Honour AllowDrawNewLight: when disabled, brand-new entries wait for the
			// next frame instead of competing for this frame's budget.
			if (!s_settings.AllowDrawNewLight && e.LastDrawnFrame < 0) {
				newLightSkips.push_back(&e);
				continue;
			}
			// Empty-dynamic sleep: a moverless light with a valid, fresh static bake
			// would redraw an identical tile, so skip entirely and sample the cached tile.
			if (SleepSkipEligible(e, i, now)) {
				s_schedDiag.sleep_skips++;
				s_sleepSkipTotal.fetch_add(1, std::memory_order_relaxed);
				e.skippedThisFrame = true;
				e.schedDirty = false;
				sleepSkips.push_back(&e);
				continue;
			}
			// Zero-demand skip, tested after sleep so the two never double-count.
			// schedDirty stays at its last value so a wrongly-skipped truly-dirty
			// light still shows starvation risk; skippedThisFrame freezes the stall counter.
			if (DemandSkipEligible(e, i, now)) {
				s_schedDiag.demand_skips++;
				s_demandSkipTotal.fetch_add(1, std::memory_order_relaxed);
				e.skippedThisFrame = true;
				highPrioritySkips.push_back({ &e, e.Light });
				continue;
			}
			s_highPrioritySkipLogged.erase(e.Light);
			pending.push_back(&e);
		}
	}

	/// Debug-logs demand-skipped lights whose score outranks the live pool's
	/// median, then refreshes desiredScale for every skipped entry so none
	/// can hoard a stale-high atlas class while excluded below.
	static void LogAndRefreshSkippedLights(RE::NiCamera* camera, float baseTileTexels,
		const std::vector<double>& scoreRank, const std::vector<HighPrioritySkip>& highPrioritySkips,
		const std::vector<LightEntry*>& sleepSkips, const std::vector<LightEntry*>& cameraHoldSkips,
		const std::vector<LightEntry*>& newLightSkips)
	{
		// Debugging aid: score has no occlusion term, so a high-percentile light
		// reading zero demand is expected, not an error; logged once per episode.
		for (const auto& skip : highPrioritySkips) {
			const float percentile = ScorePercentile(scoreRank, skip.entry->lastScore);
			if (percentile < 0.5f || s_highPrioritySkipLogged.contains(skip.light))
				continue;
			s_highPrioritySkipLogged.insert(skip.light);
			auto* ni = skip.light->light.get();
			const int32_t slot = GetShadowSlot(skip.light);
			const uint32_t demandMax = (slot >= 0 && static_cast<uint32_t>(slot) < kMaxShadowDemandSlots) ?
			                               s_shadowDemand.maxLatest[slot] :
			                               0u;
			const float dist = ni ? std::sqrt(
										(ni->world.translate.x - camera->world.translate.x) * (ni->world.translate.x - camera->world.translate.x) +
										(ni->world.translate.y - camera->world.translate.y) * (ni->world.translate.y - camera->world.translate.y) +
										(ni->world.translate.z - camera->world.translate.z) * (ni->world.translate.z - camera->world.translate.z)) :
			                        -1.0f;
			logger::debug(
				"[SCM] high-priority light demand-skipped: light={:#x} name={} percentile={:.2f} score={:.1f} demandMax={} streak={} centerDist={:.1f}",
				reinterpret_cast<uintptr_t>(skip.light), ni ? ni->name.c_str() : "?", percentile,
				skip.entry->lastScore, demandMax, skip.entry->untouchedSamples, dist);
		}

		// Refresh desiredScale for skip-eligible entries too: they never reach
		// `pending`, so without this a stale-high class outranks genuinely visible
		// lights in the atlas rank budget for the whole skip window.
		auto refreshSkippedDesiredScale = [&](LightEntry* e) {
			if (auto* ni = e->Light->light.get()) {
				const auto geom = ComputeLightGeometry(e->Light, camera, ni->GetLightRuntimeData().radius.x);
				float sizeProxy = geom.sizeProxy;
				if (geom.attCam <= 0.0f && geom.attPlr <= 0.0f)
					sizeProxy = std::min(sizeProxy, 0.25f);
				e->desiredScale = AtlasActive() ?
				                      TileScaleForCoverage(sizeProxy, baseTileTexels, e->desiredScale) :
				                      1.0f;
				if (!AtlasActive())
					e->pendingScale = e->desiredScale;
			}
		};
		for (const auto& skip : highPrioritySkips)
			refreshSkippedDesiredScale(skip.entry);
		for (auto* e : sleepSkips)
			refreshSkippedDesiredScale(e);
		for (auto* e : cameraHoldSkips)
			refreshSkippedDesiredScale(e);
		for (auto* e : newLightSkips)
			refreshSkippedDesiredScale(e);
	}

	/// Computes RedrawScore (the admission deadline) and desiredScale for
	/// every pending candidate.
	static void ScorePendingLights(RE::NiCamera* camera, int32_t now, float baseTileTexels, bool auditDemand,
		const std::vector<double>& scoreRank, const std::vector<LightEntry*>& pending)
	{
		for (auto* e : pending) {
			// Measured unconditionally: both the formula and schedDirty's displacementTexels
			// check need it against the same e->lastRenderedPos; deadzoning below only
			// masks flicker jitter, never genuine cumulative drift.
			double displacementMagnitude = 0.0;
			double formulaDisplacement = 0.0;
			if (auto* nilight = e->Light->light.get()) {
				auto& curr = nilight->world.translate;
				float dx = curr.x - e->lastRenderedPos.x;
				float dy = curr.y - e->lastRenderedPos.y;
				float dz = curr.z - e->lastRenderedPos.z;
				displacementMagnitude = static_cast<double>(sqrtf(dx * dx + dy * dy + dz * dz));
				formulaDisplacement = displacementMagnitude;

				// Deadzone sub-texel motion: idle-jitter wobble under a texel previously
				// read as "displacement" every frame, showing as flicker on torches/braziers.
				const float radius = nilight->GetLightRuntimeData().radius.x;
				if (radius > 0.0f) {
					const float approxPosStep = radius /
					                            std::max(baseTileTexels * std::max(e->pendingScale, kTileScaleFloor), 1.0f);
					// approxPosStep shrinks as tile resolution grows, but flicker jitter is
					// a fixed world-space wobble that can exceed one texel at full class --
					// floor at the light's own flicker amplitude (cached per NiLight).
					static std::unordered_map<const RE::NiLight*, float> s_flickerAmplitude;
					PruneIfOversized(s_flickerAmplitude, 1024);
					auto [ampIt, ampNew] = s_flickerAmplitude.try_emplace(nilight, 0.0f);
					if (ampNew) {
						if (auto* ref = nilight->GetUserData()) {
							if (auto* base = ref->GetObjectReference()) {
								if (auto* ligh = base->As<RE::TESObjectLIGH>(); ligh && !ligh->GetNoFlicker())
									ampIt->second = ligh->data.flickerMovementAmplitude;
							}
						}
					}
					const float deadzone = std::max(approxPosStep, ampIt->second);
					if (formulaDisplacement < static_cast<double>(std::max(deadzone, 1e-4f)))
						formulaDisplacement = 0.0;
				}
			}

			double interval = 0.0;
			if (s_formulaRedrawInterval) {
				SetupLightFormula(e->Light, camera, 0);
				// e->Index is the pool index. Beyond PointLightEnd are converted slots.
				if (e->Index >= s_lights.PointLightEnd(s_settings.ShadowLightCount))
					FormulaHelper::SetParam(kFormulaParam_LightConverted, 1.0);

				// Exposed as `lightdisplacement` so the formula can prioritise fast-moving
				// lights without relying on distance-to-camera alone.
				FormulaHelper::SetParam(kFormulaParam_LightDisplacement, formulaDisplacement);

				// NaN/inf reaches std::sort via RedrawScore otherwise -- UB (see
				// the budget formula above for the same sanitize pattern).
				const double v = s_formulaRedrawInterval->Calculate();
				interval = std::isfinite(v) ? v : 0.0;
			}
			interval += 1.0;
			// The formula's displacement tail can pull interval negative; the
			// importance multiply below only sorts correctly for interval >= 0,
			// else a negative value scaled down inverts priority.
			interval = std::max(interval, 0.0);

			// Contribution-weighted: interval *= kMaxMult *
			// (kMinMult/kMaxMult)^scorePercentile. Top-percentile lights
			// get x(kMinMult/kMaxMult), bottom-percentile get x1.

			float importance = 0.0f;
			float sizeProxy = 0.0f;

			if (auto* ni = e->Light->light.get()) {
				const auto geom = ComputeLightGeometry(e->Light, camera, ni->GetLightRuntimeData().radius.x);
				// Legacy contribution metric, kept for the UI table and
				// s_highImportanceLightCount; ranking decisions use lastScore
				// (the ScoreFormula value) so one function owns priority.
				importance = geom.lum * std::max(geom.coverage, std::max(geom.attCam, geom.attPlr) * 0.3f);
				sizeProxy = geom.sizeProxy;
				// Unreachable from camera or player: cap its tile class so
				// out-of-range lights stop hoarding tiles the rank budget
				// then can't give to visible lights.
				if (geom.attCam <= 0.0f && geom.attPlr <= 0.0f)
					sizeProxy = std::min(sizeProxy, 0.25f);

				// Q2 headroom bands. Sub-tap must not be lumped into
				// "occluded": a light illuminating only thin geometry can
				// read zero for many samples (one tap per 64x64 tile)
				// while being plainly visible.
				const int32_t demandSlot = auditDemand && DemandSampleUsable() ? DemandSlotFor(*e) : -1;
				if (demandSlot >= 0) {
					s_schedDiag.demand_slotted++;
					if (s_shadowDemand.maxLatest[demandSlot] == 0) {
						s_schedDiag.demand_zero++;
						if (s_shadowDemand.tileCount > 0 &&
							geom.screenArea * static_cast<float>(s_shadowDemand.tileCount) < 1.0f)
							s_schedDiag.demand_sub_tap++;
					}
				}
			}

			// Exponential interval scaling driven by the light's priority
			// PERCENTILE among currently slotted lights (scale-invariant
			// to user formula rescaling): maxScale*(minScale/maxScale)^pct.
			float kMaxMult = s_settings.ImportanceMaxScale;
			float kMinMult = std::min(s_settings.ImportanceMinScale, kMaxMult);
			float clampedImp = ScorePercentile(scoreRank, e->lastScore);
			interval *= static_cast<double>(kMaxMult * powf(kMinMult / kMaxMult, clampedImp));

			e->RedrawScore = e->LastDrawnFrame + interval;
			e->lastImportance = importance;

			e->desiredScale = AtlasActive() ?
			                      TileScaleForCoverage(sizeProxy, baseTileTexels, e->desiredScale) :
			                      1.0f;
			// Atlas mode: the render-pass rank budget owns pendingScale.
			if (!AtlasActive())
				e->pendingScale = e->desiredScale;

			// Position step scaled to the tile class: at 128px a fire's flicker orbit
			// is sub-texel and must not bust the cache; at full class it should.
			float posStep = 1.0f;
			if (auto* ni2 = e->Light->light.get()) {
				const float texels = baseTileTexels * std::max(e->pendingScale, kTileScaleFloor);
				posStep = ni2->GetLightRuntimeData().radius.x / std::max(texels, 1.0f);
			}
			// ComputeShadowGeomHash's full geomList walk measured ~17us/light; reuse
			// the cached hash until the caster count changes or this many frames pass.
			constexpr int32_t kGeomHashRehashInterval = 4;
			const auto geomSize = static_cast<std::uint32_t>(e->Light->geomList.size());
			const bool dueForRehash = e->lastHashComputeFrame < 0 ||
			                          geomSize != e->lastHashGeomListSize ||
			                          (now - e->lastHashComputeFrame) >= kGeomHashRehashInterval;
			if (dueForRehash) {
				e->cachedPendingGeomHash = ComputeShadowGeomHash(e->Light, posStep);
				e->lastHashComputeFrame = now;
				e->lastHashGeomListSize = geomSize;
			}
			// Player proxy folded in every frame, uncached -- see
			// FoldPlayerCasterProxy's comment for why it can't ride the
			// walk-cost cache above.
			e->pendingGeomHash = FoldPlayerCasterProxy(e->cachedPendingGeomHash, e->Light->light.get(), posStep);

			// 0 (visible/unmeasured) unless a real GPU reading confirms absence. MAX
			// channel + streak, not an EMA -- an EMA's magnitude scales with tile
			// count, so no fixed threshold would be resolution-stable.
			float occlusionConfidence = 0.0f;
			if (DemandSampleUsable() && e->LastDrawnFrame >= 0) {
				const int32_t demandSlot = DemandSlotFor(*e);
				if (demandSlot >= 0) {
					// Debug-only cross-check that the demand slot and atlas slot agree.
					assert(e->Index == demandSlot && e->Index == GetShadowSlot(e->Light));
					if (s_shadowDemand.maxLatest[demandSlot] <= kDemandUntouchedMaxRaw) {
						const float fullStreak = static_cast<float>(
							std::max(1u, EffectiveZeroDemandStreak() / kOccludedStretchStreakDivisor));
						occlusionConfidence = std::clamp(
							static_cast<float>(e->untouchedSamples) / fullStreak, 0.0f, 1.0f);
					}
				}
			}
			// Floored at 1: fed into std::clamp below as `hi` against `lo`=1.0, so a
			// sub-1 setting (devbench JSON bypasses the UI's 4-60 range) would invert lo/hi (UB).
			const double redrawIntervalMaxFrames = std::max(1.0, static_cast<double>(s_settings.RedrawIntervalMaxFrames));
			const double occludedCeilingFrames = redrawIntervalMaxFrames * kOccludedRedrawMultiplier;

			// Eligibility is a FILTER ahead of RedrawScore ranking, so "not due yet"
			// and "provably unchanged" can't be conflated; staggered age fallback backstops hash false negatives.
			const double backstopBaseFrames = static_cast<double>(kSleepRedrawIntervalFrames) +
			                                  static_cast<double>(occlusionConfidence) *
			                                      (occludedCeilingFrames - static_cast<double>(kSleepRedrawIntervalFrames));
			// Modulo by the window itself, not a fixed `index % 7`, which left same-phase
			// lights re-triggering as one batch. Capped to 60% so no index shrinks it to ~1 frame.
			const int32_t backstopWindowFrames = std::max(static_cast<int32_t>(backstopBaseFrames), 1);
			const int32_t backstopSpreadCap = std::max(1, static_cast<int32_t>(backstopWindowFrames * 0.6));
			const int32_t staggeredBackstopWindow =
				backstopWindowFrames - ((e->Index * kSleepStaggerStride) % backstopSpreadCap);
			const double displacementTexels =
				formulaDisplacement / static_cast<double>(std::max(posStep, 1e-4f));
			AtlasTileTexels schedDirtyTile{};
			const bool tileInvalid = GetSlotTileTexels(e->Index, schedDirtyTile) && !schedDirtyTile.contentValid;
			e->schedDirty = e->LastDrawnFrame < 0 ||
			                e->lastGeomHash == 0 ||
			                e->pendingGeomHash != e->lastGeomHash ||
			                e->pendingScale != e->renderedScale ||
			                displacementTexels >= 1.0 ||
			                tileInvalid ||
			                (now - e->LastDrawnFrame) >= staggeredBackstopWindow;

			// VSM-style demand tiebreaker: must stay in RedrawScore's native frame-count
			// scale, or a too-large term becomes the sort key instead of a tiebreaker.
			if (occlusionConfidence > 0.0f) {
				const double demandHeadroomFrames = occludedCeilingFrames - redrawIntervalMaxFrames;
				e->RedrawScore += static_cast<double>(occlusionConfidence) * demandHeadroomFrames;
			}

			// Bound total delay so a light returning from a long skip can't monopolize
			// admission via an ancient deadline. Floor of 1 closes a tie-window where displacement is exactly 0.
			const double effectiveMaxFrames = redrawIntervalMaxFrames +
			                                  static_cast<double>(occlusionConfidence) *
			                                      (occludedCeilingFrames - redrawIntervalMaxFrames);
			const double effectiveDelay = std::clamp(e->RedrawScore - e->LastDrawnFrame, 1.0, effectiveMaxFrames);
			e->RedrawScore = e->LastDrawnFrame + effectiveDelay;
			// Backlog clamp: bound by occludedCeilingFrames (worst delay ANY light can
			// accumulate), not this light's effectiveMaxFrames -- else confidence
			// collapsing would clip legitimately-earned age.
			const double maxBacklogFrames = occludedCeilingFrames + static_cast<double>(s_settings.ShadowLightCount);
			e->RedrawScore = std::max(e->RedrawScore, static_cast<double>(now) - maxBacklogFrames);

			// Desync a tied batch: similarly-idle lights can share byte-identical
			// RedrawScore and re-sync every cycle (reads as flicker). Bounded to THIS
			// light's own delay, never the shared ceiling, so it can't invert priority.
			const int32_t staggerCapFrames = std::max(1, static_cast<int32_t>(effectiveDelay * 0.5));
			const int32_t deadlineStagger = (e->Index * 41) % staggerCapFrames;
			e->RedrawScore -= static_cast<double>(deadlineStagger);

			// Demanded redraws/frame vs actual admissions/frame distinguishes a tuning
			// problem (demand within capacity) from genuine overload.
			s_schedDiag.demand_ratio += 1.0 / effectiveDelay;
		}
	}

	/// Records a redraw admission, starting the fade-in ramp only for a light that
	/// has never rendered shadow content. LightEntry::Clear() and atlas tile
	/// eviction both reset LastDrawnFrame on lights that were already correctly
	/// shadowed; re-fading those blends a valid shadow back toward fully lit.
	static void AdmitRedraw(LightEntry* e, int32_t now)
	{
		if (e->LastDrawnFrame < 0) {
			const auto* ni = e->Light ? e->Light->light.get() : nullptr;
			PruneIfOversized(s_everHadShadow, 1024);
			if (!ni || s_everHadShadow.insert(ni).second)
				e->FadeStartSeconds = Util::GetNowSecs();
		}
		e->LastDrawnFrame = now;
	}

	/// Sorts `pending` by admission priority and admits candidates until
	/// maxRedraw/budgetRemain runs out. Also outputs entry state so the Q2
	/// counterfactual replay can retrace the identical algorithm and ordering.
	static void SortAndAdmitPendingLights(std::vector<LightEntry*>& pending, int32_t now, int& maxRedraw,
		int32_t& budgetRemain, bool& isFirst, int& cfMaxRedrawStart, int32_t& cfBudgetStart, bool& cfIsFirstStart,
		bool& anyCostRejected, uint32_t& starvationFloorFrames)
	{
		// Count lights meaningfully illuminating the viewer area.
		s_highImportanceLightCount = static_cast<uint32_t>(
			std::count_if(pending.begin(), pending.end(),
				[](const LightEntry* e) { return e->lastImportance > 0.1f; }));

		// Dirty before clean, RedrawScore ascending. Starvation escape: a light
		// pinned at confidence=1 never redraws to correct it, so a stall past
		// starvationFloorFrames forces it through the isFirst floor.
		starvationFloorFrames = static_cast<uint32_t>(
			std::max(1.0, static_cast<double>(s_settings.RedrawIntervalMaxFrames)) * kOccludedRedrawMultiplier);
		std::sort(pending.begin(), pending.end(), [starvationFloorFrames](const LightEntry* a, const LightEntry* b) {
			return ScheduleOrderLess(starvationFloorFrames, a, b);
		});

		// Winners latch the scoring-pass hash: staleness is bounded by
		// kGeomHashRehashInterval, inside the kSleepRedrawIntervalFrames backstop.
		auto latchGeomHash = [](LightEntry* e) {
			e->lastGeomHash = e->pendingGeomHash;
		};

		// Entry state so the Q2 counterfactual can replay identically on untouched copies.
		cfMaxRedrawStart = maxRedraw;
		cfBudgetStart = budgetRemain;
		cfIsFirstStart = isFirst;
		// A candidate refused purely for cost is still saturation: the loop doesn't
		// break on it (a cheaper entry may fit), so maxRedraw/budgetRemain alone can misread "not saturated".
		anyCostRejected = false;

		for (auto* e : pending) {
			if (maxRedraw <= 0)
				break;
			if (budgetRemain <= 0)
				break;
			// Due-gate (RedrawDueGateEnabled, default on): without it the budget is
			// spent unconditionally every frame. isFirst stays exempt to match its
			// unconditional admission below.
			if (s_shadowDemand.redrawDueGate && !isFirst) {
				if (!e->schedDirty)
					break;
				if (e->RedrawScore > static_cast<double>(now))
					break;
			}
			int32_t budgetEstimate = s_budget.GetCost(e->Light);
			if (isFirst) {
				if (!s_lights.Sun || e->Index > 0)
					budgetRemain -= budgetEstimate;
				maxRedraw--;
				e->RedrawFrame = true;
				AdmitRedraw(e, now);
				latchGeomHash(e);
				isFirst = false;
				continue;
			}
			if (budgetEstimate <= budgetRemain) {
				budgetRemain -= budgetEstimate;
				maxRedraw--;
				e->RedrawFrame = true;
				AdmitRedraw(e, now);
				latchGeomHash(e);
				continue;
			}
			anyCostRejected = true;
		}
	}

	/// Replays the identical admission algorithm on a counterfactual pool (scratch
	/// state only) to measure how many redraws the demand skip buys or costs.
	static void AuditDemandCounterfactual(bool auditDemand, const std::vector<LightEntry*>& pending,
		const std::vector<HighPrioritySkip>& highPrioritySkips, int cfMaxRedrawStart, int32_t cfBudgetStart,
		bool cfIsFirstStart, uint32_t starvationFloorFrames, int maxRedraw, int32_t budgetRemain,
		bool anyCostRejected)
	{
		// Q2: replay the identical admission algorithm on a counterfactual pool and
		// diff against what really admitted. Pool flips with the setting: skip OFF
		// replay REMOVES candidates from `pending`; skip ON replay ADDS them back.
		if (auditDemand) {
			int realAdmitted = 0;
			for (auto* e : pending)
				if (e->RedrawFrame)
					realAdmitted++;

			if (!s_settings.SkipZeroDemandRedraw) {
				int cfMaxRedraw = cfMaxRedrawStart;
				int32_t cfBudget = cfBudgetStart;
				bool cfIsFirst = cfIsFirstStart;
				int cfAdmitted = 0;

				auto admit = [&](LightEntry* e) {
					cfAdmitted++;
					if (e->RedrawFrame)
						return;
					// Admitted only in the counterfactual: the lights the skip would buy.
					// Their demand decides real quality win vs another invisible light taking the slot.
					s_schedDiag.demand_swap_in++;
					const int32_t slot = DemandSlotFor(*e);
					if (slot < 0 || s_shadowDemand.maxLatest[slot] > kDemandUntouchedMaxRaw)
						s_schedDiag.demand_swap_in_above_eps++;
				};

				for (auto* e : pending) {
					if (DemandSkipCandidate(*e))
						continue;
					if (cfMaxRedraw <= 0 || cfBudget <= 0)
						break;
					const int32_t cost = s_budget.GetCost(e->Light);
					if (cfIsFirst) {
						if (!s_lights.Sun || e->Index > 0)
							cfBudget -= cost;
						cfMaxRedraw--;
						cfIsFirst = false;
						admit(e);
						continue;
					}
					if (cost <= cfBudget) {
						cfBudget -= cost;
						cfMaxRedraw--;
						admit(e);
					}
				}
				// Positive only when the candidate set drops below budget; saturated
				// regime just reallocates budget, staying zero by design.
				s_schedDiag.demand_redraws_saved = realAdmitted - cfAdmitted;
				s_demandSwapInTotal.fetch_add(
					static_cast<uint64_t>(s_schedDiag.demand_swap_in), std::memory_order_relaxed);
			} else {
				int cfMaxRedraw = cfMaxRedrawStart;
				int32_t cfBudget = cfBudgetStart;
				bool cfIsFirst = cfIsFirstStart;
				int cfAdmittedUnion = 0;

				// pending already excludes the real skips; merge them back in by
				// RedrawScore so the replay sees the rank order without the skip.
				static std::vector<LightEntry*> s_cfUnion;
				s_cfUnion.clear();
				s_cfUnion.reserve(pending.size() + highPrioritySkips.size());
				s_cfUnion.insert(s_cfUnion.end(), pending.begin(), pending.end());
				for (const auto& skip : highPrioritySkips)
					s_cfUnion.push_back(skip.entry);
				// Same ordering the real sort above uses -- a mismatched comparator
				// here would desync this counterfactual from the live scheduler.
				std::sort(s_cfUnion.begin(), s_cfUnion.end(), [starvationFloorFrames](const LightEntry* a, const LightEntry* b) {
					return ScheduleOrderLess(starvationFloorFrames, a, b);
				});

				for (auto* e : s_cfUnion) {
					if (cfMaxRedraw <= 0 || cfBudget <= 0)
						break;
					const int32_t cost = s_budget.GetCost(e->Light);
					if (cfIsFirst) {
						if (!s_lights.Sun || e->Index > 0)
							cfBudget -= cost;
						cfMaxRedraw--;
						cfIsFirst = false;
						cfAdmittedUnion++;
						continue;
					}
					if (cost <= cfBudget) {
						cfBudget -= cost;
						cfMaxRedraw--;
						cfAdmittedUnion++;
					}
				}
				// Positive = redraws the live skip actually prevented this frame (union
				// replay admits more than really ran); zero in the saturated regime.
				s_schedDiag.demand_redraws_saved = cfAdmittedUnion - realAdmitted;
			}
			if (s_schedDiag.demand_redraws_saved > 0)
				s_demandRedrawsSavedTotal.fetch_add(
					static_cast<uint64_t>(s_schedDiag.demand_redraws_saved), std::memory_order_relaxed);
		}
		// anyCostRejected counts as saturated even when maxRedraw/
		// budgetRemain didn't bottom out -- see the loop above.
		if (auditDemand) {
			s_schedDiag.demand_budget_saturated = maxRedraw <= 0 || budgetRemain <= 0 || anyCostRejected;
			s_demandSkipEligibleTotal.fetch_add(
				static_cast<uint64_t>(s_schedDiag.demand_skip_eligible), std::memory_order_relaxed);
		}
	}

	static void RunShadowRedrawScheduleLoop(RE::NiCamera* camera, double budget)
	{
		ZoneScopedN("SCM::ScheduleLoop");
		int maxRedraw = std::min(s_settings.MaxRedrawPerFrame, s_lights.Size);
		int32_t budgetRemain = static_cast<int32_t>(budget * 1000.0);
		bool isFirst = true;
		int32_t now = *globals::game::frameCounter;

		// Maintains the per-slot consecutive-absence streak, consumed by the audit's
		// Q2 counterfactual, the real hard skip, and occlusionConfidence further down.
		const bool auditDemand = s_shadowDemand.instrumentation;
		AdvanceDemandStreaks();

		// Eligible population, counted over the whole pool so the ceiling metric means
		// the same thing whether or not the skip is live.
		if (auditDemand)
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light && DemandSkipCandidate(s_lights.Lights[i]))
					s_schedDiag.demand_skip_eligible++;

		// Clear RedrawFrame on slots OUTSIDE the point-light range (converted).
		// PointLightEnd accounts for the sun bookkeeping slot when Sun=true.
		for (int i = s_lights.PointLightEnd(s_settings.ShadowLightCount); i < s_lights.Size; i++)
			s_lights.Lights[i].RedrawFrame = false;

		// First pass: sun only. Point-light slots fall through to the importance-scored
		// pending loop so new lights compete fairly with existing redraws.
		for (int i = 0; i < s_lights.Size; i++) {
			auto& e = s_lights.Lights[i];
			if (!e.Light) {
				e.RedrawFrame = false;
				continue;
			}
			e.RedrawFrame = (i == 0 && s_lights.Sun);
			if (e.RedrawFrame) {
				e.LastDrawnFrame = now;
				maxRedraw--;
				// Sun's budget cost is bookkept at 0, so no budgetRemain decrement.
				// isFirst deliberately survives the sun -- it's the point lights'
				// starvation guarantee, which the sun used to consume.
			}
		}

		if (maxRedraw > 0 && budgetRemain > 0) {
			std::vector<LightEntry*> pending;
			// A demand-skipped light whose lastScore ranks above the live pool's
			// median contradicts the demand system's verdict; logged once per episode.
			static std::vector<HighPrioritySkip> highPrioritySkips;
			highPrioritySkips.clear();
			// Sleep-skipped entries never reach `pending` either, so without their own
			// desiredScale refresh a skipped light can go small/distant/occluded and
			// keep outranking visible lights on stale geometry.
			static std::vector<LightEntry*> sleepSkips;
			sleepSkips.clear();
			// Same reasoning applies to held and not-yet-admitted lights.
			static std::vector<LightEntry*> cameraHoldSkips;
			cameraHoldSkips.clear();
			static std::vector<LightEntry*> newLightSkips;
			newLightSkips.clear();
			ClassifyPendingLights(now, pending, highPrioritySkips, sleepSkips, cameraHoldSkips, newLightSkips);

			// Base texels for the coverage classifier; lazily captured from the live
			// texture, so fall back until it is readable.
			const float baseTileTexels = s_initialShadowMapResolution > 0 ?
			                                 static_cast<float>(s_initialShadowMapResolution) :
			                                 2048.0f;

			// Priority percentile across the live pool: 1.0 = highest score.
			// Rank, not raw value, so the redraw curve is invariant to the
			// user formula's scale.
			static std::vector<double> scoreRank;
			scoreRank.clear();
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light)
					scoreRank.push_back(s_lights.Lights[i].lastScore);
			std::sort(scoreRank.begin(), scoreRank.end());

			LogAndRefreshSkippedLights(camera, baseTileTexels, scoreRank, highPrioritySkips, sleepSkips,
				cameraHoldSkips, newLightSkips);

			ScorePendingLights(camera, now, baseTileTexels, auditDemand, scoreRank, pending);

			int cfMaxRedrawStart = 0;
			int32_t cfBudgetStart = 0;
			bool cfIsFirstStart = false;
			bool anyCostRejected = false;
			uint32_t starvationFloorFrames = 0;
			SortAndAdmitPendingLights(pending, now, maxRedraw, budgetRemain, isFirst, cfMaxRedrawStart,
				cfBudgetStart, cfIsFirstStart, anyCostRejected, starvationFloorFrames);

			AuditDemandCounterfactual(auditDemand, pending, highPrioritySkips, cfMaxRedrawStart, cfBudgetStart,
				cfIsFirstStart, starvationFloorFrames, maxRedraw, budgetRemain, anyCostRejected);
		}
	}

	static void TrackRedrawStallStats()
	{
		// Deliberately outside the maxRedraw>0/budgetRemain>0 guard above: a
		// frame where the scheduler bails entirely is the worst case for a
		// stall, and it must still be counted.
		s_redrawnLightsThisFrame = 0;
		s_schedDiag.stall_max = 0;
		s_schedDiag.stall_over_threshold = 0;
		s_schedDiag.stall_worst_slot = -1;
		for (int j = s_lights.PointLightFirst(); j < s_lights.PointLightEnd(s_settings.ShadowLightCount); j++) {
			auto& e = s_lights.Lights[j];
			if (e.RedrawFrame)
				++s_redrawnLightsThisFrame;
			// Reset on admission or going clean; frozen while skippedThisFrame.
			if (!e.Light || e.RedrawFrame || !e.schedDirty)
				e.dirtyStallFrames = 0;
			else if (!e.skippedThisFrame && e.dirtyStallFrames < 0xFFFFu)
				++e.dirtyStallFrames;
			if (static_cast<int>(e.dirtyStallFrames) > s_schedDiag.stall_max) {
				s_schedDiag.stall_max = e.dirtyStallFrames;
				s_schedDiag.stall_worst_slot = j;
			}
			if (e.dirtyStallFrames >= kStallReportThreshold)
				s_schedDiag.stall_over_threshold++;
		}

		// EWMA so the UI counter doesn't flicker frame-to-frame.
		s_redrawnLightsSmoothed = 0.8f * s_redrawnLightsSmoothed + 0.2f * s_redrawnLightsThisFrame;
	}

	// Paired with IsLightAliveNow as the two-stage validity check before any
	// virtual dispatch: still alive, and vtable non-zero (freed-and-zeroed via
	// a path that bypassed ref-counting).
	static bool IsVtableValid(RE::BSShadowLight* l)
	{
		return l && *reinterpret_cast<const uintptr_t*>(l) != 0;
	}

	// Still in activeShadowLights? ClearLightArrays (+ ResetSession/skip) keeps us
	// from scanning a torn-down array; SafeUsable (SEH) is the remaining backstop.
	static bool IsLightAliveNow(RE::ShadowSceneNode::RUNTIME_DATA* shadowSceneNodeRT, RE::BSShadowLight* sunLight, RE::BSShadowLight* l)
	{
		if (!l)
			return false;
		if (l == sunLight)
			return true;
		for (auto& sp : shadowSceneNodeRT->activeShadowLights)
			if (sp.get() == l)
				return true;
		return false;
	}

	static bool RunAtomicMutationLoop(RE::ShadowSceneNode* ssn, RE::NiCamera* camera,
		RE::BSShadowLight* sunLight, std::vector<CandidateLight>& candidates, int& doneLightCount)
	{
		auto* shadowSceneNodeRT = &ssn->GetRuntimeData();

		// See IsLightAliveNow/IsVtableValid for the two-stage validity contract.
		auto isAliveNow = [shadowSceneNodeRT, sunLight](RE::BSShadowLight* l) -> bool {
			return IsLightAliveNow(shadowSceneNodeRT, sunLight, l);
		};
		auto isVtableValid = [](RE::BSShadowLight* l) -> bool {
			return IsVtableValid(l);
		};
		auto isUsableLight = [&](RE::BSShadowLight* l) -> bool {
			return isAliveNow(l) && isVtableValid(l);
		};

		auto findSlotForLight = [](RE::BSShadowLight* l) -> int {
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light == l)
					return i;
			return -1;
		};

		// Convert (keeps diffuse via cluster) or Disable (vanishes)? Spots always
		// Disable -- ConvertLight would make the cone spherical and bleed through walls.
		auto convertOrDisable = [&](RE::BSShadowLight* light, bool allowConvert) -> bool {
			const bool isSpot = light->GetIsFrustumLight();
			const bool forceConvert = s_pinConvert.count(reinterpret_cast<uintptr_t>(light)) > 0;
			if (allowConvert && (s_settings.ConvertExcessToNormal || forceConvert) && !isSpot) {
				ConvertLight(light, ssn, false);
				return true;
			}
			DisableLight(light);
			return false;
		};

		// Sun slot processed inline: setup already happened; just mark its mask index.
		if (s_lights.Sun && s_lights.Lights[0].Light && s_lights.Lights[0].RedrawFrame) {
			ShadowField(s_lights.Lights[0].Light, maskIndex) = 0;
			doneLightCount++;
		}

		// Per-candidate Begin/EnableLight/End mutation loop. EnableLight may trigger
		// synchronous shadow render dispatches, so this zone captures both scheduler
		// and engine-side rendering work for chosen lights.
		{
			// Hold the graph lock shared for the whole loop so ResetScene can't null
			// ssn->portalGraph while ConvertLight/DisableLight/EnableLight deref it.
			// try_to_lock: skip the pass if a teardown holds it exclusive, rather than block.
			std::shared_lock<std::shared_mutex> graphLock(s_portalGraphMutex, std::try_to_lock);
			if (!graphLock.owns_lock())
				return false;
			if (IsPortalGraphTransitioning())  // re-check once; stable now under the lock
				return false;
			ZoneNamedN(zoneAtomic, "SCM::AtomicMutationLoop", true);
			for (auto& c : candidates) {
				if (c.invalid) {
					// isUsableLight is the same gate the excess branch uses. Both
					// ConvertLight and DisableLight fan into virtually-dispatched
					// callees (ReturnShadowmaps), so a freed pointer must be skipped either way.
					if (!isUsableLight(c.light))
						continue;

					// allowConvert=c.invalidCamera so portal-occluded omnis fall to
					// Disable (cluster lighting has no portal-graph awareness).
					ZoneNamedN(zCvt, "SCM::Engine::convertOrDisable(invalid)", true);
					if (convertOrDisable(c.light, /*allowConvert=*/c.invalidCamera)) {
						s_schedDiag.converted_invalid++;
						// Re-validate first: a concurrent free could recycle c.light, and a
						// lodDimmer store would corrupt the new occupant's vtable -> CTD.
						if (SafeUsable(isUsableLight, c.light))
							RestoreZeroedLodDimmer(c.light);
					} else {
						s_schedDiag.disabled_invalid++;
					}
					continue;
				}

				if (c.chosen) {
					int slot = findSlotForLight(c.light);
					if (slot < 0)
						continue;  // matches old behaviour: chosen-but-no-slot is a no-op
					if (slot == 0 && s_lights.Sun)
						continue;  // sun handled above

					auto& e = s_lights.Lights[slot];

					// Render-this-frame path is reserved for chosen point-light slots
					// (excludes converted slots). Sun-aware bound includes pool[ShadowLightCount].
					if (e.RedrawFrame && slot < s_lights.PointLightEnd(s_settings.ShadowLightCount)) {
						// A previous iteration's EnableLight may have transitively freed
						// this light via game-side scene mutations, so re-validate first.
						if (!SafeUsable(isUsableLight, e.Light)) {
							e.Light = nullptr;
							continue;
						}

						auto* lightSnapshot = e.Light;  // value snapshot for budget pairing
						s_budget.BeginLight(lightSnapshot, 0);
						// EnableLight can null e.Light or free the BSShadowLight mid-call (then read
						// shadowMapCount from it) -> AV. SafeEnableAndValidate catches it (noinline SEH).
						bool stillUsable;
						{
							ZoneNamedN(zEnable, "SCM::Engine::EnableLight", true);
							s_cpuEnableUs.fetch_add(TimeUs([&] { stillUsable = SafeEnableAndValidate(e, camera, ssn, slot, isUsableLight); }), std::memory_order_relaxed);
							s_cpuEnableN.fetch_add(1, std::memory_order_relaxed);
						}
						if (!stillUsable)
							continue;
						s_budget.EndLight(lightSnapshot, 0);

						if (auto* nilight = e.Light->light.get())
							e.lastRenderedPos = nilight->world.translate;

						ShadowField(e.Light, maskIndex) = static_cast<uint32_t>(slot);
						doneLightCount++;
					}
					// Cached-shadow path (chosen + !RedrawFrame): do nothing here. The
					// non-redrawn light keeps its stale shadow map, re-inserted by the
					// GameSetShadowCasterSlot loop below. DisableLight here would invoke
					// ReturnShadowmaps, releasing cached data and producing visible flicker.
					continue;
				}

				if (c.excess) {
					if (!isUsableLight(c.light))
						continue;

					// All chosen lights have completed Begin/EnableLight/End by now, so
					// ReturnShadowmaps can only invalidate pointers we're no longer walking.
					ZoneNamedN(zCvt, "SCM::Engine::convertOrDisable(excess)", true);
					if (convertOrDisable(c.light, /*allowConvert=*/true))
						s_schedDiag.converted_excess++;
					else
						s_schedDiag.disabled_excess++;
					continue;
				}
			}
		}  // end SCM::AtomicMutationLoop
		return true;
	}

	static void ReinsertNonRedrawnLights(RE::ShadowSceneNode* ssn, RE::BSShadowLight* sunLight, const RE::NiCamera* camera)
	{
		auto* shadowSceneNodeRT = &ssn->GetRuntimeData();
		// See IsLightAliveNow/IsVtableValid for the two-stage validity contract.
		auto isAliveNow = [shadowSceneNodeRT, sunLight](RE::BSShadowLight* l) -> bool {
			return IsLightAliveNow(shadowSceneNodeRT, sunLight, l);
		};
		auto isVtableValid = [](RE::BSShadowLight* l) -> bool {
			return IsVtableValid(l);
		};
		auto isUsableLight = [&](RE::BSShadowLight* l) -> bool {
			return isAliveNow(l) && isVtableValid(l);
		};

		// Non-redrawn chosen lights: insert at end of shadow caster array without
		// rendering. Re-rebuild the alive set: the atomic loop above may have
		// invalidated pointers (e.g. ConvertLight on excess), so skip entries no
		// longer in the scene to avoid dereferencing freed memory below.
		{
			ZoneNamedN(zonePostAtomic, "SCM::PostAtomicRevalidate", true);
			std::unordered_set<RE::BSShadowLight*> aliveAfterAtomic;
			{
				auto& alive = ssn->GetRuntimeData().activeShadowLights;
				aliveAfterAtomic.reserve(alive.size() + 1);
				if (sunLight)
					aliveAfterAtomic.insert(sunLight);
				for (auto& sp : alive)
					if (auto* l = sp.get())
						aliveAfterAtomic.insert(l);
			}

			int endIdx = (int)*GetAccumLightSlot();

			for (int i = 0; i < s_lights.Size; i++) {
				auto& e = s_lights.Lights[i];
				// Re-insert (without rendering) every chosen+!RedrawFrame light AND every
				// converted-slot light. PointLightEnd bound is sun-aware.
				if (e.Light && (!e.RedrawFrame || i >= s_lights.PointLightEnd(s_settings.ShadowLightCount))) {
					// Membership check uses the snapshot built above (aliveAfterAtomic
					// captures scene state for O(1) queries, since the atomic loop may have invalidated pointers).
					if (aliveAfterAtomic.find(e.Light) == aliveAfterAtomic.end()) {
						s_schedDiag.reconciliation_clears++;
						e.Clear();
						continue;
					}
					// A chosen light with LastDrawnFrame < 0 has no valid content in its
					// kSHADOWMAPS slice yet; skip it (still illuminates via cluster).
					if (i < s_lights.PointLightEnd(s_settings.ShadowLightCount) &&
						e.LastDrawnFrame < 0 &&
						!(s_lights.Sun && i == 0)) {
						s_schedDiag.first_render_skips++;
						continue;
					}

					// Sample the cached slice even on a geometry hash mismatch -- a 1-2
					// frame lag beats hash-gated flicker; the mismatch priority hint keeps
					// stale entries at the queue front so it self-corrects. GameSetShadowCasterSlot
					// calls Accumulate virtually; reuse isUsableLight's vtable guard.
					if (!isVtableValid(e.Light)) {
						e.Light = nullptr;
						continue;
					}
					GameSetShadowCasterSlot(ssn, e.Light, endIdx, 1);
					// Same hazard as the post-EnableLight site: the engine can free the
					// light during this call, so use isUsableLight, not just a null check.
					if (!e.Light || !SafeUsable(isUsableLight, e.Light))
						continue;
					// Mirrors EnableLight's mask bit for a demand-skipped light, or it
					// drops out of firstPersonShadowMask every skip frame (flicker).
					// Scoped like EnableLight: converted (i >= PointLightEnd) entries
					// never earn a bit either.
					if (camera && i < s_lights.PointLightEnd(s_settings.ShadowLightCount) &&
						endIdx < static_cast<int>(kShadowMaskBits) &&
						LightContainsCamera(e.Light->light.get(), camera)) {
						*GetShadowMask() |= 1u << endIdx;
					}
					endIdx += e.Light->shadowMapCount;
					ShadowField(e.Light, maskIndex) = static_cast<uint32_t>(i);

					// GameSetShadowCasterSlot overwrites shadowmapIndex with a sequential
					// counter, diverging from the stable index CopyShadowLightData/Prepass expect. Restore it to i.
					if (s_settings.ShadowLightCount > 4 && i < s_lights.PointLightEnd(s_settings.ShadowLightCount)) {
						// Sun (pool[0]) is skipped: it renders via the directional cascade
						// path, so its shadowmapIndex is unused.
						if (s_lights.Sun && i == 0)
							continue;
						if (globals::game::isVR) {
							for (auto& desc : e.Light->GetVRRuntimeData().shadowmapDescriptors)
								desc.shadowmapIndex = static_cast<uint32_t>(i);
						} else {
							for (auto& desc : e.Light->GetRuntimeData().shadowmapDescriptors)
								desc.shadowmapIndex = static_cast<uint32_t>(i);
						}
					}
				}
			}
		}
	}

	static void FinalizeFrameBookkeeping(RE::ShadowSceneNode* ssn, int doneLightCount)
	{
		// Update rolling redraw and budget statistics.
		{
			int redrawing = 0;
			int32_t consumed = 0;
			for (int i = 0; i < s_lights.Size; i++) {
				auto& e = s_lights.Lights[i];
				if (e.Light && e.RedrawFrame) {
					if (i != 0 || !s_lights.Sun)
						consumed += s_budget.GetCost(e.Light);
					redrawing++;
				}
			}
			s_redrawSum -= s_redrawHistory[s_redrawHistoryPos];
			s_redrawHistory[s_redrawHistoryPos] = redrawing;
			s_redrawSum += redrawing;
			s_redrawHistoryPos = (s_redrawHistoryPos + 1) % kRedrawHistorySize;

			s_budgetSum -= s_budgetHistory[s_budgetHistoryPos];
			s_budgetHistory[s_budgetHistoryPos] = consumed;
			s_budgetSum += consumed;
			s_budgetHistoryPos = (s_budgetHistoryPos + 1) % kRedrawHistorySize;
		}

		ssn->GetRuntimeData().firstPersonShadowMask = *GetShadowMask();
		*GetFrameLightCount() = static_cast<uint32_t>(doneLightCount);
	}

	static void EmitSchedulerDiagnostics(bool wantDiag)
	{
		// Tracy per-frame plots: scheduler counters (scm.*) + live config (cfg.*),
		// emitted together so a capture supports A/B queries without a rerun.
		{
			// Read + reset per-frame counters here, not inside TracyPlot's arg --
			// that expression is elided in non-Tracy builds, which would leak the counter.
			const uint32_t culledThisFrame = s_casterCullCount.exchange(0, std::memory_order_relaxed);
			if (culledThisFrame)
				s_casterCullTotal.fetch_add(culledThisFrame, std::memory_order_relaxed);
			const uint32_t poolDropsThisFrame = s_cullPoolDropCount.exchange(0, std::memory_order_relaxed);
			if (poolDropsThisFrame)
				s_cullPoolDropTotal.fetch_add(poolDropsThisFrame, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t staticDraws = s_staticCasterDraws.exchange(0, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t dynamicDraws = s_dynamicCasterDraws.exchange(0, std::memory_order_relaxed);
			// Accumulated, not just plotted: publishes a running total for headless A/B diffing.
			const uint32_t bakesThisFrame = s_staticBakeCount.exchange(0, std::memory_order_relaxed);
			if (bakesThisFrame)
				s_staticBakeTotal.fetch_add(bakesThisFrame, std::memory_order_relaxed);

			// Sample slot occupancy at frame end (post-reconciliation).
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light)
					s_schedDiag.slots_in_use++;

			// Publish the scheduling snapshot for headless inspection (devbench
			// inspect kind=llfshadows), under the lock so the listener thread reads a
			// consistent snapshot, then consume one dump-request pass.
			if (wantDiag) {
				SchedSnapshot snap;
				snap.valid = true;
				snap.frame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
				snap.total = s_schedDiag.candidates_total;
				snap.chosen = s_schedDiag.candidates_chosen;
				snap.excess = s_schedDiag.candidates_excess;
				snap.invalidCamera = s_schedDiag.candidates_invalid_camera;
				snap.invalidPortal = s_schedDiag.candidates_invalid_portal;
				snap.invalidFrustum = s_schedDiag.candidates_invalid_frustum;
				snap.invalidLod = s_schedDiag.candidates_invalid_lod;
				snap.invalidOther = s_schedDiag.candidates_invalid_other;
				snap.slotsInUse = s_schedDiag.slots_in_use;
				snap.demoted.reserve(s_convertReason.size());
				for (const auto& [ptr, reason] : s_convertReason)
					snap.demoted.emplace_back(ptr, static_cast<uint8_t>(reason));
				const auto& slotInfos = GetSlotInfos();
				for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
					const auto& e = s_lights.Lights[i];
					if (!e.Light)
						continue;
					SchedSnapshot::SlotState st;
					st.index = i;
					st.light = reinterpret_cast<uintptr_t>(e.Light);
					st.importance = e.lastImportance;
					st.score = e.lastScore;
					st.desiredScale = e.desiredScale;
					st.budgetScale = e.budgetScale;
					st.pendingScale = e.pendingScale;
					st.renderedScale = e.renderedScale;
					st.redrawnThisFrame = e.RedrawFrame;
					st.schedDirty = e.schedDirty;
					st.dirtyStallFrames = e.dirtyStallFrames;
					st.redrawScore = e.RedrawScore;
					st.lastDrawnFrame = e.LastDrawnFrame;
					st.cameraHold = s_cameraHold.count(e.Light) > 0;
					st.geomListSize = static_cast<uint32_t>(e.Light->geomList.size());
					AtlasTileTexels tile{};
					if (GetSlotTileTexels(i, tile)) {
						st.tileX = tile.x;
						st.tileY = tile.y;
						st.tileSize = tile.size;
						st.tileContentValid = tile.contentValid;
					}
					{
						uint64_t staticHash = 0;
						GetSlotStaticState(i, staticHash, st.staticValid, &st.staticEmpty);
					}
					if (static_cast<size_t>(i) < slotInfos.size()) {
						const auto& rec = slotInfos[i];
						st.uploadRecorded = rec.valid;
						st.uploadParamY = rec.paramY;
						st.uploadRange = rec.range;
					}
					st.suppressed = IsSuppressed(reinterpret_cast<uintptr_t>(e.Light));
					if (auto* ni = e.Light->light.get()) {
						std::scoped_lock convLock(s_shadowConvertMutex);
						st.promoted = s_shadowConvert.count(ni) > 0;
					}
					snap.slots.push_back(st);
				}
				snap.baseTileTexels = s_initialShadowMapResolution > 0 ?
				                          static_cast<float>(s_initialShadowMapResolution) :
				                          2048.0f;
				snap.atlasDim = AtlasDim();
				snap.atlasCapacityCells = AtlasCapacityCells();
				snap.atlasOccupancy = AtlasOccupancy();
				snap.atlasVramBytes = AtlasVRAMBytes();
				const auto clearStats = GetAtlasClearStats();
				snap.atlasTileReallocs = clearStats.tileReallocs;
				snap.atlasOwnerInvalidations = clearStats.ownerInvalidations;
				snap.atlasAllocDenied = clearStats.allocDenied;
				snap.cullPoolDropsTotal = s_cullPoolDropTotal.load(std::memory_order_relaxed);
				snap.casterCullDropsTotal = s_casterCullTotal.load(std::memory_order_relaxed);
				if (const uint32_t n = s_cpuAccumN.exchange(0, std::memory_order_relaxed))
					snap.cpuAccumUsAvg = static_cast<uint32_t>(s_cpuAccumUs.exchange(0, std::memory_order_relaxed) / n);
				if (const uint32_t n = s_cpuSubmitN.exchange(0, std::memory_order_relaxed))
					snap.cpuSubmitUsAvg = static_cast<uint32_t>(s_cpuSubmitUs.exchange(0, std::memory_order_relaxed) / n);
				if (const uint32_t n = s_cpuEnableN.exchange(0, std::memory_order_relaxed))
					snap.cpuEnableUsAvg = static_cast<uint32_t>(s_cpuEnableUs.exchange(0, std::memory_order_relaxed) / n);
				snap.avgLightCostUs = s_budget.GetAverageCostUs();
				snap.avgRedrawsPerFrame = static_cast<float>(s_redrawSum) / static_cast<float>(kRedrawHistorySize);
				snap.staticBakesTotal = s_staticBakeTotal.load(std::memory_order_relaxed);
				snap.cellResetsTotal = s_cellResetTotal.load(std::memory_order_relaxed);
				snap.sleepSkips = s_schedDiag.sleep_skips;
				snap.sleepSkipsTotal = s_sleepSkipTotal.load(std::memory_order_relaxed);
				snap.demandSkips = s_schedDiag.demand_skips;
				snap.demandSkipsTotal = s_demandSkipTotal.load(std::memory_order_relaxed);
				snap.alphaGroupPeak = s_alphaGroupPeak.load(std::memory_order_relaxed);
				snap.alphaGroupDrops = s_alphaGroupDrops.load(std::memory_order_relaxed);
				snap.frustumAuditCandidates = s_schedDiag.frustum_audit_candidates;
				snap.frustumAuditKeptOut = s_schedDiag.frustum_audit_kept_out;
				snap.frustumAuditSuspects = s_schedDiag.frustum_audit_suspects;
				snap.demandSlotted = s_schedDiag.demand_slotted;
				snap.demandZero = s_schedDiag.demand_zero;
				snap.demandSubTap = s_schedDiag.demand_sub_tap;
				snap.demandSkipEligible = s_schedDiag.demand_skip_eligible;
				snap.demandSwapIn = s_schedDiag.demand_swap_in;
				snap.demandSwapInAboveEps = s_schedDiag.demand_swap_in_above_eps;
				snap.demandRedrawsSaved = s_schedDiag.demand_redraws_saved;
				snap.demandBudgetSaturated = s_schedDiag.demand_budget_saturated;
				// The demand penalty already does part of DemandSkipCandidate's job
				// via the sort, so every Q2 reading is only interpretable alongside
				// the config it was taken under.
				snap.demandPhase1Enabled = true;
				snap.demandSkipActive = s_settings.SkipZeroDemandRedraw;
				snap.demandSkipEligibleTotal = s_demandSkipEligibleTotal.load(std::memory_order_relaxed);
				snap.demandSwapInTotal = s_demandSwapInTotal.load(std::memory_order_relaxed);
				snap.demandRedrawsSavedTotal = s_demandRedrawsSavedTotal.load(std::memory_order_relaxed);
				snap.stallMax = s_schedDiag.stall_max;
				snap.stallWorstSlot = s_schedDiag.stall_worst_slot;
				snap.demandRatio = s_schedDiag.demand_ratio;
				{
					std::scoped_lock lock(s_schedSnapshotMutex);
					s_schedSnapshot = std::move(snap);
				}
				if (s_schedDumpFrames.load(std::memory_order_relaxed) > 0)
					s_schedDumpFrames.fetch_sub(1, std::memory_order_relaxed);
			}

			TracyPlot("scm.candidates.total", (int64_t)s_schedDiag.candidates_total);
			TracyPlot("scm.candidates.chosen", (int64_t)s_schedDiag.candidates_chosen);
			TracyPlot("scm.candidates.excess", (int64_t)s_schedDiag.candidates_excess);
			TracyPlot("scm.candidates.invalid_camera", (int64_t)s_schedDiag.candidates_invalid_camera);
			TracyPlot("scm.candidates.invalid_portal", (int64_t)s_schedDiag.candidates_invalid_portal);
			TracyPlot("scm.candidates.invalid_frustum", (int64_t)s_schedDiag.candidates_invalid_frustum);
			TracyPlot("scm.candidates.invalid_lod", (int64_t)s_schedDiag.candidates_invalid_lod);
			TracyPlot("scm.candidates.invalid_other", (int64_t)s_schedDiag.candidates_invalid_other);
			TracyPlot("scm.converted.invalid", (int64_t)s_schedDiag.converted_invalid);
			TracyPlot("scm.converted.excess", (int64_t)s_schedDiag.converted_excess);
			TracyPlot("scm.disabled.invalid", (int64_t)s_schedDiag.disabled_invalid);
			TracyPlot("scm.disabled.excess", (int64_t)s_schedDiag.disabled_excess);
			TracyPlot("scm.reconciliation.clears", (int64_t)s_schedDiag.reconciliation_clears);
			TracyPlot("scm.slots.in_use", (int64_t)s_schedDiag.slots_in_use);
			TracyPlot("scm.first_render_skips", (int64_t)s_schedDiag.first_render_skips);
			TracyPlot("scm.sleep_skips", (int64_t)s_schedDiag.sleep_skips);
			TracyPlot("scm.demand_skips", (int64_t)s_schedDiag.demand_skips);
			TracyPlot("scm.alpha_groups", (int64_t)s_alphaGroupPeak.load(std::memory_order_relaxed));
			TracyPlot("scm.demand.skip_eligible", (int64_t)s_schedDiag.demand_skip_eligible);
			TracyPlot("scm.demand.swap_in", (int64_t)s_schedDiag.demand_swap_in);
			TracyPlot("scm.demand.swap_in_above_eps", (int64_t)s_schedDiag.demand_swap_in_above_eps);
			TracyPlot("scm.demand.redraws_saved", (int64_t)s_schedDiag.demand_redraws_saved);
			TracyPlot("scm.demand.zero", (int64_t)s_schedDiag.demand_zero);
			TracyPlot("scm.demand.sub_tap", (int64_t)s_schedDiag.demand_sub_tap);
			TracyPlot("scm.frustum_audit.suspects", (int64_t)s_schedDiag.frustum_audit_suspects);
			TracyPlot("scm.redraw.stall_max", (int64_t)s_schedDiag.stall_max);
			TracyPlot("scm.redraw.stall_over", (int64_t)s_schedDiag.stall_over_threshold);
			TracyPlot("scm.redraw.demand_ratio", (double)s_schedDiag.demand_ratio);

			// Live config plots — record the *current* settings on each frame so
			// a single capture spanning a settings change captures both sides.
			TracyPlot("cfg.ShadowLightCount", (int64_t)s_settings.ShadowLightCount);
			TracyPlot("cfg.MaxRedrawPerFrame", (int64_t)s_settings.MaxRedrawPerFrame);
			TracyPlot("cfg.ConvertExcessToNormal", (int64_t)(s_settings.ConvertExcessToNormal ? 1 : 0));
			TracyPlot("cfg.Enabled", (int64_t)(s_settings.Enabled ? 1 : 0));
			TracyPlot("cfg.RedrawBudgetMs", (double)s_settings.RedrawBudgetMs);
			TracyPlot("cfg.CasterCullAngularMin", (double)s_settings.CasterCullAngularMin);
			TracyPlot("scm.casters_culled", (int64_t)culledThisFrame);
			TracyPlot("scm.cull_pool_drops", (int64_t)poolDropsThisFrame);
			TracyPlot("scm.casters_static", (int64_t)staticDraws);
			TracyPlot("scm.casters_dynamic", (int64_t)dynamicDraws);
			TracyPlot("scm.static_bakes", (int64_t)bakesThisFrame);

			if (s_shadowDemand.instrumentation)
				EmitDemandAuditLog(*globals::game::frameCounter);
		}
	}

	void ScheduleShadowCasters()
	{
		ZoneScopedN("SCM::ScheduleShadowCasters");
		// Per-frame diagnostic counters; emitted via TracyPlot at function exit.
		s_schedDiag = SchedDiagCounters{};
		// VR calls CalculateAndDrawShadowCasterLights twice per frame (once per
		// eye). Block the second call: s_lights isn't reentrancy-safe.
		static std::atomic<bool> s_inSchedule{ false };
		if (s_inSchedule.exchange(true, std::memory_order_acquire))
			return;
		struct Guard
		{
			~Guard() { s_inSchedule.store(false, std::memory_order_release); }
		} guard;

		// Advance once per frame, before accumulate and render-split, so a caster
		// classifies identically across both (a mid-frame bump would double-count).
		if (++s_casterClassEpoch % 300 == 0) {
			std::erase_if(s_casterMobility,
				[](const auto& kv) { return s_casterClassEpoch - kv.second.lastEpoch > 300; });
			// Split state keys dead lights until this cap; rebuilds harmlessly.
			PruneIfOversized(s_splitState, 512);
		}

		// VR display guard: skip scheduling when the HMD display is not active.
		if (globals::game::isVR && !GetVRDrawShadows())
			return;

		auto* ssn = GetShadowSceneNode();
		auto* camera = GetWorldCamera();
		if (!ssn || !camera)
			return;

		// Pause while the interior portal graph is mid-rebuild (cell transition).
		if (IsPortalGraphTransitioning())
			return;

		// Exclusive against ShadowCasterManager::Update's pool resize (a settings
		// change can delete[]/new[] s_lights.Lights concurrently); held for the whole
		// pass so every s_lights.Lights[slot] access sees a consistent pointer/Size pair.
		std::shared_lock poolLock(s_lightsPoolMutex);

		// Drain a pending teardown reset before touching any slot, then SKIP this pass:
		// ClearLightArrays frees the lights but doesn't shrink activeShadowLights, so
		// scoring it now touches freed memory. Load-bearing: don't remove this return.
		if (s_pendingSessionReset.exchange(false, std::memory_order_acquire)) {
			s_everHadShadow.clear();
			ResetSession();
			return;
		}

		// Drain a pending cell-grid shift. Slots stay owned (unlike the session reset
		// above), so this doesn't skip the pass -- it only drops caches keyed on
		// caster geometry identity, which a cell swap can silently recycle.
		if (s_pendingCellReset.exchange(false, std::memory_order_acquire)) {
			s_cellResetTotal.fetch_add(1, std::memory_order_relaxed);
			s_casterMobility.clear();
			s_everHadShadow.clear();  // a cell swap recycles NiLight addresses
			s_splitState.clear();     // bakeQueued defaults true: every light re-bakes
			ShadowCasterManager::InvalidateAllStaticBakes();
		}

		// Hold a strong ref to every active shadow light: a concurrent bulk cell-teardown
		// (ReleaseChildren, bypassing our RemoveLight hook) can free one mid-pass, and a
		// later write through the stale raw pointer corrupts the recycled occupant's vtable -> CTD.
		std::vector<RE::NiPointer<RE::BSShadowLight>> heldRefs;
		{
			auto& alive = ssn->GetRuntimeData().activeShadowLights;
			heldRefs.reserve(alive.size() + 1);
			for (auto& sp : alive)
				if (sp)
					heldRefs.push_back(sp);
		}

		// Couple the shadow-cull distance to the light fade before UpdateCamera
		// reads the cached square. No-op unless MatchShadowToLightFade is enabled.
		ApplyShadowToLightFadeMatch();

		// Maintain demotion diagnostics only when something can read them (open settings
		// menu or a recent devbench dump), keeping the per-light hash churn off the hot path.
		const bool wantDiag = Menu::GetSingleton()->IsEnabled ||
		                      s_schedDumpFrames.load(std::memory_order_relaxed) > 0;

		ReserveFocusShadowSlots();

		// Do NOT clear shadowLightsAccum or reset the slot counter here:
		// ResetCalculatedShadowCasterLights (called before this hook) already did that
		// and installed the sun at slot 0; re-clearing wipes the sun's cascade.

		s_budget.Begin(0);

		RE::BSShadowLight* sunLight = SetupSunLight(ssn);

		ScrubFocusShadowFlags(ssn);

		*GetSunPtr() = 0;

		// ---- Score all candidate lights ----
		// Reuse a static vector so we don't allocate per frame -- the candidate
		// list is the same shape size each call (a few hundred lights at most).
		static std::vector<CandidateLight> candidates;

		ScoreShadowCandidates(ssn, camera, sunLight, candidates);

		{
			// Validation, redraw-interval scoring, and RedrawFrame marking dominate
			// this function's runtime (98%+), so a dedicated zone scopes it separately.
			ZoneNamedN(zoneValBudget, "SCM::ValidateAndScheduleBudget", true);

			ValidateCandidates(camera, wantDiag, candidates);

			UpdatePoolMembership(ssn, sunLight, candidates);

			double budget = ComputeRedrawBudget();
			s_redrawnLightsThisFrame = 0;
			s_totalShadowLightsThisFrame = s_settings.ShadowLightCount;
			RunShadowRedrawScheduleLoop(camera, budget);
		}

		TrackRedrawStallStats();

		int doneLightCount = 0;
		if (!RunAtomicMutationLoop(ssn, camera, sunLight, candidates, doneLightCount))
			return;

		ReinsertNonRedrawnLights(ssn, sunLight, camera);

		FinalizeFrameBookkeeping(ssn, doneLightCount);

		EmitSchedulerDiagnostics(wantDiag);
	}

	// Render hook: replaces RenderActiveShadowCasterLights. Iterates s_lights
	// and calls Render() on lights flagged RedrawFrame.

	void RenderScheduledShadowLights()
	{
		// A teardown landing between schedule and this pass would flush a freed
		// accumulator -> AV in BSBatchRenderer. Skip while a reset is pending; only
		// LOAD here, never exchange, or s_lights would never reset (ScheduleShadowCasters owns the drain).
		if (s_pendingSessionReset.load(std::memory_order_acquire))
			return;

		// Pause during portal-graph rebuild: BSShadowLight::Render walks portal
		// culling that derefs ssn->portalGraph.
		if (IsPortalGraphTransitioning())
			return;

		// Exclusive against ShadowCasterManager::Update's pool resize; see the matching
		// lock in ScheduleShadowCasters for why it must cover the whole pass.
		std::shared_lock poolLock(s_lightsPoolMutex);

		// Reader side of the teardown serialization: ClearLightArrays must not free
		// lights/passes while this pass iterates them. Counter (not a shared_mutex)
		// so nested engine re-entry on this thread stays defined.
		if (s_teardownWaiting.load(std::memory_order_acquire))
			return;
		s_shadowFlushReaders.fetch_add(1, std::memory_order_acq_rel);
		struct FlushReaderGuard
		{
			~FlushReaderGuard() { s_shadowFlushReaders.fetch_sub(1, std::memory_order_acq_rel); }
		} flushReaderGuard;
		if (s_teardownWaiting.load(std::memory_order_acquire))
			return;  // teardown won the race between our check and increment

		// Atlas resource creation happens here at the pass boundary so
		// readiness cannot flip between draws of the same frame.
		UpdateAtlas();

		// Atlas rank budget: in importance order, each light gets the biggest class
		// that still leaves a quarter cell for every lower-ranked light; without it,
		// first arrivals hoard full tiles and later lights get no tile at all.
		if (AtlasActive()) {
			static std::vector<LightEntry*> ranked;
			ranked.clear();
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++)
				if (s_lights.Lights[i].Light && !s_cameraHold.count(s_lights.Lights[i].Light))
					// Held lights excluded here so only actively-scheduled lights compete.
					ranked.push_back(&s_lights.Lights[i]);
			// Two-key rank: geometry picks the class band, priority orders within it;
			// score jitter can never reorder across bands, keeping tile assignments cache-stable.
			std::sort(ranked.begin(), ranked.end(),
				[](const LightEntry* a, const LightEntry* b) {
					if (a->desiredScale != b->desiredScale)
						return a->desiredScale > b->desiredScale;
					return a->lastScore > b->lastScore;
				});
			uint32_t cellsLeft = AtlasCapacityCells();
			uint32_t remaining = static_cast<uint32_t>(ranked.size());
			for (auto* e : ranked) {
				remaining--;
				float scale = e->desiredScale;
				while (scale > kTileScaleFloor &&
					   (cellsLeft < CellsForScale(scale) || cellsLeft - CellsForScale(scale) < remaining))
					scale *= 0.5f;
				e->budgetScale = scale;
				// No demotion hold: holding pendingScale above budget target
				// reproducibly crashes the engine batch renderer -- root-cause
				// before reintroducing. Promotion hold IS safe (avoids flicker).
				if (e->desiredScale > e->pendingScale) {
					if (e->promoteStreak < kPromoteStreakFrames)
						e->promoteStreak++;
				} else if (e->promoteStreak > 0) {
					e->promoteStreak--;
				}
				float target = std::min(e->desiredScale, scale);
				if (target > e->pendingScale && e->promoteStreak < kPromoteStreakFrames)
					target = e->pendingScale;  // not enough sustained desire yet
				e->pendingScale = target;
				cellsLeft -= std::min(cellsLeft, CellsForScale(scale));
			}
		}

		// VR: RenderActiveShadowCasterLights normally saves+clears g_drawStereo, then
		// restores it. Without this, each hemisphere render doubles for both eyes -> 4-quadrant texture.
		bool savedStereo = false;
		if (globals::game::isVR) {
			savedStereo = *globals::game::drawStereo;
			*globals::game::drawStereo = false;
		}

		ZoneScopedN("SCM::RenderScheduledShadowLights");
		CS_GPU_PASS("SCM::RenderScheduledShadowLights");

		s_budget.Begin(1);
		s_budget.BeginRenderBatch();

		uint32_t tmp = 0;
		// Sun first: writes cascade depth maps to kSHADOWMAPS_ESRAM. Vanilla
		// RenderActiveShadowCasterLights dispatches this via the same vtable walk we
		// replaced with this loop, so sun.Render must be called explicitly here.
		// Without this, exterior scenes render with no sun shadow.
		if (s_lights.Sun && s_lights.Lights[0].Light &&
			!s_pendingSessionReset.load(std::memory_order_acquire)) {
			ZoneNamedN(zSun, "SCM::Render::Sun", true);
			CS_GPU_PASS("SCM::Render::Sun");
			s_budget.BeginLight(s_lights.Lights[0].Light, 1);
			s_lights.Lights[0].Light->Render(tmp);
			s_budget.EndLight(s_lights.Lights[0].Light, 1);
		}

		// Point lights from PointLightFirst onwards; skips slot 0 (sun) and includes
		// the highest point-light slot when Sun=true.
		{
			ZoneNamedN(zPoint, "SCM::Render::PointLights", true);
			CS_GPU_PASS("SCM::Render::PointLights");
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
				// A teardown can begin mid-pass; ClearLightArrays sets the flag before
				// freeing, so bailing here narrows the window to a single in-flight Render call.
				if (s_pendingSessionReset.load(std::memory_order_acquire))
					break;
				auto& e = s_lights.Lights[i];
				if (!e.Light || !e.RedrawFrame)
					continue;
				bool keepPriorContent = false;
				bool emptyRender = false;
				if (AtlasActive()) {
					// Tile before raster: (re)size for the pending class and rect-clear
					// once per redraw -- the paraboloid halves share the tile, so
					// clearing inside the cascade would wipe the first half.
					if (!EnsureSlotTile(i, e.pendingScale)) {
						// Atlas exhausted even at the quarter class: raster must still run --
						// EnableLight already registered this light's passes, and an unconsumed
						// group is freed-but-not-unlinked at frame end. The viewport hook
						// collapses a tile-less raster to zero size, so this draws nothing.
						s_budget.BeginLight(e.Light, 1);
						s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
						s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
						s_budget.EndLight(e.Light, 1);
						// This frame bailed before running the bake EnableLight already
						// armed -- restore bakeQueued so it retries instead of being lost.
						if (auto it = s_splitState.find(e.Light); it != s_splitState.end() && it->second.bakeThisFrame)
							it->second.bakeQueued = true;
						continue;
					}
					// On a bake frame, render the static subset into the cache atlas;
					// otherwise copy the cache into the tile and composite movers over it.
					if (StaticAtlasReady() &&
						!s_splitState[e.Light].splitExcluded && !s_splitState[e.Light].fullThisFrame) {
						SplitState& st = s_splitState[e.Light];
						if (st.bakeThisFrame) {
							ZoneNamedN(zBake, "SCM::Render::StaticBake", true);
							s_staticPassActive.store(true, std::memory_order_relaxed);
							ClearStaticSlotTile(i);
							s_budget.BeginLight(e.Light, 1);
							s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
							s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
							s_budget.EndLight(e.Light, 1);
							s_staticPassActive.store(false, std::memory_order_relaxed);
							MarkSlotStaticRendered(i, st.pendingHash, st.bakeSawStatic);  // atlas slot = source of truth
							st.bakeThisFrame = false;
							// Keep last frame's complete live content on bake frames -- copying
							// the fresh static-only bake in flashed a mover-less shadow for one
							// frame. Only a tile with no valid content takes the copy.
							AtlasTileTexels bakeTile{};
							const bool liveHasContent = GetSlotTileTexels(i, bakeTile) && bakeTile.contentValid;
							// A bake that captured nothing is a blank rect: seeding a fresh
							// tile from it would advertise a flat tile as content.
							if (!liveHasContent && st.bakeSawStatic)
								CopyStaticTileToLive(i);
							if (liveHasContent || st.bakeSawStatic) {
								e.renderedScale = e.pendingScale;
								// Live content unchanged this frame: never swap a staged promotion in on a bake.
								MarkSlotTileRendered(i, false);
							}
							continue;
						}
						{
							// Re-check bake validity AT COMPOSITE TIME: mode selection ran before
							// per-frame owner reconciliation, so a reassigned slot can reach here
							// holding the PREVIOUS light's bake, compositing a different light's
							// shadow. Movers-only for one frame beats that; the queued bake heals it.
							uint64_t compositeHash = 0;
							bool compositeValid = false;
							bool compositeEmpty = false;
							GetSlotStaticState(i, compositeHash, compositeValid, &compositeEmpty);
							// Real content = a non-blank static seed, or movers this frame.
							// Neither means this frame draws a flat tile: hold prior content instead.
							const bool composedContent =
								(compositeValid && !compositeEmpty) || st.sawDynamicLastAccum;
							AtlasTileTexels liveTile{};
							const bool compositeKeepPrior = !composedContent &&
							                                GetSlotTileTexels(i, liveTile) && liveTile.contentValid;
							if (compositeValid) {
								if (!compositeKeepPrior)
									CopyStaticTileToLive(i);  // seed the tile with cached static depth
							} else {
								if (!compositeKeepPrior)
									ClearSlotTile(i);
								st.bakeQueued = true;
							}
							s_budget.BeginLight(e.Light, 1);
							s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);  // composite movers on top (no clear)
							s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
							s_budget.EndLight(e.Light, 1);
							// A movers-only frame (invalid seed) must not swap a staged promotion
							// in: keep sampling the old complete tile until a seeded composite lands.
							if (!compositeKeepPrior && composedContent) {
								e.renderedScale = e.pendingScale;
								MarkSlotTileRendered(i, compositeValid);
							}
							continue;
						}
					}
					// A redraw with no casters would clear the tile and mark it valid,
					// degenerating a good shadow to flat black -- keep prior content instead.
					emptyRender = e.Light->geomList.empty();
					AtlasTileTexels held{};
					keepPriorContent = emptyRender &&
					                   GetSlotTileTexels(i, held) && held.contentValid;
					if (!keepPriorContent)
						ClearSlotTile(i);
				}
				s_budget.BeginLight(e.Light, 1);
				s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
				s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
				s_budget.EndLight(e.Light, 1);
				// Commit the content scale only after the raster actually ran: a skipped
				// render must keep advertising the slot's held scale, or shaders sample
				// tile UVs against full-slice content until the geometry hash changes.
				if (!keepPriorContent) {
					e.renderedScale = e.pendingScale;
					// Never mark an EMPTY render valid: a starved fresh tile stays
					// contentValid=false, so the sample side reads UNSHADOWED, never a
					// degenerate (cleared) tile, even under high pressure.
					if (!emptyRender)
						MarkSlotTileRendered(i);
				}
			}
		}

		ServiceShadowFrameRecord();
		s_budget.EndRenderBatch();

		if (globals::game::isVR)
			*globals::game::drawStereo = savedStereo;
	}
}
