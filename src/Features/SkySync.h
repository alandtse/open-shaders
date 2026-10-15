#pragma once

#include "RE/M/Moon.h"

#include "Utils/Moon.h"

/** @brief Synchronizes volumetric lighting and shadow direction with actual sun and moon positions. */
struct SkySync : Feature
{
private:
	static constexpr std::string_view MOD_ID = "153543";

public:
	virtual inline std::string GetName() override { return "Sky Sync"; }
	virtual std::string GetDisplayName() override { return T("feature.sky_sync.name", "Sky Sync"); }
	virtual inline std::string GetShortName() override { return "SkySync"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.sky_sync.description", "Synchronizes volumetric lighting and shadows with the actual sun and moon positions in the sky."),
			{ T("feature.sky_sync.key_feature_1", "Fixes the mismatch between the positions of the sun and moons and the lighting direction"),
				T("feature.sky_sync.key_feature_2", "Includes a configurable alternative sun path for more realistic and dramatic lighting"),
				T("feature.sky_sync.key_feature_3", "Smoothly switches the light source between the sun and moons based on visibility"),
				T("feature.sky_sync.key_feature_4", "Moon light source can be switched between Masser, Secunda, or the brightest"),
				T("feature.sky_sync.key_feature_5", "Automatic calculation of moon lighting intensity based on moon phase"),
				T("feature.sky_sync.key_feature_6", "Fixes the sun appearing higher on the horizon when the player gains altitude") } };
	};

	struct Settings
	{
		bool Enabled = true;
		bool UseAlternateSunPath = false;
		int32_t MoonLightSource = 0;
		int32_t SunPath = 0;
		float CustomAngle = -35.0f;
		float MinShadowElevation = 0.0f;
		float ShadowTransitionDuration = 100.0f;
		bool DimSunlightUnderHorizon = true;
		bool DimVolumetricLighting = true;
		float HorizonFadeHours = 0.7f;
		float NewMoonIntensity = 0.05f;
		float CrescentMoonIntensity = 0.25f;
		float FullMoonIntensity = 1.0f;
	};

	Settings settings;

	/** @brief Draws the ImGui settings panel for Sky Sync configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	/** @brief Exposes the active celestial source and horizon transition state. */
	void RegisterUxActions() override;

	virtual bool IsCore() const override { return true; }
	virtual bool SupportsVR() override { return true; }

	/**
	 * @brief Dims sunlight color when the sun is below the horizon during sky color updates.
	 * @param sky The sky object whose directional light color may be modified.
	 */
	void OnSkyUpdateColors(RE::Sky* sky);
	/** @brief Blends the weather's night lighting into the selected moon contribution before preset processing. */
	void OnWeatherColorsComputed(RE::Sky* sky) override;

	/** @brief Returns sun/Masser/Secunda transition weights, or nullopt when synchronization is inactive. */
	std::optional<float3> GetCelestialLightWeights() const;
	/** @brief Returns the blended world-space celestial direction before shadow elevation limits, or nullopt when inactive. */
	std::optional<RE::NiPoint3> GetCelestialLightDirection() const;

	/** @brief Installs rendering hooks and detects conflicting mods after plugin load. */
	virtual void PostPostLoad() override;
	/** @brief Checks for conflicting ESP files after game data is loaded. */
	virtual void DataLoaded() override;
	/** @brief Requests immediate celestial synchronization after loading an existing save. */
	virtual void GameLoaded() override;

	struct Sky_Update
	{
		static void thunk(RE::Sky* sky);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Returns the visible moon selected to illuminate the scene. */
	RE::Moon* GetVisibleMoonLightSource(const RE::Sky* sky) const;

	enum class Caster : uint8_t
	{
		Sun,
		Masser,
		Secunda,
		None
	};

	/** @brief Returns a body's synchronized world direction before shadow clamping, or nullopt when inactive. */
	std::optional<RE::NiPoint3> GetCelestialDirection(Caster caster) const;

private:
	enum class CellFlagExt : uint16_t
	{
		kSunlightShadows = 1 << 15,
	};

	enum class MoonLightSource : uint8_t
	{
		Brightest,
		Masser,
		Secunda,
		Count
	};

	enum class SunPath : uint8_t
	{
		Southern,
		Northern,
		Vanilla,
		Custom,
		Count
	};

	struct ShadowFader
	{
		RE::NiPoint3 currentDir = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 startDir = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 celestialDir = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 startCelestialDir = { 0.0f, 0.0f, 1.0f };
		float3 lightWeights = { 1.0f, 0.0f, 0.0f };
		float3 startLightWeights = { 1.0f, 0.0f, 0.0f };
		Caster target = Caster::Sun;
		Caster previousTarget = Caster::Sun;
		float fadeTimer = 0.0f;
		float immediateTransitionRemaining = 0.0f;
		bool transitioning = false;

		void Update(const RE::Sky* sky, RE::NiPoint3 dirs[], float intensities[], float fadeDuration, float fadeAdvance, bool a_immediateTransition);
		static void SetLighting(const RE::Sky* sky, RE::NiPoint3 dir);
		static void SetDirection(RE::NiPoint3& dir, float headingRadians, float elevRadians);
		static void SetElevation(RE::NiPoint3& dir, float elevRadians);
		static void ClampDirection(RE::NiPoint3& dir);
		void Reset();
	};

	static constexpr float SunHorizonDistance = 280.0f;
	static constexpr float SunPeakDistance = 400.0f;
	static constexpr float SouthernSunAngle = 90.0f - 35.0f;
	static constexpr float NorthernSunAngle = 90.0f + 35.0f;
	static constexpr float VanillaSunAngle = 90.0f + 5.0f;
	static constexpr float SecondsPerGameHour = 3600.0f;
	static constexpr float MaxHorizonFadeHours = 1.5f;
	static Caster SelectMoonCaster(float masser, float secunda, Caster previous);

	inline static RE::NiPoint3* gSunPosition = nullptr;
	inline static RE::BSVolumetricLightingRenderData* gVolumetricLighting = nullptr;
	inline static RE::Setting* gSunAlphaTransTime = nullptr;

	RE::TESObjectCELL* currentCell = nullptr;
	bool currentCellInterior = false;
	RE::TESWorldSpace* currentCellWorldspace = nullptr;
	float sunAngle = 90.0f;
	float currentSkyRotation = D3D11_FLOAT32_MAX;
	float lastGameHour = -1.0f;
	bool immediateTransitionReady = false;

	float4 colors[3] = {};
	RE::NiPoint3 rawDirections[3] = {};
	float currentDim = 1.0f;
	bool sunSetting = false;
	bool sunRising = false;
	bool sunBelowHorizon = false;
	bool celestialLightingValid = false;
	ShadowFader shadowFader;

	void DisableOnConflict(std::string_view conflictName);

	void PreparePendingTransitions();

	bool Update(const RE::Sky* sky);

	void SetSunAngle();

	void SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell);

	void ProcessSun(const RE::Sky* sky, RE::NiPoint3 dirs[], float intensities[]);
	struct SunFadeTimes
	{
		float sunriseBegin;
		float sunriseMiddle;
		float sunsetMiddle;
		float sunsetEnd;
	};
	static SunFadeTimes GetSunFadeTimes(const RE::Sky* sky);

	/** @brief Hides the sun disc and glare at and outside vanilla's fade boundaries. */
	static void HideSunOutsideFadeWindow(const RE::Sky* sky);

	void ProcessMoon(const RE::Sky* sky, Caster type, RE::NiPoint3 dirs[], float intensities[]);

	static void CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance);

	static void CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, float time, float sunrise, float sunset, float sunAngle);

	static void SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, float distance);

	// --- Open Shaders fork: altitude (horizon-dip) correction restored from the pre-#2408
	// SkySync. Upstream #2408 deleted this yet still lists "fixes the sun appearing higher
	// when you gain altitude" in its feature summary. Re-implemented as an additive helper
	// applied inside ProcessSun/ProcessMoon so upstream #2408 merges stay clean.
	static constexpr float RenderDistance = 325000.0f;
	static RE::NiPoint3 GetApparentDirection(const RE::NiPoint3& dir, float altitude);
	static float GetPlayerAltitude();
};
