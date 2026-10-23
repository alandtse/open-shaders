#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/HeightMap.h"

// C4324: the 16-byte-aligned PerFrame member requires padding in SnowCover.
#pragma warning(push)
#pragma warning(disable: 4324)

struct SnowCover : Feature
{
private:
	static constexpr float DEFAULT_FOLIAGE_EFFECT_OFFSET = -2048.0f;
	static constexpr float DEFAULT_UV_SCALE = 0.5f;
	static constexpr float DEFAULT_PEAK_MAIN_ANGLE = 0.45f;
	static constexpr float DEFAULT_PEAK_ALT_ANGLE = 0.9f;
	static constexpr float DEFAULT_MIN_ANGLE = 0.3f;
	static constexpr float DEFAULT_MAX_ANGLE = 0.9f;
	static constexpr float DEFAULT_MAIN_SPEC = 0.02f;
	static constexpr float DEFAULT_ALT_SPEC = 0.02f;
	static constexpr float DEFAULT_MAP_ZSCALE = 75000.0f;
	static constexpr float DEFAULT_GLINT_1 = 1.2f;
	static constexpr float DEFAULT_GLINT_2 = 33.f;
	static constexpr float DEFAULT_GLINT_3 = .15f;
	static constexpr float DEFAULT_GLINT_4 = 2.f;
	static constexpr float DEFAULT_BLEND_SMOOTHNESS = 5000.0f;
	static constexpr float DEFAULT_WEATHER_FADE_START = 60000.0f;
	static constexpr float DEFAULT_WEATHER_FADE_END = 150000.0f;
	static constexpr float DEFAULT_OBJECT_FADE_START = 6144.0f;
	static constexpr float DEFAULT_OBJECT_FADE_END = 9192.0f;
	static constexpr float DEFAULT_OBJECT_FADE_AMOUNT = 0.5f;
	static constexpr float2 DEFAULT_MAP_MIN = float2(-233472.0, 208896.0);
	static constexpr float2 DEFAULT_MAP_MAX = float2(253952.0, -176128.0);
	static constexpr float DEFAULT_SUMMER_HEIGHT_OFFSET = 20000.0f;
	static constexpr float DEFAULT_WINTER_HEIGHT_OFFSET = -20000.0f;
	static constexpr uint DEFAULT_PEAK_SUMMER_MONTH = 6;
	static constexpr uint DEFAULT_PEAK_WINTER_MONTH = 0;
	static constexpr uint32_t FIRST_SRV_SLOT = 38;
	static constexpr uint32_t MAX_FIRE_MELT_SOURCES = 8;
	static constexpr uint32_t MAX_TRACKED_FIRES = 64;
	static constexpr uint32_t MAX_FIRE_BASE_CLASSIFICATIONS_PER_SCAN = 512;
	static constexpr float FIRE_MELT_MAX_DISTANCE = 16384.0f;
	static constexpr float FIRE_MELT_DISTANCE_FADE = 2048.0f;
	static constexpr float FIRE_MELT_SLOT_FADE = 512.0f;
	static constexpr float FIRE_MELT_MIN_RADIUS = 16.0f;
	static constexpr float FIRE_MELT_MAX_RADIUS = 512.0f;
	static constexpr float FIRE_MELT_CLUSTER_DISTANCE = 256.0f;
	static constexpr float FIRE_MELT_MAX_REF_OFFSET = 4096.0f;
	static constexpr float FIRE_MELT_SCAN_INTERVAL = 0.25f;
	static constexpr uint32_t FIRE_MELT_GRACE_SCANS = 3;
	static constexpr float FIRE_MELT_FADE_TIME = 1.0f;
	static constexpr uint32_t FIRE_MELT_FOOTPRINT_SAMPLES = 32;
	static constexpr float FIRE_MELT_FOOTPRINT_SMOOTHING = 0.5f;
	static constexpr float DEFAULT_ROOFTOP_SNOW = 3000.0f;
	static constexpr float DEFAULT_ROOFTOP_HEIGHT = 192.0f;
	static constexpr float DEFAULT_ROOFTOP_EXPOSURE = 0.7f;

public:
	std::string GetName() override { return "Snow Cover"; }
	std::string GetShortName() override { return "SnowCover"; }
	std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	std::string_view GetShaderDefineName() override { return "SNOW_COVER"; }

	bool HasShaderDefine(RE::BSShader::Type type) override
	{
		return type == RE::BSShader::Type::Lighting || type == RE::BSShader::Type::Grass || type == RE::BSShader::Type::DistantTree;
	}
	bool SupportsVR() override { return true; }
	std::string GetDisplayName() override { return T("feature.snow_cover.name", "Snow Cover"); }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.snow_cover.summary", "Seasonal snow cover, weather accumulation, foliage tint and melting near placed fires."), {} };
	}

	struct UserSettings
	{
		uint EnableExpensiveFoliage = 1;
		float SnowHeightOffset = 0.0f;
		uint AffectHavok = 0;
		uint AffectFloraTint = 0;

		float RooftopSnow = DEFAULT_ROOFTOP_SNOW;
		float RooftopHeight = DEFAULT_ROOFTOP_HEIGHT;
		float RooftopExposure = DEFAULT_ROOFTOP_EXPOSURE;
		uint Enable = 0;
	};
	static_assert(sizeof(UserSettings) % 16 == 0);

	struct WorldSettings
	{
		uint EnableSnowCover = false;
		uint AffectGrassTint = true;
		uint AffectTreeTint = true;
		float FoliageHeightOffset = DEFAULT_FOLIAGE_EFFECT_OFFSET;

		float UVScale = DEFAULT_UV_SCALE;
		float PeakMainAngle = DEFAULT_PEAK_MAIN_ANGLE;
		float PeakAltAngle = DEFAULT_PEAK_ALT_ANGLE;
		float MinAngle = DEFAULT_MIN_ANGLE;

		float MaxAngle = DEFAULT_MAX_ANGLE;
		float MainSpec = DEFAULT_MAIN_SPEC;
		float AltSpec = DEFAULT_ALT_SPEC;
		float mapZscale = DEFAULT_MAP_ZSCALE;

		float2 mapScale;
		float2 mapOffset;
		float ScreenSpaceScale = DEFAULT_GLINT_1;
		float LogMicrofacetDensity = DEFAULT_GLINT_2;
		float MicrofacetRoughness = DEFAULT_GLINT_3;
		float DensityRandomization = DEFAULT_GLINT_4;

		float4 MainTint = float4(1.0f, 1.0f, 1.0f, 1.0f);
		float4 AltTint = float4(1.0f, 1.0f, 1.0f, 1.0f);

		float BlendSmoothness = DEFAULT_BLEND_SMOOTHNESS;
		float WeatherFadeStart = DEFAULT_WEATHER_FADE_START;
		float WeatherFadeEnd = DEFAULT_WEATHER_FADE_END;
		float ObjectFadeStart = DEFAULT_OBJECT_FADE_START;

		float ObjectFadeEnd = DEFAULT_OBJECT_FADE_END;
		float ObjectFadeAmount = DEFAULT_OBJECT_FADE_AMOUNT;
		uint pad[2]{};
	};
	static_assert(sizeof(WorldSettings) % 16 == 0);
	static_assert(sizeof(WorldSettings) == 144);

	struct FireMeltData
	{
		uint Count = 0;
		float Strength = 0.0f;
		float RadiusScale = 1.0f;
		uint pad = 0;
		float4 Spheres[MAX_FIRE_MELT_SOURCES]{};
	};
	static_assert(sizeof(FireMeltData) == 16 + 16 * MAX_FIRE_MELT_SOURCES);

	struct alignas(16) PerFrame
	{
		float Month;
		float TimeSnowing;
		float SnowingDensity;
		float SeasonalAltitude;

		UserSettings settings;
		WorldSettings wsettings;
		FireMeltData fireMelt;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(sizeof(PerFrame) == 192 + sizeof(FireMeltData));

	struct FireMeltSettings
	{
		bool Enabled = true;
		float RadiusScale = 4.0f;
		float Strength = 1.0f;
	};

	UserSettings settings;
	WorldSettings wsettings;
	FireMeltSettings fireMeltSettings;
	PerFrame perFrame{};

	struct TrackedFire
	{
		RE::FormID refID = 0;
		uint32_t cluster = 0;
		RE::NiPoint3 sampleCenter;
		float sampleRadius = 0.0f;
		RE::NiPoint3 center;
		float radius = 0.0f;
		float strength = 0.0f;
		uint32_t missedScans = 0;
		uint32_t samples = 0;
	};

	struct FireCluster
	{
		RE::FormID refID = 0;
		uint32_t cluster = 0;
		RE::NiBound bound;
		float distanceSq = 0.0f;
	};

	std::vector<TrackedFire> trackedFires;
	std::vector<RE::NiBound> fireSamples;
	std::vector<FireCluster> fireClusters;
	std::vector<std::pair<float, uint32_t>> fireOrder;
	std::unordered_map<RE::FormID, bool> fireBaseCache;
	float fireScanTimer = FIRE_MELT_SCAN_INTERVAL;
	bool fireMeltSnap = true;

	void UpdateFireMelt();
	bool ScanFireSources();
	void UploadFireMelt();

	PerFrame GetCommonBufferData(bool inWorld);

	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 7> views;

	std::string status;
	std::string lastWorldspace;
	std::filesystem::path mapTexture;
	std::filesystem::path mainTexture;
	std::filesystem::path alternateTexture;
	char mapTextureInput[256] = "";
	char mainTextureInput[256] = "";
	char alternateTextureInput[256] = "";

	float snowingSpeed = 1.0f;
	float meltingSpeed = 1.0f;
	float2 mapMin = DEFAULT_MAP_MIN;
	float2 mapMax = DEFAULT_MAP_MAX;
	uint MaxSummerMonth = DEFAULT_PEAK_SUMMER_MONTH;
	uint MaxWinterMonth = DEFAULT_PEAK_WINTER_MONTH;
	float SummerHeightOffset = DEFAULT_SUMMER_HEIGHT_OFFSET;
	float WinterHeightOffset = DEFAULT_WINTER_HEIGHT_OFFSET;

	float lastGameHours = -1.0f;
	Util::FrameChecker frame;
	float timeSnowing = 0.0f;
	float snowingDensity = 0.0f;
	std::vector<std::string> geometryNames;
	std::unordered_set<std::string_view> whitelist;
	std::unordered_set<std::string_view> blacklist;

	void SetupResources() override;
	/** @brief Applies the master switch without recompiling shaders. */
	void Reset() override;
	void Prepass() override;
	/** @brief Binds snow textures for cubemap materials. */
	void ReflectionsPrepass() override { Prepass(); }
	/** @brief Adds an optional snow condition to Survival Mode's region spell. */
	void DataLoaded() override;
	/** @brief Publishes the snow condition from the game thread. */
	void OnWeatherColorsUpdated(RE::Sky*) override;
	/** @brief Restores Survival Mode's original region behavior. */
	void OnRuntimeDisabled() override;

	void DrawSettings() override;

	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	void RestoreDefaultSettings() override;
	void Reload();
	void SaveConfig();
	void ResetWorldConfig();
	void UpdateMapTransform();

	/** @brief Applies geometry exclusions through the existing render-pass contract. */
	bool WantsRenderPassHook() const override { return true; }
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* pass) override;
	void OnSceneTransitionReset(bool opening) override;
	void RegisterUxActions() override;
	json GetDiagnostics() override;

private:
	std::atomic<bool> active{ false };
	bool resourcesReady = false;
	bool texturesLoaded = false;
	void ResetRuntimeState();
	void UpdateFrame();
	void LoadGeometryLists();
	void LoadWorldConfig(const json& config);
	bool LoadTextures();
	bool TryLoadTextures();
	void ValidateSettings();
	void DrawWorldSettings();
	void DrawMaterialSettings();
	bool IsBuilding(RE::TESObjectREFR* ref);
	bool IsSnowFree(const RE::BSRenderPass* pass, uint32_t flags) const;
	void UpdateSurvival();
	void ReleaseTextures();
	Util::HeightMap snowMap;
	std::unordered_map<RE::FormID, bool> buildingBaseCache;
	bool survivalEnabled = true;
	std::atomic<bool> survivalSnowy{ false };
	float survivalSnowAmount = 0.0f;
	RE::TESGlobal* survivalGlobal = nullptr;
};

#pragma warning(pop)
