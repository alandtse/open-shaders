#pragma once

#include "I18n/I18n.h"
#include "OverlayFeature.h"

struct SceneSelector : OverlayFeature
{
public:
	virtual inline std::string GetName() override { return "Weather Picker"; }
	virtual std::string GetDisplayName() override { return T("feature.weather_picker.name", "Scene Selector"); }
	virtual inline std::string GetShortName() override { return "WeatherPicker"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.weather_picker.description", "Browse, inspect, and switch weather in-game."),
			{ T("feature.weather_picker.key_feature_1", "Instantly switch between any weather with immediate or gradual transitions"),
				T("feature.weather_picker.key_feature_2", "Filter weather by type (Pleasant, Cloudy, Rainy, Snow, Aurora) for easy browsing"),
				T("feature.weather_picker.key_feature_3", "View detailed weather information including wind, precipitation, and lightning data"),
				T("feature.weather_picker.key_feature_4", "Color-coded weather names show all weather properties at a glance"),
				T("feature.weather_picker.key_feature_5", "Persistent overlay window for continuous weather monitoring while playing") } };
	}

	virtual void DrawSettings() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void Prepass() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual bool IsOverlayVisible() const override;

	/**
	 * Renders the standalone weather details window.
	 * @param open Pointer to the open/close state owned by the caller.
	 */
	void RenderWeatherDetailsWindow(bool* open);
	void ResetWindowLayout();

	/**
	 * Displays weather info for a given weather record.
	 * @param weather Weather record to display.
	 * @param weatherPct Optional blend percentage (0-1), or -1 to hide.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void DisplayWeatherInfo(RE::TESWeather* weather, float weatherPct = -1.0f, bool showInteractiveElements = true);

	/**
	 * Renders the core weather details UI section.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void RenderCoreWeatherDetails(bool showInteractiveElements = true);

	/** Renders weather analysis sections contributed by other features. */
	static void RenderFeatureWeatherAnalysis();

	/** Renders the weather controls section. */
	static void RenderWeatherControls(RE::Sky* sky);

	/** Renders the weather information display section. */
	static void RenderWeatherInformationDisplay(RE::Sky* sky, bool showInteractiveElements = true);

	struct WeatherDetailsWindowSettings
	{
		bool Enabled = false;
		bool ShowInOverlay = false;
		ImVec2 Position = ImVec2(50.f, 50.f);
		bool PositionSet = false;
	} WeatherDetailsWindow;

	static ImVec4 GetWeatherTypeColor(RE::TESWeather* weather);
	static bool RenderMultiColorWeatherName(RE::TESWeather* weather, const std::string& weatherName);
	static ImVec4 GetWeatherFlagColor(RE::TESWeather::WeatherDataFlag flag);
	static ImVec4 GetWeatherFlagColorByName(const std::string& flagName);

private:
	bool resetWindowSize = false;

	void DrawShowInOverlayToggle();
	void DrawTimeControls();
	void DrawSceneSelectorSection();
	static void DrawWeatherStatusPanel();

	static constexpr float WIND_DIRECTION_OFFSET = 30.5f;
	static constexpr uint32_t ALL_WEATHER_FLAGS = 0x7F;
	static constexpr uint32_t UNCLASSIFIED_FLAG = 0x40;

	static inline bool s_dataAvailable = false;
	static inline bool s_weathersLoaded = false;
	static inline std::vector<RE::TESWeather*> s_allWeathers;
	static inline std::vector<RE::TESWeather*> s_filteredWeathers;
	static inline int s_selectedWeatherIdx = -1;
	static inline uint32_t s_weatherFlagFilter = ALL_WEATHER_FLAGS;
	static inline uint32_t s_lastWeatherFlagFilter = UNCLASSIFIED_FLAG;
	static inline bool s_accelerateWeatherChange = true;
	static inline RE::TESWeather* s_cachedLastWeather = nullptr;

	static std::string GetDisplayName(const RE::TESWeather* weather);

	struct WeatherNameComparator
	{
		bool operator()(const RE::TESWeather* a, const RE::TESWeather* b) const
		{
			return SceneSelector::GetDisplayName(a) < SceneSelector::GetDisplayName(b);
		}
	};

	static void DisplayWeatherBasicInfo(RE::TESWeather* weather, float weatherPct);
	static void DisplayPrecipitationInfo(RE::TESWeather* weather);
	static void DisplayLightningInfo(RE::TESWeather* weather, bool showInteractiveElements);
	static void DisplayWindInfo(RE::TESWeather* weather);

	static void EnsureWeatherListLoaded();
	static void LoadAllWeathers();
	static void UpdateFilteredWeathers();
	static int FindWeatherIndex(RE::TESWeather* targetWeather);
	static std::vector<std::string> GetWeatherFlagNames(RE::TESWeather* weather);

	void DrawOverlay() override;
};
