#pragma once

#include "Buffer.h"
#include <cstddef>

struct WetnessEffects : Feature
{
private:
	static constexpr std::string_view MOD_ID = "112739";

public:
	virtual inline std::string GetName() override { return "Wetness Effects"; }
	virtual inline std::string GetShortName() override { return "WetnessEffects"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "WETNESS_EFFECTS"; }
	virtual std::string_view GetCategory() const override { return "Water"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Adds realistic wetness effects including rain-based surface wetness, puddle formation, shore wetness, and dynamic raindrop effects for enhanced weather immersion.",
			{ "Dynamic surface wetness based on weather conditions",
				"Realistic puddle formation and shore wetness effects",
				"Animated raindrop effects with splashes and ripples",
				"Configurable wetness intensity and weather transitions",
				"Support for skin wetness and material-specific responses" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	struct Settings
	{
		uint EnableWetnessEffects = true;
		float MaxRainWetness = 1.05f;
		float MaxPuddleWetness = 2.5f;
		float MaxShoreWetness = 0.75f;
		uint ShoreRange = 32;
		// User/persisted value in game units.
		float PuddleRadiusWorldUnits = 119.05f;
		float PuddleMaxAngle = 0.75f;
		float PuddleMinWetness = 0.525f;
		float MinRainWetness = 0.60f;
		float SkinWetness = 0.95f;
		float WeatherTransitionSpeed = 3.0f;
		// Surface drying-time controls in hours (1..24).
		float StoneDryingMultiplier = 6.0f;
		float DirtDryingMultiplier = 12.0f;
		float GrassDryingMultiplier = 3.0f;

		// Raindrop fx settings
		uint EnableRaindropFx = true;
		uint EnableSplashes = true;
		uint EnableRipples = true;
		uint EnableModernWetReflection = true;
		uint EnableLegacyWetReflection = false;
		// Derived runtime shader scale (from modern/legacy UI reflection sliders), not a persisted UI control.
		float WetIndirectSpecularScale = 0.8f;
		// User/persisted value in game units.
		float RaindropFxRangeWorldUnits = 2000.0f;
		float RaindropGridSize = 3.f;
		float RaindropInterval = 0.5f;
		float RaindropChance = 0.8f;
		float SplashesLifetime = 6.0f;
		float SplashesStrength = 1.2f;
		float SplashesMinRadius = .35f;
		float SplashesMaxRadius = .5f;
		float RippleStrength = 2.0f;
		float RippleRadius = .6f;
		float RippleBreadth = .40f;
		float RippleLifetime = .30f;

		// Wetness tuning controls.
		float PostRainPuddleWaterStrength = 2.5f;
		float RaindropTransitionFalloff = 2.0f;
		float WetDarkeningStrength = 0.85f;
		float WetHighlightReduction = 5.0f;
		uint EnableForwardReflectionBias = false;
		uint EnableVanillaReflectionCompensation = true;
		float WetFilmSpecularFloorScale = 1.0f;
	};
	static_assert(sizeof(Settings) == 156, "WetnessEffects::Settings layout changed; update wetness shader/CB contract.");
	static_assert(offsetof(Settings, WeatherTransitionSpeed) == 40, "WetnessEffects::Settings WeatherTransitionSpeed offset changed.");
	static_assert(offsetof(Settings, EnableRaindropFx) == 56, "WetnessEffects::Settings EnableRaindropFx offset changed.");
	static_assert(offsetof(Settings, WetIndirectSpecularScale) == 76, "WetnessEffects::Settings WetIndirectSpecularScale offset changed.");
	static_assert(offsetof(Settings, RaindropGridSize) == 84, "WetnessEffects::Settings RaindropGridSize offset changed.");
	static_assert(offsetof(Settings, RippleLifetime) == 124, "WetnessEffects::Settings RippleLifetime offset changed.");
	static_assert(offsetof(Settings, WetHighlightReduction) == 140, "WetnessEffects::Settings WetHighlightReduction offset changed.");
	static_assert(offsetof(Settings, EnableForwardReflectionBias) == 144, "WetnessEffects::Settings EnableForwardReflectionBias offset changed.");
	static_assert(offsetof(Settings, EnableVanillaReflectionCompensation) == 148, "WetnessEffects::Settings EnableVanillaReflectionCompensation offset changed.");
	static_assert(offsetof(Settings, WetFilmSpecularFloorScale) == 152, "WetnessEffects::Settings WetFilmSpecularFloorScale offset changed.");

	// Shader-facing wetness settings layout.
	// Keep this binary-compatible with Settings while exposing shader semantics directly.
	struct ShaderSettings
	{
		uint EnableWetnessEffects = true;
		float MaxRainWetness = 1.05f;
		float MaxPuddleWetness = 2.5f;
		float MaxShoreWetness = 0.75f;
		uint ShoreRange = 32;
		// Shader/runtime value in game units.
		float PuddleRadius = 119.05f;
		float PuddleMaxAngle = 0.75f;
		float PuddleMinWetness = 0.525f;
		float MinRainWetness = 0.60f;
		float SkinWetness = 0.95f;
		float PuddleLayout = 3.0f;
		float StoneDryingMultiplier = 6.0f;
		float DirtDryingMultiplier = 12.0f;
		float GrassDryingMultiplier = 3.0f;

		uint EnableRaindropFx = true;
		uint EnableSplashes = true;
		uint EnableRipples = true;
		uint EnableModernWetReflection = true;
		uint EnableLegacyWetReflection = false;
		float WetIndirectSpecularScale = 0.8f;
		// Shader/runtime value in game units.
		float RaindropFxRange = 2000.0f;
		float RaindropGridSize = 3.f;
		float RaindropInterval = 0.5f;
		float RaindropChance = 0.8f;
		float SplashesLifetime = 6.0f;
		float SplashesStrength = 1.2f;
		float SplashesMinRadius = .35f;
		float SplashesMaxRadius = .5f;
		float RippleStrength = 2.0f;
		float RippleRadius = .6f;
		float RippleBreadth = .40f;
		float RippleLifetime = .30f;

		float PostRainPuddleWaterStrength = 2.5f;
		float RaindropTransitionFalloff = 2.0f;
		float WetDarkeningStrength = 0.85f;
		float WetHighlightReduction = 5.0f;
		uint EnableForwardReflectionBias = false;
		uint EnableVanillaReflectionCompensation = true;
		float WetFilmSpecularFloorScale = 1.0f;
	};
	static_assert(sizeof(ShaderSettings) == sizeof(Settings), "WetnessEffects::ShaderSettings must stay binary-compatible with Settings.");
	static_assert(offsetof(ShaderSettings, PuddleLayout) == offsetof(Settings, WeatherTransitionSpeed),
		"WetnessEffects::ShaderSettings::PuddleLayout must match Settings::WeatherTransitionSpeed offset.");
	static_assert(offsetof(ShaderSettings, EnableRaindropFx) == offsetof(Settings, EnableRaindropFx),
		"WetnessEffects::ShaderSettings toggle offsets must match Settings.");
	static_assert(offsetof(ShaderSettings, WetIndirectSpecularScale) == offsetof(Settings, WetIndirectSpecularScale),
		"WetnessEffects::ShaderSettings reflection offsets must match Settings.");
	static_assert(offsetof(ShaderSettings, EnableForwardReflectionBias) == offsetof(Settings, EnableForwardReflectionBias),
		"WetnessEffects::ShaderSettings forward reflection bias offsets must match Settings.");
	static_assert(offsetof(ShaderSettings, EnableVanillaReflectionCompensation) == offsetof(Settings, EnableVanillaReflectionCompensation),
		"WetnessEffects::ShaderSettings vanilla reflection compensation offsets must match Settings.");
	static_assert(offsetof(ShaderSettings, WetFilmSpecularFloorScale) == offsetof(Settings, WetFilmSpecularFloorScale),
		"WetnessEffects::ShaderSettings wet-film specular floor offset must match Settings.");

	struct PerFrame
	{
		REX::W32::XMFLOAT4X4 OcclusionViewProj;
		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;
		ShaderSettings settings;
		// Explicit tail padding so the packed feature buffer and shader CB stay 16-byte aligned.
		uint ReservedPerFramePadding0 = 0;
		uint ReservedPerFramePadding1 = 0;
		uint ReservedPerFramePadding2 = 0;
		uint ReservedPerFramePadding3 = 0;
		uint ReservedPerFramePadding4 = 0;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(offsetof(PerFrame, settings) == 80, "WetnessEffects::PerFrame settings offset changed.");
	static_assert(sizeof(PerFrame) == 256, "WetnessEffects::PerFrame size changed; update wetness shader/CB contract.");
	static_assert((sizeof(PerFrame) % 16) == 0, "WetnessEffects::PerFrame must stay 16-byte sized");

	struct DebugSettings
	{
		bool EnableWetnessOverride = false;
		bool EnablePuddleOverride = false;
		bool EnableRainOverride = false;
		bool EnableIntExOverride = false;
		float2 WetnessOverride = float2(0.0f, 0.0f);
		float2 PuddleWetnessOverride = float2(0.0f, 0.0f);
		float2 RainOverride = float2(0.0f, 0.0f);
	} debugSettings;

	Settings settings;
	bool enableWeatherDrivenDryingModel = true;
	bool inactivateRainPuddleAutoExpansion = false;
	float puddleDryingHours = 18.0f;
	float puddleLayout = 3.0f;
	float modernWetIndirectSpecularScale = 0.80f;
	float legacyWetIndirectSpecularScale = 0.40f;
	float rainReflectionBalance = 0.2f;
	float postRainWaterClarity = 0.8f;
	float shorePersistentDarkeningStrength = 1.0f;
	float wetnessDistanceFadeRange = 10240.0f;
	bool preventPuddlesOnGrass = false;
	bool enableMaterialWetShineScaling = false;
	// Climate preset system
	enum class ClimatePreset : uint32_t
	{
		Custom = 0,
		Legacy = 1,
		NordicStandard = 2,
		ArcticTundra = 3,
		TemperateCoastal = 4,
		MonsoonExtreme = 5
	};
	struct ClimateSettings
	{
		float wetnessMultiplier;
		float puddleMultiplier;
		float transitionSpeed;
		float raindropChance;
		float raindropGridSize;
		float raindropInterval;
	};
	static constexpr ClimatePreset defaultPreset = ClimatePreset::NordicStandard;
	ClimatePreset climatePreset = defaultPreset;

	PerFrame GetCommonBufferData() const;

	virtual void SetupResources() override;
	virtual void Prepass() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	// Override to provide weather analysis configuration
	virtual WeatherAnalysisConfig GetWeatherAnalysisConfig() const override
	{
		return WeatherAnalysisConfig("Rain & Wetness Analysis", [this]() {
			this->DrawWeatherAnalysis();
		});
	}

	// Constants and utilities for rain intensity calculations
	static constexpr float MAX_RAIN_PARTICLE_DENSITY = 3.0f;

	float CalculatePrecipitationRate(float raindropChance, float raindropGridSizeGameUnits, float raindropIntervalSeconds, float mlPerDrop = 0.01f) const;
	static const ClimateSettings& GetClimateSettings(ClimatePreset preset);
	void ApplyClimatePreset(ClimatePreset preset);
	bool DoesCurrentSettingsMatchPreset(ClimatePreset preset) const;
	void DetectCurrentPreset();

private:
	void DrawWeatherAnalysis() const;
	void ResetRuntimeState();
	void InvalidateSanitizedSettingsCache();
	const Settings& GetSanitizedSettings() const;

	struct RuntimeState
	{
		float wetnessDepth = 0.0f;
		float puddleDepth = 0.0f;
		float rainEventExposure = 0.0f;
		float rainEventWeight = 0.0f;
		float postRainEventWeight = 0.0f;
		float postRainElapsedSeconds = 0.0f;
		float postRainStartWetnessDepth = 0.0f;
		float postRainStartPuddleDepth = 0.0f;
		double lastGameTimeSeconds = 0.0;
		bool hasLastGameTime = false;
		bool wasRainingLastFrame = false;
	};
	mutable RuntimeState runtimeState{};
	mutable Settings sanitizedSettingsCache{};
	mutable bool sanitizedSettingsCacheValid = false;

};
