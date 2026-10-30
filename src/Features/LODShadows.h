#pragma once

#include "Buffer.h"
#include "LODShadowUpdateBudget.h"
#include "Utils/GpuTimestamps.h"
#include "Utils/LazyShader.h"

#include <bit>

/** @brief Caches directional depth from terrain, distant objects and trees. */
struct LODShadows : Feature
{
	std::string GetName() override { return "LOD Shadows"; }
	std::string GetShortName() override { return "LODShadows"; }
	std::string GetDisplayName() override { return T("feature.lod_shadows.name", "LOD Shadows"); }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	std::string_view GetShaderDefineName() override { return "LOD_SHADOWS"; }
	bool HasShaderDefine(RE::BSShader::Type) override { return true; }
	bool SupportsVR() override { return true; }
	bool IsCore() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	struct Settings
	{
		bool Enabled = true;
		float UpdateBudgetMs = 0.1f;
		float DepthBias = 8.0f;
	} settings;

	/** @brief Loads and validates runtime settings. */
	void LoadSettings(json& a_json) override;
	/** @brief Serializes runtime settings. */
	void SaveSettings(json& a_json) override;
	/** @brief Restores defaults and invalidates cached depth. */
	void RestoreDefaultSettings() override;
	/** @brief Draws the localized feature controls. */
	void DrawSettings() override;
	/** @brief Creates the depth cache and standalone caster pipeline. */
	void SetupResources() override;
	/** @brief Recompiles the standalone caster shaders. */
	void ClearShaderCache() override;
	/** @brief Updates a bounded batch of casters and publishes completed maps. */
	void EarlyPrepass() override;
	/** @brief Filters the opaque receiver mask before directional lighting. */
	void Prepass() override;
	/** @brief Binds the completed maps for reflection shading. */
	void ReflectionsPrepass() override;
	bool WantsDrawHook() const override { return true; }
	/** @brief Binds effect map data and restores comparison filtering after engine state setup. */
	void OnDraw() override;
	/** @brief Invalidates depth when a saved game loads. */
	void GameLoaded() override;
	/** @brief Queues cache invalidation across loading screens. */
	void OnSceneTransitionReset(bool) override;
	/** @brief Exposes cache rebuild and timing diagnostics to devbench. */
	void RegisterUxActions() override;
	/** @brief Exposes nonpersistent refresh, history and filtering comparison controls. */
	json GetRuntimeFlags() override;
	/** @brief Changes rendering comparison controls without saving settings. */
	bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	/** @brief Returns whether complete terrain maps can replace heightmap shadows. */
	bool ReplacesTerrainShadows() const;
	/** @brief Returns the comparison sampler for standalone pixel passes. */
	ID3D11SamplerState* GetComparisonSampler() const { return comparisonSampler.get(); }
	/** @brief Pixel sampler slot shared with LOD shadow consumers. */
	static constexpr uint32_t kComparisonSamplerSlot = 14;

private:
	static constexpr uint32_t kMapCount = 3;
	static constexpr uint32_t kDepthSlicesPerMap = 3;
	static constexpr uint32_t kDepthSliceCount = kMapCount * kDepthSlicesPerMap;
	static constexpr uint32_t kResolution = 2048;
	static constexpr uint32_t kDepthSlot = 117;
	static constexpr uint32_t kMaskSlot = 127;
	static constexpr uint32_t kMaskDownsample = 2;
	static constexpr uint32_t kDataSlot = 118;
	static constexpr uint32_t kRayBoundsSlot = 61;
	static constexpr uint32_t kEffectMapBufferSlot = 7;
	static constexpr uint32_t kRayBoundsTileSize = 16;
	static constexpr uint32_t kRayBoundsResolution = kResolution / kRayBoundsTileSize;
	static constexpr uint32_t kRayBoundsMipCount = std::bit_width(kRayBoundsResolution);
	static constexpr uint32_t kHardwareComparison = 1;
	static constexpr uint32_t kRayBounds = 2;
	static constexpr uint32_t kQueryFrames = 4;
	static constexpr std::array<float, kMapCount> kMinimumHalfExtents{ 18432.0f, 55296.0f, 165888.0f };
	static constexpr std::array<double, kMapCount> kHistoryIntervalsMs{ 600.0, 1500.0, 2500.0 };

	struct alignas(16) MapData
	{
		DirectX::XMFLOAT4X4 worldToClip{};
		float4 parameters{};
		float4 filter{};
		DirectX::XMFLOAT4X4 historyWorldToClip{};
		float4 history{};
	};
	struct alignas(16) DrawData
	{
		DirectX::XMFLOAT4X4 worldToClip{};
		float4 uvTransform{ 1, 1, 0, 0 };
		float4 alpha{ 0, 1, 0, 0 };
		DirectX::XMFLOAT4X4 world{};
		float4 landBounds{};
	};
	static_assert(sizeof(MapData) == 176);
	static_assert(sizeof(DrawData) == 176);
	struct Caster
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		winrt::com_ptr<ID3D11Buffer> vertices;
		winrt::com_ptr<ID3D11Buffer> indices;
		winrt::com_ptr<ID3D11Buffer> instances;
		winrt::com_ptr<ID3D11ShaderResourceView> texture;
		RE::NiTransform transform;
		RE::NiBound bound;
		float4 uvTransform{ 1, 1, 0, 0 };
		float alphaThreshold = 0;
		float materialAlpha = 1;
		uint64_t vertexDescriptor = 0;
		uint32_t stride = 0;
		uint32_t indexCount = 0;
		uint32_t firstIndex = 0;
		uint32_t instanceStride = 0;
		uint32_t instanceCount = 1;
		bool lodLandscape = false;
		bool nativeShadowCaster = false;
	};
	struct Map
	{
		MapData data;
		RE::NiPoint3 center{};
		RE::NiPoint3 direction{};
		float halfExtent = 0;
		float depthRange = 0;
		uint32_t slice = 0;
		uint64_t scene = 0;
		uint32_t frame = 0;
		uint32_t transitionGeneration = 0;
		double completedTimeMs = 0;
		double blendDurationMs = 0;
		double buildDurationMs = 0;
		uint64_t triangleCount = 0;
		bool valid = false;
		bool terrainValid = false;
		bool rayBoundsValid = false;
	};
	struct Query
	{
		Util::TimestampQueryBatch batch;
		uint32_t triangles = 0;
		uint32_t frame = 0;
		bool immediateRefresh = false;
		bool budgetLimited = false;
		bool pending = false;
	};

	void CompileShaders();
	void Invalidate();
	void Publish();
	void CreateMaskResources();
	void DrawMask();
	void CollectTerrain();
	void CollectLoadedObjects();
	void CollectScene(bool a_refreshLoaded);
	void Collect(RE::NiAVObject* a_object, bool a_ignoreAppCulling = false, bool a_loadedObject = false);
	void AddCaster(RE::BSGeometry* a_geometry);
	void BeginMap(uint32_t a_index, const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction);
	void UpdateMaps(const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction, bool a_refreshImmediately);
	void RenderBatch(bool a_refreshImmediately);
	void CaptureDistantDepth();
	bool BuildRayBounds(uint32_t a_slice);
	void ResolveTimings();
	float GetHistoryBlend(uint32_t a_index) const;
	void PublishCompletedMaps(bool a_refreshImmediately);
	bool NeedsUpdate(uint32_t a_index, const RE::NiPoint3& a_position, const RE::NiPoint3& a_direction) const;
	bool Intersects(const Caster& a_caster) const;
	ID3D11InputLayout* GetLayout(const Caster& a_caster);

	std::atomic_bool resetRequested{ true };
	bool ready = false;
	bool clearPending = false;
	bool distantDepthCaptured = false;
	bool wasEnabled = false;
	bool refreshEveryFrame = false;
	bool throttleCollect = true;
	uint32_t framesSinceCollect = 0;
	int32_t collectedGridX = 0;
	int32_t collectedGridY = 0;
	bool interpolateHistory = true;
	bool hardwareComparison = true;
	bool rayBounds = true;
	double frameTimeMs = 0;
	double frameDurationMs = 0;
	bool maskReady = false;
	bool maskFailed = false;
	std::array<uint32_t, 2> maskDimensions{};
	uint32_t worldSpaceID = 0;
	uint32_t transitionGeneration = 0;
	uint64_t sceneHash = 0;
	uint32_t terrainGeometryCount = 0;
	uint32_t loadedTerrainGeometryCount = 0;
	bool terrainAvailable = false;
	float4 landBounds{};
	float4 pendingLandBounds{};
	std::vector<RE::NiPointer<RE::BSGeometry>> visibleGeometry;
	std::vector<Caster> casters;
	std::unordered_set<RE::BSGeometry*> collectedGeometry;
	std::array<uint64_t, 4> sourceHashes{};
	std::array<uint32_t, 4> geometryCounts{};
	uint32_t lastUpdateFrame = 0;
	uint32_t lastRefreshFrame = 0;
	uint32_t lastFilterFrame = 0;
	std::array<Map, kMapCount> maps;
	std::array<Map, kMapCount> previousMaps;
	std::array<Map, kMapCount> completedDepthMaps;
	Map pendingMap;
	int pendingIndex = -1;
	size_t casterIndex = 0;
	uint32_t indexOffset = 0;
	uint32_t instanceOffset = 0;
	uint32_t nextMap = 0;
	LODShadowUpdateBudget updateBudget;
	uint32_t lastDraws = 0;
	uint32_t lastTriangles = 0;
	uint32_t completedMaps = 0;
	uint32_t unsupportedCasters = 0;
	double lastUpdateMs = 0;
	double maxUpdateMs = 0;
	double lastRefreshMs = 0;
	double maxRefreshMs = 0;
	double lastFilterMs = 0;
	uint64_t timingSamples = 0;
	std::array<Query, kQueryFrames> queries;
	std::array<Query, kQueryFrames> filterQueries;
	uint32_t queryCursor = 0;
	uint32_t filterQueryCursor = 0;
	mutable std::mutex diagnosticsMutex;
	json diagnostics;
	winrt::com_ptr<ID3D11Texture2D> depthTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> depthSRV;
	winrt::com_ptr<ID3D11Texture2D> rayBoundsTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> rayBoundsSRV;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kRayBoundsMipCount> rayBoundsMipSRVs;
	std::array<std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, kRayBoundsMipCount>, kDepthSliceCount> rayBoundsUAVs;
	std::array<winrt::com_ptr<ID3D11DepthStencilView>, kMapCount * kDepthSlicesPerMap> depthViews;
	winrt::com_ptr<ID3D11RasterizerState> rasterizer;
	winrt::com_ptr<ID3D11RasterizerState> terrainRasterizer;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11SamplerState> alphaSampler;
	winrt::com_ptr<ID3D11SamplerState> comparisonSampler;
	std::unique_ptr<ConstantBuffer> drawBuffer;
	std::unique_ptr<Buffer> mapBuffer;
	std::unique_ptr<ConstantBuffer> effectMapBuffer;
	std::array<std::unique_ptr<Texture2D>, 2> maskTextures;
	std::unique_ptr<ConstantBuffer> maskBuffer;
	Util::LazyShader<ID3D11ComputeShader> maskShader;
	Util::LazyShader<ID3D11ComputeShader> filterShader;
	Util::LazyShader<ID3D11ComputeShader> rayBoundsShader;
	Util::LazyShader<ID3D11ComputeShader> rayBoundsReduceShader;
	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11VertexShader> treeVertexShader;
	winrt::com_ptr<ID3D11PixelShader> alphaShader;
	winrt::com_ptr<ID3DBlob> vertexBytecode;
	winrt::com_ptr<ID3DBlob> treeBytecode;
	std::array<std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>>, 2> layouts;
};
