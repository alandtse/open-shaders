#pragma once

#include "FurShells/InertiaSpring.h"
#include "FurShells/Interactors.h"

struct FurShells : Feature
{
	virtual inline std::string GetName() override { return "Fur Shells"; }
	virtual std::string GetDisplayName() override { return T("feature.fur_shells.name", "Fur Shells"); }
	virtual inline std::string GetShortName() override { return "FurShells"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fur_shells.description", "Grows layered fur on armors and creatures at draw time from the shell textures that fur mesh mods ship, so the layered meshes themselves are no longer needed."),
			{ T("feature.fur_shells.key_feature_1", "Uses the _shell.dds texture next to an armor or creature texture"),
				T("feature.fur_shells.key_feature_2", "Works on any body shape, refit or physics mesh"),
				T("feature.fur_shells.key_feature_3", "Shell count drops with distance"),
				T("feature.fur_shells.key_feature_4", "Adjustable length, density and droop") } };
	}

	/** @brief Global fur settings, used for every shell texture that has no override. */
	struct Settings
	{
		bool Enabled = true;
		uint32_t ShellCount = 12;
		float Length = 1.2f;
		float BodyLength = 0.4f;
		float Droop = 0.15f;
		float RootThreshold = 0.0f;
		float TipThreshold = 1.0f;
		float RootDarkening = 0.6f;
		float ShellColor = 1.0f;
		float FadeStart = 700.0f;
		float FadeEnd = 2500.0f;
		bool HideCoveredFur = true;
		bool OverlayFur = true;
		float MinFurPixels = 0.0f;
		float WindStrength = 1.0f;
		float WetStrength = 1.0f;
		float InertiaStrength = 1.0f;
		uint32_t InertiaPreset = static_cast<uint32_t>(FurInertia::Preset::Stylized);
		float InertiaFrequency = FurInertia::Tuning{}.frequencyHz;
		float InertiaGain = FurInertia::Tuning{}.gain;
		float InertiaLengthExponent = FurInertia::Tuning{}.lengthExponent;
		float InertiaDroopExponent = FurInertia::Tuning{}.droopExponent;
		float InertiaMaxBend = FurInertia::Tuning{}.maxBend;
		float TouchStrength = 1.0f;
		bool ShowTouchEdges = false;
	} settings;

	/** @brief Per-shell-texture values, loaded from the .json beside the texture. */
	struct ShellOverride
	{
		bool Enabled = true;
		uint32_t ShellCount = 12;
		float Length = 1.2f;
		float Droop = 0.15f;
		float RootThreshold = 0.0f;
		float TipThreshold = 1.0f;
		float RootDarkening = 0.6f;
		float ShellColor = 1.0f;
		float FadeStart = 700.0f;
		float FadeEnd = 2500.0f;
	};

	virtual bool SupportsVR() override { return true; }

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void PostPostLoad() override;
	virtual void Reset() override;
	virtual void GenerateShaderPermutations(RE::BSShader* a_shader) override;

	/** @brief Replays the deferred fur passes after terrain blending and before blended decals. */
	virtual void OnBeforeBlendedDecals() override;

	/** @brief Reports pass counts, deferred passes, resolved shell textures and depth-copy state for devbench. */
	virtual json GetDiagnostics() override;

	/**
	 * @brief Starts a fur pass for the render pass the engine just set up, or defers it when covered-fur hiding is on.
	 * @param a_shader The lighting shader that set up the pass.
	 * @param a_pass The render pass about to draw.
	 */
	void BeginPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);

	/** @brief Ends the active fur pass and restores the engine's shaders. */
	void EndPass();

	/**
	 * @brief Replaces one engine draw with the instanced fur draws when a fur pass is active.
	 * @param a_indexCount Index count of the engine draw.
	 * @param a_engineInstances Instance count of the engine draw; 2 for a VR stereo draw, otherwise 1.
	 * @param a_startIndexLocation First index of the engine draw.
	 * @param a_baseVertexLocation Base vertex of the engine draw.
	 * @return True when the fur draws replaced the engine draw.
	 */
	bool DrawShells(UINT a_indexCount, UINT a_engineInstances, UINT a_startIndexLocation, INT a_baseVertexLocation);

	/** @brief True while a fur pass is active or deferred passes are replaying, except during the fur draws themselves so VR's DrawIndexedInstanced hook does not re-enter. */
	bool IsDrawing() const { return !issuingDraw && (instanceCount != 0 || replaying); }

private:
	struct Hooks;

	struct PerPass
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float RootTest;
		float Density;
		float TipWeight;
		float Pad0 = 0.0f;
		float Pad1 = 0.0f;
		float4 Wind;
		float WetStrength;
		float3 Inertia;
		float TouchEye[2][4];
		float TouchCount;
		float TouchStrength;
		float TouchDebug;
		float TouchPad1 = 0.0f;
		float TouchA[FurInteractors::MaxCapsulesPerPass][4];
		float TouchB[FurInteractors::MaxCapsulesPerPass][4];
		float TouchMove[FurInteractors::MaxCapsulesPerPass][4];
	};
	static_assert(sizeof(PerPass) == 704);

	struct DeferredPass
	{
		RE::BSRenderPass* pass = nullptr;
		uint32_t technique = 0;
		bool alphaTest = false;
		uint32_t renderFlags = 0;
	};

	struct ShellData
	{
		ShellOverride values;
		bool overridden = false;
		bool skin = false;
	};

	struct Entry
	{
		RE::BSFixedString name;
		RE::NiSourceTexturePtr shell;
		ShellData* data = nullptr;
		bool emptyOverlay = false;
	};

	struct ShellLookup
	{
		RE::NiSourceTexture* shell = nullptr;
		ShellData* data = nullptr;
		bool resolved = false;
		bool emptyOverlay = false;
	};

	struct FrameBody
	{
		RE::NiSkinPartition* partition = nullptr;
		RE::NiAVObject* rootParent = nullptr;
		RE::BSGeometry* geometry = nullptr;
		winrt::com_ptr<ID3D11ShaderResourceView> shellView;
		winrt::com_ptr<ID3D11ShaderResourceView> normalView;
		PerPass perPass{};
		bool deferred = false;
	};

	struct DepthStates
	{
		winrt::com_ptr<ID3D11DepthStencilState> source;
		winrt::com_ptr<ID3D11DepthStencilState> prepass;
		winrt::com_ptr<ID3D11DepthStencilState> shade;
		winrt::com_ptr<ID3D11DepthStencilState> overlay;
	};

	ShellLookup FindShell(RE::BSRenderPass* a_pass);
	const FrameBody* FindFrameBody(RE::BSGeometry* a_geometry) const;
	void RecordFrameBody(RE::NiSkinInstance* a_skinInstance, RE::BSGeometry* a_geometry, const PerPass& a_perPass, ID3D11ShaderResourceView* a_shellView, ID3D11ShaderResourceView* a_normalView, bool a_deferred);
	/** @brief Wind pushing a mesh at a_position, scaled by Wind Strength; zero indoors or when the Wind feature has no sample. */
	float3 GetWind(const RE::NiPoint3& a_position) const;

	/** @brief Advances the geometry's inertia spring once per frame and returns the world-space tip lag in game units. */
	float3 GetInertia(const RE::BSGeometry* a_geometry, float a_length, float a_droop);

	/** @brief The spring tuning the selected inertia preset stands for; Custom uses the sliders. */
	FurInertia::Tuning GetInertiaTuning() const;

	/** @brief Draws the inertia preset selector and, for Custom, its tuning sliders. */
	void DrawInertiaSettings();

	/** @brief Fills a pass's touch capsules with the interactors near a mesh, and the camera offsets the shader needs to place them. */
	void FillTouch(PerPass& a_perPass, const RE::BSGeometry* a_geometry);

	/** @brief True while touch deformation or the Show Touch Edges tint is on, so the capsules are gathered and selected for either. */
	[[nodiscard]] bool IsTouchActive() const { return settings.TouchStrength > 0.0f || settings.ShowTouchEdges; }

	void BeginOverlayPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, const FrameBody& a_body);
	void BindPass(const PerPass& a_perPass, ID3D11ShaderResourceView* a_shellView, ID3D11ShaderResourceView* a_normalView);
	const DepthStates* GetDepthStates(ID3D11DepthStencilState* a_source);
	ShellOverride GetGlobalValues(bool a_skin) const;
	void LoadOverride(const std::string& a_shellPath, ShellData& a_data);
	bool SaveOverride(const std::string& a_shellPath, const ShellOverride& a_values, std::string& a_outputPath);
	void UpdateOverrideFadeEnd();
	void DrawOverrideSettings();
	bool CopySceneDepth();
	void DrawInstanced(UINT a_indexCount, UINT a_instanceCount, UINT a_startIndexLocation, INT a_baseVertexLocation);

	std::unordered_map<std::string, ShellData> shellTextures;
	std::string selectedShellName;
	ShellData* selectedShell = nullptr;
	std::string overrideStatus;
	bool overrideStatusFailed = false;
	float overrideFadeEnd = 0.0f;

	DeferredPass currentPass;
	std::vector<DeferredPass> deferredPasses;
	std::vector<DeferredPass> deferredOverlayPasses;
	std::vector<FrameBody> frameBodies;
	winrt::com_ptr<ID3D11Texture2D> depthCopy;
	winrt::com_ptr<ID3D11ShaderResourceView> depthCopyView;
	bool depthCopyFailed = false;
	bool deferralClosed = false;
	bool replaying = false;
	bool rootTest = false;
	bool overlayPass = false;
	bool issuingDraw = false;
	uint32_t stereoDraws = 0;
	uint32_t monoDraws = 0;
	uint32_t lastStereoDraws = 0;
	uint32_t lastMonoDraws = 0;
	uint32_t maxShells = 0;
	uint32_t lastMaxShells = 0;
	float minDensity = 1.0f;
	float lastMinDensity = 1.0f;
	FurInteractors::Memory dentMemory;
	uint32_t touchCapsules = 0;
	/** @brief The capsules each fur mesh took last frame, for diagnostics. */
	struct TouchSelection
	{
		std::string mesh;
		std::vector<FurInteractors::Capsule> capsules;
	};
	std::vector<TouchSelection> touchSelections;
	std::vector<TouchSelection> lastTouchSelections;
	uint32_t lastTouchCapsules = 0;
	uint32_t maxTouchPerPass = 0;
	uint32_t lastMaxTouchPerPass = 0;
	float touchNearestGap = FLT_MAX;
	float lastTouchNearestGap = FLT_MAX;
	float windSpeed = 0.0f;
	float lastWindSpeed = 0.0f;

	eastl::unique_ptr<ConstantBuffer> perPassCB;
	winrt::com_ptr<ID3D11BlendState> noColorWrite;
	ankerl::unordered_dense::map<ID3D11DepthStencilState*, DepthStates> depthStates;
	ankerl::unordered_dense::map<RE::NiSourceTexture*, Entry> entries;
	ankerl::unordered_dense::set<std::string> missingShells;
	/** @brief False once a background scan proves no `_shell.dds` exists, so passes skip shell lookups; true until then. */
	std::atomic<bool> shellAssetsPresent{ true };

	/** @brief Inertia spring of one geometry and the last frame it was advanced. */
	struct InertiaState
	{
		FurInertia::Spring spring;
		uint32_t frame = 0;
	};
	ankerl::unordered_dense::map<const RE::BSGeometry*, InertiaState> inertiaStates;
	uint32_t frameIndex = 0;
	float maxInertia = 0.0f;
	float lastMaxInertia = 0.0f;
	RE::NiSourceTexture* lastDiffuse = nullptr;
	ShellLookup lastLookup;
	PerPass lastPerPass{};
	winrt::com_ptr<ID3D11VertexShader> savedVertexShader;
	winrt::com_ptr<ID3D11PixelShader> savedPixelShader;
	ID3D11VertexShader* furVertexShader = nullptr;
	ID3D11PixelShader* furPixelShader = nullptr;
	ID3D11PixelShader* depthPixelShader = nullptr;
	uint32_t instanceCount = 0;
	uint32_t resolveBudget = 0;
	uint32_t passCount = 0;
	uint32_t lastPassCount = 0;
};
