#include "WetnessEffects.h"
#include "Menu.h"
#include "State.h"
#include "WeatherPicker.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace
{
	constexpr uint32_t kWetnessPsSrvPrecipOcclusionSlot = 70u;

	// Reference depth-model constants used for wetness persistence behavior.
	constexpr float RAIN_DELTA_PER_SECOND = 2.0f / 3600.0f;
	constexpr float SNOWY_DAY_DELTA_PER_SECOND = -0.489f / 3600.0f;
	constexpr float CLOUDY_DAY_DELTA_PER_SECOND = -0.735f / 3600.0f;
	constexpr float CLEAR_DAY_DELTA_PER_SECOND = -1.518f / 3600.0f;
	constexpr float WETNESS_SCALE = 2.0f;
	constexpr float PUDDLE_SCALE = 1.0f;
	constexpr float MAX_WETNESS_DEPTH = 2.0f;
	constexpr float MAX_PUDDLE_DEPTH = 3.0f;
	constexpr float MAX_OUTPUT_WETNESS = 1.0f;
	constexpr float MAX_OUTPUT_PUDDLE_WETNESS = 1.0f;
	constexpr double SECONDS_IN_A_DAY = 86400.0;
	// Accept large in-game waits/sleeps; clamp only extreme jumps (load anomalies, time rewinds).
	constexpr double MAX_TIME_DELTA_SECONDS = SECONDS_IN_A_DAY * 7.0;
	constexpr float MIN_TRANSITION_SPEED = 0.2f;
	constexpr float MAX_TRANSITION_SPEED = 8.0f;
	constexpr float MIN_RAINDROP_GRID_SIZE = 1e-3f;
	constexpr float MIN_RAINDROP_INTERVAL = 1e-3f;
	constexpr float MIN_RIPPLE_LIFETIME = 1e-3f;
	constexpr float MIN_RIPPLE_BREADTH = 1e-3f;
	constexpr float MIN_SPLASH_LIFETIME = 1e-3f;
	// World-unit conversions and wetness distance controls.
	constexpr float GAME_UNITS_PER_METER = 1.0f / Util::Units::GAME_UNIT_TO_M;
	// Radius/range sliders are user-facing and persisted in world units.
	constexpr float PUDDLE_RADIUS_UI_MIN_GAME_UNITS = 0.15f * GAME_UNITS_PER_METER;
	constexpr float PUDDLE_RADIUS_UI_MAX_GAME_UNITS = 50.0f * GAME_UNITS_PER_METER;
	constexpr float PUDDLE_RADIUS_CLAMP_MAX_GAME_UNITS = PUDDLE_RADIUS_UI_MAX_GAME_UNITS;
	constexpr float DEFAULT_PUDDLE_RADIUS_GAME_UNITS = 1.7f * GAME_UNITS_PER_METER;
	constexpr float DEFAULT_RAINDROP_FX_RANGE_GAME_UNITS = 2000.0f;
	constexpr float RAINDROP_FX_RANGE_UI_MIN_GAME_UNITS = 1.0f * GAME_UNITS_PER_METER;
	constexpr float RAINDROP_FX_RANGE_UI_MAX_GAME_UNITS = 75.0f * GAME_UNITS_PER_METER;
	constexpr float RAINDROP_FX_RANGE_CLAMP_MAX_GAME_UNITS = RAINDROP_FX_RANGE_UI_MAX_GAME_UNITS;
	constexpr float WETNESS_DISTANCE_FADE_RANGE_UI_MIN_GAME_UNITS = 100.0f * GAME_UNITS_PER_METER;
	constexpr float WETNESS_DISTANCE_FADE_RANGE_UI_MAX_GAME_UNITS = 25000.0f;
	constexpr float DEFAULT_WETNESS_DISTANCE_FADE_RANGE_GAME_UNITS = 10000.0f;
	constexpr float RAINDROP_VISIBILITY_BOOST_MIN = 0.0f;
	constexpr float RAINDROP_VISIBILITY_BOOST_MAX = 2.0f;
	constexpr float DEFAULT_RAINDROP_VISIBILITY_BOOST = 0.0f;
	constexpr float WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MIN = 0.0f;
	constexpr float WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MAX = 1.0f;
	constexpr float DEFAULT_WET_CUBEMAP_STABILITY_BIAS_STRENGTH = 0.0f;
	constexpr float WET_CUBEMAP_STABILITY_BIAS_RANGE_UI_MIN_GAME_UNITS = 1.0f * GAME_UNITS_PER_METER;
	constexpr float WET_CUBEMAP_STABILITY_BIAS_RANGE_UI_MAX_GAME_UNITS = 25000.0f;
	constexpr float DEFAULT_WET_CUBEMAP_STABILITY_BIAS_RANGE_GAME_UNITS = 5000.0f;
	constexpr float PUDDLE_LAYOUT_MIN = 0.3f;
	constexpr float PUDDLE_LAYOUT_MAX = 10.0f;
	constexpr float LEGACY_WET_REFLECTION_UI_SCALE_MAX = 1.00f;
	constexpr float MODERN_WET_REFLECTION_BASE_UI_MAX = 1.00f;
	constexpr float MAX_MODERN_WET_REFLECTION_UI_SCALE = 2.00f;
	constexpr float DEFAULT_WET_INDIRECT_SPECULAR_SCALE = 0.8f;
	constexpr float DEFAULT_LEGACY_WET_REFLECTION_UI = 0.40f;
	constexpr float DEFAULT_MODERN_WET_REFLECTION_UI = 0.80f;
	constexpr float DEFAULT_POST_RAIN_PUDDLE_SHINE = 2.5f;
	constexpr float POST_RAIN_PUDDLE_SHINE_MIN = 0.0f;
	constexpr float POST_RAIN_PUDDLE_SHINE_MAX = 5.0f;
	constexpr float RAIN_REFLECTION_BALANCE_MIN = 0.0f;
	constexpr float RAIN_REFLECTION_BALANCE_MAX = 1.0f;
	constexpr float DEFAULT_RAIN_REFLECTION_BALANCE = 0.2f;
	constexpr float POST_RAIN_WATER_CLARITY_MIN = 0.0f;
	constexpr float POST_RAIN_WATER_CLARITY_MAX = 1.0f;
	constexpr float DEFAULT_POST_RAIN_WATER_CLARITY = 0.8f;
	constexpr float MODERN_REFLECTION_UI_ANCHOR_T = 0.30f;
	constexpr float LEGACY_REFLECTION_UI_ANCHOR_T = 0.28f;
	constexpr float MODERN_REFLECTION_SCALE_AT_ANCHOR = 0.02f;
	constexpr float LEGACY_REFLECTION_SCALE_AT_ANCHOR = 0.001f;
	constexpr float WET_HIGHLIGHT_REDUCTION_MIN = 0.25f;
	constexpr float WET_HIGHLIGHT_REDUCTION_MAX = 10.0f;
	constexpr float WET_FILM_SPECULAR_FLOOR_SCALE_MIN = 0.0f;
	constexpr float WET_FILM_SPECULAR_FLOOR_SCALE_MAX = 3.0f;
	constexpr float SHORE_PERSISTENT_DARKENING_MIN = 0.0f;
	constexpr float SHORE_PERSISTENT_DARKENING_MAX = 2.0f;
	constexpr float SHORE_PERSISTENT_DARKENING_DEFAULT = 1.0f;
	constexpr float DRYING_HOURS_MIN = 1.0f;
	constexpr float DRYING_HOURS_MAX = 24.0f;
	constexpr float DRYING_SECONDS_PER_HOUR = 3600.0f;
	constexpr float POST_RAIN_RADIUS_SETTLE_HOURS = 1.0f;
	constexpr float POST_RAIN_RADIUS_SETTLE_SECONDS = POST_RAIN_RADIUS_SETTLE_HOURS * DRYING_SECONDS_PER_HOUR;
	constexpr float DEFAULT_STONE_DRYING_HOURS = 6.0f;
	constexpr float DEFAULT_GRASS_DRYING_HOURS = 3.0f;
	constexpr float DEFAULT_DIRT_DRYING_HOURS = 12.0f;
	constexpr float DEFAULT_PUDDLE_DRYING_HOURS = 18.0f;
	constexpr float DEFAULT_PUDDLE_LAYOUT = 3.0f;
	constexpr float WEATHER_DRIVEN_NON_PUDDLE_MIN_HOURS = 3.0f;
	constexpr float WEATHER_DRIVEN_NON_PUDDLE_MAX_HOURS = 18.0f;
	constexpr float WEATHER_DRIVEN_PUDDLE_MIN_HOURS = 12.0f;
	constexpr float WEATHER_DRIVEN_PUDDLE_MAX_HOURS = 24.0f;
	constexpr float SUNNY_DRYING_SPEED_MULT = 1.20f;
	constexpr float CLEAR_DRYING_SPEED_MULT = 1.00f;
	constexpr float OVERCAST_DRYING_SPEED_MULT = 0.80f;
	constexpr float SUMMER_DRYING_SPEED_MULT = 1.15f;
	constexpr float SPRING_AUTUMN_DRYING_SPEED_MULT = 1.00f;
	constexpr float WINTER_DRYING_SPEED_MULT = 0.80f;
	constexpr float SURFACE_DRYING_WEIGHT_STONE = 0.20f;
	constexpr float SURFACE_DRYING_WEIGHT_GRASS = 0.35f;
	constexpr float SURFACE_DRYING_WEIGHT_DIRT = 0.45f;
	constexpr float SNOW_PUDDLE_DRY_SLOW_MULT = 0.35f;
	// Persistence tuning:
	// - Long/strong rain events should keep puddles for much longer than ground wetness.
	// - With default transition speed, strong events can keep puddles around roughly half a day.
	constexpr float RAIN_EVENT_REFERENCE_SECONDS = 1800.0f;
	constexpr float RAIN_EVENT_DECAY_SECONDS = 43200.0f;
	constexpr float MIN_WETNESS_DRY_SCALE_AT_MAX_EVENT = 0.12f;
	constexpr float RUNTIME_DRY_EPSILON = 1e-4f;
	struct WetnessUiPresetDefinition
	{
		const char* name;
		const char* description;
		float raindropFxRange;
		float wetnessFadeRange;
	};

	constexpr std::array<WetnessUiPresetDefinition, 3> WETNESS_UI_PRESETS = { {
		{ "Performance",
			"Same wetness look, but shorter raindrop and wetness distance ranges for the largest performance savings.",
			1000.0f,
			5000.0f },
		{ "Balanced",
			"Same wetness look, with moderate distance-range reductions for balanced performance.",
			1500.0f,
			7500.0f },
		{ "Quality",
			"Current default settings. Same wetness look, with the farthest wetness and raindrop coverage.",
			2000.0f,
			10000.0f }
	} };

	// Cached per-frame data for debug/weather analysis UI.
	// Keep this outside WetnessEffects object layout to avoid class-level alignment padding warnings.
	WetnessEffects::PerFrame g_lastFrameData{};
	bool g_hasLastFrameData = false;
	WetnessEffects::PerFrame g_cachedCommonBufferData{};
	bool g_hasCachedCommonBufferData = false;
	uint32_t g_cachedCommonBufferFrame = 0;
	REX::W32::XMFLOAT4X4 g_lastValidOcclusionViewProj{};
	bool g_hasLastValidOcclusionViewProj = false;

	float GetWeatherRainIntensity(RE::TESWeather* weather)
	{
		if (!weather || !weather->precipitationData || !weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
			return 0.0f;
		}

		const float maxDensity = weather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
		if (maxDensity <= 0.0f) {
			return 0.0f;
		}

		return std::clamp(maxDensity / WetnessEffects::MAX_RAIN_PARTICLE_DENSITY, 0.0f, 1.0f);
	}

	float GetWeatherDepthDeltaPerSecond(RE::TESWeather* weather)
	{
		if (!weather) {
			return CLEAR_DAY_DELTA_PER_SECOND;
		}

		if (weather->precipitationData && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
			return RAIN_DELTA_PER_SECOND;
		}
		if (weather->precipitationData && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow)) {
			return SNOWY_DAY_DELTA_PER_SECOND;
		}
		if (weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kCloudy)) {
			return CLOUDY_DAY_DELTA_PER_SECOND;
		}
		return CLEAR_DAY_DELTA_PER_SECOND;
	}

	void GetDepthDeltaRates(RE::TESWeather* weather, float rainIntensity, float& wetnessDeltaPerSecond, float& puddleDeltaPerSecond)
	{
		float deltaPerSecond = GetWeatherDepthDeltaPerSecond(weather);
		const bool snowing = weather && weather->precipitationData && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
		if (deltaPerSecond > 0.0f) {
			deltaPerSecond *= std::clamp(rainIntensity, 0.0f, 1.0f);
		}

		wetnessDeltaPerSecond = deltaPerSecond * WETNESS_SCALE;
		puddleDeltaPerSecond = deltaPerSecond * PUDDLE_SCALE;
		if (snowing && deltaPerSecond < 0.0f) {
			// During snowfall after rain, puddles should persist longer before they fully dry out.
			puddleDeltaPerSecond *= SNOW_PUDDLE_DRY_SLOW_MULT;
		}
	}

	bool IsRainyWeather(const RE::TESWeather* weather)
	{
		return weather && weather->precipitationData && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy);
	}

	float ClampDryingHours(float hours, float fallback = DRYING_HOURS_MAX)
	{
		if (!std::isfinite(hours))
			return fallback;
		return std::clamp(hours, DRYING_HOURS_MIN, DRYING_HOURS_MAX);
	}

	float DryingHoursToSeconds(float hours)
	{
		return ClampDryingHours(hours) * DRYING_SECONDS_PER_HOUR;
	}

	struct EffectiveDryingHours
	{
		float stoneHours = DRYING_HOURS_MAX;
		float grassHours = DRYING_HOURS_MAX;
		float dirtHours = DRYING_HOURS_MAX;
		float puddleHours = DRYING_HOURS_MAX;
	};

	EffectiveDryingHours GetManualDryingHours(const WetnessEffects::Settings& settings, float puddleDryingHours)
	{
		EffectiveDryingHours result{};
		result.stoneHours = ClampDryingHours(settings.StoneDryingMultiplier, DEFAULT_STONE_DRYING_HOURS);
		result.grassHours = ClampDryingHours(settings.GrassDryingMultiplier, DEFAULT_GRASS_DRYING_HOURS);
		result.dirtHours = ClampDryingHours(settings.DirtDryingMultiplier, DEFAULT_DIRT_DRYING_HOURS);
		result.puddleHours = ClampDryingHours(puddleDryingHours, DEFAULT_PUDDLE_DRYING_HOURS);
		return result;
	}

	float GetSeasonDryingSpeedMultiplier()
	{
		if (const auto* calendar = RE::Calendar::GetSingleton()) {
			const uint32_t month = calendar->GetMonth() % 12u;
			switch (month) {
			// Midyear, Sun's Height, Last Seed
			case 5u:
			case 6u:
			case 7u:
				return SUMMER_DRYING_SPEED_MULT;
			// Morning Star, Sun's Dawn, Evening Star
			case 0u:
			case 1u:
			case 11u:
				return WINTER_DRYING_SPEED_MULT;
			// Spring and Autumn
			default:
				return SPRING_AUTUMN_DRYING_SPEED_MULT;
			}
		}
		return CLEAR_DRYING_SPEED_MULT;
	}

	float GetWeatherDryingSpeedMultiplier(const RE::TESWeather* weather)
	{
		if (!weather) {
			return CLEAR_DRYING_SPEED_MULT;
		}

		const auto flags = weather->data.flags;
		if (flags.any(RE::TESWeather::WeatherDataFlag::kRainy) ||
		    flags.any(RE::TESWeather::WeatherDataFlag::kSnow) ||
		    flags.any(RE::TESWeather::WeatherDataFlag::kCloudy)) {
			return OVERCAST_DRYING_SPEED_MULT;
		}
		if (flags.any(RE::TESWeather::WeatherDataFlag::kPleasant)) {
			return SUNNY_DRYING_SPEED_MULT;
		}
		return CLEAR_DRYING_SPEED_MULT;
	}

	EffectiveDryingHours GetWeatherDrivenDryingHours(
		const RE::TESWeather* currentWeather,
		const RE::TESWeather* lastWeather,
		float currentWeight,
		float lastWeight,
		float rainEventWeight)
	{
		EffectiveDryingHours result{};
		const float safeCurrentWeight = std::clamp(currentWeight, 0.0f, 1.0f);
		const float safeLastWeight = std::clamp(lastWeight, 0.0f, 1.0f);
		const float totalWeight = std::max(safeCurrentWeight + safeLastWeight, 1e-3f);
		const float weatherSpeed =
			(GetWeatherDryingSpeedMultiplier(currentWeather) * safeCurrentWeight +
				GetWeatherDryingSpeedMultiplier(lastWeather) * safeLastWeight) /
			totalWeight;
		const float seasonSpeed = GetSeasonDryingSpeedMultiplier();
		const float combinedSpeed = std::clamp(weatherSpeed * seasonSpeed, 0.55f, 1.35f);

		const float rainWeight = std::clamp(rainEventWeight, 0.0f, 1.0f);
		const float baseNonPuddleHours = std::lerp(
			WEATHER_DRIVEN_NON_PUDDLE_MIN_HOURS,
			WEATHER_DRIVEN_NON_PUDDLE_MAX_HOURS,
			rainWeight);

		result.stoneHours = std::clamp(
			(baseNonPuddleHours - 3.0f) / combinedSpeed,
			WEATHER_DRIVEN_NON_PUDDLE_MIN_HOURS,
			WEATHER_DRIVEN_NON_PUDDLE_MAX_HOURS);
		result.grassHours = std::clamp(
			baseNonPuddleHours / combinedSpeed,
			WEATHER_DRIVEN_NON_PUDDLE_MIN_HOURS,
			WEATHER_DRIVEN_NON_PUDDLE_MAX_HOURS);
		result.dirtHours = std::clamp(
			(baseNonPuddleHours + 3.0f) / combinedSpeed,
			WEATHER_DRIVEN_NON_PUDDLE_MIN_HOURS,
			WEATHER_DRIVEN_NON_PUDDLE_MAX_HOURS);

		// Enforce drying order: stone/wood first, grass next, dirt last.
		result.grassHours = std::max(result.grassHours, result.stoneHours);
		result.dirtHours = std::max(result.dirtHours, result.grassHours);

		const float basePuddleHours = std::lerp(
			WEATHER_DRIVEN_PUDDLE_MIN_HOURS,
			WEATHER_DRIVEN_PUDDLE_MAX_HOURS,
			rainWeight);
		result.puddleHours = std::clamp(
			basePuddleHours / combinedSpeed,
			WEATHER_DRIVEN_PUDDLE_MIN_HOURS,
			WEATHER_DRIVEN_PUDDLE_MAX_HOURS);

		return result;
	}

	float GetWeightedSurfaceDryingHours(const EffectiveDryingHours& hours)
	{
		const float weightedHours =
			hours.stoneHours * SURFACE_DRYING_WEIGHT_STONE +
			hours.grassHours * SURFACE_DRYING_WEIGHT_GRASS +
			hours.dirtHours * SURFACE_DRYING_WEIGHT_DIRT;
		return ClampDryingHours(weightedHours, DRYING_HOURS_MAX);
	}

	bool JsonValueToBool(const json& value, bool fallback)
	{
		if (value.is_boolean()) {
			return value.get<bool>();
		}
		if (value.is_number()) {
			return value.get<double>() != 0.0;
		}
		return fallback;
	}

	template <class T>
	T JsonValueOr(const json& root, const char* key, T fallback)
	{
		if (!root.is_object()) {
			return fallback;
		}
		const auto it = root.find(key);
		if (it == root.end()) {
			return fallback;
		}
		try {
			return it->get<T>();
		} catch (...) {
			return fallback;
		}
	}

	bool JsonHasAnyNonDebugKey(const json& root)
	{
		if (!root.is_object()) {
			return false;
		}
		static constexpr std::array<std::string_view, 3> ignoredKeys = {
			"DebugSettings",
			"PreventPuddlesOnGrass",
			"EnableMaterialWetShineScaling"
		};
		for (const auto& [key, _] : root.items()) {
			const bool ignored = std::find(ignoredKeys.begin(), ignoredKeys.end(), key) != ignoredKeys.end();
			if (!ignored) {
				return true;
			}
		}
		return false;
	}

	WetnessEffects::ShaderSettings MakeShaderSettings(const WetnessEffects::Settings& settings)
	{
		WetnessEffects::ShaderSettings shaderSettings{};
		shaderSettings.EnableWetnessEffects = settings.EnableWetnessEffects;
		shaderSettings.MaxRainWetness = settings.MaxRainWetness;
		shaderSettings.MaxPuddleWetness = settings.MaxPuddleWetness;
		shaderSettings.MaxShoreWetness = settings.MaxShoreWetness;
		shaderSettings.ShoreRange = settings.ShoreRange;
		shaderSettings.PuddleRadius = settings.PuddleRadiusWorldUnits;
		shaderSettings.PuddleMaxAngle = settings.PuddleMaxAngle;
		shaderSettings.PuddleMinWetness = settings.PuddleMinWetness;
		shaderSettings.MinRainWetness = settings.MinRainWetness;
		shaderSettings.SkinWetness = settings.SkinWetness;
		shaderSettings.PuddleLayout = settings.WeatherTransitionSpeed;
		shaderSettings.StoneDryingMultiplier = settings.StoneDryingMultiplier;
		shaderSettings.DirtDryingMultiplier = settings.DirtDryingMultiplier;
		shaderSettings.GrassDryingMultiplier = settings.GrassDryingMultiplier;
		shaderSettings.EnableRaindropFx = settings.EnableRaindropFx;
		shaderSettings.EnableSplashes = settings.EnableSplashes;
		shaderSettings.EnableRipples = settings.EnableRipples;
		shaderSettings.EnableModernWetReflection = settings.EnableModernWetReflection;
		shaderSettings.EnableLegacyWetReflection = settings.EnableLegacyWetReflection;
		shaderSettings.WetIndirectSpecularScale = settings.WetIndirectSpecularScale;
		shaderSettings.RaindropFxRange = settings.RaindropFxRangeWorldUnits;
		shaderSettings.RaindropGridSize = settings.RaindropGridSize;
		shaderSettings.RaindropInterval = settings.RaindropInterval;
		shaderSettings.RaindropChance = settings.RaindropChance;
		shaderSettings.SplashesLifetime = settings.SplashesLifetime;
		shaderSettings.SplashesStrength = settings.SplashesStrength;
		shaderSettings.SplashesMinRadius = settings.SplashesMinRadius;
		shaderSettings.SplashesMaxRadius = settings.SplashesMaxRadius;
		shaderSettings.RippleStrength = settings.RippleStrength;
		shaderSettings.RippleRadius = settings.RippleRadius;
		shaderSettings.RippleBreadth = settings.RippleBreadth;
		shaderSettings.RippleLifetime = settings.RippleLifetime;
		shaderSettings.PostRainPuddleWaterStrength = settings.PostRainPuddleWaterStrength;
		shaderSettings.RaindropTransitionFalloff = settings.RaindropTransitionFalloff;
		shaderSettings.WetDarkeningStrength = settings.WetDarkeningStrength;
		shaderSettings.WetHighlightReduction = settings.WetHighlightReduction;
		shaderSettings.EnableForwardReflectionBias = settings.EnableForwardReflectionBias;
		shaderSettings.EnableVanillaReflectionCompensation = settings.EnableVanillaReflectionCompensation;
		shaderSettings.WetFilmSpecularFloorScale = settings.WetFilmSpecularFloorScale;
		shaderSettings.ShorePersistentDarkeningStrength = 0.0f;
		return shaderSettings;
	}

	void ApplyPostRainDepthEnvelope(float& depth, float startDepth, float maxDepth, float elapsedSeconds, float durationSeconds)
	{
		if (durationSeconds <= 0.0f) {
			return;
		}

		const float remaining = std::clamp(1.0f - (elapsedSeconds / durationSeconds), 0.0f, 1.0f);
		const float startCap = std::clamp(startDepth, 0.0f, maxDepth) * remaining;
		const float globalCap = maxDepth * remaining;
		const float envelopeCap = std::min(startCap, globalCap);
		// Envelope is a cap only: never increase depth during post-rain decay.
		depth = std::clamp(depth, 0.0f, envelopeCap);
	}

	void ApplyDepthDelta(float deltaSeconds, float wetnessDeltaPerSecond, float puddleDeltaPerSecond, float& wetnessDepth, float& puddleDepth)
	{
		wetnessDepth = std::clamp(wetnessDepth + wetnessDeltaPerSecond * deltaSeconds, 0.0f, MAX_WETNESS_DEPTH);
		puddleDepth = std::clamp(puddleDepth + puddleDeltaPerSecond * deltaSeconds, 0.0f, MAX_PUDDLE_DEPTH);
	}

	float SanitizeUnitScale(float value, float fallback = DEFAULT_WET_INDIRECT_SPECULAR_SCALE)
	{
		if (value >= 0.0f && value <= MAX_MODERN_WET_REFLECTION_UI_SCALE) {
			return value;
		}
		if (value < 0.0f) {
			return 0.0f;
		}
		if (value > MAX_MODERN_WET_REFLECTION_UI_SCALE) {
			return MAX_MODERN_WET_REFLECTION_UI_SCALE;
		}
		return fallback;
	}

	float GetLegacyReflectionUiMax()
	{
		return LEGACY_WET_REFLECTION_UI_SCALE_MAX;
	}

	float ReflectionScaleFromUi(
		float uiValue,
		float maxScale,
		float anchorScale,
		float anchorT)
	{
		const float clampedUi = std::clamp(uiValue, 0.0f, 1.0f);
		if (clampedUi <= 0.0f) {
			return 0.0f;
		}

		const float safeMaxScale = std::max(maxScale, 1e-6f);
		const float safeAnchorScale = std::clamp(anchorScale, 1e-6f, safeMaxScale * 0.999999f);
		const float safeAnchorT = std::clamp(anchorT, 1e-3f, 0.999f);
		const float exponent = std::log(safeAnchorScale / safeMaxScale) / std::log(safeAnchorT);
		return safeMaxScale * std::pow(clampedUi, exponent);
	}

	float ModernWetReflectionScaleFromUi(float uiValue)
	{
		const float clampedUi = std::clamp(uiValue, 0.0f, MAX_MODERN_WET_REFLECTION_UI_SCALE);
		if (clampedUi <= MODERN_WET_REFLECTION_BASE_UI_MAX) {
			// Preserve the established response across the base [0..1] slider range.
			return ReflectionScaleFromUi(clampedUi, MODERN_WET_REFLECTION_BASE_UI_MAX, MODERN_REFLECTION_SCALE_AT_ANCHOR, MODERN_REFLECTION_UI_ANCHOR_T);
		}

		// Extend only the upper range [1..2] linearly so the base response remains unchanged.
		const float extendedT = (clampedUi - MODERN_WET_REFLECTION_BASE_UI_MAX) /
			(MAX_MODERN_WET_REFLECTION_UI_SCALE - MODERN_WET_REFLECTION_BASE_UI_MAX);
		return std::lerp(MODERN_WET_REFLECTION_BASE_UI_MAX, MAX_MODERN_WET_REFLECTION_UI_SCALE, extendedT);
	}

	float LegacyWetReflectionScaleFromUi(float uiValue, float legacyScaleMax)
	{
		return ReflectionScaleFromUi(uiValue, legacyScaleMax, LEGACY_REFLECTION_SCALE_AT_ANCHOR, LEGACY_REFLECTION_UI_ANCHOR_T);
	}

	uint SanitizeToggle(uint value)
	{
		return value != 0 ? 1u : 0u;
	}

	void SanitizeReflectionSettings(WetnessEffects::Settings& settings)
	{
		settings.EnableModernWetReflection = SanitizeToggle(settings.EnableModernWetReflection);
		settings.EnableLegacyWetReflection = SanitizeToggle(settings.EnableLegacyWetReflection);
		if (settings.EnableModernWetReflection && settings.EnableLegacyWetReflection) {
			// Keep modes mutually exclusive; modern wins on invalid combined input.
			settings.EnableLegacyWetReflection = 0u;
		}
		settings.WetIndirectSpecularScale = SanitizeUnitScale(settings.WetIndirectSpecularScale, DEFAULT_WET_INDIRECT_SPECULAR_SCALE);
		if (settings.EnableLegacyWetReflection && !settings.EnableModernWetReflection) {
			settings.WetIndirectSpecularScale = std::min(settings.WetIndirectSpecularScale, GetLegacyReflectionUiMax());
		}
		if (!settings.EnableModernWetReflection && !settings.EnableLegacyWetReflection) {
			settings.WetIndirectSpecularScale = 0.0f;
		}
	}

	float SanitizeReflectionScale(float value, float maxValue, float fallback)
	{
		if (!std::isfinite(value)) {
			return fallback;
		}
		return std::clamp(value, 0.0f, maxValue);
	}

	template <class TSettings>
	void SyncActiveReflectionScale(
		TSettings& settings,
		float modernScale,
		float legacyScale)
	{
		if (settings.EnableLegacyWetReflection && !settings.EnableModernWetReflection) {
			settings.WetIndirectSpecularScale = legacyScale;
			return;
		}
		if (settings.EnableModernWetReflection) {
			settings.WetIndirectSpecularScale = modernScale;
			return;
		}
		settings.WetIndirectSpecularScale = 0.0f;
	}

	void SanitizePersistentReflectionSettings(
		WetnessEffects::Settings& settings,
		float& modernScale,
		float& legacyScale)
	{
		SanitizeReflectionSettings(settings);
		const float legacyScaleMax = GetLegacyReflectionUiMax();
		modernScale = SanitizeReflectionScale(modernScale, MAX_MODERN_WET_REFLECTION_UI_SCALE, DEFAULT_MODERN_WET_REFLECTION_UI);
		legacyScale = SanitizeReflectionScale(legacyScale, legacyScaleMax, DEFAULT_LEGACY_WET_REFLECTION_UI);
		const float modernMappedScale = ModernWetReflectionScaleFromUi(modernScale);
		const float legacyMappedScale = LegacyWetReflectionScaleFromUi(legacyScale, legacyScaleMax);
		SyncActiveReflectionScale(settings, modernMappedScale, legacyMappedScale);
	}

	float ClampFiniteOrDefault(float value, float minValue, float maxValue, float fallback);

	float ClampWetnessDistanceFadeRange(float gameUnits)
	{
		return (std::isfinite(gameUnits)) ?
			std::clamp(gameUnits, WETNESS_DISTANCE_FADE_RANGE_UI_MIN_GAME_UNITS, WETNESS_DISTANCE_FADE_RANGE_UI_MAX_GAME_UNITS) :
			DEFAULT_WETNESS_DISTANCE_FADE_RANGE_GAME_UNITS;
	}

	float ClampWetCubemapStabilityBiasRange(float gameUnits)
	{
		return (std::isfinite(gameUnits)) ?
			std::clamp(gameUnits, WET_CUBEMAP_STABILITY_BIAS_RANGE_UI_MIN_GAME_UNITS, WET_CUBEMAP_STABILITY_BIAS_RANGE_UI_MAX_GAME_UNITS) :
			DEFAULT_WET_CUBEMAP_STABILITY_BIAS_RANGE_GAME_UNITS;
	}

	uint32_t EncodeFloatToUint(float value)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value), "Float/uint32 size mismatch.");
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint32_t PackThreeUnorm10(float value0, float value1, float value2)
	{
		const auto toUnorm10 = [](float value) -> uint32_t {
			const float clamped = std::clamp(value, 0.0f, 1.0f);
			return static_cast<uint32_t>(std::lround(clamped * 1023.0f));
		};
		const uint32_t bits0 = toUnorm10(value0) & 0x3FFu;
		const uint32_t bits1 = toUnorm10(value1) & 0x3FFu;
		const uint32_t bits2 = toUnorm10(value2) & 0x3FFu;
		return bits0 | (bits1 << 10) | (bits2 << 20);
	}

	float ClampRaindropVisibilityBoost(float value)
	{
		return std::isfinite(value) ?
			std::clamp(value, RAINDROP_VISIBILITY_BOOST_MIN, RAINDROP_VISIBILITY_BOOST_MAX) :
			DEFAULT_RAINDROP_VISIBILITY_BOOST;
	}

	void SanitizePersistentUiState(
		WetnessEffects::Settings& settings,
		float& modernScale,
		float& legacyScale,
		float& puddleHours,
		float& layout,
		float& rainReflectionBalance,
		float& postRainWaterClarity,
		float& shoreDarkeningStrength,
		float& wetnessDistanceFadeRange,
		float& raindropVisibilityBoost)
	{
		SanitizePersistentReflectionSettings(settings, modernScale, legacyScale);
		puddleHours = ClampDryingHours(puddleHours, DEFAULT_PUDDLE_DRYING_HOURS);
		layout = std::clamp(
			std::isfinite(layout) ? layout : DEFAULT_PUDDLE_LAYOUT,
			PUDDLE_LAYOUT_MIN,
			PUDDLE_LAYOUT_MAX);
		rainReflectionBalance = ClampFiniteOrDefault(
			rainReflectionBalance,
			RAIN_REFLECTION_BALANCE_MIN,
			RAIN_REFLECTION_BALANCE_MAX,
			DEFAULT_RAIN_REFLECTION_BALANCE);
		postRainWaterClarity = ClampFiniteOrDefault(
			postRainWaterClarity,
			POST_RAIN_WATER_CLARITY_MIN,
			POST_RAIN_WATER_CLARITY_MAX,
			DEFAULT_POST_RAIN_WATER_CLARITY);
		shoreDarkeningStrength = ClampFiniteOrDefault(
			shoreDarkeningStrength,
			SHORE_PERSISTENT_DARKENING_MIN,
			SHORE_PERSISTENT_DARKENING_MAX,
			SHORE_PERSISTENT_DARKENING_DEFAULT);
		wetnessDistanceFadeRange = ClampWetnessDistanceFadeRange(wetnessDistanceFadeRange);
		raindropVisibilityBoost = ClampRaindropVisibilityBoost(raindropVisibilityBoost);
	}

	void ApplyWetnessUiPreset(
		WetnessEffects::Settings& settings,
		float& wetnessFadeRange,
		const WetnessUiPresetDefinition& preset)
	{
		settings.RaindropFxRangeWorldUnits = preset.raindropFxRange;
		wetnessFadeRange = preset.wetnessFadeRange;
	}

	constexpr size_t DEFAULT_WETNESS_UI_PRESET_INDEX = 2;  // Quality

	void ApplyDefaultWetnessUiPreset(
		WetnessEffects::Settings& settings,
		float& wetnessFadeRange)
	{
		static_assert(DEFAULT_WETNESS_UI_PRESET_INDEX < WETNESS_UI_PRESETS.size(), "Default wetness preset index out of range.");
		ApplyWetnessUiPreset(settings, wetnessFadeRange, WETNESS_UI_PRESETS[DEFAULT_WETNESS_UI_PRESET_INDEX]);
	}

	void SanitizeToggleSettings(WetnessEffects::Settings& settings)
	{
		settings.EnableWetnessEffects = SanitizeToggle(settings.EnableWetnessEffects);
		settings.EnableRaindropFx = SanitizeToggle(settings.EnableRaindropFx);
		settings.EnableSplashes = SanitizeToggle(settings.EnableSplashes);
		settings.EnableRipples = SanitizeToggle(settings.EnableRipples);
		settings.EnableForwardReflectionBias = SanitizeToggle(settings.EnableForwardReflectionBias);
		settings.EnableVanillaReflectionCompensation = SanitizeToggle(settings.EnableVanillaReflectionCompensation);
		SanitizeReflectionSettings(settings);
	}

	float ClampFiniteOrDefault(float value, float minValue, float maxValue, float fallback)
	{
		if (!std::isfinite(value)) {
			return fallback;
		}
		return std::clamp(value, minValue, maxValue);
	}

	void SanitizeShaderFacingSettings(WetnessEffects::Settings& settings)
	{
		settings.WeatherTransitionSpeed = ClampFiniteOrDefault(settings.WeatherTransitionSpeed, MIN_TRANSITION_SPEED, MAX_TRANSITION_SPEED, 3.0f);
		settings.ShoreRange = std::max(settings.ShoreRange, 1u);
		settings.PuddleRadiusWorldUnits = ClampFiniteOrDefault(settings.PuddleRadiusWorldUnits, PUDDLE_RADIUS_UI_MIN_GAME_UNITS, PUDDLE_RADIUS_CLAMP_MAX_GAME_UNITS, DEFAULT_PUDDLE_RADIUS_GAME_UNITS);
		settings.PuddleMaxAngle = ClampFiniteOrDefault(settings.PuddleMaxAngle, 0.0f, 1.0f, 0.75f);
		settings.PuddleMinWetness = ClampFiniteOrDefault(settings.PuddleMinWetness, 0.0f, 1.0f, 0.525f);
		settings.MinRainWetness = ClampFiniteOrDefault(settings.MinRainWetness, 0.0f, 1.0f, 0.60f);
		settings.SkinWetness = ClampFiniteOrDefault(settings.SkinWetness, 0.0f, 1.0f, 0.95f);

		settings.StoneDryingMultiplier = ClampFiniteOrDefault(settings.StoneDryingMultiplier, DRYING_HOURS_MIN, DRYING_HOURS_MAX, DEFAULT_STONE_DRYING_HOURS);
		settings.DirtDryingMultiplier = ClampFiniteOrDefault(settings.DirtDryingMultiplier, DRYING_HOURS_MIN, DRYING_HOURS_MAX, DEFAULT_DIRT_DRYING_HOURS);
		settings.GrassDryingMultiplier = ClampFiniteOrDefault(settings.GrassDryingMultiplier, DRYING_HOURS_MIN, DRYING_HOURS_MAX, DEFAULT_GRASS_DRYING_HOURS);

		settings.RaindropFxRangeWorldUnits = ClampFiniteOrDefault(
			settings.RaindropFxRangeWorldUnits,
			RAINDROP_FX_RANGE_UI_MIN_GAME_UNITS,
			RAINDROP_FX_RANGE_CLAMP_MAX_GAME_UNITS,
			DEFAULT_RAINDROP_FX_RANGE_GAME_UNITS);
		settings.RaindropGridSize = ClampFiniteOrDefault(settings.RaindropGridSize, MIN_RAINDROP_GRID_SIZE, 100.0f, 3.0f);
		settings.RaindropInterval = ClampFiniteOrDefault(settings.RaindropInterval, MIN_RAINDROP_INTERVAL, 60.0f, 0.5f);
		settings.RaindropChance = ClampFiniteOrDefault(settings.RaindropChance, 0.0f, 1.0f, 0.8f);
		settings.SplashesLifetime = ClampFiniteOrDefault(settings.SplashesLifetime, MIN_SPLASH_LIFETIME, 120.0f, 6.0f);
		settings.RippleLifetime = ClampFiniteOrDefault(settings.RippleLifetime, MIN_RIPPLE_LIFETIME, 60.0f, 0.30f);
		settings.RippleBreadth = ClampFiniteOrDefault(settings.RippleBreadth, MIN_RIPPLE_BREADTH, 10.0f, 0.40f);
		settings.PostRainPuddleWaterStrength = ClampFiniteOrDefault(
			settings.PostRainPuddleWaterStrength,
			POST_RAIN_PUDDLE_SHINE_MIN,
			POST_RAIN_PUDDLE_SHINE_MAX,
			DEFAULT_POST_RAIN_PUDDLE_SHINE);
		settings.RaindropTransitionFalloff = ClampFiniteOrDefault(settings.RaindropTransitionFalloff, 0.5f, 6.0f, 2.0f);
		settings.WetDarkeningStrength = ClampFiniteOrDefault(settings.WetDarkeningStrength, 0.0f, 2.0f, 0.85f);
		settings.WetHighlightReduction = ClampFiniteOrDefault(settings.WetHighlightReduction, WET_HIGHLIGHT_REDUCTION_MIN, WET_HIGHLIGHT_REDUCTION_MAX, 5.0f);
		settings.WetFilmSpecularFloorScale = ClampFiniteOrDefault(settings.WetFilmSpecularFloorScale, WET_FILM_SPECULAR_FLOOR_SCALE_MIN, WET_FILM_SPECULAR_FLOOR_SCALE_MAX, 1.0f);
		settings.WetCubemapStabilityBiasStrength = ClampFiniteOrDefault(
			settings.WetCubemapStabilityBiasStrength,
			WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MIN,
			WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MAX,
			DEFAULT_WET_CUBEMAP_STABILITY_BIAS_STRENGTH);
		settings.WetCubemapStabilityBiasRangeWorldUnits = ClampWetCubemapStabilityBiasRange(settings.WetCubemapStabilityBiasRangeWorldUnits);
	}

	RE::BSParticleShaderRainEmitter* GetRainEmitterFromPrecipGeometry(RE::BSGeometry* precipObject)
	{
		if (!precipObject) {
			return nullptr;
		}

		auto* shaderProp = precipObject->GetGeometryRuntimeData().shaderProperty.get();
		auto particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(shaderProp);
		if (!particleShaderProperty || !particleShaderProperty->particleEmitter) {
			return nullptr;
		}

		auto* emitter = particleShaderProperty->particleEmitter;
		if (!emitter->emitterType.any(RE::BSParticleShaderEmitter::EMITTER_TYPE::kRain)) {
			return nullptr;
		}

		return static_cast<RE::BSParticleShaderRainEmitter*>(emitter);
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WetnessEffects::Settings,
	EnableWetnessEffects,
	MaxRainWetness,
	MaxPuddleWetness,
	MaxShoreWetness,
	ShoreRange,
	PuddleRadiusWorldUnits,
	PuddleMaxAngle,
	PuddleMinWetness,
	MinRainWetness,
	SkinWetness,
	WeatherTransitionSpeed,
	StoneDryingMultiplier,
	DirtDryingMultiplier,
	GrassDryingMultiplier,
	EnableRaindropFx,
	EnableSplashes,
	EnableRipples,
	EnableModernWetReflection,
	EnableLegacyWetReflection,
	RaindropFxRangeWorldUnits,
	RaindropGridSize,
	RaindropInterval,
	RaindropChance,
	SplashesLifetime,
	SplashesStrength,
	SplashesMinRadius,
	SplashesMaxRadius,
	RippleStrength,
	RippleRadius,
	RippleBreadth,
	RippleLifetime,
	PostRainPuddleWaterStrength,
	RaindropTransitionFalloff,
	WetDarkeningStrength,
	WetHighlightReduction,
	EnableForwardReflectionBias,
	EnableVanillaReflectionCompensation,
	WetFilmSpecularFloorScale,
	WetCubemapStabilityBiasStrength,
	WetCubemapStabilityBiasRangeWorldUnits)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WetnessEffects::DebugSettings,
	EnableWetnessOverride,
	EnablePuddleOverride,
	EnableRainOverride,
	EnableIntExOverride,
	WetnessOverride,
	PuddleWetnessOverride,
	RainOverride)

// Climate preset data - defines regional weather characteristics
// Precipitation rates calculated from actual shader mechanics: grid size, interval, and raindrop chance

struct ClimatePresetInfo
{
	const char* name;
	const char* shortDescription;
	const char* const* detailedDescription;
	const char* const* effectDescription;
	WetnessEffects::ClimateSettings settings;
};

// Climate preset detailed descriptions
static constexpr const char* LEGACY_DETAILED[] = {
	"Riverwood's original lighter rain profile.",
	"Max precipitation: ~0.66 mm/hr (very light)",
	"Multipliers: Wetness 1.0x, Puddle 1.0x, Transition 1.0x.",
	"Raindrop: 30% chance, grid 4.0 units, interval 0.5s.",
	"Performance impact: Minimal (baseline)",
	nullptr
};
static constexpr const char* LEGACY_EFFECTS[] = {
	"Original wetness accumulation (1.0x)",
	"Original puddle formation (1.0x)",
	"Original weather transitions (1.0x)",
	"Original raindrop frequency (1.0x)",
	nullptr
};

static constexpr const char* ARCTIC_DETAILED[] = {
	"Cold, dry climate with minimal precipitation.",
	"Max precipitation: ~1.08 mm/hr (light)",
	"Multipliers: Wetness 0.5x, Puddle 0.3x, Transition 0.5x.",
	"Raindrop: 30% chance, grid 3.5 units, interval 0.4s.",
	"Performance impact: Minimal",
	nullptr
};
static constexpr const char* ARCTIC_EFFECTS[] = {
	"Slow wetness accumulation (0.5x)",
	"Minimal puddle formation (0.3x)",
	"Slow weather transitions (0.5x)",
	"Sparse precipitation (30% chance)",
	"",
	nullptr
};

static constexpr const char* NORDIC_DETAILED[] = {
	"Balanced temperate Nordic climate.",
	"Max precipitation: ~3.35 mm/hr (moderate)",
	"Multipliers: Wetness 1.0x, Puddle 1.0x, Transition 1.0x.",
	"Raindrop: 80% chance, grid 3.0 units, interval 0.5s.",
	"Performance impact: Low",
	nullptr
};
static constexpr const char* NORDIC_EFFECTS[] = {
	"Standard wetness accumulation (1.0x)",
	"Standard puddle formation (1.0x)",
	"Standard weather transitions (1.0x)",
	"Moderate raindrop frequency (80% chance)",
	nullptr
};

static constexpr const char* COASTAL_DETAILED[] = {
	"Maritime climate with frequent, heavy precipitation.",
	"Max precipitation: ~8.06 mm/hr (heavy)",
	"Multipliers: Wetness 1.5x, Puddle 1.7x, Transition 1.7x.",
	"Raindrop: 80% chance, grid 2.5 units, interval 0.25s.",
	"Performance impact: Moderate",
	nullptr
};
static constexpr const char* COASTAL_EFFECTS[] = {
	"Fast wetness accumulation (1.5x)",
	"Enhanced puddle formation (1.7x)",
	"Rapid weather transitions (1.7x)",
	"Frequent rain events (80% chance)",
	nullptr
};

static constexpr const char* MONSOON_DETAILED[] = {
	"Tropical/monsoon climate with extreme precipitation.",
	"Max precipitation: ~22 mm/hr (extreme)",
	"Multipliers: Wetness 2.0x, Puddle 2.5x, Transition 2.0x.",
	"Raindrop: 100% chance, grid 2.0 units, interval 0.2s.",
	"Skryim light rain will not match wetness.",
	"Performance impact: High (may impact GPU)",
	nullptr
};
static constexpr const char* MONSOON_EFFECTS[] = {
	"Rapid wetness accumulation (2.0x)",
	"Maximum puddle formation (2.5x)",
	"Very dynamic weather (2.0x)",
	"Maximum raindrop frequency (100% chance)",
	nullptr
};

static constexpr std::array<ClimatePresetInfo, 6> CLIMATE_PRESET_INFO = {
	{ { "Custom",
		  "User-defined custom settings",
		  nullptr,
		  nullptr,
		  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },
		// Legacy (Original Skyrim)
		{
			"Legacy",
			"Original rain effect values (very light)",
			LEGACY_DETAILED,
			LEGACY_EFFECTS,
			{ 1.0f, 1.0f, 1.0f, 0.3f, 4.0f, 0.5f } },
		// Nordic Standard
		{
			"Nordic (Default)",
			"Balanced Nordic climate (moderate rain)",
			NORDIC_DETAILED,
			NORDIC_EFFECTS,
			{ 1.0f, 1.0f, 1.0f, 0.8f, 3.0f, 0.5f } },
		// Arctic Tundra
		{
			"Arctic Tundra",
			"Cold, dry Arctic climate (light rain)",
			ARCTIC_DETAILED,
			ARCTIC_EFFECTS,
			{ 0.5f, 0.3f, 0.5f, 0.3f, 3.5f, 0.4f } },
		// Temperate Coastal
		{
			"Temperate Coastal",
			"Maritime climate (heavy rain)",
			COASTAL_DETAILED,
			COASTAL_EFFECTS,
			{ 1.5f, 1.7f, 1.7f, 0.8f, 2.5f, 0.25f } },
		// Monsoon/Extreme
		{
			"Monsoon/Extreme",
			"Extreme monsoon climate (extreme rain)",
			MONSOON_DETAILED,
			MONSOON_EFFECTS,
			{ 2.0f, 2.5f, 2.0f, 1.0f, 2.0f, 0.2f } } }
};

// Extract just the settings for the actual climate preset array
static const std::array<WetnessEffects::ClimateSettings, 6> CLIMATE_PRESETS = { {
	CLIMATE_PRESET_INFO[0].settings,  // Custom
	CLIMATE_PRESET_INFO[1].settings,  // Legacy
	CLIMATE_PRESET_INFO[2].settings,  // Nordic Standard
	CLIMATE_PRESET_INFO[3].settings,  // Arctic Tundra
	CLIMATE_PRESET_INFO[4].settings,  // Temperate Coastal
	CLIMATE_PRESET_INFO[5].settings   // Monsoon/Extreme
} };

void WetnessEffects::SetupResources()
{
	// No authored puddle-mask resources are required.
	// Puddle placement is generated procedurally in shader.
}

void WetnessEffects::ResetRuntimeState()
{
	runtimeState = {};
	g_lastFrameData = {};
	g_hasLastFrameData = false;
	g_cachedCommonBufferData = {};
	g_hasCachedCommonBufferData = false;
	g_cachedCommonBufferFrame = 0;
	g_lastValidOcclusionViewProj = {};
	g_hasLastValidOcclusionViewProj = false;
}

void WetnessEffects::InvalidateSanitizedSettingsCache()
{
	sanitizedSettingsCacheValid = false;
}

const WetnessEffects::Settings& WetnessEffects::GetSanitizedSettings() const
{
	if (!sanitizedSettingsCacheValid) {
		sanitizedSettingsCache = settings;
		SanitizeToggleSettings(sanitizedSettingsCache);
		SanitizeShaderFacingSettings(sanitizedSettingsCache);
		sanitizedSettingsCacheValid = true;
	}
	return sanitizedSettingsCache;
}

void WetnessEffects::DrawSettings()
{
	InvalidateSanitizedSettingsCache();
	const auto drawUintCheckbox = [](const char* label, uint& value) {
		bool enabled = value != 0;
		const bool changed = ImGui::Checkbox(label, &enabled);
		value = enabled ? 1u : 0u;
		return changed;
	};
	const auto drawUintCheckboxWithTooltip = [&](const char* label, uint& value, const char* tooltip) {
		const bool changed = drawUintCheckbox(label, value);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(tooltip);
		}
		return changed;
	};
	const auto markPresetDirtyIfEdited = [this]() {
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			DetectCurrentPreset();
		}
	};
	const auto drawSectionDivider = []() {
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
	};

	// Climate Preset Selection - Always visible at the top
	Util::DrawSectionHeader("Climate Presets", false, false);

	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.3f, 0.4f, 0.6f));    // Subtle blue background
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.35f, 0.45f, 0.8f));  // Slightly darker for button

	// Extract names for combo box
	const char* presetNames[CLIMATE_PRESET_INFO.size()];
	for (size_t i = 0; i < CLIMATE_PRESET_INFO.size(); ++i) {
		presetNames[i] = CLIMATE_PRESET_INFO[i].name;
	}
	// Map preset enum to combo index (Custom=0, Legacy=1, Nordic=2, Arctic=3, Coastal=4, Monsoon=5)
	int currentComboIndex = static_cast<int>(climatePreset);

	if (ImGui::Combo("Climate Preset", &currentComboIndex, presetNames, static_cast<int>(CLIMATE_PRESET_INFO.size()))) {  // Map combo index back to preset enum
		// Simplified: map combo index directly to enum, with bounds check
		ClimatePreset newPreset = (currentComboIndex >= 0 && currentComboIndex < static_cast<int>(CLIMATE_PRESET_INFO.size())) ? static_cast<ClimatePreset>(currentComboIndex) : defaultPreset;

		// Update the preset selection
		climatePreset = newPreset;

		// Apply preset settings (but not for Custom, which just means user-modified)
		if (newPreset != ClimatePreset::Custom) {
			ApplyClimatePreset(newPreset);
		}
	}

	ImGui::PopStyleColor(2);  // Pop both style colors
	if (auto _tt = Util::HoverTooltipWrapper()) {
		if (currentComboIndex >= 0 && currentComboIndex < static_cast<int>(CLIMATE_PRESET_INFO.size())) {
			const auto& info = CLIMATE_PRESET_INFO[currentComboIndex];

			// Handle Custom preset differently
			if (currentComboIndex == 0) {  // Custom preset
				Util::DrawMultiLineTooltip({ "Custom settings - you have modified the preset values.",
					"Select a preset above to apply predefined climate settings." });
			} else {
				// Build combined description lines for actual presets
				std::vector<const char*> tooltipLines;
				tooltipLines.push_back(info.shortDescription);
				// Add detailed description
				for (const char* const* line = info.detailedDescription; *line != nullptr; ++line) {
					tooltipLines.push_back(*line);
				}
				tooltipLines.push_back("Effects:");
				// Add effect descriptions
				for (const char* const* effect = info.effectDescription; *effect != nullptr; ++effect) {
					tooltipLines.push_back(*effect);
				}

				std::vector<std::string> tooltipLinesStr;
				tooltipLinesStr.reserve(tooltipLines.size());
				for (const char* line : tooltipLines) {
					tooltipLinesStr.emplace_back(line);
				}
				Util::DrawMultiLineTooltip(tooltipLinesStr);
			}
		}
	}

	drawSectionDivider();

	drawUintCheckbox("Enable Wetness", settings.EnableWetnessEffects);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted("Enables wetness visuals. Off = no rain film, puddles, or shore wetness.");
	}

	ImGui::TextUnformatted("Wetness Presets");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted("Quick profiles for wetness performance/quality balance.");
	}
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.38f, 0.72f, 0.95f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.48f, 0.86f, 0.95f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.30f, 0.60f, 0.95f));
	for (size_t i = 0; i < WETNESS_UI_PRESETS.size(); ++i) {
		const auto& preset = WETNESS_UI_PRESETS[i];
		if (ImGui::Button(preset.name)) {
			ApplyWetnessUiPreset(settings, wetnessDistanceFadeRange, preset);
			DetectCurrentPreset();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(preset.description);
		}
		if (i + 1 < WETNESS_UI_PRESETS.size()) {
			ImGui::SameLine();
		}
	}
	ImGui::PopStyleColor(3);

	drawSectionDivider();

	if (ImGui::TreeNodeEx("Raindrop Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
		drawUintCheckbox("Enable Raindrop Effects", settings.EnableRaindropFx);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Master switch for raindrop splashes and ripple motion. One of the main wetness performance drivers.");
		}

		const bool raindropSettingsDisabled = settings.EnableRaindropFx == 0;
		const bool raindropAdvancedDisabled = raindropSettingsDisabled || settings.EnableWetnessEffects == 0;

		ImGui::BeginDisabled(raindropSettingsDisabled);

		drawUintCheckbox("Enable Splashes", settings.EnableSplashes);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Shows splash marks where raindrops hit. Off = no splash marks. Reduces raindrop effect cost when disabled.");
		}

		drawUintCheckbox("Enable Ripples", settings.EnableRipples);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Shows circular ripple rings on wet surfaces. Off = no ripple rings. Can be a noticeable cost in heavy rain.");
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(raindropAdvancedDisabled);
		ImGui::SliderFloat("Raindrop End Fade", &settings.RaindropTransitionFalloff, 0.5f, 6.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How quickly raindrop effects fade when rain ends. Higher = faster fade, lower = slower fade.");
		}
		ImGui::SliderFloat("Raindrop Visibility Boost", &raindropVisibilityBoost, RAINDROP_VISIBILITY_BOOST_MIN, RAINDROP_VISIBILITY_BOOST_MAX, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Adds a local gloss/specular boost under active raindrops so ripples and splashes stay easier to read in bright scenes. 0 = current behavior.");
		}
		ImGui::SliderFloat("Raindrop Effect Range", &settings.RaindropFxRangeWorldUnits, RAINDROP_FX_RANGE_UI_MIN_GAME_UNITS, RAINDROP_FX_RANGE_UI_MAX_GAME_UNITS, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			const float rangeMeters = Util::Units::GameUnitsToMeters(settings.RaindropFxRangeWorldUnits);
			std::vector<std::string> tooltipLines = {
				"Higher = raindrop effects cover a larger area around you (terrain/objects and water).",
				"Lower = raindrop effects stay closer to you.",
				"Higher values increase performance cost.",
				std::format("{:.1f} units", settings.RaindropFxRangeWorldUnits),
				std::format("{:.2f} m", rangeMeters)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		if (ImGui::TreeNodeEx("Raindrops")) {
			ImGui::SliderFloat("Grid Size", &settings.RaindropGridSize, 1.0f, 10.0f, "%.1f units");
			markPresetDirtyIfEdited();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"Higher = raindrops are spaced farther apart.",
					"Lower = denser raindrops and fuller rain coverage.",
					"Lower values are more expensive.",
					std::format("{:.1f} units", settings.RaindropGridSize)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
			ImGui::SliderFloat("Interval", &settings.RaindropInterval, 0.1f, 2.0f, "%.1f sec");
			markPresetDirtyIfEdited();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("How often new raindrops are added. Lower = more frequent updates, higher = slower updates. Lower values are more expensive.");
			}
			ImGui::SliderFloat("Chance", &settings.RaindropChance, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			markPresetDirtyIfEdited();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("How many possible drops actually appear. Higher = denser drops, lower = fewer drops. Higher values are more expensive.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Splashes")) {
			ImGui::SliderFloat("Strength", &settings.SplashesStrength, 0.f, 2.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("How visible splash marks are. Higher = bolder splashes, lower = subtler splashes.");
			}
			ImGui::SliderFloat("Min Radius", &settings.SplashesMinRadius, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Minimum splash size. Higher = no tiny splashes, lower = allows smaller splashes.");
			}

			ImGui::SliderFloat("Max Radius", &settings.SplashesMaxRadius, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Maximum splash size. Higher = bigger possible splashes, lower = caps splash size.");
			}

			ImGui::SliderFloat("Lifetime", &settings.SplashesLifetime, 0.1f, 20.f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("How long each splash stays visible. Higher = lingers longer, lower = fades sooner.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Ripples")) {
			ImGui::SliderFloat("Strength", &settings.RippleStrength, 0.f, 2.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("How strong ripple rings look. Higher = stronger ripples, lower = softer ripples.");
			}

			ImGui::SliderFloat("Radius", &settings.RippleRadius, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Ripple ring size. Higher = wider rings, lower = tighter rings.");
			}

			ImGui::SliderFloat("Breadth", &settings.RippleBreadth, 0.f, 1.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Ripple ring thickness. Higher = thicker rings, lower = thinner rings.");
			}

			ImGui::SliderFloat("Lifetime", &settings.RippleLifetime, 0.f, settings.RaindropInterval, "%.2f sec", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("How long each ripple remains visible. Higher = longer rings, lower = faster fade.");
			}
			ImGui::TreePop();
		}

		ImGui::EndDisabled();
		ImGui::TreePop();
	}

	drawSectionDivider();

	if (ImGui::TreeNodeEx("Wet Reflection Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
		int reflectionMode = 0;  // 0 = Off, 1 = Modern, 2 = Legacy
		if (settings.EnableModernWetReflection != 0 && settings.EnableLegacyWetReflection == 0) {
			reflectionMode = 1;
		} else if (settings.EnableLegacyWetReflection != 0 && settings.EnableModernWetReflection == 0) {
			reflectionMode = 2;
		}
		const char* reflectionModeItems[] = { "Off", "Modern", "Legacy" };
		if (ImGui::Combo("Reflection Mode", &reflectionMode, reflectionModeItems, IM_ARRAYSIZE(reflectionModeItems))) {
			settings.EnableModernWetReflection = (reflectionMode == 1) ? 1u : 0u;
			settings.EnableLegacyWetReflection = (reflectionMode == 2) ? 1u : 0u;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Choose Off, Modern, or Legacy wet reflection style.");
		}

		SanitizePersistentReflectionSettings(settings, modernWetIndirectSpecularScale, legacyWetIndirectSpecularScale);
		const bool anyReflectionModeEnabled = settings.EnableModernWetReflection != 0 || settings.EnableLegacyWetReflection != 0;
		const bool legacyReflectionModeEnabled = settings.EnableLegacyWetReflection != 0 && settings.EnableModernWetReflection == 0;
		const float legacyReflectionScaleMax = GetLegacyReflectionUiMax();
		ImGui::BeginDisabled(!anyReflectionModeEnabled);
		if (legacyReflectionModeEnabled) {
			ImGui::SliderFloat("Wet Reflection Shine", &legacyWetIndirectSpecularScale, 0.0f, legacyReflectionScaleMax, "%.4f", ImGuiSliderFlags_AlwaysClamp);
		} else {
			ImGui::SliderFloat("Wet Reflection Shine", &modernWetIndirectSpecularScale, 0.0f, MAX_MODERN_WET_REFLECTION_UI_SCALE, "%.4f", ImGuiSliderFlags_AlwaysClamp);
		}
		ImGui::SliderFloat("Wet Cubemap Stability Bias", &settings.WetCubemapStabilityBiasStrength, WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MIN, WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MAX, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Wet-only dynamic cubemap test control. Biases puddle reflections toward the surface normal/world up on upward-facing surfaces near the player, making them broader and more stable but also more sky-heavy.");
		}
		ImGui::SliderFloat("Wet Cubemap Stability Range", &settings.WetCubemapStabilityBiasRangeWorldUnits, WET_CUBEMAP_STABILITY_BIAS_RANGE_UI_MIN_GAME_UNITS, WET_CUBEMAP_STABILITY_BIAS_RANGE_UI_MAX_GAME_UNITS, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			const float stabilityRangeMeters = Util::Units::GameUnitsToMeters(settings.WetCubemapStabilityBiasRangeWorldUnits);
			Util::DrawMultiLineTooltip({
				"Controls how far from the player the wet cubemap stability bias remains active.",
				std::format("{:.1f} units", settings.WetCubemapStabilityBiasRangeWorldUnits),
				std::format("{:.1f} m", stabilityRangeMeters)
			});
		}
		ImGui::EndDisabled();
		SanitizePersistentReflectionSettings(settings, modernWetIndirectSpecularScale, legacyWetIndirectSpecularScale);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Base wet reflection intensity. During rain this is the main reflection control. After rain, this remains the baseline and the post-rain controls bias puddle reflections up or down while puddles persist.");
		}

		drawUintCheckboxWithTooltip(
			"Forward Reflection Bias",
			settings.EnableForwardReflectionBias,
			"On = keeps wet reflections stronger at forward/top-down views. Off = default angle falloff.");

		drawUintCheckboxWithTooltip(
			"Non-PBR Reflection Lift",
			settings.EnableVanillaReflectionCompensation,
			"Non-PBR only. On = brighter/cleaner vanilla wet reflections, Off = neutral vanilla reflection response.");

		ImGui::TreePop();
	}

	drawSectionDivider();

	if (ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto drawDryingSlider = [](const char* label, float& value, const char* tooltip) {
			ImGui::SliderFloat(label, &value, DRYING_HOURS_MIN, DRYING_HOURS_MAX, "%.0f h", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(tooltip);
			}
		};

		ImGui::Checkbox("Enable Weather-Driven Drying", &enableWeatherDrivenDryingModel);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Automatically controls drying based on weather and season. When on, manual drying-time sliders below are ignored.");
		}

		ImGui::SliderFloat("Weather transition speed", &settings.WeatherTransitionSpeed, 0.2f, 8.0f);
		markPresetDirtyIfEdited();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How fast wetness responds to weather changes. Higher = quicker wet/dry transitions, lower = slower transitions.");
		}

		ImGui::Separator();
		ImGui::TextUnformatted("Drying Times");

		ImGui::BeginDisabled(enableWeatherDrivenDryingModel);
		drawDryingSlider("Stone Drying Time", settings.StoneDryingMultiplier, "Drying time for stone-like surfaces after rain. Higher = dries slower, lower = dries faster.");
		drawDryingSlider("Grass Drying Time", settings.GrassDryingMultiplier, "Drying time for grass-like surfaces after rain. Higher = dries slower, lower = dries faster.");
		drawDryingSlider("Dirt Drying Time", settings.DirtDryingMultiplier, "Drying time for dirt-like surfaces after rain. Higher = dries slower, lower = dries faster.");
		drawDryingSlider("Puddle Drying Time", puddleDryingHours, "How long puddles remain after rain. Higher = puddles last longer, lower = puddles fade sooner.");
		ImGui::EndDisabled();
		if (enableWeatherDrivenDryingModel) {
			ImGui::TextDisabled("Manual drying-time sliders are disabled while weather-driven drying is enabled.");
		}

		ImGui::Separator();
		ImGui::TextUnformatted("Rain");

		ImGui::SliderFloat("Rain Wetness", &settings.MaxRainWetness, 0.0f, 2.5f);
		markPresetDirtyIfEdited();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How strong the rain wet-film looks. Higher = wetter/stronger rain film, lower = lighter rain film.");
		}

		ImGui::SliderFloat("Min Rain Wetness", &settings.MinRainWetness, 0.0f, 0.9f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Minimum rain wetness floor on surfaces. Higher = more surfaces stay visibly wet, lower = wetness favors only more exposed/up-facing surfaces.");
		}

		ImGui::SliderFloat("Wet Surface Darkening", &settings.WetDarkeningStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How much wet ground darkens. Higher = darker wet patches, lower = closer to original brightness.");
		}

		ImGui::SliderFloat("Wet Film Specular Floor", &settings.WetFilmSpecularFloorScale, WET_FILM_SPECULAR_FLOOR_SCALE_MIN, WET_FILM_SPECULAR_FLOOR_SCALE_MAX, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Controls subtle wet-film reflections outside standing puddles. Higher = stronger ground glitter between puddles.");
		}

		ImGui::SliderFloat("Wet Highlight Reduction", &settings.WetHighlightReduction, WET_HIGHLIGHT_REDUCTION_MIN, WET_HIGHLIGHT_REDUCTION_MAX, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Reduces bright white wet highlights. Higher = less white glare, lower = brighter highlights.");
		}

		ImGui::SliderFloat("Wetness Fade Range", &wetnessDistanceFadeRange, WETNESS_DISTANCE_FADE_RANGE_UI_MIN_GAME_UNITS, WETNESS_DISTANCE_FADE_RANGE_UI_MAX_GAME_UNITS, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			const float fadeRangeMeters = Util::Units::GameUnitsToMeters(wetnessDistanceFadeRange);
			std::vector<std::string> tooltipLines = {
				"View-depth fade range for dev-style wetness culling.",
				"Higher = puddles, wet sheen, wet darkening, and wet-normal flattening stay visible farther away.",
				"Lower = those wetness effects fade sooner for performance.",
				"Does not affect raindrop FX or water ripple range.",
				std::format("{:.1f} units", wetnessDistanceFadeRange),
				std::format("{:.2f} m", fadeRangeMeters)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		ImGui::SliderFloat("Skin Wetness", &settings.SkinWetness, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How wet skin and hair look in rain. Higher = stronger wet look, lower = subtler wet look.");
		}

		ImGui::Separator();
		ImGui::TextUnformatted("Puddles");

		ImGui::SliderFloat("Puddle Wetness", &settings.MaxPuddleWetness, 0.0f, 6.0f);
		markPresetDirtyIfEdited();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Overall puddle strength. Higher = stronger puddles and broader puddle coverage, lower = weaker/more limited puddles.");
		}

		ImGui::SliderFloat("Puddle Max Angle", &settings.PuddleMaxAngle, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Slope limit for puddle formation. Higher = puddles restricted to flatter ground, lower = puddles can appear on steeper slopes.");
		}

		ImGui::SliderFloat("Puddle Radius", &settings.PuddleRadiusWorldUnits, PUDDLE_RADIUS_UI_MIN_GAME_UNITS, PUDDLE_RADIUS_UI_MAX_GAME_UNITS, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			const float puddleRadiusMeters = Util::Units::GameUnitsToMeters(settings.PuddleRadiusWorldUnits);
			std::vector<std::string> tooltipLines = {
				"Higher = larger individual puddles, lower = smaller individual puddles.",
				"When Inactivate Rain Puddle Auto-Expansion is off, rain expands radius to maximum.",
				"After rain stops, radius settles toward this value over about 1 in-game hour.",
				"When Inactivate Rain Puddle Auto-Expansion is on, this slider is used directly during rain and after rain.",
				"Does not control puddle layout/placement.",
				std::format("{:.1f} units", settings.PuddleRadiusWorldUnits),
				std::format("{:.2f} m", puddleRadiusMeters)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		ImGui::SliderFloat("Puddle Layout", &puddleLayout, PUDDLE_LAYOUT_MIN, PUDDLE_LAYOUT_MAX, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Changes puddle shape/placement pattern only (not puddle size). Lower values = smoother, broader patches. Higher values = more irregular, broken-up placement.");
		}

		ImGui::SliderFloat("Puddle Water Look", &settings.PuddleMinWetness, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Wetness threshold for puddles to look like standing water. Higher = only stronger puddles become flat/reflective; lower = watery look appears sooner.");
		}

		ImGui::SliderFloat("Rain Reflection Balance", &rainReflectionBalance, RAIN_REFLECTION_BALANCE_MIN, RAIN_REFLECTION_BALANCE_MAX, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Balances rain puddle reflections. 0 = more cubemap reflection. Higher = less cubemap and more specular wet-film response. 1 = strongest shift.");
		}

		ImGui::Dummy(ImVec2(0.0f, 12.0f));
		ImGui::SliderFloat("Post-Rain Puddle Shine", &settings.PostRainPuddleWaterStrength, POST_RAIN_PUDDLE_SHINE_MIN, POST_RAIN_PUDDLE_SHINE_MAX, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Controls post-rain puddle reflection intensity relative to Wet Reflection. 2.5 = neutral. Lower = dimmer/subtler, higher = stronger/brighter.");
		}
		ImGui::SliderFloat("Post-Rain Water Clarity", &postRainWaterClarity, POST_RAIN_WATER_CLARITY_MIN, POST_RAIN_WATER_CLARITY_MAX, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Shapes post-rain puddle reflections on top of Wet Reflection. 0 = more cubemap mirror. Higher = less sky glare, deeper puddle body, and clearer water-like puddles.");
		}

		ImGui::Dummy(ImVec2(0.0f, 8.0f));
		ImGui::Checkbox("Inactivate Rain Puddle Auto-Expansion (adjust Post-Rain Puddle Size/Pattern)", &inactivateRainPuddleAutoExpansion);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("On = disables automatic max puddle size during rain, so Puddle Radius controls puddle size during rain and after rain. Off = rain forces max puddle size, then settles back after rain.");
		}

		ImGui::Separator();
		ImGui::TextUnformatted("Shore");

		ImGui::SliderFloat("Shore Wetness", &settings.MaxShoreWetness, 0.0f, 1.0f);
		markPresetDirtyIfEdited();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Wetness near rivers, lakes, and shorelines. Higher = wetter banks, lower = drier edges.");
		}

		int shoreRange = static_cast<int>(settings.ShoreRange);
		if (ImGui::SliderInt("Shore Range", &shoreRange, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp)) {
			settings.ShoreRange = static_cast<uint>(shoreRange);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			const float shoreRangeMeters = Util::Units::GameUnitsToMeters(static_cast<float>(settings.ShoreRange));
			std::vector<std::string> tooltipLines = {
				"How far shore wetness reaches from water.",
				"Higher = wetness extends farther from the shoreline.",
				"Lower = wetness stays close to the shoreline.",
				std::format("{} units", settings.ShoreRange),
				std::format("{:.2f} m", shoreRangeMeters)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		ImGui::SliderFloat("Shore Persistent Darkening", &shorePersistentDarkeningStrength, SHORE_PERSISTENT_DARKENING_MIN, SHORE_PERSISTENT_DARKENING_MAX, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Persistent shoreline darkening independent of rain runtime. Higher = darker shore band, lower = subtler darkening.");
		}

		ImGui::TreePop();
	}

	drawSectionDivider();

	auto& weatherPicker = globals::features::weatherPicker;
	if (weatherPicker.loaded) {
		if (ImGui::SmallButton(("Open " + weatherPicker.GetName()).c_str())) {
			// Navigate to the replacement feature in the menu
			Menu::GetSingleton()->SelectFeatureMenu(weatherPicker.GetShortName());
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Open the installed %s feature", weatherPicker.GetShortName().c_str());
		}
	}

	if (ImGui::TreeNodeEx("Debug (Testing)")) {
		ImGui::Checkbox("Enable Wetness Override", &debugSettings.EnableWetnessOverride);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Use manual wetness values instead of automatic values.");
		}

		ImGui::Checkbox("Enable Puddle Override", &debugSettings.EnablePuddleOverride);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Use manual puddle values instead of automatic values.");
		}

		ImGui::Checkbox("Enable Rain Override", &debugSettings.EnableRainOverride);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Use manual rain-signal values instead of weather rain values.");
		}

		ImGui::Checkbox("Enable Interior/Exterior Override", &debugSettings.EnableIntExOverride);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("When disabled, only exterior override values are used.");
		}

		if (debugSettings.EnableWetnessOverride) {
			ImGui::SliderFloat2("Wetness In/Exterior", &debugSettings.WetnessOverride.x, 0.0f, 2.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Manual wetness values. Higher = wetter look, lower = drier look.");
			}
		}

		if (debugSettings.EnablePuddleOverride) {
			ImGui::SliderFloat2("Puddle Wetness In/Exterior", &debugSettings.PuddleWetnessOverride.x, 0.0f, 2.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Manual puddle values. Higher = stronger puddles, lower = weaker puddles.");
			}
		}

		if (debugSettings.EnableRainOverride) {
			ImGui::SliderFloat2("Rain In/Exterior", &debugSettings.RainOverride.x, 0.0f, 1.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Manual rain signal. Higher = stronger rain signal, lower = weaker rain signal.");
			}
		}
		ImGui::TreePop();
	}
}

// =====================
// UI/ImGui Helper Functions
// =====================

// Helper for meteorological rain type classification
static void DrawRainTypeLabel(const char* prefix, float rate)
{
	// Meteorological categories (mm/hr):
	// Light: <2.5, Moderate: 2.5-7.5, Heavy: 7.5-15, Extreme: >15
	const char* label = "";
	ImVec4 valueColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	Util::ColorCodedValueConfig config;
	config.format = "%.2f mm/hr";
	config.sameLine = true;
	config.tooltipText = nullptr;
	config.thresholds = {
		{ 2.5f, ImVec4(0.5f, 0.7f, 1.0f, 1.0f) },    // Light (Blue)
		{ 7.5f, ImVec4(0.2f, 0.8f, 0.2f, 1.0f) },    // Moderate (Green)
		{ 15.0f, ImVec4(1.0f, 0.7f, 0.2f, 1.0f) },   // Heavy (Orange)
		{ FLT_MAX, ImVec4(1.0f, 0.2f, 0.2f, 1.0f) }  // Extreme (Red)
	};
	if (rate < 2.5f) {
		label = "Light Rain";
		valueColor = config.thresholds[0].color;
	} else if (rate < 7.5f) {
		label = "Moderate Rain";
		valueColor = config.thresholds[1].color;
	} else if (rate < 15.0f) {
		label = "Heavy Rain";
		valueColor = config.thresholds[2].color;
	} else {
		label = "Extreme Rain";
		valueColor = config.thresholds[3].color;
	}
	// Print prefix (uncolored), then value (colored), then meteorological label (colored, after value)
	ImGui::Text("%s:", prefix);
	ImGui::SameLine();
	ImGui::TextColored(valueColor, config.format, rate);
	ImGui::SameLine();
	ImGui::TextColored(valueColor, "(%s)", label);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Meteorological rain types:");
		ImGui::BulletText("Light: <2.5 mm/hr");
		ImGui::BulletText("Moderate: 2.5 - 7.5 mm/hr");
		ImGui::BulletText("Heavy: 7.5 - 15 mm/hr");
		ImGui::BulletText("Extreme: >15 mm/hr");
	}
}

// =====================
// Weather/Precipitation Analysis Helpers
// =====================

// Helper function to calculate precipitation rate from shader data and settings
float WetnessEffects::CalculatePrecipitationRate(float raindropChance, float raindropGridSizeGameUnits, float raindropIntervalSeconds, float mlPerDrop) const
{
	// Validate inputs to prevent division by zero and invalid calculations
	if (raindropGridSizeGameUnits <= 0.0f || raindropIntervalSeconds <= 0.0f) {
		logger::warn("[WetnessEffects] Invalid parameters: gridSize={}, interval={}", raindropGridSizeGameUnits, raindropIntervalSeconds);
		return 0.0f;
	}

	if (raindropChance < 0.0f || raindropChance > 1.0f) {
		logger::warn("[WetnessEffects] Invalid raindrop chance: {}, clamping to [0,1]", raindropChance);
		raindropChance = std::clamp(raindropChance, 0.0f, 1.0f);
	}
	// Use physically realistic default if not specified (10 microliters typical for large raindrop)
	if (mlPerDrop <= 0.0f)
		mlPerDrop = 0.01f;
	// Convert grid size from game units to meters
	float gridSizeMeters = Util::Units::GameUnitsToMeters(raindropGridSizeGameUnits);
	float gridAreaSqMeters = gridSizeMeters * gridSizeMeters;
	// Calculate drops per second per grid cell
	float dropsPerSecond = raindropChance / raindropIntervalSeconds;
	float dropsPerSqMeterPerSec = dropsPerSecond / gridAreaSqMeters;
	// Convert to equivalent mm/hr precipitation rate
	// 1 mm/hr = 1 L/m^2/hr = 1 mL/cm^2/hr = 1 mm depth
	const float SEC_PER_HOUR = 3600.0f;
	const float SQ_M_TO_SQ_CM = 10000.0f;
	return dropsPerSqMeterPerSec * mlPerDrop * SEC_PER_HOUR / SQ_M_TO_SQ_CM;
}

// =====================
// Preset/Configuration Helpers
// =====================

const WetnessEffects::ClimateSettings& WetnessEffects::GetClimateSettings(ClimatePreset preset)
{
	auto index = static_cast<size_t>(preset);
	if (index >= CLIMATE_PRESETS.size()) {
		index = magic_enum::enum_integer<ClimatePreset>(defaultPreset);  // Use defaultPreset
	}
	return CLIMATE_PRESETS[index];
}

void WetnessEffects::ApplyClimatePreset(ClimatePreset preset)
{
	const auto& climate = GetClimateSettings(preset);

	// Update the climate preset
	climatePreset = preset;

	// Set settings to preset base values instead of multiplying existing values
	// This ensures consistent, predictable behavior
	Settings defaultSettings{};  // Get default values

	settings.MaxRainWetness = defaultSettings.MaxRainWetness * climate.wetnessMultiplier;
	settings.MaxPuddleWetness = defaultSettings.MaxPuddleWetness * climate.puddleMultiplier;
	settings.WeatherTransitionSpeed = defaultSettings.WeatherTransitionSpeed * climate.transitionSpeed;

	// Use the preset's raindrop chance, grid size, and interval as the base values
	settings.RaindropChance = climate.raindropChance;
	settings.RaindropGridSize = climate.raindropGridSize;
	settings.RaindropInterval = climate.raindropInterval;

	// Removed clamping for all settings to allow full preset range
	InvalidateSanitizedSettingsCache();
}

WetnessEffects::PerFrame WetnessEffects::GetCommonBufferData() const
{
	const bool canUseFrameCache = globals::state != nullptr;
	const uint32_t frameIndex = canUseFrameCache ? globals::state->frameCount : 0u;
	if (canUseFrameCache && g_hasCachedCommonBufferData && g_cachedCommonBufferFrame == frameIndex) {
		return g_cachedCommonBufferData;
	}

	PerFrame data{};

	data.Raining = 0.0f;
	data.Wetness = 0.0f;
	data.PuddleWetness = 0.0f;
	float currentWeight = 1.0f;
	float lastWeight = 0.0f;
	float currentRainingFX = 0.0f;
	float lastRainingFX = 0.0f;
	float currentRainingAccum = 0.0f;
	float lastRainingAccum = 0.0f;
	const bool isInterior = Util::IsInterior();
	RE::TESWeather* currentWeather = nullptr;
	RE::TESWeather* lastWeather = nullptr;
	bool fullSkyMode = false;
	bool engineRaining = false;

	if (auto sky = globals::game::sky) {
		currentWeather = sky->currentWeather;
		lastWeather = sky->lastWeather;
		currentWeight = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
		lastWeight = 1.0f - currentWeight;
		fullSkyMode = sky->mode.get() == RE::Sky::Mode::kFull;
		engineRaining = fullSkyMode && sky->IsRaining();

		// Wetness accumulation uses weather-level precipitation data, independent of active precipitation geometry.
		currentRainingAccum = GetWeatherRainIntensity(currentWeather);
		lastRainingAccum = GetWeatherRainIntensity(lastWeather);

		if (fullSkyMode) {
			const bool needsOcclusionProjection =
				engineRaining ||
				runtimeState.wasRainingLastFrame ||
				runtimeState.postRainEventWeight > RUNTIME_DRY_EPSILON ||
				runtimeState.puddleDepth > RUNTIME_DRY_EPSILON ||
				runtimeState.wetnessDepth > RUNTIME_DRY_EPSILON;
			if (g_hasLastValidOcclusionViewProj && needsOcclusionProjection) {
				data.OcclusionViewProj = g_lastValidOcclusionViewProj;
			}
			if (auto precip = sky->precip) {
				RE::BSParticleShaderRainEmitter* currentRainEmitter = nullptr;
				RE::BSParticleShaderRainEmitter* lastRainEmitter = nullptr;
				if (precip->currentPrecip) {
					currentRainEmitter = GetRainEmitterFromPrecipGeometry(precip->currentPrecip.get());
				}
				if (precip->lastPrecip) {
					lastRainEmitter = GetRainEmitterFromPrecipGeometry(precip->lastPrecip.get());
				}

				auto* rainEmitter = currentRainEmitter ? currentRainEmitter : lastRainEmitter;
				if (rainEmitter) {
					data.OcclusionViewProj = rainEmitter->occlusionProjection;
					g_lastValidOcclusionViewProj = data.OcclusionViewProj;
					g_hasLastValidOcclusionViewProj = true;
				}

				if (currentRainEmitter && currentWeather) {
					currentRainingFX = GetWeatherRainIntensity(currentWeather);
				}
				if (lastRainEmitter && lastWeather) {
					lastRainingFX = GetWeatherRainIntensity(lastWeather);
				}
			}
		}
	}

	const float currentRainWeight = IsRainyWeather(currentWeather) ? currentWeight : 0.0f;
	const float lastRainWeight = IsRainyWeather(lastWeather) ? lastWeight : 0.0f;
	// Keep rain-driven visuals transition-aware: if either current or last weather is rainy,
	// blend both contributions so rain fades out/in over the handoff instead of hard-dropping.
	const float weatherRainTransitionWeight = std::clamp(currentRainWeight + lastRainWeight, 0.0f, 1.0f);
	// Prevent long raindrop linger near transition end by requiring both:
	// - active precipitation FX intensity
	// - weather-level rain state
	const float currentRainingVisual = std::min(currentRainingFX, currentRainingAccum);
	const float lastRainingVisual = std::min(lastRainingFX, lastRainingAccum);
	const float blendedRainingAccum = std::clamp(currentRainingAccum * currentWeight + lastRainingAccum * lastWeight, 0.0f, 1.0f);
	const float blendedRainingVisualWeighted = std::clamp(currentRainingVisual * currentRainWeight + lastRainingVisual * lastRainWeight, 0.0f, 1.0f);
	const float blendedRainingVisual = blendedRainingVisualWeighted;
	const float raindropTransitionFalloff = std::clamp(settings.RaindropTransitionFalloff, 0.5f, 6.0f);
	const float blendedRainingVisualShaped = std::pow(std::clamp(blendedRainingVisual, 0.0f, 1.0f), raindropTransitionFalloff);
	const float blendedRainingVisualSnapped = (weatherRainTransitionWeight > 0.005f && blendedRainingVisualShaped >= 0.0025f) ? blendedRainingVisualShaped : 0.0f;
	const bool hasPrecipitationFxSignal = (currentRainingFX > 0.0f) || (lastRainingFX > 0.0f);
	const bool rainingByWeatherMetadata =
		(engineRaining && blendedRainingAccum > 0.0f) ||
		((weatherRainTransitionWeight > 0.01f) && (blendedRainingAccum > 0.01f));
	const bool rainingNow = (blendedRainingVisualSnapped > 0.0f) || rainingByWeatherMetadata;
	const float rainExposureSource = hasPrecipitationFxSignal ? blendedRainingVisualWeighted : blendedRainingAccum;
	const auto getEffectiveDryingHours = [&](float rainEventWeight) {
		if (enableWeatherDrivenDryingModel) {
			return GetWeatherDrivenDryingHours(currentWeather, lastWeather, currentWeight, lastWeight, rainEventWeight);
		}
		return GetManualDryingHours(settings, puddleDryingHours);
	};
	EffectiveDryingHours effectiveDryingHours{};

	double deltaGameSeconds = 0.0;
	const bool gamePaused = globals::game::ui && globals::game::ui->GameIsPaused();
	const auto* calendar = RE::Calendar::GetSingleton();
	if (calendar) {
		const double currentGameSeconds = static_cast<double>(calendar->GetCurrentGameTime()) * SECONDS_IN_A_DAY;
		if (runtimeState.hasLastGameTime) {
			deltaGameSeconds = currentGameSeconds - runtimeState.lastGameTimeSeconds;
			if (deltaGameSeconds < 0.0) {
				deltaGameSeconds = 0.0;
			} else if (deltaGameSeconds > MAX_TIME_DELTA_SECONDS) {
				deltaGameSeconds = MAX_TIME_DELTA_SECONDS;
			}
		}
		runtimeState.lastGameTimeSeconds = currentGameSeconds;
		runtimeState.hasLastGameTime = true;
	} else if (!gamePaused) {
		deltaGameSeconds = RE::GetSecondsSinceLastFrame();
	}

	// Allow wetness timelines to progress from calendar delta even while the game is paused
	// (e.g. wait/sleep), but still avoid frame-time updates while paused without calendar data.
	const bool canAdvanceWetnessTime = deltaGameSeconds > 0.0 && (calendar != nullptr || !gamePaused);
	const bool hasDebugOverrides =
		debugSettings.EnableWetnessOverride ||
		debugSettings.EnablePuddleOverride ||
		debugSettings.EnableRainOverride;
	const bool dryTimelineSettled =
		runtimeState.wetnessDepth <= RUNTIME_DRY_EPSILON &&
		runtimeState.puddleDepth <= RUNTIME_DRY_EPSILON &&
		runtimeState.rainEventExposure <= RUNTIME_DRY_EPSILON &&
		runtimeState.postRainEventWeight <= RUNTIME_DRY_EPSILON;
	bool runtimeInactive = settings.EnableWetnessEffects == 0 ||
		(settings.EnableWetnessEffects != 0 &&
		!hasDebugOverrides &&
		!rainingNow &&
		dryTimelineSettled);

	if (runtimeInactive) {
		runtimeState.wetnessDepth = 0.0f;
		runtimeState.puddleDepth = 0.0f;
		runtimeState.rainEventExposure = 0.0f;
		runtimeState.rainEventWeight = 0.0f;
		runtimeState.postRainEventWeight = 0.0f;
		runtimeState.postRainElapsedSeconds = 0.0f;
		runtimeState.postRainStartWetnessDepth = 0.0f;
		runtimeState.postRainStartPuddleDepth = 0.0f;
		runtimeState.wasRainingLastFrame = false;
		effectiveDryingHours = getEffectiveDryingHours(0.0f);
	}

	if (canAdvanceWetnessTime && !runtimeInactive) {
		float wetnessCurrentRate = 0.0f;
		float puddleCurrentRate = 0.0f;
		float wetnessLastRate = 0.0f;
		float puddleLastRate = 0.0f;
		GetDepthDeltaRates(currentWeather, currentRainingAccum, wetnessCurrentRate, puddleCurrentRate);
		GetDepthDeltaRates(lastWeather, lastRainingAccum, wetnessLastRate, puddleLastRate);

		float blendedWetnessRate = wetnessCurrentRate * currentWeight + wetnessLastRate * lastWeight;
		float blendedPuddleRate = puddleCurrentRate * currentWeight + puddleLastRate * lastWeight;
		const float transitionSpeed = std::clamp(settings.WeatherTransitionSpeed, MIN_TRANSITION_SPEED, MAX_TRANSITION_SPEED);
		const float scaledDeltaSeconds = static_cast<float>(deltaGameSeconds) * transitionSpeed;

		// Do not keep increasing wetness/puddles when rain is effectively off for the current render context.
		if (!rainingNow) {
			blendedWetnessRate = std::min(blendedWetnessRate, 0.0f);
			// Post-rain puddle timeline is controlled by the envelope duration only
			// (manual Puddle Drying Time or weather-driven puddle hours).
			blendedPuddleRate = 0.0f;
		} else {
			// While rain is active/transitioning, puddles should only accumulate.
			blendedPuddleRate = std::max(blendedPuddleRate, 0.0f);
		}

		// Track rain-event memory (duration-weighted intensity) for post-rain persistence.
		if (rainingNow) {
			runtimeState.rainEventExposure += rainExposureSource * scaledDeltaSeconds;
			runtimeState.rainEventExposure = std::min(runtimeState.rainEventExposure, RAIN_EVENT_REFERENCE_SECONDS * 4.0f);
			runtimeState.rainEventWeight = std::clamp(runtimeState.rainEventExposure / RAIN_EVENT_REFERENCE_SECONDS, 0.0f, 1.0f);
			runtimeState.postRainEventWeight = runtimeState.rainEventWeight;
			runtimeState.postRainElapsedSeconds = 0.0f;
			runtimeState.postRainStartWetnessDepth = runtimeState.wetnessDepth;
			runtimeState.postRainStartPuddleDepth = runtimeState.puddleDepth;
		} else {
			if (runtimeState.wasRainingLastFrame) {
				runtimeState.postRainEventWeight = std::clamp(runtimeState.rainEventExposure / RAIN_EVENT_REFERENCE_SECONDS, 0.0f, 1.0f);
				runtimeState.postRainElapsedSeconds = 0.0f;
				runtimeState.postRainStartWetnessDepth = runtimeState.wetnessDepth;
				runtimeState.postRainStartPuddleDepth = runtimeState.puddleDepth;
			} else {
				runtimeState.postRainElapsedSeconds += static_cast<float>(deltaGameSeconds);
			}

			if (runtimeState.rainEventExposure > 0.0f) {
				const float memoryDecayPerSecond = RAIN_EVENT_REFERENCE_SECONDS / RAIN_EVENT_DECAY_SECONDS;
				runtimeState.rainEventExposure = std::max(0.0f, runtimeState.rainEventExposure - memoryDecayPerSecond * scaledDeltaSeconds);
			}

			runtimeState.rainEventWeight = std::clamp(runtimeState.rainEventExposure / RAIN_EVENT_REFERENCE_SECONDS, 0.0f, 1.0f);
		}

		// Long rain events slow down drying after rain stops.
		const float dryingEventWeight = rainingNow ? runtimeState.rainEventWeight : runtimeState.postRainEventWeight;
		effectiveDryingHours = getEffectiveDryingHours(dryingEventWeight);
		const float wetnessDurationHours = GetWeightedSurfaceDryingHours(effectiveDryingHours);
		const float puddleDurationHours = ClampDryingHours(effectiveDryingHours.puddleHours, DRYING_HOURS_MAX);
		if (blendedWetnessRate < 0.0f) {
			const float wetnessDryScale = std::lerp(1.0f, MIN_WETNESS_DRY_SCALE_AT_MAX_EVENT, runtimeState.rainEventWeight);
			blendedWetnessRate *= wetnessDryScale;
			const float wetnessRateScaleFromDuration = std::clamp(DRYING_HOURS_MAX / wetnessDurationHours, 0.25f, DRYING_HOURS_MAX);
			blendedWetnessRate *= wetnessRateScaleFromDuration;
		}
		ApplyDepthDelta(scaledDeltaSeconds, blendedWetnessRate, blendedPuddleRate, runtimeState.wetnessDepth, runtimeState.puddleDepth);

		if (!rainingNow && (runtimeState.wetnessDepth > 0.0f || runtimeState.puddleDepth > 0.0f)) {
			const float elapsedSeconds = std::max(0.0f, runtimeState.postRainElapsedSeconds);
			// Surface dry-out cap:
			// - weather-driven model: non-puddles dry within 3..18h
			// - manual model: uses slider values
			// Use weighted surface duration so each slider has visible influence.
			const float wetnessDurationSeconds = DryingHoursToSeconds(wetnessDurationHours);
			// Puddle dry-out cap:
			// - weather-driven model: 12..24h
			// - manual model: explicit slider control
			const float puddleDurationSeconds = DryingHoursToSeconds(puddleDurationHours);

			ApplyPostRainDepthEnvelope(
				runtimeState.wetnessDepth,
				runtimeState.postRainStartWetnessDepth,
				MAX_WETNESS_DEPTH,
				elapsedSeconds,
				wetnessDurationSeconds);
			ApplyPostRainDepthEnvelope(
				runtimeState.puddleDepth,
				runtimeState.postRainStartPuddleDepth,
				MAX_PUDDLE_DEPTH,
				elapsedSeconds,
				puddleDurationSeconds);
		}

		runtimeState.wasRainingLastFrame = rainingNow;
	}

	if (settings.EnableWetnessEffects) {
		if (fullSkyMode) {
			data.Raining = blendedRainingVisualSnapped;
			data.Wetness = std::min(runtimeState.wetnessDepth, MAX_OUTPUT_WETNESS);
			data.PuddleWetness = std::min(runtimeState.puddleDepth, MAX_OUTPUT_PUDDLE_WETNESS);
			if (debugSettings.EnableWetnessOverride) {
				data.Wetness = debugSettings.WetnessOverride.y;
			}
			if (debugSettings.EnablePuddleOverride) {
				data.PuddleWetness = debugSettings.PuddleWetnessOverride.y;
			}
			if (debugSettings.EnableRainOverride) {
				data.Raining = debugSettings.RainOverride.y;
			}
		} else {
			if (debugSettings.EnableWetnessOverride) {
				data.Wetness = debugSettings.EnableIntExOverride ? debugSettings.WetnessOverride.x : debugSettings.WetnessOverride.y;
			}
			if (debugSettings.EnablePuddleOverride) {
				data.PuddleWetness = debugSettings.EnableIntExOverride ? debugSettings.PuddleWetnessOverride.x : debugSettings.PuddleWetnessOverride.y;
			}
			if (debugSettings.EnableRainOverride) {
				data.Raining = debugSettings.EnableIntExOverride ? debugSettings.RainOverride.x : debugSettings.RainOverride.y;
			}
		}
	}

	// Strict interior guard: never render rain/wetness indoors.
	if (isInterior) {
		data.Raining = 0.0f;
		data.Wetness = 0.0f;
		data.PuddleWetness = 0.0f;
	}

	static size_t rainTimer = 0;  // size_t for precision
	if (!gamePaused)
		rainTimer += (size_t)(RE::GetSecondsSinceLastFrame() * 1000);  // BSTimer::delta is always 0 for some reason
	data.Time = rainTimer / 1000.f;

	if (!canAdvanceWetnessTime && !runtimeInactive) {
		const float dryingEventWeight = rainingNow ? runtimeState.rainEventWeight : runtimeState.postRainEventWeight;
		effectiveDryingHours = getEffectiveDryingHours(dryingEventWeight);
	}

	const Settings& sanitizedSettings = GetSanitizedSettings();
	data.settings = MakeShaderSettings(sanitizedSettings);
	if (runtimeInactive) {
		data.settings.EnableWetnessEffects = 0u;
	}
	// Raindrops should stop when rain weather transition is complete (weight reaches zero),
	// not immediately when current weather pointer flips.
	if (weatherRainTransitionWeight <= 0.005f || isInterior) {
		data.settings.EnableRaindropFx = 0u;
	}
	const float modernReflectionUi = SanitizeReflectionScale(modernWetIndirectSpecularScale, MAX_MODERN_WET_REFLECTION_UI_SCALE, DEFAULT_MODERN_WET_REFLECTION_UI);
	const float legacyReflectionUi = SanitizeReflectionScale(legacyWetIndirectSpecularScale, GetLegacyReflectionUiMax(), DEFAULT_LEGACY_WET_REFLECTION_UI);
	const float modernReflectionScale = ModernWetReflectionScaleFromUi(modernReflectionUi);
	const float legacyReflectionScale = LegacyWetReflectionScaleFromUi(legacyReflectionUi, GetLegacyReflectionUiMax());
	SyncActiveReflectionScale(data.settings, modernReflectionScale, legacyReflectionScale);
	const float clampedPuddleLayout = std::clamp(puddleLayout, PUDDLE_LAYOUT_MIN, PUDDLE_LAYOUT_MAX);
	// Shader CB carries puddle layout in its dedicated shader-facing field.
	// CPU simulation above still uses settings.WeatherTransitionSpeed.
	data.settings.PuddleLayout = clampedPuddleLayout;
	const float basePuddleRadiusGameUnits = ClampFiniteOrDefault(data.settings.PuddleRadius, PUDDLE_RADIUS_UI_MIN_GAME_UNITS, PUDDLE_RADIUS_CLAMP_MAX_GAME_UNITS, DEFAULT_PUDDLE_RADIUS_GAME_UNITS);
	const float rainExpandedPuddleRadiusGameUnits = std::max(basePuddleRadiusGameUnits, PUDDLE_RADIUS_UI_MAX_GAME_UNITS);
	float effectivePuddleRadiusGameUnits = basePuddleRadiusGameUnits;
	bool rainAutoExpansionPhaseActive = false;
	if (!inactivateRainPuddleAutoExpansion) {
		// Keep rain-expanded radius tied to visible rain intensity, not lingering weather metadata,
		// so post-rain radius controls become responsive immediately after rain visuals stop.
		const bool rainRadiusPhaseActive = (data.Raining > 0.01f);
		const bool hasPostRainPuddleSignal = (!rainRadiusPhaseActive) &&
			((data.PuddleWetness > RUNTIME_DRY_EPSILON) || (runtimeState.puddleDepth > RUNTIME_DRY_EPSILON));
		if (rainRadiusPhaseActive) {
			effectivePuddleRadiusGameUnits = rainExpandedPuddleRadiusGameUnits;
			rainAutoExpansionPhaseActive = true;
		} else if (hasPostRainPuddleSignal) {
			// Ease out quickly so the user-selected radius regains influence early in post-rain phase.
			const float settleLinearT = std::clamp(runtimeState.postRainElapsedSeconds / POST_RAIN_RADIUS_SETTLE_SECONDS, 0.0f, 1.0f);
			const float settleT = std::sqrt(settleLinearT);
			effectivePuddleRadiusGameUnits = std::lerp(rainExpandedPuddleRadiusGameUnits, basePuddleRadiusGameUnits, settleT);
		}
	}
	data.settings.PuddleRadius = std::clamp(effectivePuddleRadiusGameUnits, PUDDLE_RADIUS_UI_MIN_GAME_UNITS, PUDDLE_RADIUS_UI_MAX_GAME_UNITS);
	data.settings.StoneDryingMultiplier = effectiveDryingHours.stoneHours;
	data.settings.GrassDryingMultiplier = effectiveDryingHours.grassHours;
	data.settings.DirtDryingMultiplier = effectiveDryingHours.dirtHours;
	data.settings.MaxShoreWetness = data.settings.EnableWetnessEffects ? data.settings.MaxShoreWetness : 0.0f;
	const float clampedShorePersistentDarkeningStrength = ClampFiniteOrDefault(
		shorePersistentDarkeningStrength,
		SHORE_PERSISTENT_DARKENING_MIN,
		SHORE_PERSISTENT_DARKENING_MAX,
		SHORE_PERSISTENT_DARKENING_DEFAULT);
	const bool masterWetnessEnabled = settings.EnableWetnessEffects != 0;
	const float activeShorePersistentDarkeningStrength = masterWetnessEnabled ? clampedShorePersistentDarkeningStrength : 0.0f;
	const float activeRainReflectionBalance = masterWetnessEnabled ?
		ClampFiniteOrDefault(
			rainReflectionBalance,
			RAIN_REFLECTION_BALANCE_MIN,
			RAIN_REFLECTION_BALANCE_MAX,
			DEFAULT_RAIN_REFLECTION_BALANCE) :
		0.0f;
	const float activePostRainWaterClarity = masterWetnessEnabled ?
		ClampFiniteOrDefault(
			postRainWaterClarity,
			POST_RAIN_WATER_CLARITY_MIN,
			POST_RAIN_WATER_CLARITY_MAX,
			DEFAULT_POST_RAIN_WATER_CLARITY) :
		0.0f;
	const float postRainCubemapGlareReductionFromClarity = activePostRainWaterClarity;
	const float postRainSpecBoostFromClarity = activePostRainWaterClarity;
	const float inRainCubemapSuppressionFromBalance = activeRainReflectionBalance;
	const float inRainSpecularBoostFromBalance = activeRainReflectionBalance;
	// Pack auto-expansion flag + post-rain spec boost into one float for shader padding slot.
	// Fractional lane stores boost in [0, 0.999] so 1.0 remains reserved for the auto-expansion flag boundary.
	const float packedPostRainControl = postRainSpecBoostFromClarity * 0.999f + (rainAutoExpansionPhaseActive ? 1.0f : 0.0f);
	data.settings.ShorePersistentDarkeningStrength = activeShorePersistentDarkeningStrength;
	data.PackedPostRainControl = EncodeFloatToUint(packedPostRainControl);
	// Pack derived [0..1] controls into one uint lane using UNORM10 triplet:
	// bits  0.. 9 = post-rain cubemap glare reduction (derived from Post-Rain Water Clarity)
	// bits 10..19 = in-rain cubemap suppression (derived from Rain Reflection Balance)
	// bits 20..29 = in-rain specular boost      (derived from Rain Reflection Balance)
	data.PackedRainReflectionControl = PackThreeUnorm10(postRainCubemapGlareReductionFromClarity, inRainCubemapSuppressionFromBalance, inRainSpecularBoostFromBalance);
	data.RaindropVisibilityBoostPacked = EncodeFloatToUint(ClampRaindropVisibilityBoost(raindropVisibilityBoost));
	// Remaining wetness packed lanes carry range/bias test controls in game units.
	data.settings.PostRainPuddleWaterStrength = ClampFiniteOrDefault(
		data.settings.PostRainPuddleWaterStrength,
		POST_RAIN_PUDDLE_SHINE_MIN,
		POST_RAIN_PUDDLE_SHINE_MAX,
		DEFAULT_POST_RAIN_PUDDLE_SHINE);
	data.settings.RaindropFxRange = std::clamp(
		ClampFiniteOrDefault(
			data.settings.RaindropFxRange,
			RAINDROP_FX_RANGE_UI_MIN_GAME_UNITS,
			RAINDROP_FX_RANGE_CLAMP_MAX_GAME_UNITS,
			DEFAULT_RAINDROP_FX_RANGE_GAME_UNITS),
		RAINDROP_FX_RANGE_UI_MIN_GAME_UNITS,
		RAINDROP_FX_RANGE_UI_MAX_GAME_UNITS);
	data.WetnessDistanceFadeRangePacked = EncodeFloatToUint(ClampWetnessDistanceFadeRange(wetnessDistanceFadeRange));
	data.WetCubemapStabilityBiasStrengthPacked = EncodeFloatToUint(ClampFiniteOrDefault(
		sanitizedSettings.WetCubemapStabilityBiasStrength,
		WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MIN,
		WET_CUBEMAP_STABILITY_BIAS_STRENGTH_MAX,
		DEFAULT_WET_CUBEMAP_STABILITY_BIAS_STRENGTH));
	data.WetCubemapStabilityBiasRangePacked = EncodeFloatToUint(ClampWetCubemapStabilityBiasRange(sanitizedSettings.WetCubemapStabilityBiasRangeWorldUnits));
	data.settings.RaindropChance *= data.Raining * data.Raining;
	data.settings.RaindropChance = std::clamp(data.settings.RaindropChance, 0.0f, 1.0f);
	const float safeRaindropGridSize = std::max(data.settings.RaindropGridSize, MIN_RAINDROP_GRID_SIZE);
	const float safeRaindropInterval = std::max(data.settings.RaindropInterval, MIN_RAINDROP_INTERVAL);
	const float safeRippleLifetime = std::max(data.settings.RippleLifetime, MIN_RIPPLE_LIFETIME);
	data.settings.RaindropGridSize = 1.0f / safeRaindropGridSize;
	data.settings.RaindropInterval = 1.0f / safeRaindropInterval;
	data.settings.RippleLifetime = safeRaindropInterval / safeRippleLifetime;

	g_lastFrameData = data;
	g_hasLastFrameData = true;
	if (canUseFrameCache) {
		g_cachedCommonBufferData = data;
		g_hasCachedCommonBufferData = true;
		g_cachedCommonBufferFrame = frameIndex;
	}

	return data;
}

void WetnessEffects::Prepass()
{
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	if (!renderer || !context) {
		return;
	}

	if (g_hasLastFrameData && g_lastFrameData.settings.EnableWetnessEffects == 0u) {
		ID3D11ShaderResourceView* nullSrv = nullptr;
		context->PSSetShaderResources(kWetnessPsSrvPrecipOcclusionSlot, 1, &nullSrv);
		return;
	}

	ID3D11ShaderResourceView* precipOcclusionSrv = nullptr;
	auto& precipOcclusionTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
	if (precipOcclusionTexture.depthSRV) {
		precipOcclusionSrv = precipOcclusionTexture.depthSRV;
	}

	context->PSSetShaderResources(kWetnessPsSrvPrecipOcclusionSlot, 1, &precipOcclusionSrv);
}

void WetnessEffects::LoadSettings(json& o_json)
{
	const bool isObject = o_json.is_object();
	const bool hasExplicitWetnessSettings = JsonHasAnyNonDebugKey(o_json);
	settings = {};
	if (isObject) {
		try {
			settings = o_json.get<Settings>();
		} catch (const std::exception& e) {
			logger::warn("[{}] Failed to parse wetness settings object ({}); using defaults.", GetName(), e.what());
			settings = {};
		}
	} else {
		logger::warn("[{}] Wetness settings root is not an object; using defaults.", GetName());
	}
	puddleLayout = JsonValueOr<float>(o_json, "PuddleLayout", DEFAULT_PUDDLE_LAYOUT);
	puddleLayout = std::clamp(puddleLayout, PUDDLE_LAYOUT_MIN, PUDDLE_LAYOUT_MAX);
	rainReflectionBalance = JsonValueOr<float>(o_json, "RainReflectionBalance", DEFAULT_RAIN_REFLECTION_BALANCE);
	postRainWaterClarity = JsonValueOr<float>(o_json, "PostRainWaterClarity", DEFAULT_POST_RAIN_WATER_CLARITY);
	shorePersistentDarkeningStrength = JsonValueOr<float>(o_json, "ShorePersistentDarkeningStrength", SHORE_PERSISTENT_DARKENING_DEFAULT);
	wetnessDistanceFadeRange = JsonValueOr<float>(o_json, "WetnessFadeRange", DEFAULT_WETNESS_DISTANCE_FADE_RANGE_GAME_UNITS);
	raindropVisibilityBoost = JsonValueOr<float>(o_json, "RaindropVisibilityBoost", DEFAULT_RAINDROP_VISIBILITY_BOOST);
	modernWetIndirectSpecularScale = DEFAULT_MODERN_WET_REFLECTION_UI;
	legacyWetIndirectSpecularScale = DEFAULT_LEGACY_WET_REFLECTION_UI;

	if (!isObject || !o_json.contains("PostRainPuddleWaterStrength")) {
		settings.PostRainPuddleWaterStrength = DEFAULT_POST_RAIN_PUDDLE_SHINE;
	}

	const bool hasModernWetReflectionScale = isObject && o_json.contains("ModernWetIndirectSpecularScale");
	if (hasModernWetReflectionScale && o_json["ModernWetIndirectSpecularScale"].is_number()) {
		modernWetIndirectSpecularScale = o_json["ModernWetIndirectSpecularScale"].get<float>();
	}

	const bool hasLegacyWetReflectionScale = isObject && o_json.contains("LegacyWetIndirectSpecularScale");
	if (hasLegacyWetReflectionScale && o_json["LegacyWetIndirectSpecularScale"].is_number()) {
		legacyWetIndirectSpecularScale = o_json["LegacyWetIndirectSpecularScale"].get<float>();
	}

	puddleDryingHours = JsonValueOr<float>(o_json, "PuddleDryingHours", DEFAULT_PUDDLE_DRYING_HOURS);
	puddleDryingHours = ClampDryingHours(puddleDryingHours, DEFAULT_PUDDLE_DRYING_HOURS);
	enableWeatherDrivenDryingModel =
		(isObject && o_json.contains("EnableWeatherDrivenDryingModel")) ?
			JsonValueToBool(o_json["EnableWeatherDrivenDryingModel"], true) :
			true;
	inactivateRainPuddleAutoExpansion =
		(isObject && o_json.contains("InactivateRainPuddleAutoExpansion")) ?
			JsonValueToBool(o_json["InactivateRainPuddleAutoExpansion"], false) :
			false;

	if (!hasExplicitWetnessSettings) {
		ApplyDefaultWetnessUiPreset(settings, wetnessDistanceFadeRange);
		climatePreset = defaultPreset;
		ApplyClimatePreset(climatePreset);
	}

	SanitizeToggleSettings(settings);
	SanitizePersistentUiState(settings, modernWetIndirectSpecularScale, legacyWetIndirectSpecularScale, puddleDryingHours, puddleLayout, rainReflectionBalance, postRainWaterClarity, shorePersistentDarkeningStrength, wetnessDistanceFadeRange, raindropVisibilityBoost);
	SanitizeShaderFacingSettings(settings);
	InvalidateSanitizedSettingsCache();
	ResetRuntimeState();

	// Auto-detect which preset matches the loaded settings
	DetectCurrentPreset();

	debugSettings = {};
	if (isObject && o_json.contains("DebugSettings")) {
		try {
			debugSettings = o_json["DebugSettings"].get<DebugSettings>();
		} catch (const std::exception& e) {
			logger::warn("[{}] Failed to parse wetness debug settings ({}); using defaults.", GetName(), e.what());
			debugSettings = {};
		}
	}
}

void WetnessEffects::SaveSettings(json& o_json)
{
	SanitizePersistentUiState(settings, modernWetIndirectSpecularScale, legacyWetIndirectSpecularScale, puddleDryingHours, puddleLayout, rainReflectionBalance, postRainWaterClarity, shorePersistentDarkeningStrength, wetnessDistanceFadeRange, raindropVisibilityBoost);
	SanitizeToggleSettings(settings);
	SanitizeShaderFacingSettings(settings);
	InvalidateSanitizedSettingsCache();
	o_json = settings;
	o_json["ModernWetIndirectSpecularScale"] = modernWetIndirectSpecularScale;
	o_json["LegacyWetIndirectSpecularScale"] = legacyWetIndirectSpecularScale;
	o_json["PuddleDryingHours"] = puddleDryingHours;
	o_json["PuddleLayout"] = puddleLayout;
	o_json["RainReflectionBalance"] = rainReflectionBalance;
	o_json["PostRainWaterClarity"] = postRainWaterClarity;
	o_json["ShorePersistentDarkeningStrength"] = shorePersistentDarkeningStrength;
	o_json["WetnessFadeRange"] = wetnessDistanceFadeRange;
	o_json["RaindropVisibilityBoost"] = raindropVisibilityBoost;
	o_json["EnableWeatherDrivenDryingModel"] = enableWeatherDrivenDryingModel;
	o_json["InactivateRainPuddleAutoExpansion"] = inactivateRainPuddleAutoExpansion;

	o_json["DebugSettings"] = debugSettings;
}

void WetnessEffects::RestoreDefaultSettings()
{
	settings = {};
	enableWeatherDrivenDryingModel = true;
	inactivateRainPuddleAutoExpansion = false;
	puddleDryingHours = DEFAULT_PUDDLE_DRYING_HOURS;
	puddleLayout = DEFAULT_PUDDLE_LAYOUT;
	rainReflectionBalance = DEFAULT_RAIN_REFLECTION_BALANCE;
	postRainWaterClarity = DEFAULT_POST_RAIN_WATER_CLARITY;
	shorePersistentDarkeningStrength = SHORE_PERSISTENT_DARKENING_DEFAULT;
	wetnessDistanceFadeRange = DEFAULT_WETNESS_DISTANCE_FADE_RANGE_GAME_UNITS;
	raindropVisibilityBoost = DEFAULT_RAINDROP_VISIBILITY_BOOST;
	modernWetIndirectSpecularScale = DEFAULT_MODERN_WET_REFLECTION_UI;
	legacyWetIndirectSpecularScale = DEFAULT_LEGACY_WET_REFLECTION_UI;
	climatePreset = defaultPreset;
	ApplyDefaultWetnessUiPreset(settings, wetnessDistanceFadeRange);
	ApplyClimatePreset(climatePreset);
	ResetRuntimeState();
	SanitizePersistentUiState(settings, modernWetIndirectSpecularScale, legacyWetIndirectSpecularScale, puddleDryingHours, puddleLayout, rainReflectionBalance, postRainWaterClarity, shorePersistentDarkeningStrength, wetnessDistanceFadeRange, raindropVisibilityBoost);
	SanitizeShaderFacingSettings(settings);
	InvalidateSanitizedSettingsCache();
	DetectCurrentPreset();

}

void WetnessEffects::DrawWeatherAnalysis() const
{
	// Only show rain system analysis when it's raining and wetness effects are enabled
	if (!settings.EnableWetnessEffects)
		return;

	auto sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || !sky->IsRaining())
		return;

	// Get the current frame data (reuses already calculated values)
	if (!g_hasLastFrameData) {
		return;
	}
	const auto frameData = g_lastFrameData;

	// Get weather particle density for precipitation calculations
	float weatherMaxParticleDensity = 0.0f;
	if (sky->currentWeather && sky->currentWeather->precipitationData) {
		weatherMaxParticleDensity = sky->currentWeather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
	}

	// Check last weather if transitioning
	if (weatherMaxParticleDensity <= 0.0f && sky->lastWeather && sky->lastWeather->precipitationData) {
		weatherMaxParticleDensity = sky->lastWeather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
	}
	// // Consolidated Shader & Weather Analysis
	static bool rainAnalysisExpanded = true;
	Util::DrawSectionHeader("Rain Analysis", false, true, &rainAnalysisExpanded);

	if (rainAnalysisExpanded) {
		// Climate Preset Information Section
		auto climateSection = Util::SectionWrapper("Current Climate Preset");
		if (climateSection) {
			const auto& presetInfo = CLIMATE_PRESET_INFO[static_cast<size_t>(climatePreset)];

			ImGui::Text("Active Preset: %s", presetInfo.name);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", presetInfo.shortDescription);
			}

			ImGui::Text("Precipitation Rate Calculation");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				Util::DrawMultiLineTooltip({ "Precipitation rates are calculated using shader mechanics:",
					"- Raindrop chance (probability per interval)",
					"- Grid size (spatial density)",
					"- Interval (time between attempts)",
					"- All values reflect what is sent to the shader.",
					"Rates are shown in mm/hr, based on drops/sec and grid size." });
			}

			// Show current preset-applied values vs defaults
			Settings defaultSettings{};
			ImGui::TextUnformatted(climatePreset == ClimatePreset::Custom ? "Current Settings:" : "Current Settings (applied from preset):");
			ImGui::Indent();
			ImGui::Text("Rain Wetness: %.2f (default %.2f × %.1fx)", settings.MaxRainWetness, defaultSettings.MaxRainWetness, presetInfo.settings.wetnessMultiplier);
			ImGui::Text("Puddle Wetness: %.2f (default %.2f × %.1fx)", settings.MaxPuddleWetness, defaultSettings.MaxPuddleWetness, presetInfo.settings.puddleMultiplier);
			ImGui::Text("Transition Speed: %.2f (default %.2f × %.1fx)", settings.WeatherTransitionSpeed, defaultSettings.WeatherTransitionSpeed, presetInfo.settings.transitionSpeed);
			ImGui::Text("Raindrop Chance: %.1f%% (preset value)", settings.RaindropChance * 100.0f);
			ImGui::Unindent();
		}
		auto section = Util::SectionWrapper("Rain System State");
		if (section && sky->currentWeather) {
			float gridSizeGameUnits = 1.0f / frameData.settings.RaindropGridSize;
			float gridSizeMeters = Util::Units::GameUnitsToMeters(gridSizeGameUnits);
			float intervalSeconds = 1.0f / frameData.settings.RaindropInterval;
			float weatherBasedRainRate = CalculatePrecipitationRate(frameData.settings.RaindropChance, gridSizeGameUnits, intervalSeconds);
			float actualRainRate = weatherBasedRainRate;

			// Theoretical max using preset values and intensity = 1.0
			const bool customClimateSettings = climatePreset == ClimatePreset::Custom;
			const auto& presetSettings = GetClimateSettings(customClimateSettings ? defaultPreset : climatePreset);
			const float maxRateChance = customClimateSettings ? settings.RaindropChance : presetSettings.raindropChance;
			const float maxRateGridSize = customClimateSettings ? settings.RaindropGridSize : presetSettings.raindropGridSize;
			const float maxRateInterval = customClimateSettings ? settings.RaindropInterval : presetSettings.raindropInterval;
			float theoreticalMaxRainRate = CalculatePrecipitationRate(
				maxRateChance, maxRateGridSize, maxRateInterval);

			if (ImGui::BeginTable("RainAnalysis", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersV)) {
				ImGui::TableSetupColumn("Current Shader State", ImGuiTableColumnFlags_WidthStretch, 0.5f);
				ImGui::TableSetupColumn("Precipitation Analysis", ImGuiTableColumnFlags_WidthStretch, 0.5f);
				ImGui::TableHeadersRow();

				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				Util::DrawColorCodedValue("Rain Intensity", frameData.Raining * 100.0f, std::format("{:.1f}%", frameData.Raining * 100.0f), Util::ColorCodedValueConfig::HighIsGood(10.0f, 50.0f, 80.0f));
				Util::DrawColorCodedValue("Wetness", frameData.Wetness * 100.0f, std::format("{:.1f}%", frameData.Wetness * 100.0f), Util::ColorCodedValueConfig::HighIsGood(25.0f, 60.0f, 85.0f));
				Util::DrawColorCodedValue("Puddle Wetness", frameData.PuddleWetness * 100.0f, std::format("{:.1f}%", frameData.PuddleWetness * 100.0f), Util::ColorCodedValueConfig::HighIsGood(15.0f, 40.0f, 70.0f));
				ImGui::Text("Puddle Formation: %.1f%% min wetness", frameData.settings.PuddleMinWetness * 100.0f);
				ImGui::Text("Rain Event Memory: %.1f%%", runtimeState.rainEventWeight * 100.0f);
				ImGui::Text("Weather Transition: %.1f%%", sky->currentWeatherPct * 100.0f);
				ImGui::Text("Raindrop Chance: %.1f%%", frameData.settings.RaindropChance * 100.0f);
				ImGui::Text("Grid Size: %.2f m (%.1f units)", gridSizeMeters, gridSizeGameUnits);
				ImGui::Text("Interval: %.1f sec", intervalSeconds);

				ImGui::TableNextColumn();
				// Live (Current):
				DrawRainTypeLabel("Current", actualRainRate);
				// Max (in Heavy Rain):
				DrawRainTypeLabel("Max (in Heavy Rain)", theoreticalMaxRainRate);
				ImGui::EndTable();
			}
		}
	}
}

// Helper function to auto-detect which preset matches current settings
void WetnessEffects::DetectCurrentPreset()
{
	if (DoesCurrentSettingsMatchPreset(ClimatePreset::Legacy)) {
		climatePreset = ClimatePreset::Legacy;
	} else if (DoesCurrentSettingsMatchPreset(ClimatePreset::NordicStandard)) {
		climatePreset = ClimatePreset::NordicStandard;
	} else if (DoesCurrentSettingsMatchPreset(ClimatePreset::ArcticTundra)) {
		climatePreset = ClimatePreset::ArcticTundra;
	} else if (DoesCurrentSettingsMatchPreset(ClimatePreset::TemperateCoastal)) {
		climatePreset = ClimatePreset::TemperateCoastal;
	} else if (DoesCurrentSettingsMatchPreset(ClimatePreset::MonsoonExtreme)) {
		climatePreset = ClimatePreset::MonsoonExtreme;
	} else {
		climatePreset = ClimatePreset::Custom;
	}
}

bool WetnessEffects::DoesCurrentSettingsMatchPreset(ClimatePreset preset) const
{
	// Custom preset never matches (it means user has customized settings)
	if (preset == ClimatePreset::Custom) {
		return false;
	}

	const auto& climate = GetClimateSettings(preset);
	Settings defaultSettings{};  // Get default values

	// Calculate what the settings should be for this preset
	float expectedMaxRainWetness = defaultSettings.MaxRainWetness * climate.wetnessMultiplier;
	float expectedMaxPuddleWetness = defaultSettings.MaxPuddleWetness * climate.puddleMultiplier;
	float expectedWeatherTransitionSpeed = defaultSettings.WeatherTransitionSpeed * climate.transitionSpeed;
	float expectedRaindropChance = climate.raindropChance;
	float expectedRaindropGridSize = climate.raindropGridSize;
	float expectedRaindropInterval = climate.raindropInterval;

	const float tolerance = 0.001f;
	return (std::abs(settings.MaxRainWetness - expectedMaxRainWetness) < tolerance &&
			std::abs(settings.MaxPuddleWetness - expectedMaxPuddleWetness) < tolerance &&
			std::abs(settings.WeatherTransitionSpeed - expectedWeatherTransitionSpeed) < tolerance &&
			std::abs(settings.RaindropChance - expectedRaindropChance) < tolerance &&
			std::abs(settings.RaindropGridSize - expectedRaindropGridSize) < tolerance &&
			std::abs(settings.RaindropInterval - expectedRaindropInterval) < tolerance);
}
