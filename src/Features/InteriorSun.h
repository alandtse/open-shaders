#pragma once
#include "Buffer.h"
#include "OverlayFeature.h"
#include "ShaderCache.h"

struct InteriorSun : OverlayFeature
{
public:
	virtual inline std::string GetName() override { return "Interior Sun"; }
	virtual std::string GetDisplayName() override { return T("feature.interior_sun.name", "Interior Sun"); }
	virtual inline std::string GetShortName() override { return "InteriorSun"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.interior_sun.description", "Allows for the sun and moon to cast light and shadows into interior spaces."),
			{ T("feature.interior_sun.key_feature_1", "Lets the sun in only through windows and openings, so it does not leak through walls"),
				T("feature.interior_sun.key_feature_2", "Utilizes existing sun, moon, and weather systems"),
				T("feature.interior_sun.key_feature_3", "Includes an option to force double-sided rendering for unprepared interiors"),
				T("feature.interior_sun.key_feature_4", "Fixes geometry culling issues that cause light leakage") } };
	};

	/** @brief Draws the ImGui settings UI for Interior Sun configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual bool SupportsVR() override { return true; }
	virtual void PostPostLoad() override;
	/** @brief Updates the interior sun state based on the current cell during the early prepass. */
	virtual void EarlyPrepass() override;
	/** @brief Binds the window planes for the lighting shader (t66), or nothing when they are off. */
	virtual void Prepass() override;
	/** @brief Creates the GPU resources used to draw window glass into the sun shadow maps. */
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	/** @brief Debug view: the traced sun ray and the shapes it crosses. */
	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override { return traceValid; }

	/** @brief One shape the traced sun ray crosses. Kind: 0 glass, 1 opening, 2 rejected see-through shape, 3 solid. */
	struct TraceHit
	{
		float distance;
		int kind;
		std::string what;
	};
	bool traceValid = false;
	float3 traceStart = float3(0, 0, 0);
	float3 traceDirection = float3(0, 0, 1);
	std::string traceStartWhat;
	std::vector<TraceHit> traceHits;

	/** @brief Debug tools, drawn at the end of the settings when debug sections are shown. */
	void DrawDebugTools();
	/** @brief Debug: from the point under the crosshair, lists every shape along the sun's ray, both ways (what lets a leak through). */
	void TraceSunFromCrosshair();

	/** @brief Registers the menu open/close event handler for tracking main menu state. */
	virtual void DataLoaded() override
	{
		MenuOpenCloseEventHandler::Register();
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		heliosLoaded = dataHandler && dataHandler->LookupModByName("Helios.esp");
	}

	struct Settings
	{
		bool ForceDoubleSidedRendering = true;
		float InteriorShadowDistance = 5000;
		bool WindowLightOnly = true;
		bool DetectOpenings = true;
		bool HideFakeWindowGlows = true;
		bool SkyLitGlass = false;
		float SkyLitGlassStrength = 1.0f;
		bool AutoSunlitInteriors = false;
		bool SunFacingHolesOnly = true;
		bool LightOnlyPastWindow = true;
		bool CloseShadowGaps = false;
		int ShadowGapTexels = 2;
		bool FrameShadowsFromGlowMaps = true;
		float GlowMapThreshold = 0.5f;
		bool SunColorFromWeather = true;
	};

	Settings settings;

	std::atomic<bool> isInteriorWithSun = false;

	/** @brief Hook that intercepts world space queries to enable/disable interior sun based on cell flags. */
	struct GetWorldSpace
	{
		static RE::TESWorldSpace* thunk(RE::TES* tes);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that overrides directional shadow light culling to include rooms visible from the sun direction. */
	struct DirShadowLightCulling
	{
		static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>& nodes);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that updates the raster cull mode before each render pass to support double-sided rendering. */
	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that runs before a camera's accumulated geometry is drawn, once its targets are bound and cleared. */
	struct BSShaderAccumulator_StartAccumulating
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, RE::NiCamera* a_camera);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Conditionally disables backface culling for shadow rendering in interior sun cells.
	 * @param pass The current render pass being processed.
	 * @param technique The shader technique flags for the pass.
	 */
	void UpdateRasterStateCullMode(const RE::BSRenderPass* pass, const uint32_t technique) const
	{
		if (isInteriorWithSun && settings.ForceDoubleSidedRendering && technique & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmap)) {
			const auto flags = pass->shaderProperty->flags;
			const auto renderTwoSided = flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) || flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask, RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);
			if (renderTwoSided && *rasterStateCullMode != 0) {
				*rasterStateCullMode = 0;
				globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
			} else if (!renderTwoSided && *rasterStateCullMode != RE::BSGraphics::RasterStateCullMode::RASTER_STATE_CULL_MODE_BACK) {
				*rasterStateCullMode = RE::BSGraphics::RasterStateCullMode::RASTER_STATE_CULL_MODE_BACK;
				globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
			}
		}
	}

	/** @brief Event handler that resets interior sun state when the main menu opens. */
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		/** @brief Processes menu open/close events to reset interior sun state on main menu. */
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*);

		/** @brief Registers the singleton event handler with the UI event source. */
		static bool Register()
		{
			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;
			if (!ui) {
				logger::error("UI event source not found");
				return false;
			}
			ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

			logger::info("Registered {}", typeid(singleton).name());
			return true;
		}
	};

	/**
	 * @brief Checks whether a cell is an interior with sunlight shadow flags enabled.
	 * @param cell The cell to check.
	 * @return True if the cell is an interior configured for sun and sky lighting with sunlight shadows.
	 */
	static bool IsInteriorWithSun(const RE::TESObjectCELL* cell);
	/** @brief Returns whether this feature is loaded and the current cell has interior sun enabled. */
	bool IsActiveInteriorSun() const { return loaded && isInteriorWithSun.load(); }
	virtual bool IsCore() const override { return true; };

private:
	enum class CellFlagExt : uint16_t
	{
		kSunlightShadows = 1 << 15,
	};

	float* gShadowDistance = nullptr;
	float* gInteriorShadowDistance = nullptr;
	uint32_t* rasterStateCullMode = nullptr;

	RE::TESObjectCELL* currentCell = nullptr;
	RE::NiPointer<RE::BSPortalGraph> currentPortalGraph = nullptr;

	bool arraysCleared = true;
	RE::BSTArray<RE::NiPointer<RE::NiAVObject>> currentCellRoomsAndPortals = {};
	RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>> replacementJobArrays = {};
	eastl::hash_set<RE::NiAVObject*> addedSet = {};

	static RE::TESWorldSpace* enableInteriorSun;
	static RE::TESWorldSpace* disableInteriorSun;

	void ClearArrays();

	void InitialiseOnNewCell(const RE::NiPointer<RE::BSPortalGraph>& portalGraph);

	bool IsInSunDirectionAndWithinShadowDistance(const RE::NiPointer<RE::NiAVObject>& object, const RE::NiPoint3& lightDir, const RE::NiPoint3& playerPos) const;

	void PopulateReplacementJobArrays(RE::TESObjectCELL* cell, const RE::NiPointer<RE::BSPortalGraph>& portalGraph, const RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays);

	static void SetShadowDistance(bool inInterior);

	struct alignas(16) WindowHolesCB
	{
		float4x4 ViewProj;
		float3 PosAdjust;
		float GlowThreshold;   // glass opens only where its glow map is at least this bright
		uint32_t Mode;         // HoleMode: how the bound texture decides where the hole opens
		float AlphaThreshold;  // openings open only where their texture is see-through (below this alpha)
		float pad0[2];
	};

	enum HoleMode : uint32_t
	{
		kWhole = 0,       // the whole shape opens
		kBrightGlow = 1,  // glass: opens where its glow map is bright (panes), not on the frame
		kSeeThrough = 2,  // openings: opens only where its own texture is see-through
	};

	/** @brief A run of hole triangles sharing one glow map (none: the whole shape opens). */
	struct HoleDraw
	{
		uint32_t first;
		uint32_t count;
		RE::NiPointer<RE::NiSourceTexture> texture;
		HoleMode mode = kWhole;
		float alphaThreshold = 0.5f;
		float3 outward = float3(0, 0, 0);  // the side the sun must be on to come through; zero when unknown (always open)
	};

	// Rescans after entering a cell, in frames: its 3D can still be loading on the first ones
	static constexpr uint32_t WindowGlassScanFrames[] = { 1, 60, 300 };

	/** @brief A see-through shape that may be an opening in the cell's shell, and what the shell test found. */
	struct OpeningCandidate
	{
		std::string model;
		std::string shape;
		RE::FormID ref = 0;
		uint32_t firstTriangle = 0;
		uint32_t triangleCount = 0;
		uint32_t openSamples = 0;
		uint32_t samples = 0;
		float3 outward = float3(0, 0, 0);            // mean direction of the open side
		RE::NiPointer<RE::NiSourceTexture> diffuse;  // its see-through pixels are the opening
		float alphaThreshold = 0.5f;
		bool opening = false;
	};

	/** @brief One glass shape, for the sun trace. */
	struct GlassShape
	{
		RE::FormID ref;
		std::string model;
		std::string shape;
		uint32_t first;
		uint32_t count;
	};
	std::vector<GlassShape> windowGlassShapeInfo;

	/** @brief Shell test of one cell's candidates, run on a worker thread on copies of the cell's triangles. */
	struct OpeningScan
	{
		RE::TESObjectCELL* cell = nullptr;
		std::vector<float3> occluders;           // triangle list of everything that blocks light; freed once tested
		std::vector<uint32_t> occluderShapes;    // first triangle of each occluder shape, plus the end
		std::vector<float3> candidateTriangles;  // triangle list of the candidates
		std::vector<float2> candidateUVs;
		std::vector<float3> glassTriangles;                      // the glass, to find which way each pane faces outward
		std::vector<std::pair<uint32_t, uint32_t>> glassShapes;  // first and count per pane
		std::vector<float3> glassOutward;                        // result per pane; zero when the test can't tell
		std::vector<OpeningCandidate> candidates;
		std::vector<float3> openings;  // result: triangle list of the shapes found to be openings
		std::vector<float2> openingUVs;
		std::vector<HoleDraw> openingRuns;  // per opening shape, in openings
		std::atomic<bool> done = false;
	};

	struct GlassEmissive
	{
		RE::NiPointer<RE::BSLightingShaderProperty> property;
		RE::NiColor color;
		float mult;
	};

	std::thread::id renderThread;
	RE::TESObjectCELL* windowGlassCell = nullptr;
	uint32_t windowGlassCellFrames = 0;
	uint32_t windowGlassVertexCount = 0;
	uint32_t windowHolesDrawn = 0;

	std::shared_ptr<OpeningScan> openingScan;  // in flight or finished; dropped on leaving the cell
	std::shared_ptr<std::vector<float3>> lastCandidateTriangles;
	std::shared_ptr<OpeningScan> lastOpeningScan;  // the finished scan of this cell, for the debug views
	bool glassEmissivesLit = false;
	std::vector<float3> windowOpeningVertices;
	std::vector<float2> windowOpeningUVs;
	std::vector<HoleDraw> windowOpeningRuns;

	std::vector<RE::NiPointer<RE::NiAVObject>> hiddenFakeGlows;
	std::vector<GlassEmissive> glassEmissives;
	std::unordered_map<RE::TESObjectCELL*, uint16_t> autoSunlitCells;  // cells given the sun, with their own flags to restore

	struct alignas(16) ShadowGapsCB
	{
		uint32_t Size[2];
		uint32_t Slice;
		int32_t Radius;
	};

	ConstantBuffer* shadowGapsCB = nullptr;
	std::unique_ptr<Texture2D> shadowGapsGrown;
	winrt::com_ptr<ID3D11ComputeShader> shadowGapsMinCS[2];  // plain and array shadow textures
	winrt::com_ptr<ID3D11VertexShader> shadowGapsVS;
	winrt::com_ptr<ID3D11PixelShader> shadowGapsPS;

	/** @brief Closes slivers of light in the sun cascades of a sunlit interior that has no window holes. */
	void CloseShadowGaps();

	std::vector<float3> windowGlassVertices;
	std::vector<float2> windowGlassUVs;
	std::vector<HoleDraw> windowGlassRuns;   // per glass shape, in windowGlassVertices
	std::vector<float3> windowGlassOutward;  // per glass shape, from the shell test
	std::vector<uint8_t> lastOpenMask;       // which holes were open at the last upload, glass then openings

	// Per sun cascade, the plane of the window each shadow-map texel's light came through (outward xyz, offset w)
	static constexpr uint32_t WindowPlaneSize = 1024;
	winrt::com_ptr<ID3D11Texture2D> windowPlanes;
	winrt::com_ptr<ID3D11ShaderResourceView> windowPlanesSRV;
	winrt::com_ptr<ID3D11RenderTargetView> windowPlanesRTV[2];
	winrt::com_ptr<ID3D11PixelShader> windowPlanesPS;
	uint32_t windowPlanesWritten = 0;  // cascades written this frame (bits)
	bool windowPlanesValid = false;    // the lighting shader may read them this frame

	/** @brief Writes the holes' window planes for one sun cascade, at the plane texture's resolution. */
	void DrawWindowPlanes(uint32_t cascade, WindowHolesCB data, const D3D11_VIEWPORT& shadowViewport, uint32_t shadowWidth, uint32_t shadowHeight);

	/** @brief Direction toward the sun in world space; zero when there is none. */
	static float3 GetToSun();
	/** @brief Whether a hole lets the sun through now: open unless the sun is behind its outward side. */
	bool IsHoleOpen(const float3& outward, const float3& toSun) const;
	/** @brief Which holes are open now, glass then openings, to rebuild the holes when it changes. */
	std::vector<uint8_t> ComputeOpenMask(const float3& toSun) const;
	std::vector<HoleDraw> windowHoleDraws;  // what the hole pass draws, grouped by glow map
	winrt::com_ptr<ID3D11PixelShader> windowHolesPS;
	winrt::com_ptr<ID3D11SamplerState> windowHolesSampler;
	std::unique_ptr<Buffer> windowGlass;
	ConstantBuffer* windowHolesCB = nullptr;
	winrt::com_ptr<ID3D11VertexShader> windowHolesVS;
	winrt::com_ptr<ID3D11RasterizerState> windowHolesRasterState;
	winrt::com_ptr<ID3D11DepthStencilState> windowHolesDepthState;

	ID3D11VertexShader* GetWindowHolesVS();

	ID3D11PixelShader* GetWindowHolesPS();

	void UpdateWindowGlass(RE::TESObjectCELL* cell);

	void CollectWindowGlass(RE::TESObjectCELL* cell);

	/** @brief Copies the cell's solid static geometry into the scan, for the shell test's rays; names each shape when asked (sun trace). */
	static void CollectOccluders(RE::TESObjectCELL* cell, OpeningScan& scan, std::vector<std::string>* names = nullptr);

	/** @brief Rebuilds the GPU triangle list from the glass and the openings found so far. */
	void UploadWindowHoles(const RE::TESObjectCELL* cell);

	/** @brief Takes the result of a finished opening scan, if any. */
	void TakeOpeningScan();

	/** @brief The shell test: marks the candidates that have the room on one side and nothing on the other. */
	static void RunOpeningScan(OpeningScan& scan);

	/** @brief Gives a cell with windows or openings the sun, unless it is a cave or dungeon. */
	void UpdateAutoSunlit(RE::TESObjectCELL* cell);

	/** @brief Puts back the flags of every cell given the sun automatically. */
	void RestoreAutoSunlitCells();

	void RestoreFakeGlows();

	void RestoreGlassEmissives();

	/** @brief Lights the glass from the sky's colour while the cell has the sun. */
	void UpdateSkyLitGlass();
	/**
	 * @brief In sunlit interiors, overwrites the sun light's diffuse every frame with the weather's sunlight colour,
	 * since indoors the engine takes it from the cell's lighting record (dim in dungeons); other writers are overwritten.
	 */
	void UpdateSunColor();

	void ReleaseWindowGlass();

	/** @brief Turns the bound sun shadow map into "all shadowed" with a hole at each window pane. */
	void DrawWindowHoles(const RE::NiCamera* camera);

	/** @brief Draws the holes into the bound depth target with the given camera data; puts back every state it changes. */
	void DrawHolesInto(WindowHolesCB data, const D3D11_VIEWPORT& viewport);

	// Settings whose change needs the cell scanned or the holes rebuilt again
	bool lastDetectOpenings = true;
	bool lastFrameShadows = true;
	bool lastHideFakeGlows = true;
	bool lastSunFacing = true;
	bool lastInteriorWithSun = false;
	RE::NiColor engineSunColor{}, writtenSunColor{};  // the cell's own sun colour, and what we last wrote over it
	bool sunColorWritten = false;
	bool forceHolesUpload = false;
	bool heliosLoaded = false;  // Helios drives window glow itself, so Sky-Lit Glass stands down
};
