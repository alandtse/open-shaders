#pragma once

#include "Buffer.h"
#include "Feature.h"

struct SnowDeformation : Feature
{
	virtual inline std::string GetName() override { return "Snow Deformation"; }
	virtual std::string GetDisplayName() override { return T("feature.snow_deformation.name", "Snow Deformation"); }
	virtual inline std::string GetShortName() override { return "SnowDeformation"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.snow_deformation.description", "Presses footprints and trails into snowy landscape as real displaced geometry that slowly fills back in."),
			{ T("feature.snow_deformation.key_feature_1", "Footprints and body impressions from the player, NPCs and creatures"),
				T("feature.snow_deformation.key_feature_2", "Works on vanilla snow textures and on Snow Cover snow"),
				T("feature.snow_deformation.key_feature_3", "Prints fill back in over time, faster while it snows"),
				T("feature.snow_deformation.key_feature_4", "Optional snow blanket raises snowy ground so prints become trenches down to the ground") } };
	}

	static constexpr uint32_t kTexels = 2048;
	static constexpr uint32_t kTileTexels = 64;
	static constexpr uint32_t kSlots = kTexels / kTileTexels;
	static constexpr int32_t kWindowTiles = 15;
	static constexpr int32_t kWindowSpan = 2 * kWindowTiles + 1;
	static constexpr uint32_t kLevels = 2;
	static constexpr float kCellSize = 0.75f;
	static constexpr float kCoarseScale = 4.0f;
	static constexpr float kTileSize = kCellSize * static_cast<float>(kTileTexels);
	static constexpr float kCoarseTileSize = kTileSize * kCoarseScale;
	static constexpr uint32_t kMaxStamps = 96;
	static constexpr uint32_t kMaxStampsPerActor = 12;
	static constexpr uint32_t kMaxTileEntries = kSlots * kSlots;
	static constexpr int32_t kBlanketTexels = 128;
	static constexpr float kBlanketCell = 64.0f;
	static constexpr float kMaxBlanketHeight = 64.0f;

	static_assert((kBlanketTexels & (kBlanketTexels - 1)) == 0);

	static_assert((kTexels & (kTexels - 1)) == 0);
	static_assert(kWindowSpan < static_cast<int32_t>(kSlots));

	struct Settings
	{
		bool Enabled = true;
		bool IncludeNPCs = true;
		bool IncludeObjects = true;
		bool UseSnowCover = true;
		bool DisplaceShadows = true;
		bool SinkActors = true;
		float BlanketHeight = 24.0f;
		float Depth = 6.0f;
		float Lifetime = 240.0f;
		float TargetSpacing = 2.0f;
	};

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void EarlyPrepass() override;
	virtual void Prepass() override;
	virtual void GameLoaded() override;
	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Binds the hull and domain stages and patch topology for a routed landscape draw, after the engine has flushed its state. */
	void ApplyDrawState();

	/** @brief Updates the footprint field once per frame; called before the shadow maps so every pass of the frame sees the same field. */
	void UpdateFieldOnce();

	struct ImpactEvent
	{
		uint8_t kind = 0;
		uint8_t element = 0;
		bool voice = false;
		RE::NiPoint3 position;
		RE::NiPoint3 direction;
		RE::FormID source = 0;
		RE::FormID space = 0;
		float radius = 0.0f;
		std::chrono::steady_clock::time_point time{};
	};

	/** @brief Queues a spell, shout or explosion impact for the next footprint gather; safe from any thread. */
	void QueueImpact(ImpactEvent a_event);

	/** @brief Remembers a geometry decal attached to landscape so its draws follow the displaced surface. */
	void RegisterDecal(RE::BSTempEffectGeometryDecal* a_decal);

	struct SpellCastSink : RE::BSTEventSink<RE::TESSpellCastEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override;
	};

	struct EquipSink : RE::BSTEventSink<RE::TESEquipEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override;
	};

	struct Hooks
	{
		template <class T>
		struct Projectile_AddImpact
		{
			static void thunk(RE::Projectile* a_projectile, RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity, RE::hkpCollidable* a_collidable, int32_t a_arg6, uint32_t a_arg7);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Explosion_Initialize
		{
			static void thunk(RE::Explosion* a_explosion);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSTempEffectGeometryDecal_Initialize
		{
			static void thunk(RE::BSTempEffectGeometryDecal* a_decal);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSUtilityShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSUtilityShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderDepth
		{
			static void thunk(bool a1, bool a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_Update
		{
			static void thunk();
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};

private:
	enum class DrawKind
	{
		Lighting,
		Depth,
		Shadow,
		Decal
	};

	struct DecalShaders
	{
		winrt::com_ptr<ID3D11HullShader> hs;
		winrt::com_ptr<ID3D11DomainShader> ds;
		bool failed = false;
	};

	enum class Layout : uint8_t
	{
		Unknown,
		Valid,
		Invalid
	};

	struct Stamp
	{
		double x = 0.0;
		double y = 0.0;
		float radius = 0.0f;
		float depth = 0.0f;
		float forwardX = 0.0f;
		float forwardY = 0.0f;
		float halfWidth = 0.0f;
		float rim = 0.0f;
		float motionX = 0.0f;
		float motionY = 0.0f;
		float vanillaSnow = 0.0f;
		float landZ = 0.0f;
		float normalZ = 1.0f;
		float clearance = 0.0f;
		RE::FormID owner = 0;
		uint8_t kind = 0;
	};

	struct ActorSink
	{
		RE::ObjectRefHandle handle;
		RE::NiPoint3 written;
		RE::NiPoint3 offset;
		float sink = 0.0f;
		float target = 0.0f;
		float sinceMeasure = 0.0f;
		uint64_t frame = 0;
		bool applied = false;
	};

	struct ProbeFrame
	{
		std::vector<RE::FormID> owners;
		bool pending = false;
	};

	struct Contact
	{
		const void* key = nullptr;
		RE::NiPoint3 centre;
		float radius = 0.0f;
		RE::bhkNiCollisionObject* object = nullptr;
	};

	struct CachedShape
	{
		RE::NiPointer<RE::bhkNiCollisionObject> object;
		RE::NiPointer<RE::NiAVObject> owner;
	};

	struct ActorShapes
	{
		RE::NiPointer<RE::NiAVObject> root;
		std::vector<CachedShape> shapes;
		uint64_t audit = 0;
		uint64_t seen = 0;
		bool ragdoll = false;
	};

	struct BlanketRequest
	{
		int32_t x = 0;
		int32_t y = 0;
		float landZ = 0.0f;
		bool wantLand = false;
		bool wantCap = false;
	};

	struct BlanketResult
	{
		int32_t x = 0;
		int32_t y = 0;
		float landZ = 0.0f;
		float snow = -1.0f;
		float cap = 1.0f;
		bool hasLand = false;
		bool hasCap = false;
	};

	static constexpr uint32_t kProbeLatency = 3;

	struct Slot
	{
		int32_t tileX = 0;
		int32_t tileY = 0;
		float bound = 0.0f;
		bool active = false;
	};

	struct Level
	{
		Texture2D* texture = nullptr;
		Texture2D* activityTexture = nullptr;
		std::array<Slot, kSlots * kSlots> slots{};
		std::array<float, kSlots * kSlots> activity{};
		std::array<uint16_t, (kWindowSpan + 1) * (kWindowSpan + 1)> activitySum{};
		float cellSize = 1.0f;
		float tileSize = 1.0f;
		float fadeStart = 0.0f;
		float fadeEnd = 1.0f;
		int32_t windowTileX = 0;
		int32_t windowTileY = 0;
		int32_t prevTileX = 0;
		int32_t prevTileY = 0;
		float maxBound = 0.0f;
		bool windowValid = false;
		bool prevValid = false;
		bool anyActive = false;
		bool activityDirty = true;
	};

	struct TrackedShape
	{
		float x = 0.0f;
		float y = 0.0f;
		uint64_t frame = 0;
		bool contact = false;
		float width = 0.0f;
	};

	struct CameraKey
	{
		std::array<float, 16> viewProj{};
		float4 posAdjust{};
		uint64_t serial = 0;
		bool clamped = false;
		bool shadow = false;
		bool valid = false;
	};

	struct DebugInfo
	{
		uint32_t stamps = 0;
		uint32_t activeTiles = 0;
		uint32_t dispatchedTiles = 0;
		uint32_t depthDraws = 0;
		uint32_t lightingDraws = 0;
		uint32_t shadowDraws = 0;
		uint32_t decalDraws = 0;
		uint32_t skippedDraws = 0;
		uint32_t layoutRejects = 0;
		bool snowCover = false;
	};

	bool EnsureShaders();
	bool IsActiveContext() const;
	bool IsSnowCoverActive() const;
	float SnowCoverUpperBound(const Stamp& a_stamp, float a_reach) const;
	void UpdateBlanket(const RE::NiPoint3& a_position, RE::FormID a_worldspace);
	bool InBlanketRange(const RE::NiBound& a_bound) const;
	void ClearLevel(Level& a_level);
	void ClearField();
	void UpdateField();
	void GatherStamps();
	void MeasureBlanket();
	void AppendActorStamps(RE::Actor* a_actor, const RE::NiPoint3& a_anchor, std::vector<Stamp>& a_stamps);
	const std::vector<CachedShape>& ActorCollisions(RE::Actor* a_actor, RE::NiAVObject* a_root);
	void AppendObjectStamps(std::vector<Stamp>& a_stamps);
	void AppendHeatStamps(std::vector<Stamp>& a_stamps);
	void AppendImpactStamps(std::vector<Stamp>& a_stamps);
	void BuildImpact(const ImpactEvent& a_event, std::vector<Stamp>& a_stamps);
	void TrackShapeMotion(const void* a_key, const RE::NiPoint3& a_position, bool a_contact, float& a_motionX, float& a_motionY);
	void UpdateActorSinks(const std::vector<RE::ActorPtr>& a_actors);
	void ApplySink(RE::TESObjectREFR* a_ref, ActorSink& a_state, float a_sink);
	void RestoreActorSinks();
	bool IsSinkBlocked(RE::Actor* a_actor) const;
	void DispatchProbes(uint32_t a_stampCount);
	void ReadProbes();
	void PublishMeasurements(const std::vector<std::pair<RE::FormID, float>>& a_measurements);
	void UpdateLevel(uint32_t a_index, const RE::NiPoint3& a_position, float a_decay, float a_deltaTime, bool a_sinking, bool a_gateSnowCover, bool a_skylightingReady);
	void RebuildActivity(Level& a_level);
	bool OverlapsLevel(const Level& a_level, const RE::NiBound& a_bound) const;
	bool OverlapsActivity(const RE::NiBound& a_bound) const;
	bool IsLandscapePass(RE::BSRenderPass* a_pass) const;
	Layout ValidateBoundVertexShader(DrawKind a_kind);
	bool PrepareCamera(DrawKind a_kind, bool a_clamped);
	void BeginLandscapeDraw(RE::BSRenderPass* a_pass, DrawKind a_kind);
	bool BeginDecalDraw(RE::BSRenderPass* a_pass);
	const DecalShaders* DecalShadersFor(ID3D11VertexShader* a_bound);
	bool IsDrawVertexShaderValid() const;
	bool IsLightingBlocked(const RE::BSGeometry* a_geometry) const;
	void BlockLighting(const RE::BSGeometry* a_geometry);
	void EndLandscapeDraw();

	std::array<Level, kLevels> levels{};
	Texture2D* beforeTexture = nullptr;
	eastl::unique_ptr<ConstantBuffer> updateConstants;
	eastl::unique_ptr<ConstantBuffer> drawConstants;
	eastl::unique_ptr<StructuredBuffer> tileList;
	winrt::com_ptr<ID3D11SamplerState> fieldSampler;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11ComputeShader> updateCS;
	winrt::com_ptr<ID3D11ComputeShader> probeCS;
	winrt::com_ptr<ID3D11ComputeShader> blanketCS;
	Texture2D* blanketLandTexture = nullptr;
	Texture2D* blanketTexture = nullptr;
	std::vector<std::array<float, 4>> blanketLand;
	std::vector<std::array<int32_t, 2>> blanketOwner;
	std::vector<uint64_t> blanketRetry;
	std::vector<uint64_t> blanketCapFrame;
	std::vector<BlanketRequest> blanketRequests;
	std::vector<BlanketRequest> blanketScratch;
	std::vector<BlanketResult> blanketResults;
	uint32_t blanketCapCursor = 0;
	RE::FormID blanketWorldspace = 0;
	int32_t blanketMinX = 0;
	int32_t blanketMinY = 0;
	RE::NiPoint3 blanketCentre;
	float blanketHeight = 0.0f;
	bool blanketActive = false;
	uint32_t blanketReady = 0;
	eastl::unique_ptr<Buffer> probeBuffer;
	std::array<winrt::com_ptr<ID3D11Buffer>, kProbeLatency> probeStaging;
	std::array<ProbeFrame, kProbeLatency> probeFrames;
	std::vector<RE::FormID> probeOwners;
	uint32_t probeWrite = 0;
	winrt::com_ptr<ID3D11HullShader> lightingHS;
	winrt::com_ptr<ID3D11DomainShader> lightingDS;
	winrt::com_ptr<ID3D11HullShader> depthHS;
	winrt::com_ptr<ID3D11DomainShader> depthDS;
	winrt::com_ptr<ID3D11Buffer> sharedSnapshot;
	winrt::com_ptr<ID3D11Buffer> featureSnapshot;
	bool shaderCompileFailed = false;
	bool updateSkylighting = false;
	bool snapshotValid = false;
	RE::NiPoint3 snapshotEye;

	bool windowValid = false;
	RE::FormID windowWorldspace = 0;
	uint64_t fieldSerial = 0;
	uint32_t frameStampCount = 0;
	std::vector<float> stampSnow;
	std::vector<float> stampReach;
	RE::NiPoint3 mainPosAdjust;

	std::mutex stampLock;
	std::vector<Stamp> pendingStamps;
	std::vector<Stamp> gatherStamps;
	std::vector<Stamp> frameStamps;
	std::unordered_map<const void*, TrackedShape> shapeMotion;
	std::unordered_map<RE::FormID, bool> bootedRaces;
	std::unordered_map<RE::FormID, float> measuredSinks;
	std::unordered_map<RE::FormID, float> gatherMeasured;
	std::unordered_map<RE::FormID, ActorSink> actorSinks;
	std::vector<Contact> contactScratch;
	std::vector<RE::ObjectRefHandle> objectRefs;
	uint64_t objectScanFrame = 0;
	std::array<ImpactEvent, 64> impactQueue{};
	size_t impactHead = 0;
	size_t impactCount = 0;
	std::mutex impactLock;
	SpellCastSink spellCastSink;
	EquipSink equipSink;
	std::unordered_map<RE::FormID, ActorShapes> actorShapes;
	std::unordered_set<RE::FormID> dirtyActors;
	std::unordered_set<RE::FormID> dirtyScratch;
	std::mutex dirtyLock;
	uint64_t sinkFrame = 0;
	uint32_t sunkActors = 0;
	float deepestSink = 0.0f;
	uint64_t gatherFrame = 0;
	std::atomic<bool> gatherEnabled = false;
	std::atomic<bool> loadClearField = false;
	std::atomic<bool> loadClearMotion = false;
	bool gatherOnUpdate = false;

	bool inZPrepass = false;
	CameraKey mainCamera;
	CameraKey boundCamera;
	std::unordered_set<const RE::BSGeometry*> prepassRouted;
	std::unordered_set<const RE::BSGeometry*> prepassRejected;
	std::unordered_map<const RE::BSGeometry*, uint64_t> lightingRejected;
	std::unordered_map<uint64_t, Layout> lightingLayouts;
	std::unordered_map<uint64_t, Layout> depthLayouts;
	std::unordered_map<uint64_t, DecalShaders> decalShaders;
	std::unordered_map<const RE::BSGeometry*, RE::NiPointer<RE::BSGeometry>> decalGeometries;
	std::mutex decalLock;
	bool lightingLayoutSeen = false;
	bool fieldUpdated = false;
	uint64_t frameIndex = 0;
	std::array<ID3D11VertexShader*, 3> allowedShaders{};
	uint32_t allowedShaderCount = 0;

	bool drawActive = false;
	bool stagesBound = false;
	DrawKind activeKind = DrawKind::Lighting;
	const RE::BSGeometry* activeGeometry = nullptr;
	ID3D11HullShader* activeHS = nullptr;
	ID3D11DomainShader* activeDS = nullptr;

	DebugInfo debug;
	DebugInfo debugShown;
};
