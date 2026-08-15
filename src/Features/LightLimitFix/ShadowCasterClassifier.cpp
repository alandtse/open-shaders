// ShadowCasterClassifier.cpp
// Contribution-based caster culling, the static/dynamic split-cache
// classifier, the AppendVirtual cull hooks, and the shadowmap recorder.

#include <filesystem>
#include <fstream>

#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "ShadowCasterInternal.h"

namespace ShadowCasterManager
{
	/// Casters culled last frame across all lights (Tracy plot for A/B).
	std::atomic<uint32_t> s_casterCullCount{ 0 };

	/// Appends dropped because the culling process's free pool was near
	/// exhaustion (see the guard in Hook_ParabolicCullAppend).
	std::atomic<uint32_t> s_cullPoolDropCount{ 0 };

	/// Running total of s_cullPoolDropCount (frame-reset, Tracy-only),
	/// published for devbench inspect kind=llfshadows -- catches drops the
	/// empty-render guard's geomList.empty() check misses.
	std::atomic<uint64_t> s_cullPoolDropTotal{ 0 };

	/// Running total of s_casterCullCount; same blind spot as above,
	/// for the angular-cull path.
	std::atomic<uint64_t> s_casterCullTotal{ 0 };

	/// The shadow light currently being accumulated; only non-null across an
	/// EnableLight Accumulate call, read synchronously by the AppendVirtual hook.
	std::atomic<RE::BSShadowLight*> s_currentCullLight{ nullptr };

	/// Thread running that accumulate, 0 when idle. DrawWorld cull jobs walk
	/// the same hooked vtables from other threads; only this thread's walk is ours.
	std::atomic<std::uint32_t> s_cullThreadId{ 0 };

	void SetCurrentCullLight(RE::BSShadowLight* a_light)
	{
		// Thread id first on arm, cleared first on disarm, so a concurrent job
		// thread never sees itself as the accumulate owner.
		if (a_light) {
			s_cullThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
			s_currentCullLight.store(a_light, std::memory_order_relaxed);
		} else {
			s_cullThreadId.store(0, std::memory_order_relaxed);
			s_currentCullLight.store(nullptr, std::memory_order_relaxed);
		}
	}

	RE::BSShadowLight* CurrentCullLight()
	{
		if (s_cullThreadId.load(std::memory_order_relaxed) != GetCurrentThreadId())
			return nullptr;
		return s_currentCullLight.load(std::memory_order_relaxed);
	}

	// True while accumulating a light with an EMPTY geomList: a light created
	// after scene attach never gets geometry from AttachNearbyLights on its
	// own, so the append hook rebuilds it via the engine's AttachGeometry.
	std::atomic<bool> s_accumRebuildAttach{ false };

	// Dedupes AttachGeometry re-attachment: dual-paraboloid walks append the
	// same geometry once per half, and the engine's raw pair-insert has no
	// dedupe of its own. Accumulate-thread only (see CurrentCullLight).
	std::unordered_set<const RE::BSGeometry*> s_healAttached;

	// Multi-frame diagnostic recorder (devbench capture kind=shadowmaps,
	// frames=N[, slot=S]): per-pass slot state plus a target slot's visited
	// caster set, for validating the static/dynamic split. Zero cost while disarmed.
	struct RecCaster
	{
		const void* geom;
		std::string name;
		bool dynamic;
		int mode;  ///< CasterPass value (ShadowCasterInternal.h)
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
		if (ec) {
			logger::error("[SCM] Frame record directory failed: {} ({})", dir.string(), ec.message());
			s_recFrames.clear();
			return;
		}
		const auto path = dir / std::format("scm_frames_{}.json", rec.frame);
		std::ofstream out(path);
		if (!out) {
			logger::error("[SCM] Frame record open failed: {}", path.string());
			s_recFrames.clear();
			return;
		}
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

	// Static/dynamic split caching: the parabolic AppendVirtual hook filters
	// casters per s_cullPassMode (StaticOnly on a rare atlas rebake, DynamicOnly
	// the rest of the time). Frame-flow doc: SplitState in ShadowScheduler.cpp.
	std::atomic<int> s_cullPassMode{ static_cast<int>(CasterPass::All) };

	/// Static/dynamic caster draws classified last frame (Tracy plots for A/B).
	std::atomic<uint32_t> s_staticCasterDraws{ 0 };
	std::atomic<uint32_t> s_dynamicCasterDraws{ 0 };

	// Per-caster movement history: stays dynamic until it has held still for
	// kStaticStabilityFrames, so a caster that just stopped isn't yanked into
	// the static pass mid-transition (which would blink its shadow).
	constexpr int kStaticStabilityFrames = 8;
	// Rejoin churn damping: each oscillation doubles the required hold-still
	// time before re-promotion (leaving stays immediate; repeated rejoin
	// would otherwise force a full static re-bake every time).
	constexpr int kStaticPromoteBackoffMax = 8;  // 8 * 8 = 64 frames, ~1s at 60fps
	// Bounds framesSinceMove; must clear the longest decay threshold below.
	constexpr int kStaticFramesCap = kStaticStabilityFrames * kStaticPromoteBackoffMax * 4;
	// Settled casters re-verify worldBound only every N epochs; kept well
	// inside kSleepRedrawIntervalFrames (45) so a missed move is still caught.
	constexpr int kSettledRecheckFrames = 16;
	constexpr int kSettledAtFactor = 4;  // matches the promoteAt * 4 backoff-reset below
	// Open-addressing map: probed once per appended caster by the cull-walk
	// hook, so lookup cost lands directly on EnableLight's accumulate time.
	// Accumulate-thread only (see CurrentCullLight) -- a concurrent insert
	// would rehash under another thread's probe.
	ankerl::unordered_dense::map<RE::BSGeometry*, CasterMobility> s_casterMobility;
	int s_casterClassEpoch{ 0 };

	/// Classifies a caster static vs dynamic from quantized worldBound
	/// movement (1-unit, matching the redraw hash), memoized once per frame.
	static CasterMobility& ClassifyCaster(RE::BSGeometry& geom)
	{
		auto [it, inserted] = s_casterMobility.try_emplace(&geom);
		auto& r = it->second;
		if (r.lastEpoch == s_casterClassEpoch)
			return r;  // already classified this frame

		// Settled fast path: trust the cached classification instead of
		// re-quantizing worldBound. A move mid-window is still caught at the next checkpoint.
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
			// Leaving the static set is an oscillation (not a first sighting): make
			// the caster earn its way back, so repeated pausing can't re-bake every time.
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
	// Unsynchronized by design: only the accumulate thread's walk folds into it.
	std::uint64_t s_visitStaticHash{ 0 };

	// Dynamic casters the current accumulate appended, counted on every pass
	// (including StaticOnly, which filters them out but must still see them).
	// Reset with the hash seed in EnableLight, latched into SplitState for
	// the sleep skip and the due-gate's dynamic-caster dirty term.
	std::atomic<uint32_t> s_visitDynamicCount{ 0 };
	// Static casters the current StaticOnly bake appended, so an empty bake
	// is never advertised as real cached content.
	std::atomic<uint32_t> s_visitStaticCount{ 0 };

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

	/// Classifies `geom` for the active split-cache pass, folding it into the
	/// running static hash; returns true when the caller should skip it.
	/// Shared by both cull-append hooks for identical StaticOnly/DynamicOnly semantics.
	static bool CasterFilteredByPass(RE::BSGeometry& geom)
	{
		if (!AtlasActive())
			return false;
		// Skinned = always dynamic (see IsCasterDynamic); only non-skinned
		// casters have a mobility record, classified once per frame.
		CasterMobility* rec = geom.GetGeometryRuntimeData().skinInstance ?
		                          nullptr :
		                          &ClassifyCaster(geom);
		const bool dynamic = !rec || rec->dynamic;
		// Every static caster folds into the hash regardless of pass, so a
		// static-set change is seen during whichever accumulate runs this frame.
		if (!dynamic)
			FoldStaticCasterHash(geom, *rec);
		switch (static_cast<CasterPass>(s_cullPassMode.load(std::memory_order_relaxed))) {
		case CasterPass::StaticOnly:
			if (dynamic) {
				// Counted even though filtered: the mover latch must see
				// dynamics on bake passes too, not only on composites.
				s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
				return true;  // bake pass skips movers
			}
			s_visitStaticCount.fetch_add(1, std::memory_order_relaxed);
			break;
		case CasterPass::DynamicOnly:
			if (!dynamic)
				return true;  // composite pass skips baked static geometry
			s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
			break;
		case CasterPass::All:
			(dynamic ? s_dynamicCasterDraws : s_staticCasterDraws).fetch_add(1, std::memory_order_relaxed);
			if (dynamic)
				s_visitDynamicCount.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		return false;
	}

	// RE::BSCullingProcess::Data free-object-pool layout, Ghidra-verified
	// byte-identical across SE/AE/VR. kFreePoolOffset/kPoolHeadOffset/
	// kPoolTailOffset map PtrMultiProdCons<Data,8192,0>'s free/start/end;
	// PopFreeQueueEntry's CAS loop guarantees tail >= head (no underflow).
	constexpr std::uintptr_t kFreePoolOffset = 0x20150;
	constexpr std::uintptr_t kPoolHeadOffset = 0x10000;
	constexpr std::uintptr_t kPoolTailOffset = 0x10008;
	constexpr std::uint32_t kFreeEntryMargin = 16;

	/// True when the free pool backing `a_this` is within kFreeEntryMargin of
	/// exhaustion. AppendVirtual writes through PopFreeQueueEntry's result
	/// with no null check, so exhaustion is a guaranteed CTD -- the caller
	/// drops the caster instead. Margin absorbs concurrent worker pops.
	static bool CullPoolNearExhaustion(const RE::BSCullingProcess* a_this)
	{
		const auto* pool = reinterpret_cast<const std::uint8_t*>(a_this) + kFreePoolOffset;
		const auto head = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolHeadOffset)->load(std::memory_order_relaxed);
		const auto tail = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolTailOffset)->load(std::memory_order_relaxed);
		return tail - head < kFreeEntryMargin;
	}

	/// Hook of BSCullingProcess::AppendVirtual on the parabolic culling vtable.
	/// Drops a caster (skips the append) when below the contribution-cull
	/// threshold, or when it does not belong to the active split-cache pass.
	struct Hook_ParabolicCullAppend
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			const float angularMin = s_settings.CasterCullAngularMin;
			RE::BSShadowLight* light = CurrentCullLight();
			// Heals a missed geometry attachment (see s_accumRebuildAttach).
			// Gated on s_accumRebuildAttach alone, not `!light->objectNode`:
			// that tracks scene-node attach, unrelated to this light's geomList.
			if (light && s_accumRebuildAttach.load(std::memory_order_relaxed)) {
				// Dedupe per walk (see s_healAttached).
				const bool newlyAttached = s_healAttached.insert(&a_visible).second;
				if (auto* ni = light->light.get();
					ni && newlyAttached &&
					GameLightIsInRange(light, &a_visible.worldBound, ni, 1.0f))
					GameAttachGeometry(light, &a_visible);
			}
			if (angularMin > 0.0f && light) {
				const auto& wb = a_visible.worldBound;
				const float distSq = wb.center.GetSquaredDistance(s_cullCameraPos);
				const float radiusSq = wb.radius * wb.radius;
				// Squared form of dist > radius && radius/dist < angularMin, avoiding
				// the per-caster sqrt; also skips casters enclosing the camera (shadow could be anywhere).
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
			// Gate on `light`: this shared base-class code also reaches the sun's
			// cascade cull (see CurrentCullLight).
			if (light && CasterFilteredByPass(a_visible))
				return;
			if (CullPoolNearExhaustion(a_this)) {
				s_cullPoolDropCount.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			func(a_this, a_visible, a_alphaGroupIndex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/// Pool-exhaustion guard for the base culling process (frustum/spot lights'
	/// vtable, RE::VTABLE_BSCullingProcess[0], also reached by the engine's
	/// room/scene cull walks) -- same unchecked AppendVirtual write as above.
	struct Hook_BaseCullAppendGuard
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
		{
			// Same missed-geometry-attachment heal as Hook_ParabolicCullAppend,
			// for frustum/spot lights (this vtable) instead of point/omni.
			RE::BSShadowLight* light = CurrentCullLight();
			if (light && s_accumRebuildAttach.load(std::memory_order_relaxed)) {
				const bool newlyAttached = s_healAttached.insert(&a_visible).second;
				if (auto* ni = light->light.get();
					ni && newlyAttached &&
					GameLightIsInRange(light, &a_visible.worldBound, ni, 1.0f))
					GameAttachGeometry(light, &a_visible);
			}
			// Gate on `light`: null means a foreign cull walk sharing this
			// vtable slot (see CurrentCullLight), not our accumulate.
			if (light && CasterFilteredByPass(a_visible))
				return;
			if (CullPoolNearExhaustion(a_this)) {
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
}
