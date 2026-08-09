// ShadowCasterManager.h
// Shadow caster scheduling subsystem for LightLimitFix CPU-side shadow allocation.

#pragma once

#include <array>
#include <d3d11.h>
#include <functional>

#include "RE/B/BSShadowLight.h"
#include "RE/S/ShadowSceneNode.h"

#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "Utils/RestartSettings.h"

struct ImVec4;

namespace ShadowCasterManager
{
	/// Checks intrinsic BSShadowLight type, bypassing IsShadowLight vtable overrides.
	inline bool IsShadowLightType(RE::BSLight* bsLight)
	{
		return skyrim_cast<RE::BSShadowLight*>(bsLight) != nullptr;
	}

	/// A zero here drops the light below the cluster builder's filter
	/// (color*fade > 1e-4); restore only when fully zeroed, not mid-fade.
	inline void RestoreZeroedLodDimmer(RE::BSShadowLight* light)
	{
		if (light && light->lodDimmer == 0.0f)
			light->lodDimmer = 1.0f;
	}

	/// Independent of LightLimitFix::MAX_SHADOW_DEMAND_SLOTS (headers can't
	/// see each other's constant); cross-checked via static_assert in LightLimitFix.cpp.
	inline constexpr uint32_t kMaxShadowDemandSlots = 128;

	/// One frame's published GPU screen-visibility measurement.
	struct ShadowDemandSample
	{
		/// Asymmetric EMA of the per-slot summed demand.
		std::array<float, kMaxShadowDemandSlots> ema{};
		/// Raw last-drained per-slot tile maximum (accumulator units,
		/// 1024==1.0 demand); unfiltered -- only filter is the consumer's sample streak.
		std::array<uint32_t, kMaxShadowDemandSlots> maxLatest{};
		/// False until a reading has landed; every slot must read as fully visible
		/// until then, since "never measured" is not "measured absent".
		bool initialized = false;
		/// A cluster hit its per-cluster light cap this sample, so a visible
		/// light may read as absent everywhere; such a sample advances no streak.
		bool clusterSaturated = false;
		/// Devbench-only, default off: stops redraw admission once `pending`'s
		/// DIRTY partition is exhausted, instead of spending the full budget.
		bool redrawDueGate = false;
		/// Debug instrumentation is live (jittered taps, audit counters enabled).
		bool instrumentation = false;
		/// Increments once per drained frame; distinguishes a fresh sample from a
		/// re-read of the same one behind a stalled readback.
		uint32_t sampleSerial = 0;
		uint64_t lastDrainFrame = 0;
		uint64_t frameCounter = 0;
		/// Demand tiles this frame (ClusterSize.x * ClusterSize.y), the divisor
		/// that turns a projected screen-area fraction into a tap-pitch comparison.
		uint32_t tileCount = 0;
	};

	/// Conservative upper bound on shadowLightsAccum iteration index based on active scheduler settings.
	std::uint32_t MaxShadowAccumIterationBound();

	/// kSHADOWMAPS texture-array slot count allocated by the engine.
	std::uint32_t GetInstalledSlotCount();

	/// True while an engine shadow-scene teardown (ClearLightArrays) is pending.
	bool IsSessionResetPending();

	/// Live VRAM telemetry used for shadow-array sizing decisions and stats.
	struct VRAMInfo
	{
		std::uint64_t currentUsageBytes = 0;  ///< VRAM currently allocated to this process (local heap)
		std::uint64_t budgetBytes = 0;        ///< Driver-suggested budget for this process
		std::uint64_t shadowArrayBytes = 0;   ///< Bytes currently used by the kSHADOWMAPS texture array
		std::uint32_t shadowWidth = 0;        ///< Per-slice width
		std::uint32_t shadowHeight = 0;       ///< Per-slice height
		std::uint32_t shadowSlices = 0;       ///< Current kSHADOWMAPS ArraySize
		std::uint32_t bytesPerSlice = 0;      ///< Per-slice byte cost (width*height*format size)
		bool valid = false;
	};
	VRAMInfo GetVRAMInfo();

	/// Predicts the kSHADOWMAPS texture-array byte size for a given slice count.
	std::uint64_t ProjectShadowArrayBytes(std::uint32_t sliceCount);

	template <typename Fn>
	inline void ForEachShadowLight(const RE::BSTArray<RE::BSShadowLight*>& accum, Fn&& fn)
	{
		const std::uint32_t maxIdx = MaxShadowAccumIterationBound();
		std::uint32_t idx = 0;
		while (idx < maxIdx) {
			RE::BSShadowLight* light = accum[idx];
			const auto raw = reinterpret_cast<std::uintptr_t>(light);
			if (!IsPlausibleShadowLightPtr(raw))
				break;
			fn(light);
			const std::uint32_t step = light->shadowMapCount;
			if (step == 0)
				break;
			const std::uint64_t next = static_cast<std::uint64_t>(idx) + step;
			if (next >= maxIdx)
				break;
			idx = static_cast<std::uint32_t>(next);
		}
	}

	// -------------------------------------------------------------------------
	// Formula parameter indices
	// -------------------------------------------------------------------------
	enum FormulaParams
	{
		kFormulaParam_LightIndex,
		kFormulaParam_LightIntensity,
		kFormulaParam_LightDistance,
		kFormulaParam_LightRadius,
		kFormulaParam_LightX,
		kFormulaParam_LightY,
		kFormulaParam_LightZ,
		kFormulaParam_LightR,
		kFormulaParam_LightG,
		kFormulaParam_LightB,
		kFormulaParam_LightAmbientR,
		kFormulaParam_LightAmbientG,
		kFormulaParam_LightAmbientB,
		kFormulaParam_LightChosenLastFrame,
		kFormulaParam_LightFramesSinceRender,  ///< Frames since slot was last rendered
		kFormulaParam_LightNeverFades,
		kFormulaParam_LightPortalStrict,
		kFormulaParam_LightNS,
		kFormulaParam_LightConverted,
		kFormulaParam_LightDisplacement,    ///< Distance moved since last shadow map render (game units)
		kFormulaParam_PlayerLightDistance,  ///< Distance from player character to light (game units)
		kFormulaParam_LightIsSpot,          ///< 1 if spot light, 0 otherwise
		kFormulaParam_LightSpotVisible,     ///< 1 if spot cone is visible to camera, 1 for non-spots
		kFormulaParam_LightPlayerAttached,  ///< 1 if light is attached to player scene graph
		kFormulaParam_LightCoverage,        ///< Projected solid-angle proxy
		kFormulaParam_LightScreenArea,      ///< Viewport-clamped projected sphere area [0,1]
		kFormulaParam_LightLum,             ///< Rec.709 luminance of diffuse x engine fade
		kFormulaParam_LightAttCam,          ///< Skyrim falloff attenuation at camera
		kFormulaParam_LightAttPlayer,       ///< Skyrim falloff attenuation at player

		kFormulaParam_CameraX,
		kFormulaParam_CameraY,
		kFormulaParam_CameraZ,
		kFormulaParam_IsInterior,
		kFormulaParam_TimeOfDay,

		kFormulaParam_FrameTime,     ///< EMA-smoothed frame time (ms)
		kFormulaParam_FrameTarget,   ///< 90th-percentile frame time (ms) target budget ceiling
		kFormulaParam_StableFrames,  ///< Consecutive frames EMA has been below FrameTarget

		kFormulaParam_Max
	};

	// -------------------------------------------------------------------------
	// Expression-based formula evaluator (wraps exprtk)
	// -------------------------------------------------------------------------
	struct FormulaHelper
	{
		FormulaHelper();
		~FormulaHelper();

		FormulaHelper(const FormulaHelper&) = delete;
		FormulaHelper& operator=(const FormulaHelper&) = delete;
		FormulaHelper(FormulaHelper&&) = delete;
		FormulaHelper& operator=(FormulaHelper&&) = delete;

		bool Parse(const std::string& input);
		double Calculate();

		/// Re-parses with a new expression, replacing previous compiled formula.
		bool Reparse(const std::string& input);

		/// Validates `input` string expression compilation without altering active formula.
		static bool Validate(const std::string& input, std::string& errorOut);

		static void SetParam(int32_t index, double value);
		static double GetParam(int32_t index);

	private:
		void* _ptr;
	};

	// -------------------------------------------------------------------------
	// Budget mode enum
	// -------------------------------------------------------------------------
	enum class BudgetModeEnum : int32_t
	{
		Auto = 0,     ///< Deprecated: legacy save-file compatibility (migrated to Formula on load)
		Manual = 1,   ///< Fixed manual slider value
		Formula = 2,  ///< User-editable exprtk expression
	};

	// -------------------------------------------------------------------------
	// Settings
	// -------------------------------------------------------------------------
	struct Settings
	{
		/// Enables shadow caster scheduler (requires restart).
		bool Enabled = true;

		/// Number of simultaneous shadow-casting point/spot lights.
		int32_t ShadowLightCount = 16;

		/// Number of additional converted-light slots when ConvertExcessToNormal is enabled.
		int32_t ConvertedShadowSlots = 32;

		/// Allows newly-chosen lights to draw even if unchosen last frame.
		bool AllowDrawNewLight = true;

		/// Maximum shadow map re-renders permitted per frame.
		int32_t MaxRedrawPerFrame = 16;

		/// Lower bound for MaxRedrawPerFrame to avoid shadow flicker across TAA frames.
		static constexpr int32_t kMinMaxRedrawPerFrame = 4;

		/// Mode determining the per-frame shadow redraw budget allocation.
		BudgetModeEnum BudgetMode = BudgetModeEnum::Manual;

		/// Per-frame time budget for shadow re-renders in milliseconds (Manual mode).
		float RedrawBudgetMs = 5.0f;

		/// Demotes shadow lights exceeding caster limit to normal non-shadow lights.
		bool ConvertExcessToNormal = true;

		/// Promotes normal lights to shadow casters when budget permits.
		bool PromoteNormalToShadow = false;

		/// Matches shadow cull distance to light LOD fade-out distance each frame.
		bool MatchShadowToLightFade = true;

		/// Stores point/spot shadows in a variable-tile atlas texture instead of full slices.
		bool ShadowAtlas = true;

		/// Atlas texture dimension in texels (square).
		std::uint32_t AtlasResolution = 8192;

		/// Force-enables portal-strict on omni, hemisphere, and spot shadow casters.
		bool ForceEnablePortalStrictOmni = true;
		bool ForceEnablePortalStrictHemi = true;
		bool ForceEnablePortalStrictSpot = false;

		// --- Formula strings (exprtk expressions) ---

		/// Light priority scoring exprtk formula.
		std::string ScoreFormula = "max(lightscreenarea, 0.3 * max(lightattcam, lightattplayer)) * (0.5 + 0.5 * min(lightlum, 2) / 2) * (1 + max(0, 1 - lightframessincerender / 8) * 0.2)";

		/// Redraw interval exprtk formula per light.
		std::string RedrawIntervalFormula = "min(10, (max(0, min(lightdistance, playerlightdistance) - lightradius * 0.5) / 500) / max(0.5, lightintensity)) * (lightconverted * 5 + 1) - min(lightdisplacement / 5, 10)";

		/// Redraw budget exprtk formula per frame (in milliseconds).
		std::string RedrawBudgetFormula = "1 + isinterior";

		// --- Contribution-based caster culling ---

		/// Culls casters below this camera-relative screen size (radius / distance-to-viewer,
		/// NOT angular size relative to the light itself) (0 disables).
		float CasterCullAngularMin = 0.0f;

		/// Converts lights whose overall on-screen relevance is below this floor to non-shadow (0 disables).
		float ShadowImpactFloor = 0.0f;

		// --- Importance scheduling curve ---

		/// Redraw interval multiplier applied to high-importance lights.
		float ImportanceMinScale = 0.05f;

		/// Redraw interval multiplier applied to low-importance lights.
		float ImportanceMaxScale = 2.0f;

		/// Hard ceiling (frames) on redraw interval, applied after every term --
		/// those reorder the budget but never bound it. Floor of 1 closes a
		/// tie-window where RedrawIntervalFormula's displacement tail computes 0.
		float RedrawIntervalMaxFrames = 20.0f;

		/// Skips redraw for lights GPU-measured absent across a sustained
		/// streak. Every condition fails open (unmeasured/stale/saturated); requires the atlas.
		bool SkipZeroDemandRedraw = true;

		/// Stops redraw admission once `pending` runs out of DIRTY (schedDirty)
		/// candidates, instead of always spending the full budget -- see
		/// ShadowDemandSample::redrawDueGate for the dirty/clean partition.
		bool RedrawDueGateEnabled = true;
	};

	/// Legacy score formula strings kept for settings migration.
	inline constexpr const char* kLegacyScoreFormulas[] = {
		"lightradius * lightintensity / (1 + ((1 - lightneverfades) * lightdistance) / 1000) * (1 + max(0, 1 - lightframessincerender / 8) * 0.4) * (1 + lightisspot * lightspotvisible)",
	};

	NLOHMANN_JSON_SERIALIZE_ENUM(BudgetModeEnum,
		{ { BudgetModeEnum::Auto, 0 }, { BudgetModeEnum::Manual, 1 }, { BudgetModeEnum::Formula, 2 } })

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		Settings,
		Enabled,
		ShadowLightCount,
		ConvertedShadowSlots,
		AllowDrawNewLight,
		MaxRedrawPerFrame,
		BudgetMode,
		RedrawBudgetMs,
		ConvertExcessToNormal,
		PromoteNormalToShadow,
		MatchShadowToLightFade,
		ShadowAtlas,
		AtlasResolution,
		ForceEnablePortalStrictOmni,
		ForceEnablePortalStrictHemi,
		ForceEnablePortalStrictSpot,
		ScoreFormula,
		RedrawIntervalFormula,
		RedrawBudgetFormula,
		CasterCullAngularMin,
		ShadowImpactFloor,
		ImportanceMinScale,
		ImportanceMaxScale,
		RedrawIntervalMaxFrames,
		SkipZeroDemandRedraw,
		RedrawDueGateEnabled)

	/// Restart-gated hook toggles applied at boot.
	inline constexpr Util::Settings::RestartTable<Settings, 7> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, ConvertExcessToNormal, "Convert Excess Lights to Normal"),
		UTIL_RESTART_FIELD(Settings, PromoteNormalToShadow, "Promote Normal Lights to Shadow Casters"),
		UTIL_RESTART_FIELD(Settings, ForceEnablePortalStrictOmni, "Force Portal Strict on Omni Lights"),
		UTIL_RESTART_FIELD(Settings, ForceEnablePortalStrictHemi, "Force Portal Strict on Hemisphere Lights"),
		UTIL_RESTART_FIELD(Settings, ForceEnablePortalStrictSpot, "Force Portal Strict on Spot Lights"),
		UTIL_RESTART_FIELD(Settings, ShadowAtlas, "Shadow Atlas"),
		UTIL_RESTART_FIELD(Settings, AtlasResolution, "Atlas Resolution"),
	} };

	// -------------------------------------------------------------------------
	// Per-light schedule entry
	// -------------------------------------------------------------------------
	struct LightEntry
	{
		RE::BSShadowLight* Light{ nullptr };

		/// Sort key: LastDrawnFrame + computed interval. Lower = higher priority.
		double RedrawScore{ 0.0 };

		/// Frame number this light last rendered its shadow map.
		int32_t LastDrawnFrame{ -1 };

		/// Util::GetNowSecs() at first-ever render in this slot -- start of the
		/// fade-in ramp. -1 = complete/n-a. Wall-clock (not frame count) so ramp
		/// duration is fps-independent; set once when LastDrawnFrame leaves -1.
		double FadeStartSeconds{ -1.0 };

		/// Set each frame by scheduler; consumed by render hook.
		bool RedrawFrame{ false };

		/// Slot index in LightContainer array.
		int32_t Index{ -1 };

		/// World position of light at last rendered shadow map frame.
		RE::NiPoint3 lastRenderedPos{ 0.0f, 0.0f, 0.0f };

		/// Consecutive frames budget has requested a larger tile class.
		uint16_t promoteStreak{ 0 };

		/// Contribution-weighted importance score from last scheduling frame.
		float lastImportance{ 0.0f };

		/// True if shadow content changed since last render. Eligibility signal,
		/// not priority -- partitions ahead of RedrawScore in ShadowScheduler.cpp's `pending` sort.
		bool schedDirty{ false };

		/// Consecutive frames read dirty without redraw admission (stop-motion
		/// metric). FROZEN while skippedThisFrame, so occlusion isn't read as starvation.
		uint16_t dirtyStallFrames{ 0 };

		/// Set when removed from `pending` by the sleep or demand skip, not by
		/// exhausted budget -- distinguishes "never a candidate" from "lost the
		/// budget race" for dirtyStallFrames above.
		bool skippedThisFrame{ false };

		/// Hash of shadow scene at most recent successful redraw.
		std::uint64_t lastGeomHash{ 0 };

		/// Hash computed for current frame's scoring pass.
		std::uint64_t pendingGeomHash{ 0 };

		/// Result cache for ComputeShadowGeomHash.
		std::uint64_t cachedPendingGeomHash{ 0 };
		int32_t lastHashComputeFrame{ -1 };
		uint32_t lastHashGeomListSize{ 0 };

		/// Tile scale slot content was rasterized at.
		float renderedScale{ 1.0f };

		/// Tile scale scheduler desires for next redraw.
		float pendingScale{ 1.0f };

		/// Importance-domain class before atlas capacity clamp.
		float desiredScale{ 1.0f };

		/// Largest class atlas rank budget affords this light.
		float budgetScale{ 1.0f };

		/// Priority score from last scheduling frame.
		double lastScore{ 0.0 };

		/// Consecutive demand samples at or below the skip epsilon. Per pool
		/// entry, not a light-pointer map -- recycled addresses would inherit a stale streak.
		uint32_t untouchedSamples{ 0 };

		/// ShadowDemandSample::sampleSerial the streak last advanced on.
		uint32_t lastDemandSerial{ 0 };

		void Clear()
		{
			Light = nullptr;
			LastDrawnFrame = -1;
			FadeStartSeconds = -1.0;
			RedrawFrame = false;
			lastRenderedPos = { 0.0f, 0.0f, 0.0f };
			lastImportance = 0.0f;
			lastGeomHash = 0;
			pendingGeomHash = 0;
			cachedPendingGeomHash = 0;
			lastHashComputeFrame = -1;
			lastHashGeomListSize = 0;
			renderedScale = 1.0f;
			pendingScale = 1.0f;
			desiredScale = 1.0f;
			budgetScale = 1.0f;
			lastScore = 0.0;
			untouchedSamples = 0;
			lastDemandSerial = 0;
			// Slot-reuse hazard: a stale promoteStreak would let a new
			// occupant promote on its first eligible frame.
			promoteStreak = 0;
			dirtyStallFrames = 0;
			skippedThisFrame = false;
		}
	};

	// -------------------------------------------------------------------------
	// Container for the active light pool
	// -------------------------------------------------------------------------
	struct LightContainer
	{
		LightEntry* Lights{ nullptr };

		/// True when index 0 is directional sun (always active).
		bool Sun{ false };

		/// Total allocated slots (ShadowLightCount + ConvertedShadowSlots).
		int32_t Size{ 0 };

		/// Returns first free shadow-caster slot index, or -1 if full.
		int32_t FindFreeIndex(bool shadowSlot, int32_t shadowCount, int32_t convertCount) const;

		/// Returns index of light pointer in shadow-caster range, or -1.
		int32_t FindLight(RE::BSShadowLight* light, int32_t shadowCount) const;

		/// First pool index of point-light range (1 if Sun=true, 0 otherwise).
		int32_t PointLightFirst() const { return Sun ? 1 : 0; }

		/// Exclusive upper bound pool index for point-light range iteration.
		int32_t PointLightEnd(int32_t shadowCount) const { return PointLightFirst() + shadowCount; }
	};

	// -------------------------------------------------------------------------
	// Per-light GPU timing tracker (sliding-window average over 8 frames)
	// -------------------------------------------------------------------------
	static constexpr int kBudgetWindowSize = 8;

	struct BudgetEntry
	{
		uint64_t Key{ 0 };
		uint32_t Tracked[kBudgetWindowSize]{};  ///< Ring buffer of per-frame µs costs.
		int32_t TrackedCount{ 0 };
		int32_t LastTrackedHelper{ -1 };
		uint32_t Progress{ 0 };  ///< Accumulated step-0 cost awaiting step-1.
		int32_t Current{ 0 };    ///< Rolling sum of Tracked[].

		void BeginStep(int32_t step);
		void EndStep(int32_t step, int32_t helperCounter);

		/// Commits one measured render cost (µs) into the ring; used by the
		/// deferred GPU-timestamp path, which resolves several frames later.
		void CommitCost(uint32_t costUs, int32_t helperCounter);

		/// Microseconds since the matching BeginStep, without committing.
		uint32_t ElapsedSinceBeginUs() const;

		/// Returns true when the entry hasn't been updated in ~600 scheduler ticks.
		bool IsExpired(int32_t helperCounter) const;

	private:
		int64_t _startTime{ 0 };
	};

	/// D3D11 timestamp machinery for per-light GPU render cost (defined in
	/// ShadowBudget.cpp); pimpl keeps the public header free of query plumbing.
	struct BudgetGpuTimer;

	struct BudgetTracker
	{
		BudgetTracker();
		~BudgetTracker();

		void Begin(int32_t step);
		void BeginLight(RE::BSShadowLight* light, int32_t step);
		void EndLight(RE::BSShadowLight* light, int32_t step);

		/// Brackets the shadow render pass for GPU cost measurement (one
		/// disjoint timestamp batch/frame); Step-1 BeginLight/EndLight pairs
		/// inside are GPU-timed, committing async a few frames later. Falls back
		/// to CPU (QPC) timing when queries are unavailable/disjoint. Render thread only.
		void BeginRenderBatch();
		void EndRenderBatch();

		/// Returns estimated render cost (µs) for a light.
		/// Falls back to the mean of all tracked lights for unseen lights.
		int32_t GetCost(RE::BSShadowLight* light) const;

		/// Returns the mean GPU cost (µs) averaged over all currently tracked lights.
		int32_t GetAverageCostUs() const;

	private:
		int32_t _counter{ 0 };
		std::unordered_map<uint64_t, std::unique_ptr<BudgetEntry>> _map;
		std::unique_ptr<BudgetGpuTimer> _gpu;

		void CleanupExpired();
		void CommitResolved(uint64_t key, uint32_t costUs);

		friend struct BudgetGpuTimer;
	};

	// -------------------------------------------------------------------------
	// Per-slot visualization metadata (filled by LLF::CopyShadowLightData)
	// -------------------------------------------------------------------------
	struct ShadowSlotInfo
	{
		uint32_t type = 0;       ///< Shadow type: 0=spot/frustum, 1=hemisphere, 2=omnidirectional
		float range = 0.0f;      ///< Light range (world units) -- radius for point lights, cone distance for spots
		bool valid = false;      ///< true when this slot was written this frame
		uintptr_t lightKey = 0;  ///< Light object pointer (stable key for suppression)
		/// Final uploaded ShadowParam.y: >0 valid radius, 0 safe-lit sentinel
		/// (empty descriptors or missing atlas tile), <0 suppression sentinel.
		float paramY = 0.0f;
		/// Owner reference's display name (falls back to the light node's
		/// scenegraph name, then form ID) so a table row identifies the light.
		std::string name;
	};

	/// Resets slot metadata for a new frame.  Call at the start of CopyShadowLightData.
	void BeginSlotFrame(uint32_t slotCount);

	/// Records metadata for one filled shadow slot.
	void RecordSlot(uint32_t depthSlot, const ShadowSlotInfo& info);

	/// Queues a one-shot DDS dump of the shadow atlas plus a slot-manifest
	/// JSON to CommunityShaders/Captures, serviced by the render thread's next
	/// pass -- ground truth for tile contents without a RenderDoc attach
	/// (which perturbs the pipeline). Thread-safe; no-op while atlas is inactive.
	void RequestAtlasDump();
	/// Arms the multi-frame shadow recorder (frames clamped to [1,600]);
	/// with a_slot >= 0 also records that light's visited caster set and,
	/// for frames <= 16, per-frame tile DDS dumps. One JSON on completion.
	void RequestShadowFrameRecord(uint32_t a_frames, int32_t a_slot);

	/// Returns true if the light with this pointer key has been suppressed by the user.
	/// Includes implicit suppression from solo mode (every key except the soloed one).
	bool IsSuppressed(uintptr_t lightKey);

	/// Returns true if any lights are currently suppressed (explicit or via solo).
	bool HasSuppressedLights();

	/// True if any debug override is active (suppress/pin/solo). Keeps the
	/// LLF overlay's visibility gate open even without visualisation modes or ShowShadowOverlay.
	bool HasAnyOverrides();

	// -------------------------------------------------------------------------
	// Scheduling diagnostics snapshot (headless inspection via devbench
	// `inspect kind=llfshadows`); filled only while settings are open or requested, to skip the hot path otherwise.
	// -------------------------------------------------------------------------
	struct SchedSnapshot
	{
		bool valid = false;      ///< false until at least one pass has filled it
		uint32_t frame = 0;      ///< render frame the snapshot was taken on
		int total = 0;           ///< candidates examined this pass
		int chosen = 0;          ///< picked as shadow casters
		int excess = 0;          ///< over the shadow-caster budget
		int invalidCamera = 0;   ///< rejected by the engine UpdateCamera test
		int invalidPortal = 0;   ///< portal-graph unreachable
		int invalidFrustum = 0;  ///< off-screen / beyond shadow-cull distance
		int invalidLod = 0;      ///< past the light's LOD fade-out distance
		int invalidOther = 0;    ///< UpdateCamera failure, neither frustum nor LOD
		int slotsInUse = 0;      ///< occupied shadow slots at pass end
		/// Per non-chosen light: (light pointer, demotion-reason byte). Name via SchedReasonName().
		std::vector<std::pair<uintptr_t, uint8_t>> demoted;

		/// Per occupied point-light pool slot: tile classing and atlas
		/// placement, for headless assertion of the variable-resolution path.
		struct SlotState
		{
			int index = 0;  ///< pool slot (== kSHADOWMAPS slice / atlas slot key)
			uintptr_t light = 0;
			float importance = 0.0f;  ///< raw importance from the last scoring pass
			double score = 0.0;       ///< unified priority (ScoreFormula) that ranked this light
			float desiredScale = 1.0f;
			float budgetScale = 1.0f;
			float pendingScale = 1.0f;
			float renderedScale = 1.0f;
			uint32_t tileX = 0;  ///< atlas texels; tileSize 0 = no atlas tile
			uint32_t tileY = 0;
			uint32_t tileSize = 0;
			bool tileContentValid = false;
			// Read-side outcome from the last upload: paramY is the decisive
			// sentinel (>0 shadows, 0 forced lit, <0 forced dark); a healthy tile
			// with paramY 0 means the descriptor/upload stage bailed.
			float uploadParamY = 0.0f;
			float uploadRange = 0.0f;
			bool uploadRecorded = false;  ///< slot record written this frame
			bool suppressed = false;
			bool promoted = false;          ///< light was promoted to shadow caster (s_shadowConvert)
			bool redrawnThisFrame = false;  ///< RedrawFrame this frame -- did it actually redraw
			bool schedDirty = false;        ///< eligibility signal the due-gate partitions on
			uint16_t dirtyStallFrames = 0;  ///< consecutive dirty-but-unadmitted frames
			double redrawScore = 0.0;       ///< diagnostic: due-gate deadline (frame units)
			int32_t lastDrawnFrame = -1;    ///< diagnostic: frame this light was last actually redrawn (-1 = never)
			bool cameraHold = false;        ///< UpdateCamera failed this frame; slot/tile protected, not redrawn
			/// Engine's own caster count (BSShadowLight::geomList.size()); zero
			/// means genuinely no known caster geometry, not a stale tile.
			uint32_t geomListSize = 0;
			/// staticValid+staticEmpty together flag a zero-caster bake -- the only
			/// signal distinguishing it from a genuinely rendered tile (both read tileContentValid=true).
			bool staticValid = false;
			bool staticEmpty = false;
		};
		std::vector<SlotState> slots;

		// Atlas summary; all zero while the atlas is inactive.
		uint32_t atlasDim = 0;
		uint32_t atlasCapacityCells = 0;
		float atlasOccupancy = 0.0f;
		uint64_t atlasVramBytes = 0;
		uint32_t atlasTileReallocs = 0;        ///< cumulative class-change reallocs (cache health)
		uint32_t atlasOwnerInvalidations = 0;  ///< cumulative slot-reassignment content drops
		uint32_t atlasAllocDenied = 0;         ///< cumulative EnsureSlotTile calls that couldn't grant the request
		float baseTileTexels = 2048.0f;        ///< scale=1.0 reference size; classes histogram divides by this
		uint32_t cpuAccumUsAvg = 0;            ///< CPU-only avg per Accumulate (cull walk + appends)
		uint32_t cpuSubmitUsAvg = 0;           ///< CPU-only avg per Render (pass setup + submission)
		uint32_t cpuEnableUsAvg = 0;           ///< CPU-only avg per EnableLight (setup + SafeEnableAndValidate)

		// Budget-tracker aggregates (GPU timestamps): the REST perf A/B
		// reads these instead of needing an external profiler attach.
		int32_t avgLightCostUs = 0;       ///< mean measured GPU cost per caster
		float avgRedrawsPerFrame = 0.0f;  ///< rolling mean of casters redrawn per frame

		/// Cumulative StaticOnly re-bakes since load -- a bake re-rasterizes the
		/// whole static caster set into its cache tile, so differencing this run
		/// measures the rebuild cost per-frame savings are netted against.
		uint64_t staticBakesTotal = 0;

		/// Cumulative s_pendingCellReset drains since load -- diagnoses whether
		/// cell-grid-shift invalidation fires only on zone transitions or also on ordinary movement.
		uint64_t cellResetsTotal = 0;

		/// Cumulative caster appends dropped for free-pool exhaustion (see
		/// s_cullPoolDropTotal) -- climbing during flicker signals a starved
		/// accumulate the geomList.empty() guard alone can't see.
		uint64_t cullPoolDropsTotal = 0;

		/// Same blind-spot signal as cullPoolDropsTotal, for the angular-size
		/// contribution cull instead of pool exhaustion (see s_casterCullTotal).
		uint64_t casterCullDropsTotal = 0;

		/// Redraws elided by the empty-dynamic sleep skip (chosen light whose
		/// valid static bake saw no movers) -- this pass and cumulative, the direct measure of the early-out's savings.
		int sleepSkips = 0;
		uint64_t sleepSkipsTotal = 0;

		/// Redraws skipped by the zero-demand gate this frame, and since load.
		int demandSkips = 0;
		uint64_t demandSkipsTotal = 0;

		/// High-water occupancy of the engine's global 512-slot alpha
		/// GeometryGroup array, and requests refused at that ceiling -- a non-zero
		/// drop count means capacity was hit, which without the guard crashes, not degrades.
		uint32_t alphaGroupPeak = 0;
		uint64_t alphaGroupDrops = 0;

		/// Zero-demand-skip audit (populated only while LLF shadow demand
		/// instrumentation is on). Q1 is a correctness finding expected to read
		/// zero; Q2 is a capacity finding expected to be large -- never the same measurement.
		int frustumAuditCandidates = 0;      ///< Q1 candidates the oracle evaluated
		int frustumAuditKeptOut = 0;         ///< engine kept a light whose sphere is out (quadrant C)
		int frustumAuditSuspects = 0;        ///< quadrant C sustained past the persistence gate
		int demandSlotted = 0;               ///< Q2 slotted lights with a demand reading
		int demandZero = 0;                  ///< of those, per-tile max == 0
		int demandSubTap = 0;                ///< zero-demand lights whose footprint is under the tap pitch
		int demandSkipEligible = 0;          ///< SkipZeroDemandRedraw would have skipped these (ceiling on any win)
		int demandSwapIn = 0;                ///< admitted only in the counterfactual
		int demandSwapInAboveEps = 0;        ///< of those, demand above the epsilon (a real quality win)
		int demandRedrawsSaved = 0;          ///< real admissions minus counterfactual admissions
		bool demandBudgetSaturated = false;  ///< the real budget loop exited on an exhausted budget
		bool demandPhase1Enabled = false;    ///< demand tiebreaker was live during this measurement
		/// SkipZeroDemandRedraw during this measurement. Load-bearing for reading
		/// Q2: with the skip live the counterfactual is the real run, so swapIn and
		/// redrawsSaved read zero by construction while demandSkips carries the work.
		bool demandSkipActive = false;
		uint64_t demandSkipEligibleTotal = 0;
		uint64_t demandSwapInTotal = 0;
		uint64_t demandRedrawsSavedTotal = 0;

		/// Stop-motion metric: this frame's pool-wide max consecutive
		/// dirty-but-unadmitted streak (LightEntry::dirtyStallFrames).
		int stallMax = 0;
		int stallWorstSlot = -1;
		double demandRatio = 0.0;  ///< Sum(1/effective redraw delay) over `pending`
	};

	/// Requests and returns the latest scheduling-diagnostics snapshot.
	/// Thread-safe (off render thread); primes the scheduler for a short
	/// window, so the first call after idle may return valid==false -- poll again.
	SchedSnapshot RequestSchedSnapshot();

	/// Stable lowercase name for a demotion-reason byte (SchedSnapshot::demoted.second):
	/// "portal" | "frustum" | "lod" | "excess" | "other" | "none".
	const char* SchedReasonName(uint8_t reason);

	// -------------------------------------------------------------------------
	// Debugging override API
	// -------------------------------------------------------------------------
	bool IsPinnedShadow(uintptr_t lightKey);
	bool IsPinnedConvert(uintptr_t lightKey);

	void SetPinnedShadow(uintptr_t lightKey, bool pinned);
	void SetPinnedConvert(uintptr_t lightKey, bool pinned);

	uintptr_t GetSoloLight();
	void SetSoloLight(uintptr_t lightKey);

	/// Mouse-hover key for the per-frame 3D light highlight pulse.
	uintptr_t GetHoveredLight();
	void SetHoveredLight(uintptr_t lightKey);

	/// Highlights lights selected by UI group hover.
	void ClearHighlight();
	void AddHighlight(uintptr_t lightKey);
	bool IsHighlighted(uintptr_t lightKey);

	/// Lights culled by the Light Impact Floor this frame.
	void ClearBelowFloor();
	void AddBelowFloor(uintptr_t lightKey);
	bool IsBelowFloor(uintptr_t lightKey);

	/// Clears all manual debug overrides (pins, solo, suppress).
	void ClearAllOverrides();

	/// Returns number of shadow slots consumed this frame.
	uint32_t GetSlotUsage();

	/// Returns active shadow-casting lights with importance > 0.1.
	uint32_t GetHighImportanceCount();

	/// Read-only view of per-slot metadata for current frame.
	const std::vector<ShadowSlotInfo>& GetSlotInfos();

	/// Returns display name for a shadow type index (0=Spot, 1=Hemi, 2=Omni).
	const char* GetShadowTypeName(uint32_t type);

	/// Returns golden-ratio hue color for shadow-map slot as an ImVec4.
	ImVec4 ShadowSlotHueColor(uint32_t slotIdx);

	/// Draws the interactive shadow caster UI table.
	void DrawShadowLightTable(bool compact, bool showColor, bool sceneOnly = false, bool readOnly = false);

	/// Draws active caster capacity summary for settings menu and overlay header.
	void DrawShadowSummary(uint32_t clusterCount, uint32_t clusterMax, uint32_t shadowUnshadowedLightCount);

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/// Allocates light container and initializes scheduler state from settings.
	void Init(const Settings& settings);

	/// Installs scheduler game hooks.
	void Install(const Settings& settings);

	/// Per-frame update for installed slot counts and setting changes.
	void Update(const Settings& settings, RE::ShadowSceneNode* shadowSceneNode,
		RE::NiCamera* worldCamera);

	/// Resets transient pool entries and session overrides on scene transitions.
	void ResetSession();

	/// Publishes this frame's GPU-measured per-slot screen-visibility demand for
	/// the redraw scheduler to read. Call once per frame before Update().
	void SetShadowDemand(const ShadowDemandSample& sample);

	/// Returns read-only view of active light pool.
	const LightContainer& GetLights();

	/// True when SCM owns shadow scheduling (enabled at boot, no external conflict).
	bool IsActive();

	/// Returns kSHADOWMAPS texture-array slot slice index for a light, or -1.
	int32_t GetShadowSlot(RE::BSShadowLight* light);

	/// Returns tile scale slot content was last rasterized at.
	float GetRenderedTileScale(int32_t poolSlot);

	/// Returns [0,1] shadow fade-in blend: 0 just after first gaining a
	/// shadow, ramping to 1 over kShadowFadeInSeconds wall-clock time (1.0 if no active fade).
	float GetShadowFadeAlpha(int32_t poolSlot);

	/// Visits shadow lights currently demoted to non-shadow rendering.
	void ForEachConvertedLight(const std::function<void(RE::BSShadowLight*)>& visitor);

	/// Draws scheduler execution statistics.
	void DrawShadowSchedulerStats();

	/// Draws per-mode overlay info for shadow-related visualization modes.
	void DrawOverlayShadowModeInfo(uint32_t mode, uint32_t shadowUnshadowedLightCount, uint32_t totalLightCount);

	/// Appends hover tooltip text for shadow visualization modes.
	void DrawVisualisationTooltipShadowModes();

	/// Draws ImGui settings panel for shadow caster scheduler.
	void DrawSettings(Settings& settings);

	/// Draws the Quality/Balanced/Performance caster-cull preset buttons only.
	void DrawImpactCullPresetButtons(Settings& settings);

	/// Draws the raw caster cull angular / impact floor sliders only.
	void DrawImpactCullSliders(Settings& settings);

	/// Draws caster cull and impact floor UI controls (presets + sliders together).
	void DrawImpactCullControls(Settings& settings);

	/// Loads Skyrim INI preferences owned by SCM.
	void LoadINISettings();

	/// Saves edited Skyrim INI preferences to user Documents folder.
	void SaveINISettings();

}
