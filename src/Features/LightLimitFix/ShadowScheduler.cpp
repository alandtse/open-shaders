// ShadowScheduler.cpp
// The shadow caster scheduling core: geometry hashing, light transitions, ScheduleShadowCasters, and render dispatch.

#include <fstream>

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

#include <Windows.h>  // SEH (__try) for the shadow-light usability backstop

#define I18N_KEY_PREFIX "feature.light_limit_fix."

namespace ShadowCasterManager
{
	// =========================================================================
	// Shadow map content hash for cached-shadow-map detection
	// =========================================================================

	/// Mixes a 32-bit value into a running 64-bit hash. boost::hash_combine
	/// constants -- the magic number 0x9e3779b9 is the golden-ratio reciprocal,
	/// chosen for good bit distribution. Fast (a few ALU ops) and we don't
	/// need cryptographic strength -- only that distinct inputs map to
	/// distinct outputs with very high probability.
	static inline std::uint64_t HashCombine(std::uint64_t h, std::uint32_t v) noexcept
	{
		return h ^ (static_cast<std::uint64_t>(v) + 0x9e3779b9ull + (h << 6) + (h >> 2));
	}
	static inline std::uint64_t HashCombineFloat(std::uint64_t h, float f) noexcept
	{
		return HashCombine(h, std::bit_cast<std::uint32_t>(f));
	}

	/// Quantize a float to a step size before hashing. Skyrim's kFlicker /
	/// kPulse light flags oscillate animated torches by sub-unit position /
	/// radius amounts every frame. Bit-exact hashing on those oscillations
	/// produces a fresh hash every frame, defeating cache validity. Quantizing
	/// at sub-pixel-precision thresholds folds imperceptible animations into
	/// a stable hash bucket so the cached-shadow priority demotion fires
	/// correctly for visually-unchanging lights.
	static inline float QuantizeFloat(float f, float step) noexcept
	{
		return std::round(f / step) * step;
	}

	/// Hash of inputs that determine a shadow map's content: the light's
	/// pose + radius, and each caster's worldBound + identity. worldBound
	/// tracks rigid motion and BSDynamicTriShape vertex updates, so mesh
	/// data isn't inspected directly. Identical hashes across frames mean
	/// the cached slot is byte-for-byte current -- caller can skip the
	/// redraw. Returns 0 only on null light/NiLight (sentinel for "never
	/// rendered"); HashCombine constants make a real-data 0 essentially
	/// impossible.

	/// Folds everything about a light that changes the depths it rasterizes:
	/// position, orientation, and radius. Shared by the redraw hash and the
	/// static-cache hash -- if the two disagreed about what "the light moved"
	/// means, the weaker one would keep serving a tile baked for a different
	/// pose (a rotated spot, or a torch mid-flicker, shows another caster's
	/// shadow and self-shadow acne).
	/// posStep is caller-scaled to the tile class's world-units-per-texel;
	/// floored at 1.0 so sub-texel motion never busts the cache.
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
		// NiPointLight uses .x. Fire flicker drives this, which rescales the
		// projection every frame.
		h = HashCombineFloat(h, QuantizeFloat(ni->GetLightRuntimeData().radius.x, kRadiusStep));
		return h;
	}

	static std::uint64_t ComputeShadowGeomHash(RE::BSShadowLight* light, float posStep)
	{
		if (!light)
			return 0;
		auto* ni = light->light.get();
		if (!ni)
			return 0;
		std::uint64_t h = 0x9e3779b97f4a7c15ull;  // arbitrary nonzero seed
		// Coarse radius fold: a permanent radius change (scripted) must retire
		// the baked depth + snapshot; 64-unit steps ignore flame flicker.
		h = h * 31 + static_cast<std::uint64_t>(ni->GetLightRuntimeData().radius.x / 64.0f);

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

		// Player-only dynamic-caster proxy: NPCs that cast in this light are
		// already in geomList above (folded via worldBound), so they refresh it
		// as they move. The player's own geometry is reliably NOT in geomList at
		// scheduling time, so a stationary light the player walks through would
		// hash constant and get the "unchanged -> skip redraw" penalty, freezing
		// the player's shadow. Fold only the player when enclosed -- folding all
		// actors per light instead saturates the redraw budget and starves
		// distant lights into empty tiles.
		if (auto* plr = RE::PlayerCharacter::GetSingleton()) {
			const auto pp = plr->GetPosition();
			const auto& lp = ni->world.translate;
			const float dx = pp.x - lp.x, dy = pp.y - lp.y, dz = pp.z - lp.z;
			const float r = ni->GetLightRuntimeData().radius.x;
			if (dx * dx + dy * dy + dz * dz < r * r) {
				const float actorStep = std::min(kPosStep, 8.0f);
				h = HashCombineFloat(h, QuantizeFloat(pp.x, actorStep));
				h = HashCombineFloat(h, QuantizeFloat(pp.y, actorStep));
				h = HashCombineFloat(h, QuantizeFloat(pp.z, actorStep));
			}
		}
		return h;
	}

	// =========================================================================
	// Contribution-based caster culling (experimental)
	//
	// A point light's shadow render submits one draw batch per visible caster;
	// small or distant casters produce sub-pixel shadows that cost CPU
	// submission for no visible result. The engine's parabolic (point-light)
	// culling registers each visible caster through BSCullingProcess::
	// AppendVirtual (vtable slot 0x18). We hook that slot on ONLY the parabolic
	// vtable -- so it fires exclusively for point-light shadow culling, never
	// the sun, spots, or main scene -- and skip the append for a caster whose
	// camera-relative screen size (bound radius / distance to camera) is below
	// CasterCullAngularMin. s_currentCullLight identifies the light being
	// accumulated (set around EnableLight's Accumulate call, same render
	// thread), so the cost metric uses the VR-validated BSShadowLight position
	// rather than the culling process's internal members.
	// =========================================================================

	/// Casters culled last frame across all lights (Tracy plot for A/B).
	std::atomic<uint32_t> s_casterCullCount{ 0 };

	/// Appends dropped because the culling process's free pool was near
	/// exhaustion (see the guard in Hook_ParabolicCullAppend).
	std::atomic<uint32_t> s_cullPoolDropCount{ 0 };

	/// The shadow light currently being accumulated; only non-null across an
	/// EnableLight Accumulate call, read synchronously by the AppendVirtual hook.
	std::atomic<RE::BSShadowLight*> s_currentCullLight{ nullptr };

	// True while accumulating a light whose geomList is EMPTY: the engine only
	// attaches geometry to lights once per geometry (AttachNearbyLights sets
	// kRenderUse and never revisits), so a light created after the scene
	// attached -- e.g. every light of an in-game same-cell load -- never
	// receives geometry and renders an empty shadow forever. The append hook
	// rebuilds the list from the cull walk via the engine's own AttachGeometry.
	std::atomic<bool> s_accumRebuildAttach{ false };

	// Geometry already re-attached in the current heal walk (render thread).
	std::unordered_set<const RE::BSGeometry*> s_healAttached;

	// =========================================================================
	// Multi-frame diagnostic recorder (devbench capture kind=shadowmaps with
	// frames=N[, slot=S]). Per shadow pass: numeric per-slot state; with a
	// target slot also that light's visited caster set per pass mode, so a
	// frozen-scene static/dynamic split is checkable against the unsplit set
	// (split valid <=> visited(All) == union of static and dynamic, disjoint). One JSON
	// under Captures/ at completion; zero cost while disarmed.
	// =========================================================================
	struct RecCaster
	{
		const void* geom;
		std::string name;
		bool dynamic;
		int mode;  ///< CasterPass value (enum defined below; int keeps this decl order-free)
	};
	struct RecSlot
	{
		int32_t slot;
		const void* light;
		uint32_t tx, ty, ts;
		bool valid, staticValid, redrew;
		float pending, rendered;
	};
	struct RecFrame
	{
		uint32_t frame, reallocs, ownerInv;
		std::vector<RecSlot> slots;
		std::vector<RecCaster> casters;
	};
	std::vector<RecFrame> s_recFrames;
	std::vector<RecCaster> s_recCasters;  // filled by the append hook during accumulate
	std::atomic<int32_t> s_recLeft{ 0 };
	std::atomic<int32_t> s_recTargetSlot{ -1 };
	bool s_recPixels = false;
	// Arm via atomics only: the devbench thread must not touch the vectors
	// the render thread owns. frames is the trigger; store slot first.
	std::atomic<uint32_t> s_recRequestFrames{ 0 };
	std::atomic<int32_t> s_recRequestSlot{ -1 };

	void ServiceShadowFrameRecord()
	{
		if (const uint32_t req = s_recRequestFrames.exchange(0, std::memory_order_acq_rel); req != 0) {
			s_recFrames.clear();
			s_recCasters.clear();
			s_recTargetSlot.store(s_recRequestSlot.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s_recPixels = s_recTargetSlot.load(std::memory_order_relaxed) >= 0 && req <= 16u;
			s_recFrames.reserve(req);
			s_recLeft.store(static_cast<int32_t>(req), std::memory_order_relaxed);
		}
		if (s_recLeft.load(std::memory_order_relaxed) <= 0)
			return;
		RecFrame rec{};
		rec.frame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		const auto stats = GetAtlasClearStats();
		rec.reallocs = stats.tileReallocs;
		rec.ownerInv = stats.ownerInvalidations;
		for (int32_t i = 0; i < s_lights.Size; i++) {
			const auto& e = s_lights.Lights[i];
			if (!e.Light)
				continue;
			AtlasTileTexels t{};
			const bool hasTile = GetSlotTileTexels(i, t);
			uint64_t staticHash = 0;
			bool staticValid = false;
			GetSlotStaticState(i, staticHash, staticValid);
			rec.slots.push_back({ i, e.Light, hasTile ? t.x : 0u, hasTile ? t.y : 0u,
				hasTile ? t.size : 0u, hasTile && t.contentValid, staticValid,
				e.RedrawFrame, e.pendingScale, e.renderedScale });
		}
		rec.casters = std::move(s_recCasters);
		s_recCasters.clear();
		const int32_t target = s_recTargetSlot.load(std::memory_order_relaxed);
		if (s_recPixels && target >= 0)
			DumpSlotTileRegion(target, rec.frame);
		s_recFrames.push_back(std::move(rec));
		if (s_recLeft.fetch_sub(1, std::memory_order_relaxed) != 1)
			return;

		std::filesystem::path dir = "Data\\SKSE\\Plugins\\CommunityShaders\\Captures";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const auto path = dir / std::format("scm_frames_{}.json", rec.frame);
		std::ofstream out(path);
		out << "{\n\"frames\": [\n";
		for (size_t f = 0; f < s_recFrames.size(); f++) {
			const auto& fr = s_recFrames[f];
			out << (f ? ",\n" : "") << "{\"frame\":" << fr.frame << ",\"reallocs\":" << fr.reallocs
				<< ",\"ownerInv\":" << fr.ownerInv << ",\"slots\":[";
			for (size_t s = 0; s < fr.slots.size(); s++) {
				const auto& r = fr.slots[s];
				out << (s ? "," : "") << "{\"i\":" << r.slot << ",\"light\":\"" << r.light
					<< "\",\"tx\":" << r.tx << ",\"ty\":" << r.ty << ",\"ts\":" << r.ts
					<< ",\"valid\":" << (r.valid ? "true" : "false")
					<< ",\"staticValid\":" << (r.staticValid ? "true" : "false")
					<< ",\"redrew\":" << (r.redrew ? "true" : "false")
					<< ",\"pending\":" << r.pending << ",\"rendered\":" << r.rendered << "}";
			}
			out << "],\"casters\":[";
			for (size_t c = 0; c < fr.casters.size(); c++) {
				const auto& k = fr.casters[c];
				std::string nm = k.name;
				std::erase_if(nm, [](char ch) { return ch == '"' || ch == '\\'; });
				out << (c ? "," : "") << "{\"geom\":\"" << k.geom << "\",\"name\":\"" << nm
					<< "\",\"dynamic\":" << (k.dynamic ? "true" : "false") << ",\"mode\":" << k.mode << "}";
			}
			out << "]}";
		}
		out << "\n]\n}\n";
		logger::info("[SCM] Frame record written: {} ({} frames)", path.string(), s_recFrames.size());
		s_recFrames.clear();
	}

	void RequestShadowFrameRecord(uint32_t a_frames, int32_t a_slot)
	{
		s_recRequestSlot.store(a_slot, std::memory_order_relaxed);
		s_recRequestFrames.store(std::clamp(a_frames, 1u, 600u), std::memory_order_release);
	}

	/// Consecutive frames a light failed UpdateCamera (exit hysteresis at the
	/// validation gate). Render thread only; keys are never dereferenced, so a
	/// dead light's stale entry is harmless until the size prune.
	std::unordered_map<RE::BSShadowLight*, uint32_t> s_invalidStreak;

	/// Consecutive frames a light scored below ShadowImpactFloor (exit hysteresis
	/// mirroring s_invalidStreak above). Without this, a light hovering near the
	/// floor drops its atlas slot and re-bakes every time it dips back above --
	/// EnableLight's own pose-rebake counter can then latch splitExcluded after a
	/// handful of these flaps within its window, permanently downgrading the
	/// light to full renders. Render thread only; same dereference/prune notes.
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

	/// Camera world position captured when the current light's accumulate begins.
	/// The contribution cull measures each caster's size from HERE (the viewer),
	/// not from the light: a caster's on-screen shadow footprint -- how much of
	/// the view it occupies -- is what the player perceives. Set on the same
	/// render thread just before s_currentCullLight, so the hook reads it safely.
	RE::NiPoint3 s_cullCameraPos{};

	// -------------------------------------------------------------------------
	// Static/dynamic split caching -- caster classification + append filter
	//
	// The parabolic AppendVirtual hook decides which casters enter a light's
	// shadow render: fewer appends -> fewer draw batches -> less CPU (the same
	// mechanism contribution culling uses). s_cullPassMode picks the subset the
	// hook keeps for the light's single accumulate this frame: StaticOnly on a
	// rare rebake of the parallel static atlas, DynamicOnly (movers only) the
	// rest of the time. The frame-flow that sets the mode is documented at
	// SplitState below.
	// -------------------------------------------------------------------------
	enum class CasterPass : int
	{
		All = 0,         ///< keep every caster (normal / measurement)
		StaticOnly = 1,  ///< keep only pose-stable casters (build static cache)
		DynamicOnly = 2  ///< keep only moving casters (composite over cache)
	};
	std::atomic<int> s_cullPassMode{ static_cast<int>(CasterPass::All) };

	/// Static/dynamic caster draws classified last frame (Tracy plots for A/B).
	std::atomic<uint32_t> s_staticCasterDraws{ 0 };
	std::atomic<uint32_t> s_dynamicCasterDraws{ 0 };

	/// StaticOnly re-bakes issued this frame. A bake re-rasterizes a light's
	/// whole static caster set into its cache tile, so this is the cost the
	/// cache trades against: sustained non-zero means the static set keeps
	/// churning and the cache is paying for itself repeatedly.
	std::atomic<uint32_t> s_staticBakeCount{ 0 };
	/// Cumulative bakes since load, published in the snapshot so a headless A/B
	/// can difference it across a run without attaching a profiler.
	std::atomic<uint64_t> s_staticBakeTotal{ 0 };

	// Per-caster movement history. A caster is dynamic while it moved within the
	// last stability window; once stable that long it classifies static (baked
	// into the cache). The stability window lets a caster that just stopped keep
	// rendering in the dynamic pass until the static cache rebuild absorbs it, so
	// its shadow never blinks out mid-transition.
	// Single-threaded: the hook and the per-frame epoch bump both run on the
	// shadow render thread within ScheduleShadowCasters.
	constexpr int kStaticStabilityFrames = 8;
	// Joining or leaving the static set changes the static hash and forces a
	// full StaticOnly re-bake of every tile that sees the caster. Leaving must
	// stay immediate (a baked tile of a caster that moved is stale), so damp the
	// churn on the rejoin side: each oscillation doubles how long the caster must
	// hold still before it is re-admitted, up to this multiple of the base window.
	// An NPC that fidgets every second then stays in the cheap dynamic pass
	// instead of re-baking the cache each time it pauses; furniture still
	// promotes on the base window.
	constexpr int kStaticPromoteBackoffMax = 8;  // 8 * 8 = 64 frames, ~1s at 60fps
	// Bounds framesSinceMove; must clear the longest decay threshold below.
	constexpr int kStaticFramesCap = kStaticStabilityFrames * kStaticPromoteBackoffMax * 4;
	// Settled casters re-verify worldBound only every N epochs, not every
	// visit; kept well inside kSleepRedrawIntervalFrames (45) so a missed
	// move is still caught before that existing tolerance would hide it.
	constexpr int kSettledRecheckFrames = 16;
	constexpr int kSettledAtFactor = 4;  // matches the promoteAt * 4 backoff-reset below
	struct CasterMobility
	{
		int lastEpoch = -1;
		int lastVerifyEpoch = -1;  ///< epoch of last full quantize-and-compare (not skip-stamped)
		int framesSinceMove = 0;
		int promoteBackoff = 1;  ///< multiplies the promote window; grows per oscillation
		float cx = 0.0f, cy = 0.0f, cz = 0.0f, cr = 0.0f;
		/// Cached static-hash contribution (identity + quantized worldBound);
		/// valid while the quantized bound is unchanged, i.e. until "moved".
		uint64_t foldHash = 0;
		bool foldHashValid = false;
		bool dynamic = true;
	};
	// Open-addressing map: probed once per appended caster by the cull-walk
	// hook, so lookup cost lands directly on EnableLight's accumulate time.
	ankerl::unordered_dense::map<RE::BSGeometry*, CasterMobility> s_casterMobility;
	int s_casterClassEpoch{ 0 };

	/// Classifies a caster static vs dynamic from its quantized worldBound
	/// movement, memoized once per frame (a caster shared across lights, or
	/// revisited across passes, classifies identically all frame). 1-unit
	/// quantization matches the redraw hash so "moved" means "shadow changed".
	/// Classification record for a non-skinned caster (skinned geometry never
	/// reaches the map -- see IsCasterDynamic).
	static CasterMobility& ClassifyCaster(RE::BSGeometry& geom)
	{
		auto [it, inserted] = s_casterMobility.try_emplace(&geom);
		auto& r = it->second;
		if (r.lastEpoch == s_casterClassEpoch)
			return r;  // already classified this frame

		// Settled fast path: trust the cached classification instead of
		// re-quantizing worldBound. A move mid-window is still caught at the
		// next checkpoint; the mismatchStreak/rebake departure path is unchanged.
		const int settledAt = kStaticStabilityFrames * r.promoteBackoff * kSettledAtFactor;
		if (!inserted && !r.dynamic && r.framesSinceMove >= settledAt &&
			s_casterClassEpoch - r.lastVerifyEpoch < kSettledRecheckFrames) {
			r.lastEpoch = s_casterClassEpoch;  // keep same-frame memo + prune liveness
			return r;
		}

		const auto& wb = geom.worldBound;
		const float cx = std::round(wb.center.x), cy = std::round(wb.center.y),
					cz = std::round(wb.center.z), cr = std::round(wb.radius);
		const bool moved = inserted || cx != r.cx || cy != r.cy || cz != r.cz || cr != r.cr;
		// Settled casters verify sparsely, so framesSinceMove must advance by
		// the elapsed epoch count to keep real-time semantics.
		const int elapsed = r.lastVerifyEpoch < 0 ? 1 : std::max(1, s_casterClassEpoch - r.lastVerifyEpoch);
		if (moved) {
			// Leaving the static set is an oscillation: make the caster earn its
			// way back so a caster that keeps pausing can't re-bake the cache on
			// every pause. A first sighting is not an oscillation.
			if (!r.dynamic && !inserted)
				r.promoteBackoff = std::min(r.promoteBackoff * 2, kStaticPromoteBackoffMax);
			r.framesSinceMove = 0;
			r.foldHashValid = false;  // quantized bound changed
		} else {
			r.framesSinceMove = std::min(r.framesSinceMove + elapsed, kStaticFramesCap);
		}
		r.cx = cx;
		r.cy = cy;
		r.cz = cz;
		r.cr = cr;
		r.lastEpoch = s_casterClassEpoch;
		r.lastVerifyEpoch = s_casterClassEpoch;
		const int promoteAt = kStaticStabilityFrames * r.promoteBackoff;
		r.dynamic = r.framesSinceMove < promoteAt;
		// Held still far past its (backed-off) window: it has settled rather than
		// oscillating, so restore the base window for its next move.
		if (!r.dynamic && r.framesSinceMove >= promoteAt * kSettledAtFactor)
			r.promoteBackoff = 1;
		return r;
	}

	static bool IsCasterDynamic(RE::BSGeometry& geom)
	{
		// Skinned = actor/creature: never let the movement heuristic bake it
		// static, or its silhouette ghosts once it walks off a bake-starved cell.
		if (geom.GetGeometryRuntimeData().skinInstance)
			return true;
		return ClassifyCaster(geom).dynamic;
	}

	// Running static-caster hash for the light currently accumulating; seeded
	// with the light pose in EnableLight, folded per static caster by the hook.
	std::uint64_t s_visitStaticHash{ 0 };

	// Dynamic casters the current accumulate appended (DynamicOnly/All passes
	// only). Atomic because the cull walk can append from worker threads.
	// Reset alongside the hash seed in EnableLight, latched into SplitState
	// after the accumulate for the schedule-time sleep skip.
	std::atomic<uint32_t> s_visitDynamicCount{ 0 };

	// Folds one static caster into the running per-light static hash during the
	// same accumulate that appends the movers -- no second caster walk needed.
	static void FoldStaticCasterHash(RE::BSGeometry& geom, CasterMobility& r)
	{
		if (!r.foldHashValid) {
			// Summed, not chained/XORed: order-independent, and a caster appended
			// by both paraboloid halves still contributes to the set hash.
			const auto raw = reinterpret_cast<std::uintptr_t>(&geom);
			uint64_t h = 0x9e3779b97f4a7c15ull;
			h = HashCombine(h, static_cast<std::uint32_t>(raw));
			h = HashCombine(h, static_cast<std::uint32_t>(raw >> 32));
			// r.cx..cr already hold the 1-unit-quantized worldBound (std::round
			// == QuantizeFloat(x, 1.0f)), refreshed by ClassifyCaster this frame.
			h = HashCombineFloat(h, r.cx);
			h = HashCombineFloat(h, r.cy);
			h = HashCombineFloat(h, r.cz);
			h = HashCombineFloat(h, r.cr);
			r.foldHash = h;
			r.foldHashValid = true;
		}
		s_visitStaticHash += r.foldHash;
	}

	/// Hook of BSCullingProcess::AppendVirtual on the parabolic culling vtable.
	/// Drops a caster (skips the append) when below the contribution-cull
	/// threshold, or when it does not belong to the active split-cache pass.
	struct Hook_ParabolicCullAppend
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			const float angularMin = s_settings.CasterCullAngularMin;
			RE::BSShadowLight* light = s_currentCullLight.load(std::memory_order_relaxed);
			// Rebuild a missed geometry attachment (see s_accumRebuildAttach)
			// through the engine's own pair-insert, mirroring
			// AttachNearbyLights' gates. Only for lights the engine is
			// provably not attaching (empty geomList), so this can't race a
			// concurrent scene-side attach on the same light.
			if (light && s_accumRebuildAttach.load(std::memory_order_relaxed) && !light->objectNode) {
				// Dual-paraboloid walks append the same geometry once per half;
				// AttachGeometry is a raw pair-insert, so dedupe per walk.
				if (auto* ni = light->light.get();
					ni && s_healAttached.insert(&a_visible).second &&
					GameLightIsInRange(light, &a_visible.worldBound, ni, 1.0f))
					GameAttachGeometry(light, &a_visible);
			}
			if (angularMin > 0.0f && light) {
				const auto& wb = a_visible.worldBound;
				const float dx = wb.center.x - s_cullCameraPos.x;
				const float dy = wb.center.y - s_cullCameraPos.y;
				const float dz = wb.center.z - s_cullCameraPos.z;
				const float distSq = dx * dx + dy * dy + dz * dz;
				const float radiusSq = wb.radius * wb.radius;
				// Camera-relative screen size = radius / distance-to-viewer. A
				// caster far from the camera casts a small on-screen shadow and is
				// culled; one close enough to fill the view is kept regardless of
				// how large it looks from the light. Skip the test when the caster
				// encloses the camera (its shadow can be anywhere on screen).
				// Squared form of dist > radius && radius / dist < angularMin,
				// avoiding the per-caster sqrt (all terms non-negative).
				if (distSq > radiusSq && radiusSq < distSq * (angularMin * angularMin)) {
					s_casterCullCount.fetch_add(1, std::memory_order_relaxed);
					return;  // skip append -- caster dropped from this shadow
				}
			}
			if (s_recLeft.load(std::memory_order_relaxed) > 0) {
				const int32_t recSlot = s_recTargetSlot.load(std::memory_order_relaxed);
				if (recSlot >= 0 && light && recSlot < s_lights.Size &&
					s_lights.Lights[recSlot].Light == light) {
					const char* nm = a_visible.name.c_str();
					s_recCasters.push_back({ &a_visible, nm ? nm : "",
						IsCasterDynamic(a_visible),
						s_cullPassMode.load(std::memory_order_relaxed) });
				}
			}
			if (AtlasActive()) {
				// Skinned = always dynamic (see IsCasterDynamic); only non-skinned
				// casters have a mobility record, classified once per frame.
				CasterMobility* rec = a_visible.GetGeometryRuntimeData().skinInstance ?
				                          nullptr :
				                          &ClassifyCaster(a_visible);
				const bool dynamic = !rec || rec->dynamic;
				// Every static caster folds into the running hash regardless of
				// pass, so the static-set change that triggers a rebake is seen
				// during whichever single accumulate this light runs this frame.
				if (!dynamic)
					FoldStaticCasterHash(a_visible, *rec);
				switch (static_cast<CasterPass>(s_cullPassMode.load(std::memory_order_relaxed))) {
				case CasterPass::StaticOnly:
					if (dynamic)
						return;  // bake pass skips movers
					break;
				case CasterPass::DynamicOnly:
					if (!dynamic)
						return;  // composite pass skips baked static geometry
					s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
					break;
				case CasterPass::All:
					(dynamic ? s_dynamicCasterDraws : s_staticCasterDraws).fetch_add(1, std::memory_order_relaxed);
					if (dynamic)
						s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
					break;
				}
			}
			// Engine bug: AppendVirtual writes through PopFreeQueueEntry's result
			// with no null check, so exhausting the fixed 8192-entry free pool is
			// a guaranteed CTD. Drop the caster instead; the margin absorbs
			// concurrent worker pops between this read and the engine's own pop.
			{
				constexpr std::uintptr_t kFreePoolOffset = 0x20150;  // identical SE/AE/VR
				constexpr std::uintptr_t kPoolHeadOffset = 0x10000;
				constexpr std::uintptr_t kPoolTailOffset = 0x10008;
				constexpr std::uint32_t kFreeEntryMargin = 16;
				const auto* pool = reinterpret_cast<const std::uint8_t*>(a_this) + kFreePoolOffset;
				const auto head = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolHeadOffset)->load(std::memory_order_relaxed);
				const auto tail = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolTailOffset)->load(std::memory_order_relaxed);
				if (tail - head < kFreeEntryMargin) {
					s_cullPoolDropCount.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/// Pool-exhaustion guard alone for the base culling process: the engine's
	/// room/scene cull walks reach the same unchecked AppendVirtual null-write.
	struct Hook_BaseCullAppendGuard
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			constexpr std::uintptr_t kFreePoolOffset = 0x20150;
			constexpr std::uintptr_t kPoolHeadOffset = 0x10000;
			constexpr std::uintptr_t kPoolTailOffset = 0x10008;
			constexpr std::uint32_t kFreeEntryMargin = 16;
			const auto* pool = reinterpret_cast<const std::uint8_t*>(a_this) + kFreePoolOffset;
			const auto head = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolHeadOffset)->load(std::memory_order_relaxed);
			const auto tail = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolTailOffset)->load(std::memory_order_relaxed);
			if (tail - head < kFreeEntryMargin) {
				s_cullPoolDropCount.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallCasterCullHook()
	{
		stl::write_vfunc<0x18, Hook_ParabolicCullAppend>(RE::VTABLE_BSParabolicCullingProcess[0]);
		stl::write_vfunc<0x18, Hook_BaseCullAppendGuard>(RE::VTABLE_BSCullingProcess[0]);
	}

	/// True only across a static-cache bake pass; read by the depth-select
	/// hooks to redirect the engine's shadow render into the static atlas.
	std::atomic<bool> s_staticPassActive{ false };

	bool StaticPassRedirectActive()
	{
		return s_staticPassActive.load(std::memory_order_relaxed);
	}

	// -------------------------------------------------------------------------
	// Static-cache split: single accumulate per light per frame
	//
	// The engine accumulator is reset once per frame (before scheduling), so a
	// light's Accumulate APPENDS its casters -- calling it twice would draw the
	// union, not a subset. So each light does exactly ONE filtered accumulate
	// per frame (in EnableLight): normally DynamicOnly (append only movers), and
	// occasionally StaticOnly to rebake the static cache when its caster set
	// changed. The bake is staggered onto its own frame; on that rare frame the
	// tile shows static-only briefly. s_visitStaticHash (folded by the hook)
	// detects a static-set change without a second caster walk.
	// -------------------------------------------------------------------------
	// Per-light handoff between the phase-A accumulate (which picks the filter
	// mode) and the phase-B render. What is actually baked is owned by the atlas
	// slot (GetSlotStaticState), so a tile realloc invalidates the cache without
	// this state going stale.
	struct SplitState
	{
		uint64_t pendingHash = 0;      ///< static hash observed on the latest accumulate
		bool bakeQueued = true;        ///< a rebake is due -- next accumulate is StaticOnly
		bool bakeThisFrame = false;    ///< this frame's accumulate was StaticOnly (render to cache)
		uint8_t mismatchStreak = 0;    ///< consecutive accumulates whose hash differed from the bake
		RE::NiPoint3 bakePos{};        ///< light position the static tile was baked at
		uint8_t poseRebakes = 0;       ///< pose-drift rebakes inside the current window
		uint32_t poseWindowStart = 0;  ///< frame the pose-rebake window opened
		bool splitExcluded = false;    ///< jitter outruns the bake's validity: render full, no split
		bool fullThisFrame = false;    ///< this frame's accumulate was All (cap/exclusion fallback)
		/// Last completed accumulate appended >= 1 dynamic caster. Defaults
		/// true (never sleep a light until an accumulate proves it moverless).
		bool sawDynamicLastAccum = true;
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

	// Cumulative schedule-time sleep skips since load (snapshot metric).
	std::atomic<uint64_t> s_sleepSkipTotal{ 0 };

	/// True when this light's single accumulate can run DynamicOnly: split
	/// cache on and usable for it, slot bake valid, pose within bake drift.
	/// Phase A (EnableLight) picks its filter mode through this and the
	/// schedule-time sleep skip reuses it, so the two can never drift apart.
	static bool SplitDynamicOnlyEligible(RE::BSShadowLight* light, const SplitState& st, bool staticValid)
	{
		if (!StaticAtlasReady())
			return false;
		if (light->GetIsFrustumLight())
			return false;
		if (st.splitExcluded || st.bakeQueued || !staticValid)
			return false;
		// Pose freshness: compositing movers over a bake taken at a drifted
		// pose shows two misaligned shadows at once (reads as extra darkness).
		if (auto* ni = light->light.get()) {
			const float px = ni->world.translate.x - st.bakePos.x;
			const float py = ni->world.translate.y - st.bakePos.y;
			const float pz = ni->world.translate.z - st.bakePos.z;
			if (px * px + py * py + pz * pz > kSplitPoseDriftMax * kSplitPoseDriftMax)
				return false;
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
		if (!SplitDynamicOnlyEligible(e.Light, st, staticValid))
			return false;
		// Staleness backstop: never skip once the backstop redraw is due,
		// and keep pressing for it every frame until the budget grants it.
		if (now - e.LastDrawnFrame >= kSleepRedrawIntervalFrames)
			return false;
		if (((now + slot * kSleepStaggerStride) % kSleepRedrawIntervalFrames) == 0)
			return false;
		return true;
	}

	// =========================================================================
	// Light enable / disable helpers
	// =========================================================================

	/// Removes `light` from s_normalConvert and clears its geometry list.
	/// No-op if the light is not in the list.
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

	// Activates a light as a normal (non-shadow) light by inserting it into
	// the scene's active-light list without allocating a shadow slot.
	//
	// Two paths: "already-converted re-enable" (just GameEnableLight) and
	// "first conversion this session" (ReturnShadowmaps + portal-clear +
	// track in s_normalConvert + GameEnableLight). Tracy sub-zones split
	// the cost so the next capture distinguishes the steady-state cost
	// (re-enable only) from the cost of a fresh conversion.
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

	// Activates a non-sun shadow light into slot `slotIndex`.
	static void EnableLight(RE::BSShadowLight* light, RE::NiCamera* camera,
		RE::ShadowSceneNode* ssn, int slotIndex)
	{
		// Remove from conversion list if it was previously converted to normal.
		EraseFromConvertList(light);

		// Focus shadow handling. Gated on s_focusShadowSlots so we only run
		// the engine's focus accumulate when ScheduleShadowCasters has
		// reserved [kFocusShadowBaseSlotIndex .. +s_focusShadowSlots) this
		// frame -- without that reservation the engine would write focus
		// depth into texture slices currently held by point lights. With
		// it, extended mode (ShadowLightCount > 4) is safe; the previous
		// blanket `<= 4` gate is replaced by the reservation contract.
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

		// Projected bounding box for shadow map region.
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

		// Accumulate into shadow slot. Publish the light so the parabolic
		// AppendVirtual hook can contribution-cull its casters; the RAII guard
		// clears it so the hook only acts during this light's accumulate.
		//
		// Static-cache split: pick this frame's single filter mode BEFORE the
		// (one) accumulate. A queued rebake makes it StaticOnly (bake the cache);
		// otherwise DynamicOnly (append only movers). The hook folds the static
		// hash either way; a change from the baked hash queues the next rebake.
		{
			// Frustum (spot/directional) lights are excluded from the split: the
			// dynamic/static caster classification runs only in the parabolic
			// (point-light) cull hook, so a spot's StaticOnly bake would capture
			// actors (baking a mover's silhouette in permanently) and never
			// track a sun-simulating spot's rotation. They render full instead.
			bool split = StaticAtlasReady() && !light->GetIsFrustumLight();
			SplitState* st = nullptr;
			CasterPass mode = CasterPass::All;
			uint64_t bakedHash = 0;
			bool staticValid = false;
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
				GetSlotStaticState(slotIndex, bakedHash, staticValid);
				// Pose drift past kSplitPoseDriftMax rebakes: this light is
				// redrawing anyway, so the bake replaces (not adds to) a render.
				mode = SplitDynamicOnlyEligible(light, *st, staticValid) ?
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
					if (auto* ni = light->light.get())
						st->bakePos = ni->world.translate;
					st->bakeQueued = false;
				}
				st->bakeThisFrame = (mode == CasterPass::StaticOnly);
				if (st->bakeThisFrame) {
					s_staticBakeCount.fetch_add(1, std::memory_order_relaxed);
					// Exclude a light that re-bakes for ANY reason (pose drift,
					// class oscillation, churn-invalidation): a pose-stable light
					// bakes once per window, so >=4 bakes in 300 frames means the
					// cache never holds for it -- render full and stop the storm.
					const uint32_t nowFrame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
					if (nowFrame < st->poseWindowStart || nowFrame - st->poseWindowStart > 300u) {
						st->poseWindowStart = nowFrame;
						st->poseRebakes = 0;
					}
					if (++st->poseRebakes >= 4)
						st->splitExcluded = true;
				}
				// Pose fold for bake validity. posStep is coarse (16 units) on
				// purpose: flame flicker jitters the light position a few units
				// every frame, and a 1-unit step re-hashed every jitter --
				// queueing a rebake per flicker and flashing the static-only
				// bake into the live tile on a visible cycle.
				s_visitStaticHash = 0x9e3779b97f4a7c15ull;
				if (auto* ni = light->light.get())
					s_visitStaticHash = FoldLightPose(s_visitStaticHash, ni, 16.0f);
				s_visitDynamicCount.store(0, std::memory_order_relaxed);
				s_cullPassMode.store(static_cast<int>(mode), std::memory_order_relaxed);
			}

			if (camera)
				s_cullCameraPos = camera->world.translate;  // viewer, for the caster cull
			s_currentCullLight.store(light, std::memory_order_relaxed);
			struct ClearCullLight
			{
				~ClearCullLight() { s_currentCullLight.store(nullptr, std::memory_order_relaxed); }
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
				// Rebuild missed attachments while this accumulate walks the
				// scene: the engine attaches geometry to lights only once per
				// geometry (kRenderUse latch), so a light created after the
				// scene attached -- every light of an in-game same-cell load --
				// otherwise casts nothing forever.
				s_healAttached.clear();
				s_accumRebuildAttach.store(light->geomList.empty(), std::memory_order_relaxed);
				s_cpuAccumUs.fetch_add(TimeUs([&] { light->Accumulate(idx, idx, nullptr); }), std::memory_order_relaxed);
				s_accumRebuildAttach.store(false, std::memory_order_relaxed);
				s_cpuAccumN.fetch_add(1, std::memory_order_relaxed);
				*GetAccumLightSlot() += light->shadowMapCount;
			}

			if (split) {
				st->pendingHash = s_visitStaticHash;
				// Latch mover presence for the sleep skip. Only passes that
				// could see movers may clear it: a StaticOnly bake filters
				// them before the count, and a deduped accumulate saw nothing.
				if (mode != CasterPass::StaticOnly && !duplicateAccum)
					st->sawDynamicLastAccum = s_visitDynamicCount.load(std::memory_order_relaxed) != 0;
				// A DynamicOnly accumulate observes the current static set; queue
				// a rebake only after the divergence PERSISTS. A flickering hash
				// that oscillates across the baked value resets the streak and
				// never rebakes; a genuine static-set change mismatches every
				// accumulate and rebakes after three.
				if (mode == CasterPass::DynamicOnly && st->pendingHash != bakedHash) {
					if (st->mismatchStreak < 0xFF)
						st->mismatchStreak++;
					if (st->mismatchStreak >= 3)
						st->bakeQueued = true;
				} else {
					st->mismatchStreak = 0;
				}
				s_cullPassMode.store(static_cast<int>(CasterPass::All), std::memory_order_relaxed);
			}
		}

		// Extended mode: pre-set kNONE renderTarget so RenderCascade re-runs
		// its slot-allocation block (where Hook_OverwriteShadowMapIndex
		// overrides the global counter with our slot index). Without this,
		// RenderCascade keeps the slot from a prior frame and lights not
		// redrawn this frame would corrupt another light's shadow map.
		// Pool index maps 1:1 to texture slot; slice 0 stays unused.
		if (s_settings.ShadowLightCount > 4)
			InitPromotedDescriptorSlots(light);

		// Only apply lens flare when lensFlareData is non-null; calling it on parabolic lights
		// (null lensFlareData) registers them into the lens flare system, causing a crash
		// in the lens flare pass when it tries to dereference the null sprite data.
		if (light->lensFlareData)
			GameApplyLensFlare(light);
	}

	// =========================================================================
	// Main shadow caster manager
	//
	// Replaces the game's CalculateActiveShadowCasterLights entirely.
	// Runs via stl::detour_thunk; obtains all inputs from game globals.
	// =========================================================================

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
	};

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
	// The scheduler (render thread) fills s_schedSnapshot under the mutex at pass end;
	// RequestSchedSnapshot (devbench listener thread) reads it under the same mutex.
	// s_schedDumpFrames latches a short window of passes to keep filling it after a
	// request, so polling returns fresh data even while the menu is closed.
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

	// Shadow-light usability SEH backstop. The ClearLightArrays teardown hook is
	// the primary defense (clears our pool when the engine bulk-frees lights); this
	// SEH catches any residual AV in the membership/usability scan so a missed edge
	// is a skipped light, not a CTD. Kept until broad validation lets it go.
	static void LogShadowSehCatch()
	{
		static std::atomic<int> n{ 0 };
		if (n.fetch_add(1, std::memory_order_relaxed) < 20)
			logger::warn("[SCM] SEH caught AV in shadow-light usability scan (probe missed); skipping light");
	}

	// SEH backstop in its own function (no C++ unwinding objects) so MSVC accepts
	// __try. Returns false (unusable) on any access violation.
	template <class Fn>
	static bool SafeUsable(Fn&& a_fn, RE::BSShadowLight* a_light)
	{
		__try {
			return a_fn(a_light);
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			LogShadowSehCatch();
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
			LogShadowSehCatch();
			return false;
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

		// Advance the caster-classification epoch once per frame here -- before
		// this frame's accumulate and its later render-split passes -- so a
		// caster classifies identically across both phases (a mid-frame bump
		// would double-count movement and flip casters static too early).
		// Periodically prune casters not seen for a while.
		if (++s_casterClassEpoch % 300 == 0) {
			std::erase_if(s_casterMobility,
				[](const auto& kv) { return s_casterClassEpoch - kv.second.lastEpoch > 300; });
			// Split state keys dead lights until this cap; state rebuilds
			// harmlessly, so a plain size-gated clear matches the sibling maps.
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

		// Drain a pending teardown reset before touching any slot, then SKIP this pass.
		// ClearLightArrays freed the previous scene's lights but doesn't shrink
		// activeShadowLights, so snapshotting or scoring it now would touch freed memory.
		// The engine left the array vanilla-valid this frame; the next pass runs once it is
		// rebuilt. (Load-bearing: removing this return reintroduces the 20 SEH catches.)
		if (s_pendingSessionReset.exchange(false, std::memory_order_acquire)) {
			ResetSession();
			return;
		}

		// Hold a strong ref to every active shadow light for the whole pass. The scheduler
		// walks raw BSShadowLight*, but a concurrent bulk cell-teardown (ReleaseChildren,
		// which bypasses our RemoveLight hook) can free one mid-pass; a later write through
		// the stale pointer corrupts the recycled occupant's vtable -> CTD. Snapshot now
		// while activeShadowLights is valid; local so it releases at every return path.
		std::vector<RE::NiPointer<RE::BSShadowLight>> heldRefs;
		{
			auto& alive = ssn->GetRuntimeData().activeShadowLights;
			heldRefs.reserve(alive.size() + 1);
			for (auto& sp : alive)
				if (sp)
					heldRefs.push_back(sp);
		}

		// Couple the shadow-cull distance to the light fade before the validation
		// pass runs UpdateCamera (which reads the cached square). No-op unless
		// MatchShadowToLightFade is enabled.
		ApplyShadowToLightFadeMatch();

		// Maintain the demotion diagnostics this pass only when something can read them:
		// the open settings menu (Conv tooltip) or a recent devbench dump request. Keeps
		// the per-light hash churn + snapshot copy off the hot path otherwise.
		const bool wantDiag = Menu::GetSingleton()->IsEnabled ||
		                      s_schedDumpFrames.load(std::memory_order_relaxed) > 0;

		// Read the engine's per-frame focus-shadow actor count and reserve
		// matching pool slots. Eject any point lights that occupy a slot the
		// engine now claims for focus rendering -- the displaced lights are
		// reassigned to a free slot or fall through to the existing excess
		// path. When the count drops, the slots naturally rejoin the pool's
		// FindFreeIndex range on the next allocation.
		s_focusShadowSlots = std::clamp(GetFocusShadowActorCount(), 0, kFocusShadowMaxSlots);
		for (int i = kFocusShadowBaseSlotIndex; i < kFocusShadowBaseSlotIndex + s_focusShadowSlots && i < s_lights.Size; ++i) {
			if (s_lights.Lights[i].Light)
				s_lights.Lights[i].Clear();
		}

		// Do NOT clear shadowLightsAccum or reset the slot counter here. The
		// outer CalculateAndDrawShadowCasterLights calls ResetCalculatedShadow-
		// CasterLights before our hook fires, and that function clears the
		// array, resets the counter, AND installs the sun at slot 0. Re-
		// clearing here wipes the sun (sun->Accumulate is the focus vfunc,
		// not a slot allocator) and the engine then skips the directional
		// cascade pass entirely.

		s_budget.Begin(0);

		int doneLightCount = 0;
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

		// Extended mode: scrub drawFocusShadows on every active light and the
		// sun. A stale flag on a parabolic (point/spot) light occupying a
		// kSHADOWMAPS slot in [4..7] sends BSShadowParabolicLight::Render
		// into its focus-shadow loop on a non-directional light and CTDs.
		// Mirrors Intellightent's mitigation (see main.cpp:1411-1420); the
		// byte patches at SetupResources are belt-and-braces for the engine's
		// global gate, this is belt-and-braces for the per-light flag.
		if (s_settings.ShadowLightCount > 4) {
			for (auto& sp : ssn->GetRuntimeData().activeShadowLights) {
				if (auto* l = sp.get())
					ShadowField(l, drawFocusShadows) = false;
			}
			if (auto* sun2 = ssn->GetRuntimeData().sunShadowDirLight)
				ShadowField(sun2, drawFocusShadows) = false;
		}

		*GetSunPtr() = 0;

		// ---- Score all candidate lights ----
		// Reuse a static vector so we don't allocate per frame -- the
		// scheduler runs every frame and the candidate list is the same
		// shape size each call (a few hundred lights at most).
		static std::vector<CandidateLight> candidates;

		{
			ZoneScopedN("SCM::ScoreCandidates");
			SetupSceneFormula(camera);

			candidates.clear();
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
					// Exit hysteresis, same shape as s_invalidStreak above: a light
					// hovering near the floor must fail 15 consecutive frames before
					// it's actually dropped, or it flaps its atlas slot every time it
					// dips back above and re-bakes on return.
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

		// Validation, redraw-interval scoring, and RedrawFrame marking all
		// happen before the atomic loop. Tracy capture analysis showed this
		// block dominates SCM::ScheduleShadowCasters (98%+ of the function's
		// runtime), so a dedicated zone scopes that cost separately from
		// ScoreCandidates and ScheduleLoop. Named variant because the
		// enclosing function already declares a ZoneScopedN.
		ZoneNamedN(zoneValBudget, "SCM::ValidateAndScheduleBudget", true);

		// Apply debug pins: bias scoring so pinned-shadow lights sort to the
		// top (forced into the chosen pool up to ShadowLightCount) and
		// pinned-convert lights sort to the bottom (forced into the excess pool
		// where ConvertLight runs unconditionally — see c.excess branch below).
		// Pin sets are mutually exclusive (SetPinned* enforces that), but if a
		// stale entry slips through, pin-shadow wins because the bias is checked
		// first.
		for (auto& c : candidates) {
			auto key = reinterpret_cast<uintptr_t>(c.light);
			if (s_pinShadow.count(key))
				c.score += 1e15;
			else if (s_pinConvert.count(key))
				c.score -= 1e15;
		}

		// Sort descending by score (highest priority first); sun always first.
		std::sort(candidates.begin(), candidates.end(),
			[](const CandidateLight& a, const CandidateLight& b) {
				if (a.sun != b.sun)
					return a.sun;
				return a.score > b.score;
			});

		// ---- Validation pass (no game mutations) ----
		//
		// Mirrors Intellightent's per-iteration validation gates. Splitting
		// validation from mutation lets us defer all game-state changes
		// (DisableLight / ConvertLight / EnableLight) to a single atomic loop
		// later, eliminating the dangling-pointer crash window where mutations
		// in an earlier phase invalidated raw pointers held in s_lights[].
		//
		// Slot 0 is reserved for the sun; point lights fill slots 1..ShadowLightCount.
		// Do not count the sun against ShadowLightCount -- it uses focus cascade DSV slots,
		// not parabolic point-light slots.
		auto* globalCull = *reinterpret_cast<RE::BSCullingProcess**>(
			*reinterpret_cast<uintptr_t**>(
				REL::RelocationID(528077, 415022).address()));

		int wantCount = 0;

		// Per-candidate UpdateCamera vfunc + portal-graph visibility walk
		// + chosen/excess tagging. Captured separately so memoization or
		// caching of UpdateCamera/portal verdicts can be measured.
		{
			ZoneNamedN(zoneCandVal, "SCM::CandidateValidation", true);
			for (auto& c : candidates) {
				auto* l = c.light;
				// UpdateCamera (vfunc 16, +0x80) is the engine's type-aware visibility
				// test. Verified via Ghidra (BSShadowParabolicLight_UpdateCamera at
				// 0x14151b620 in 1.6.1170, 0x14132ddf0 in 1.6.640, 0x141370c80 in VR):
				//
				//   - BSShadowParabolicLight: TWO cull conditions, both setting
				//     frustrumCull=0xff:
				//       (1) BSMultiBoundSphere::WithinFrustum (BSMultiBoundShape
				//           vfunc 0x29) -- sphere(niLight.pos, niLight.Radius.x)
				//           vs camera frustum. Geometrically correct;
				//           failure means no visible pixel can be lit because the
				//           light's bounding sphere doesn't touch the camera frustum.
				//           The radius source matches what the cluster builder reads
				//           (LightLimitFix.cpp's `runtimeData.radius.x`).
				//       (2) Shadow-distance LOD -- if (lodFade flag set on
				//           BSShadowLight) AND
				//           ((camDist^2 - radius^2) * camera.LodAdjust) >
				//               ShadowDistanceSquared_Current => cull.
				//           ShadowDistanceSquared_Current = fShadowDistance^2
				//           (8000^2 outdoors, 3000^2 indoors by default).
				//           This is NOT a visibility test -- it's "skip per-light
				//           shadow rendering at this distance". A light past
				//           shadow distance can still be IN the camera frustum and
				//           illuminating visible pixels via cluster lighting.
				//
				//   - BSShadowFrustumLight: cone-vs-frustum test (cone-aware so an
				//     off-screen spot pointing INTO the frustum is correctly kept).
				//
				//   - BSShadowDirectionalLight: cascades, separate code path.
				//
				// Implication for SCM: a `frustrumCull != 0` verdict does NOT mean
				// "geometrically off-screen". The convertOrDisable path below treats
				// all c.invalid cases uniformly (omnis convert, spots disable, portal
				// disable) so distant lights past shadow distance still reach the
				// cluster pipeline. The cluster builder's own
				// `(color * fade) > 1e-4 && radius > 1e-4` filter discards lights
				// that genuinely don't contribute.
				if (!l->UpdateCamera(camera)) {
					// Exit hysteresis: the gate's inputs (light position vs
					// frustum, distance vs fShadowDistance) are flicker-jittered,
					// so a boundary light flaps valid/invalid every frame. Honor
					// invalid only after it persists; a departed light drops 15
					// frames late, off-view anyway. Any valid frame resets it.
					PruneIfOversized(s_invalidStreak, 512);
					if (++s_invalidStreak[l] < 15)
						continue;
					c.invalidCamera = true;
					c.invalid = true;
					// Recover the sub-reason from the engine's side-band flags.
					// Both can be true (a light off-screen AND LOD-faded);
					// recorded as independent bits for analysis. Action loop
					// below treats frustum-out as terminal (drop) and
					// LOD-faded-in-frustum as convert.
					c.invalidFrustum = (l->frustrumCull != 0);
					c.invalidLod = (l->lodDimmer == 0.0f);
					continue;
				}
				s_invalidStreak.erase(l);
				// Portal culling only applies in interior cells where a portal graph exists.
				// Lights with no culling process (e.g. WSU spotlights outside cell bounds)
				// or no portal are unconditionally visible; skip the check for them.
				// Promoted lights carry a rebuilt culling process whose portal-graph entry the
				// engine never room-associates (always visibleUnboundSpace), so the test
				// false-culls an in-view light. The verdict is unreliable for them by
				// construction -- always skip the demotion; native lights still get it.
				auto* cull = IsPromotedLight(l->light.get()) ? nullptr : GetLightCullingProcess(l);
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

				// Impact floor: a below-floor light converts to a non-shadow
				// light (keeps diffuse via clusters, drops its shadow redraw),
				// the same path as an over-budget light. The table's "Low"
				// group-hover highlight (with the floor off) is the preview.
				if (c.belowFloor) {
					c.excess = true;
				}
				// Effective point-light capacity excludes the engine-claimed
				// focus shadow slots; excess candidates fall through to the
				// existing convert/disable path.
				else if (wantCount < s_settings.ShadowLightCount - s_focusShadowSlots) {
					c.chosen = true;
					wantCount++;
				} else {
					c.excess = true;
				}
			}

			// Tracy candidate breakdown: emits per-frame so a capture can be
			// queried alongside the per-action counters to verify the math
			// (chosen + excess + invalid_camera + invalid_portal == total).
			// Populate the demotion map only when wantDiag (menu open or a devbench
			// dump was requested) -- skip the per-frame hash churn otherwise.
			if (wantDiag)
				s_convertReason.clear();
			for (auto& c : candidates) {
				s_schedDiag.candidates_total++;
				if (c.chosen)
					s_schedDiag.candidates_chosen++;
				if (c.excess)
					s_schedDiag.candidates_excess++;

				// Capture why a non-chosen light is demoted, for the shadow table.
				// Portal wins (distinct disable path), then frustum/distance, LOD,
				// excess -- matching the atomic loop's branch precedence.
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
				// Sub-reason breakdown of invalidCamera. A single light may
				// be both frustum-out AND LOD-faded -- both bits are counted
				// so the sum can exceed candidates_invalid_camera. The
				// "other" bucket catches UpdateCamera failures where the
				// engine cleared frustrumCull and left lodDimmer > 0 (rare
				// edge cases like internal state changes).
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

		// Pool membership update: drop expired pointers, drop unchosen,
		// add newly chosen, sync sun slot.
		{
			ZoneNamedN(zonePoolMem, "SCM::UpdatePoolMembership", true);
			// ---- Sync s_lights (our active pool) ----
			//
			// First drop entries whose pointers are no longer in the scene's
			// activeShadowLights (game-side may have freed them since last frame).
			// This protects subsequent slot-stability lookups from dereferencing
			// dangling pointers.
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

			// ---- Sync s_normalConvert (converted-to-non-shadow set) ----
			//
			// Two-tier filter:
			//
			// Tier 1: drop entries the engine has removed from BOTH active
			// lists. Hook_ConvertLights_Remove fires on individual RemoveLight
			// calls but the engine's bulk cell-teardown path bypasses it, so
			// this is our safety net for dangling pointers.
			//
			// Tier 2: drop entries that are functionally dead -- still in
			// activeShadowLights / activeLights (because GameEnableLight from
			// ConvertLight activates an entry that the engine never
			// auto-deactivates), but with fade=0 / lodDimmer=0 / null NiLight
			// so addLight in LightLimitFix would skip them anyway.
			//
			// Without tier 2 the set grows unbounded across a session: every
			// converted light stays pinned in s_normalConvert until the engine
			// triggers a removal we can hook. Heavy modlists hit 400+ entries,
			// keeping freed-then-recycled BSLight memory referenced by
			// downstream pass captures longer than necessary. The criteria
			// mirror addLight's discard filter -- entries failing it
			// contribute nothing to the cluster or engine lighting paths and
			// have no business staying in our set.
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

			// Drop entries no longer chosen. Rank-drift suppression now lives
			// in CalculateLightScore via the lightframessincerender decay term
			// in the default ScoreFormula; the slot pool itself is a dumb
			// container that follows the chosen set without policy of its own.
			// The atomic loop's c.excess / c.invalid branches handle the
			// engine-side ConvertLight / DisableLight call for the dropped
			// occupants on the same frame.
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

			// Add newly chosen lights (assigned to first free slot; keeps existing chosen lights in place).
			for (auto& c : candidates) {
				if (!c.chosen)
					continue;
				bool alreadyIn = false;
				for (int i = 0; i < s_lights.Size && !alreadyIn; i++)
					if (s_lights.Lights[i].Light == c.light)
						alreadyIn = true;
				if (alreadyIn)
					continue;

				// Array parity on re-entry: a light returning after a brief
				// eviction (gate flap) reclaims the free slot that still holds
				// ITS OWN rendered tile -- content resumes sampling immediately,
				// exactly like an array slice surviving the round-trip. The
				// entry is left intact too (its cache keys are its own).
				int idx = -1;
				if (auto* ni = c.light->light.get()) {
					const int ownerIdx = FindFreeSlotByOwner(ni);
					if (ownerIdx >= 0 && !s_lights.Lights[ownerIdx].Light) {
						idx = ownerIdx;
						s_lights.Lights[idx].Light = c.light;
						continue;
					}
				}
				idx = s_lights.FindFreeIndex(true, s_settings.ShadowLightCount, s_settings.ConvertedShadowSlots);
				if (idx < 0)
					continue;
				// Eviction nulls Light* but leaves the rest of LightEntry intact
				// so it can serve as a cache key. Clear at acquire so the new
				// occupant doesn't inherit LastDrawnFrame / lastGeomHash from the
				// previous owner (which would skip its first render and let the
				// cluster pipeline sample stale kSHADOWMAPS[idx] content).
				s_lights.Lights[idx].Clear();
				// Drop the previous occupant's tile: its depth must not be
				// advertised under the new light's projection.
				FreeSlotTile(idx);
				s_lights.Lights[idx].Light = c.light;
			}

			// Update sun slot (slot 0).
			if (sunLight) {
				if (s_lights.Lights[0].Light != sunLight) {
					s_lights.Lights[0].Clear();
					s_lights.Lights[0].Light = sunLight;
				}
				s_lights.Sun = true;
			} else {
				// Sun is gone. If slot 0 was tracking the sun, clear the stale
				// pointer. If Sun was already false coming in, slot 0 holds a
				// regular point light (sun-aware FindFreeIndex allocates point
				// lights to slot 0 when Sun=false) -- do NOT wipe it. This
				// matches Intellightent's reference behaviour (no unconditional
				// slot-0 clear in the no-sun branch).
				if (s_lights.Sun)
					s_lights.Lights[0].Clear();
				s_lights.Sun = false;
			}

			// Publish each occupant's ScoreFormula value: the one priority that
			// ordered selection also orders the atlas cell budget (within a
			// class band) and drives the redraw curve percentile.
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

		// ---- Temporal budget: decide which lights redraw this frame ----
		double budget = s_settings.RedrawBudgetMs;
		{
			// Frame-time EMA + budget formula evaluation. Scoped separately
			// from ScheduleLoop so the once-per-frame budget cost is visible
			// distinct from the per-light scheduling cost.
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
					budget = s_formulaRedrawBudget->Calculate();
				}
				s_autoBudgetMs = static_cast<float>(budget);
			}  // end SCM::ComputeBudget

			s_redrawnLightsThisFrame = 0;
			s_totalShadowLightsThisFrame = s_settings.ShadowLightCount;

			ZoneScopedN("SCM::ScheduleLoop");
			int maxRedraw = std::min(s_settings.MaxRedrawPerFrame, s_lights.Size);
			int32_t budgetRemain = static_cast<int32_t>(budget * 1000.0);
			bool isFirst = true;
			int32_t now = *globals::game::frameCounter;

			// Clear RedrawFrame on slots OUTSIDE the point-light range (converted /
			// otherwise-allocated). Note PointLightEnd accounts for the sun
			// bookkeeping slot when Sun=true, so a converted-slot light at
			// pool[ShadowLightCount + 1] correctly gets cleared.
			for (int i = s_lights.PointLightEnd(s_settings.ShadowLightCount); i < s_lights.Size; i++)
				s_lights.Lights[i].RedrawFrame = false;

			// First pass: sun only. Point-light slots fall through to the
			// importance-scored pending loop below so new lights compete
			// fairly with existing redraws (sorted by importance, not pool
			// order). AllowDrawNewLight is honoured by the pending loop's
			// filter.
			for (int i = 0; i < s_lights.Size; i++) {
				auto& e = s_lights.Lights[i];
				if (!e.Light) {
					e.RedrawFrame = false;
					continue;
				}
				e.RedrawFrame = (i == 0 && s_lights.Sun);
				if (e.RedrawFrame) {
					e.LastDrawnFrame = now;
					isFirst = false;
					maxRedraw--;
					// Sun's budget cost is bookkept at 0 (different texture
					// pipeline -- it has its own cascade buffer), so no
					// budgetRemain decrement.
				}
			}

			if (maxRedraw > 0 && budgetRemain > 0) {
				std::vector<LightEntry*> pending;
				for (int i = 0; i < s_lights.Size; i++) {
					auto& e = s_lights.Lights[i];
					if (!e.Light || e.RedrawFrame)
						continue;
					// Honour AllowDrawNewLight: when disabled, brand-new
					// entries (LastDrawnFrame < 0) wait until the next frame
					// rather than competing for this frame's budget. Existing
					// lights re-entering view still schedule normally.
					if (!s_settings.AllowDrawNewLight && e.LastDrawnFrame < 0)
						continue;
					// Empty-dynamic sleep: a moverless light with a valid, fresh
					// static bake would redraw an identical tile -- skip it
					// entirely (no accumulate, no budget); it keeps sampling its
					// cached tile via the non-redrawn insertion path.
					if (SleepSkipEligible(e, i, now)) {
						s_schedDiag.sleep_skips++;
						s_sleepSkipTotal.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					pending.push_back(&e);
				}

				// Base texels for the coverage classifier; lazily captured from
				// the live texture, so fall back until it is readable.
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
				auto scorePercentile = [&](double score) -> float {
					if (scoreRank.size() < 2)
						return 1.0f;
					const auto it = std::lower_bound(scoreRank.begin(), scoreRank.end(), score);
					return static_cast<float>(it - scoreRank.begin()) / static_cast<float>(scoreRank.size() - 1);
				};

				for (auto* e : pending) {
					double interval = 0.0;
					if (s_formulaRedrawInterval) {
						SetupLightFormula(e->Light, camera, 0);
						// e->Index is the pool index. Beyond PointLightEnd are converted slots.
						if (e->Index >= s_lights.PointLightEnd(s_settings.ShadowLightCount))
							FormulaHelper::SetParam(kFormulaParam_LightConverted, 1.0);

						// Compute how far the light has moved since its last shadow map render.
						// Exposed as `lightdisplacement` so the formula can prioritise fast-moving
						// lights (e.g. player torches) without relying on distance-to-camera alone.
						if (auto* nilight = e->Light->light.get()) {
							auto& curr = nilight->world.translate;
							float dx = curr.x - e->lastRenderedPos.x;
							float dy = curr.y - e->lastRenderedPos.y;
							float dz = curr.z - e->lastRenderedPos.z;
							FormulaHelper::SetParam(kFormulaParam_LightDisplacement,
								static_cast<double>(sqrtf(dx * dx + dy * dy + dz * dz)));
						}

						interval = s_formulaRedrawInterval->Calculate();
					}
					interval += 1.0;

					// Contribution-weighted redraw interval:
					//   importance = luminance(diffuse × fade) × max(att_cam, att_plr)
					//   att(pos)   = max(1 - (dist/radius)^2, 0)^2     (Skyrim falloff)
					//   interval  *= 2.0 * (0.025/2.0)^importance
					// importance=0 -> x2.0 (deprioritise), 0.5 -> ~x0.32, 1.0 -> ~x0.05.
					// Refs: Wimmer & Scherzer 2006 "Instant Shadow Maps" sec. 3;
					//       Valient 2014 "Practical Shadow Maps".

					float importance = 0.0f;
					float sizeProxy = 0.0f;

					if (auto* ni = e->Light->light.get()) {
						const auto geom = ComputeLightGeometry(ni, camera, ni->GetLightRuntimeData().radius.x);
						// Legacy contribution metric, kept as the lightimportance
						// formula variable; ranking decisions use lastScore (the
						// ScoreFormula value) so one function owns priority.
						importance = geom.lum * std::max(geom.coverage, std::max(geom.attCam, geom.attPlr) * 0.3f);
						sizeProxy = geom.sizeProxy;
						// A light that reaches neither the camera nor the player
						// cannot show a close-up shadow: cap its tile class so
						// out-of-range embedded lights (large radius, zero
						// attenuation at the viewer) stop hoarding full tiles the
						// rank budget then can't give to visible lights.
						if (geom.attCam <= 0.0f && geom.attPlr <= 0.0f)
							sizeProxy = std::min(sizeProxy, 0.25f);
					}

					// Exponential interval scaling driven by the light's priority
					// PERCENTILE among currently slotted lights (scale-invariant
					// to user formula rescaling): maxScale*(minScale/maxScale)^pct.
					float kMaxMult = s_settings.ImportanceMaxScale;
					float kMinMult = std::min(s_settings.ImportanceMinScale, kMaxMult);
					float clampedImp = scorePercentile(e->lastScore);
					interval *= static_cast<double>(kMaxMult * powf(kMinMult / kMaxMult, clampedImp));

					FormulaHelper::SetParam(kFormulaParam_LightImportance, static_cast<double>(importance));
					e->RedrawScore = e->LastDrawnFrame + interval;
					e->lastImportance = importance;

					e->desiredScale = AtlasActive() ?
					                      TileScaleForCoverage(sizeProxy, baseTileTexels, e->desiredScale) :
					                      1.0f;
					// Atlas mode: the render-pass rank budget owns pendingScale.
					if (!AtlasActive())
						e->pendingScale = e->desiredScale;

					// Position step scaled to the tile class: at 128px a fire's
					// flicker orbit is sub-texel and must not bust the cache;
					// at full class the same motion is visible and should.
					float posStep = 1.0f;
					if (auto* ni2 = e->Light->light.get()) {
						const float texels = baseTileTexels * std::max(e->pendingScale, kTileScaleFloor);
						posStep = ni2->GetLightRuntimeData().radius.x / std::max(texels, 1.0f);
					}
					// ComputeShadowGeomHash's full geomList walk measured ~17us/light;
					// reuse the cached hash until the caster count changes or this many
					// frames pass. Winners latch this value below (see latchGeomHash).
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
					e->pendingGeomHash = e->cachedPendingGeomHash;
					// Starvation backstop: an unchanged hash deprioritises a redraw, but a
					// perpetually-losing light's geomList never refreshes to prove otherwise.
					if (e->LastDrawnFrame >= 0 && e->lastGeomHash != 0 &&
						e->pendingGeomHash == e->lastGeomHash &&
						e->pendingScale == e->renderedScale &&
						(now - e->LastDrawnFrame) < kSleepRedrawIntervalFrames) {
						e->RedrawScore += 1e15;
					}
				}

				// Count lights meaningfully illuminating the viewer area.
				s_highImportanceLightCount = static_cast<uint32_t>(
					std::count_if(pending.begin(), pending.end(),
						[](const LightEntry* e) { return e->lastImportance > 0.1f; }));

				std::sort(pending.begin(), pending.end(),
					[](const LightEntry* a, const LightEntry* b) { return a->RedrawScore < b->RedrawScore; });

				// Winners latch the scoring-pass hash: lastGeomHash staleness is
				// bounded by kGeomHashRehashInterval, inside the
				// kSleepRedrawIntervalFrames backstop.
				auto latchGeomHash = [](LightEntry* e) {
					e->lastGeomHash = e->pendingGeomHash;
				};

				for (auto* e : pending) {
					if (maxRedraw <= 0)
						break;
					if (budgetRemain <= 0)
						break;
					int32_t budgetEstimate = s_budget.GetCost(e->Light);
					if (isFirst) {
						if (!s_lights.Sun || e->Index > 0)
							budgetRemain -= budgetEstimate;
						maxRedraw--;
						e->RedrawFrame = true;
						e->LastDrawnFrame = now;
						latchGeomHash(e);
						isFirst = false;
						continue;
					}
					if (budgetEstimate <= budgetRemain) {
						budgetRemain -= budgetEstimate;
						maxRedraw--;
						e->RedrawFrame = true;
						e->LastDrawnFrame = now;
						latchGeomHash(e);
						continue;
					}
				}
			}
		}

		// Count how many shadow lights are scheduled to redraw this frame.
		// Iterate the point-light range (sun-aware: skips pool[0] when Sun=true).
		s_redrawnLightsThisFrame = 0;
		for (int j = s_lights.PointLightFirst(); j < s_lights.PointLightEnd(s_settings.ShadowLightCount); j++) {
			if (s_lights.Lights[j].RedrawFrame)
				++s_redrawnLightsThisFrame;
		}

		// EWMA so the UI counter doesn't flicker frame-to-frame.
		s_redrawnLightsSmoothed = 0.8f * s_redrawnLightsSmoothed + 0.2f * s_redrawnLightsThisFrame;

		// Atomic per-candidate loop: process each score-sorted candidate to
		// completion before moving on. Branch dispatch:
		//   chosen + RedrawFrame + slot in budget: EnableLight + render
		//   chosen otherwise:                      DisableLight (re-added below
		//                                          via GameSetShadowCasterSlot)
		//   excess + ConvertExcessToNormal:        ConvertLight
		//   excess otherwise / invalid:            DisableLight
		//
		// Ordering matters: chosen (rank < ShadowLightCount) runs before any
		// excess. ConvertLight's ReturnShadowmaps can mutate activeShadowLights
		// and free other BSShadowLights, but by then chosen entries have
		// already completed EnableLight + budget pairing in-iteration -- no
		// later phase walks those pointers.
		//
		// isUsableLight() per-iteration guard catches dangling pointers if an
		// earlier EnableLight invalidated a later candidate via scene mutation.

		auto* shadowSceneNodeRT = &ssn->GetRuntimeData();

		// Two-stage validity check used before any virtual dispatch on a
		// BSShadowLight from s_lights[] or candidates[]:
		//   (1) Is the pointer still in the scene's activeShadowLights?
		//       (catches "removed since last frame")
		//   (2) Is the vtable non-zero?
		//       (catches "freed and zeroed by tbbmalloc / EngineFixes via a path
		//        that bypassed BSSmartPointer ref-counting" — the pointer is
		//        still in activeShadowLights but the object is dead)
		// Either failure → caller must skip the light.
		auto isAliveNow = [shadowSceneNodeRT, sunLight](RE::BSShadowLight* l) -> bool {
			if (!l)
				return false;
			if (l == sunLight)
				return true;
			// Membership scan over activeShadowLights. The ClearLightArrays
			// teardown hook (+ ResetSession/skip) keeps us from scanning a
			// torn-down array; SafeUsable (SEH) is the remaining backstop.
			for (auto& sp : shadowSceneNodeRT->activeShadowLights)
				if (sp.get() == l)
					return true;
			return false;
		};
		auto isVtableValid = [](RE::BSShadowLight* l) -> bool {
			return l && *reinterpret_cast<const uintptr_t*>(l) != 0;
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

		// Single decision point for "this light won't shadow this frame --
		// Convert (keeps diffuse via cluster pipeline) or Disable (light
		// vanishes)?". Used by both the c.invalid and c.excess branches.
		//
		// Spots always Disable: the engine has no NiSpotLight equivalent, so
		// ConvertLight on a BSShadowFrustumLight would make the cone-shaped
		// illumination spherical and bleed through walls behind the cone.
		// Omnis/hemis Convert when ConvertExcessToNormal is on or a debug
		// pin-convert is set on this light. The pin override applies even
		// when the user disabled ConvertExcessToNormal globally.
		//
		// allowConvert is a callsite veto -- the c.invalid path passes it
		// false for invalidPortal (cluster has no portal-graph awareness,
		// converting would leak light across cells) so portal-occluded
		// lights always Disable.
		//
		// Returns true on Convert, false on Disable, so callers can apply
		// path-specific follow-ups (e.g. lodDimmer=1 reset on the invalidLod
		// path so the converted light still contributes to clusters).
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

		// Sun slot (slot 0) is processed inline below — sun setup happened at the
		// top of the function; we only need to mark its mask index here.
		if (s_lights.Sun && s_lights.Lights[0].Light && s_lights.Lights[0].RedrawFrame) {
			ShadowField(s_lights.Lights[0].Light, maskIndex) = 0;
			doneLightCount++;
		}

		// Per-candidate Begin/EnableLight/End mutation loop. EnableLight may
		// trigger synchronous shadow render dispatches in the engine, so this
		// zone captures both our scheduler work and any engine-side rendering
		// it pulls in for chosen lights.
		{
			// Hold the graph lock shared for the whole mutation loop so ResetScene (main thread)
			// can't null ssn->portalGraph while the engine mutations below (ConvertLight /
			// DisableLight / EnableLight -> AccumulateLight) deref it. try_to_lock: skip the
			// pass if a teardown holds it exclusive, rather than block.
			std::shared_lock<std::shared_mutex> graphLock(s_portalGraphMutex, std::try_to_lock);
			if (!graphLock.owns_lock())
				return;
			if (IsPortalGraphTransitioning())  // re-check once; stable now under the lock
				return;
			ZoneNamedN(zoneAtomic, "SCM::AtomicMutationLoop", true);
			for (auto& c : candidates) {
				if (c.invalid) {
					// isUsableLight (membership + vtable) is the same gate the
					// excess branch uses. Both ConvertLight and DisableLight
					// fan into virtually-dispatched callees (ReturnShadowmaps),
					// so a freed-but-canonical pointer must be skipped for
					// either path.
					if (!isUsableLight(c.light))
						continue;

					// All c.invalid cases route through convertOrDisable. Per the
					// Ghidra-verified UpdateCamera analysis above, frustrumCull
					// is set both by the genuine sphere-vs-frustum cull AND by
					// the shadow-distance LOD cull; treating them uniformly lets
					// distant lights past shadow distance still reach the
					// cluster pipeline. allowConvert=c.invalidCamera so portal-
					// occluded omnis fall to Disable (cluster lighting has no
					// portal-graph awareness and would leak across cells).
					ZoneNamedN(zCvt, "SCM::Engine::convertOrDisable(invalid)", true);
					if (convertOrDisable(c.light, /*allowConvert=*/c.invalidCamera)) {
						s_schedDiag.converted_invalid++;
						// UpdateCamera zeros lodDimmer on its shadow-distance LOD cull; the cluster
						// builder multiplies fade*lodDimmer and drops the light below 1e-4. Restore
						// only when fully zeroed (preserves any smooth fade), matching UpdateLights.
						// Re-validate first: a concurrent free could recycle c.light, and a 1.0f store
						// would corrupt the new occupant's vtable -> CTD (heldRefs should cover it).
						if (SafeUsable(isUsableLight, c.light) && c.light->lodDimmer == 0.0f)
							c.light->lodDimmer = 1.0f;
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
					// (excludes converted slots which start at PointLightEnd). Use
					// the sun-aware bound so pool[ShadowLightCount] (the highest
					// point-light slot when Sun=true) is included.
					if (e.RedrawFrame && slot < s_lights.PointLightEnd(s_settings.ShadowLightCount)) {
						// Render-this-frame path. A previous iteration's EnableLight
						// may have transitively freed this light via game-side scene
						// mutations (membership change OR tbbmalloc-zeroed memory),
						// so re-validate before any virtual dispatch.
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
					// Cached-shadow path (chosen + !RedrawFrame, or i >= ShadowLightCount):
					// do nothing here. The non-redrawn light keeps its stale shadow map and
					// is re-inserted by the GameSetShadowCasterSlot loop below at endIdx.
					// Calling DisableLight here would invoke ReturnShadowmaps, releasing the
					// cached shadow data for one frame and producing visible flicker that
					// worsens as the budget gets more constrained.
					continue;
				}

				if (c.excess) {
					if (!isUsableLight(c.light))
						continue;

					// Atomic ordering: by the time we reach excess (rank
					// >= ShadowLightCount), all chosen lights have completed
					// their Begin/EnableLight/End sequence. ConvertLight's
					// ReturnShadowmaps side effect can only invalidate
					// pointers we are no longer walking. LightLimitFix::
					// UpdateLights then iterates activeShadowLights to pick
					// up converted lights for the cluster pipeline.
					//
					// Rank-drift suppression (a torch's importance score
					// bobbing across the chosen/excess boundary frame-to-
					// frame) lives in the score formula via the
					// lightframessincerender decay term, not here.
					ZoneNamedN(zCvt, "SCM::Engine::convertOrDisable(excess)", true);
					if (convertOrDisable(c.light, /*allowConvert=*/true))
						s_schedDiag.converted_excess++;
					else
						s_schedDiag.disabled_excess++;
					continue;
				}
			}
		}  // end SCM::AtomicMutationLoop

		// Non-redrawn chosen lights: insert at end of shadow caster array without rendering.
		// GetAccumLightSlot() already advanced past all EnableLight()-rendered slots.
		//
		// Re-rebuild the alive set: the atomic loop above may have invalidated
		// pointers (e.g. ConvertLight on excess removes from activeShadowLights).
		// Skip s_lights entries whose pointer is no longer in the scene to avoid
		// dereferencing freed BSShadowLight memory below.
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
				// Re-insert (without rendering) every chosen+!RedrawFrame light
				// AND every converted-slot light (i >= PointLightEnd). The
				// PointLightEnd bound is sun-aware so converted slots correctly
				// start one slot later when Sun=true.
				if (e.Light && (!e.RedrawFrame || i >= s_lights.PointLightEnd(s_settings.ShadowLightCount))) {
					// Membership check uses the snapshot built above (a
					// game-mutation in the atomic loop may have invalidated
					// pointers; aliveAfterAtomic captures the current scene
					// state in O(N) for O(1) membership queries here).
					if (aliveAfterAtomic.find(e.Light) == aliveAfterAtomic.end()) {
						s_schedDiag.reconciliation_clears++;
						e.Clear();
						continue;
					}
					// First-render gate: a chosen light whose slot has never
					// been rendered for IT (LastDrawnFrame < 0) has no valid
					// shadow content in its kSHADOWMAPS slice -- the depth
					// content is either cleared or carries the evicted
					// previous occupant's shadow. Inserting the light as a
					// shadow caster would make the cluster shader sample stale
					// depth and project a wrong shadow shape through the new
					// light. Skip insertion this frame; the light still
					// illuminates via the cluster pipeline as a non-shadow
					// light, with no false shadow. Once it wins a redraw turn
					// LastDrawnFrame goes >= 0 and it joins the shadow set
					// normally.
					//
					// Converted-slot range (i >= PointLightEnd) is unaffected:
					// converted lights don't sample kSHADOWMAPS via this slot
					// path; they participate via the s_normalConvert non-shadow
					// pipeline.
					if (i < s_lights.PointLightEnd(s_settings.ShadowLightCount) &&
						e.LastDrawnFrame < 0 &&
						!(s_lights.Sun && i == 0)) {
						s_schedDiag.first_render_skips++;
						continue;
					}

					// Cached-shadow reuse (the UE5 / CryEngine / Frostbite
					// pattern). We unconditionally sample the cached
					// kSHADOWMAPS slice even when the geometry hash mismatches
					// (light or caster moved since the cached render). For
					// small motion the staleness is sub-pixel and invisible;
					// for large motion the shadow visibly lags the light by
					// 1-2 frames, which is much less objectionable than the
					// full-frame on/off flicker that hash-gated suppression
					// produces on every animated torch. The hash-mismatch
					// priority hint above keeps stale entries at the front of
					// the redraw queue, so the lag self-corrects within budget
					// cycles.
					//
					// The first_render_skips gate above is the only safety
					// gate that DOES suppress insertion: a slot with no
					// rendered content for its current owner (LastDrawnFrame
					// < 0) has no valid cached shadow to fall back on; the
					// GPU slice is either cleared or contains an evicted
					// previous occupant. Hash mismatch on an existing slice
					// is at worst a small visual lag.
					// GameSetShadowCasterSlot calls Accumulate virtually; reuse
					// isUsableLight's vtable guard to catch tbbmalloc-zeroed
					// objects that are still in activeShadowLights but freed.
					if (!isVtableValid(e.Light)) {
						e.Light = nullptr;
						continue;
					}
					GameSetShadowCasterSlot(ssn, e.Light, endIdx, 1);
					// Same hazard as the post-EnableLight site: the engine can
					// free the light during this call. Use isUsableLight, not
					// just null check.
					if (!e.Light || !SafeUsable(isUsableLight, e.Light))
						continue;
					endIdx += e.Light->shadowMapCount;
					ShadowField(e.Light, maskIndex) = static_cast<uint32_t>(i);

					// GameSetShadowCasterSlot (via Accumulate) overwrites shadowmapIndex
					// with the sequential endIdx counter, diverging from the stable
					// container-slot index that CopyShadowLightData and Prepass expect.
					// All shadow-slot light types are affected:
					//   Spot (!IsParabolicLight): 1 descriptor, 1 atlas slice.
					//   Hemi (IsParabolicLight && !IsOmniLight): 1 descriptor, 1 atlas slice.
					//   Omni (IsParabolicLight && IsOmniLight): both paraboloids packed into
					//     a single atlas slice via UV splitting in GetOmnidirectionalShadow,
					//     so all descriptors should also point to i.
					// Restore shadowmapIndex = i for every non-redrawn shadow-slot light.
					// Only restore shadowmapIndex for point-light slots (skip converted).
					// PointLightEnd accounts for sun bookkeeping so the highest point-light
					// slot (Sun=true: pool[ShadowLightCount]) is included.
					if (s_settings.ShadowLightCount > 4 && i < s_lights.PointLightEnd(s_settings.ShadowLightCount)) {
						// Restore descriptor.shadowmapIndex for cached (non-redrawn)
						// chosen lights so RenderCascade samples their preserved
						// depth slice. Sun (pool[0] when Sun=true) is skipped —
						// it renders via the directional cascade path, not
						// kSHADOWMAPS, so its descriptor.shadowmapIndex is unused.
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

		// =====================================================================
		// Tracy per-frame plots: scheduler diagnostic counters + live config.
		// Emitting both in the same frame lets a capture be queried for A/B
		// behaviour without re-running the game: the cfg_* plots are the
		// independent variables, the scm.* plots are the dependent outcomes.
		// =====================================================================
		{
			// Read + reset the per-frame caster-cull count here (unconditionally,
			// not inside TracyPlot's arg -- that expression is elided in non-Tracy
			// builds, which would leak the counter).
			// [[maybe_unused]]: consumed only by TracyPlot below, which is elided
			// in non-Tracy builds -- but the exchange must still run to reset the
			// counters, so they're read here unconditionally.
			[[maybe_unused]] const uint32_t culledThisFrame = s_casterCullCount.exchange(0, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t poolDropsThisFrame = s_cullPoolDropCount.exchange(0, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t staticDraws = s_staticCasterDraws.exchange(0, std::memory_order_relaxed);
			[[maybe_unused]] const uint32_t dynamicDraws = s_dynamicCasterDraws.exchange(0, std::memory_order_relaxed);
			// Accumulated (not just plotted): the snapshot publishes the running
			// total so a headless A/B can difference it across a run.
			const uint32_t bakesThisFrame = s_staticBakeCount.exchange(0, std::memory_order_relaxed);
			if (bakesThisFrame)
				s_staticBakeTotal.fetch_add(bakesThisFrame, std::memory_order_relaxed);

			// Sample slot occupancy at frame end (post-reconciliation).
			for (int i = 0; i < s_lights.Size; i++)
				if (s_lights.Lights[i].Light)
					s_schedDiag.slots_in_use++;

			// Publish the scheduling snapshot for headless inspection (devbench
			// inspect kind=llfshadows). wantDiag already gated the per-light reason
			// capture above; copy + swap under the lock so the listener thread reads a
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
					AtlasTileTexels tile{};
					if (GetSlotTileTexels(i, tile)) {
						st.tileX = tile.x;
						st.tileY = tile.y;
						st.tileSize = tile.size;
						st.tileContentValid = tile.contentValid;
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
				snap.atlasDim = AtlasDim();
				snap.atlasCapacityCells = AtlasCapacityCells();
				snap.atlasOccupancy = AtlasOccupancy();
				snap.atlasVramBytes = AtlasVRAMBytes();
				const auto clearStats = GetAtlasClearStats();
				snap.atlasTileReallocs = clearStats.tileReallocs;
				snap.atlasOwnerInvalidations = clearStats.ownerInvalidations;
				if (const uint32_t n = s_cpuAccumN.exchange(0, std::memory_order_relaxed))
					snap.cpuAccumUsAvg = static_cast<uint32_t>(s_cpuAccumUs.exchange(0, std::memory_order_relaxed) / n);
				if (const uint32_t n = s_cpuSubmitN.exchange(0, std::memory_order_relaxed))
					snap.cpuSubmitUsAvg = static_cast<uint32_t>(s_cpuSubmitUs.exchange(0, std::memory_order_relaxed) / n);
				if (const uint32_t n = s_cpuEnableN.exchange(0, std::memory_order_relaxed))
					snap.cpuEnableUsAvg = static_cast<uint32_t>(s_cpuEnableUs.exchange(0, std::memory_order_relaxed) / n);
				snap.avgLightCostUs = s_budget.GetAverageCostUs();
				snap.avgRedrawsPerFrame = static_cast<float>(s_redrawSum) / static_cast<float>(kRedrawHistorySize);
				snap.staticBakesTotal = s_staticBakeTotal.load(std::memory_order_relaxed);
				snap.sleepSkips = s_schedDiag.sleep_skips;
				snap.sleepSkipsTotal = s_sleepSkipTotal.load(std::memory_order_relaxed);
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
		}
	}

	// =========================================================================
	// Render hook: replaces RenderActiveShadowCasterLights
	// Iterates s_lights and calls Render() on lights flagged RedrawFrame.
	// Uses install_context_hook at a specific call site in the render loop (see Install()).
	// =========================================================================

	void RenderScheduledShadowLights()
	{
		// A cell-transition teardown (ClearLightArrays) frees the engine accumulator's renderPass
		// storage. A teardown landing between schedule and this pass would flush a freed accumulator
		// -> AV in BSBatchRenderer. Skip while a reset is pending; ScheduleShadowCasters owns the
		// drain next frame -- only LOAD here, never exchange, or s_lights would never reset.
		if (s_pendingSessionReset.load(std::memory_order_acquire))
			return;

		// Pause while the interior portal graph is mid-rebuild (cell transition);
		// BSShadowLight::Render walks portal culling that derefs ssn->portalGraph.
		if (IsPortalGraphTransitioning())
			return;

		// Reader side of the teardown serialization: ClearLightArrays must not
		// free lights/passes while this pass iterates them. Counter (not a
		// shared_mutex) so nested engine re-entry on this thread stays defined;
		// skip the pass outright when a teardown is already waiting.
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

		// Atlas rank budget: in importance order, each light gets the biggest
		// class that still leaves a quarter cell for every lower-ranked
		// light; without it, first arrivals hoard full tiles and later
		// lights get no tile at all.
		if (AtlasActive()) {
			static std::vector<LightEntry*> ranked;
			ranked.clear();
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++)
				if (s_lights.Lights[i].Light)
					ranked.push_back(&s_lights.Lights[i]);
			// Two-key rank: geometry picks the class band, priority orders
			// within it; score jitter can never reorder across bands, which
			// keeps tile assignments cache-stable.
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
				// No demotion hold: holding pendingScale above the budget
				// target across frames reproducibly crashes the engine batch
				// renderer (either cell-accounting variant); root-cause before
				// reintroducing (reproducer: Dragonsreach recorded replay).
				//
				// Promotion hold is the opposite direction and safe: adopt a
				// LARGER class only after it stays wanted ~60 frames. Flicker
				// jitter otherwise oscillates the class across a boundary, and at
				// high occupancy each promotion realloc darkens a reclaim victim.
				float target = std::min(e->desiredScale, scale);
				if (e->pendingScale > 0.0f && target > e->pendingScale) {
					if (++e->promoteStreak < 60) {
						target = e->pendingScale;
					} else {
						e->promoteStreak = 0;
					}
				} else {
					e->promoteStreak = 0;
				}
				e->pendingScale = target;
				cellsLeft -= std::min(cellsLeft, CellsForScale(scale));
			}
		}

		// VR: RenderActiveShadowCasterLights normally saves+clears g_drawStereo before
		// iterating shadow casters, then restores it. Without this, each hemisphere
		// render is doubled for both eyes -> 4-quadrant shadow map texture.
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
		// Sun first: BSShadowDirectionalLight::Render emits the "Directional
		// Light Shadowmaps" marker and writes the cascade depth maps to
		// kSHADOWMAPS_ESRAM. The engine's vanilla RenderActiveShadowCasterLights
		// dispatches this via the same vtable walk it uses for point lights;
		// we replaced that walk with this loop, so we need to call sun.Render
		// explicitly. Without this, the directional cascade pass is skipped
		// and exterior scenes render with no sun shadow.
		if (s_lights.Sun && s_lights.Lights[0].Light &&
			!s_pendingSessionReset.load(std::memory_order_acquire)) {
			ZoneNamedN(zSun, "SCM::Render::Sun", true);
			CS_GPU_PASS("SCM::Render::Sun");
			s_budget.BeginLight(s_lights.Lights[0].Light, 1);
			s_lights.Lights[0].Light->Render(tmp);
			s_budget.EndLight(s_lights.Lights[0].Light, 1);
		}

		// Point lights from PointLightFirst onwards. PointLightFirst skips
		// slot 0 (handled above when Sun=true). PointLightEnd includes the
		// highest point-light slot when Sun=true.
		{
			ZoneNamedN(zPoint, "SCM::Render::PointLights", true);
			CS_GPU_PASS("SCM::Render::PointLights");
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
				// A teardown can begin mid-pass; ClearLightArrays sets the flag at its
				// entry before freeing, so bailing here stops flushing into an accumulator
				// being torn down (narrows the window to a single in-flight Render call).
				if (s_pendingSessionReset.load(std::memory_order_acquire))
					break;
				auto& e = s_lights.Lights[i];
				if (!e.Light || !e.RedrawFrame)
					continue;
				bool keepPriorContent = false;
				bool emptyRender = false;
				if (AtlasActive()) {
					// Tile before raster: (re)size for the pending class and
					// rect-clear once per redraw -- the paraboloid halves share
					// the tile, so clearing inside the cascade would wipe the
					// first half.
					if (!EnsureSlotTile(i, e.pendingScale)) {
						// Atlas exhausted even at the quarter class: the raster must
						// still run. EnableLight already registered this light's passes,
						// and an unconsumed group's passes are freed-but-not-unlinked at
						// frame end; a recycled pass re-registered by RegisterPassSorted
						// closes passGroupNext into a ring (RenderBatches never returns).
						// The viewport hook collapses a tile-less raster to zero size,
						// so consuming the group draws nothing. No tile marks are set,
						// so RedrawFrame keeps the retry.
						s_budget.BeginLight(e.Light, 1);
						s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);
						s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
						s_budget.EndLight(e.Light, 1);
						continue;
					}
					// Split cache: the light's single accumulate this frame was
					// filtered in EnableLight. On a bake frame render the static
					// subset into the cache atlas; otherwise copy the cache into
					// the tile and composite the movers over it (no clear -- the
					// copy is the clear). Falls through to the full pass until the
					// static atlas is ready (the first frames after atlas creation).
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
							MarkSlotStaticRendered(i, st.pendingHash);  // atlas slot = source of truth
							st.bakeThisFrame = false;
							// Keep last frame's complete live content on bake
							// frames -- copying the fresh static-only bake in
							// flashed a mover-less (wrong-looking) shadow for one
							// frame. Only a tile with no valid content (fresh
							// alloc) takes the copy, where static-only beats
							// garbage depths.
							AtlasTileTexels bakeTile{};
							if (!GetSlotTileTexels(i, bakeTile) || !bakeTile.contentValid)
								CopyStaticTileToLive(i);
							e.renderedScale = e.pendingScale;
							// Live content unchanged this frame: never swap a
							// staged promotion in on a bake.
							MarkSlotTileRendered(i, false);
							continue;
						}
						{
							// Re-check bake validity AT COMPOSITE TIME: mode selection
							// (phase A) ran before the per-frame owner reconciliation,
							// so a reassigned slot can reach here holding the PREVIOUS
							// light's bake -- compositing it displayed a different
							// light's shadow. Movers-only over a clear for one frame
							// beats that; the queued bake heals it next redraw.
							uint64_t compositeHash = 0;
							bool compositeValid = false;
							GetSlotStaticState(i, compositeHash, compositeValid);
							if (compositeValid) {
								CopyStaticTileToLive(i);  // seed the tile with cached static depth
							} else {
								ClearSlotTile(i);
								st.bakeQueued = true;
							}
							s_budget.BeginLight(e.Light, 1);
							s_cpuSubmitUs.fetch_add(TimeUs([&] { e.Light->Render(tmp); }), std::memory_order_relaxed);  // composite movers on top (no clear)
							s_cpuSubmitN.fetch_add(1, std::memory_order_relaxed);
							s_budget.EndLight(e.Light, 1);
							e.renderedScale = e.pendingScale;
							// A movers-only frame (invalid seed) must not swap a
							// staged promotion in: keep sampling the old complete
							// tile until a seeded composite or full render lands.
							// Frustum lights are split-excluded in EnableLight, so
							// their raster held the FULL accumulate: complete
							// content, and withholding froze their promotions.
							const bool fullContent =
								e.Light->GetIsFrustumLight() && !e.Light->geomList.empty();
							MarkSlotTileRendered(i, compositeValid || fullContent);
							continue;
						}
					}
					// Empty-render guard (full-pass path): a redraw whose accumulate
					// produced no casters -- e.g. a transient cull under redraw-budget
					// churn -- would clear the tile and mark the empty result valid,
					// degenerating a good shadow into a flat (wrong) tile. Mirror the
					// bake-path guard above: keep the prior content by skipping the
					// clear and the re-mark. The render MUST still run to consume the
					// passes EnableLight registered (skipping it closes passGroupNext
					// into a ring); with an empty geomList it draws nothing, so the
					// held content survives. A tile with no valid content yet (fresh
					// alloc / staged realloc reads contentValid=false) is not held --
					// it clears and renders normally.
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
				// Commit the content scale only after the raster actually ran:
				// a skipped render must keep advertising the scale the slot
				// still holds, or shaders sample tile UVs against full-slice
				// content until the geometry hash happens to change.
				if (!keepPriorContent) {
					e.renderedScale = e.pendingScale;
					// Never mark an EMPTY render valid: a starved or transiently
					// culled fresh tile stays contentValid=false, so the sample side
					// skips it and the light reads UNSHADOWED -- never a degenerate
					// (cleared) tile. A high-pressure scene thus cannot show one.
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
