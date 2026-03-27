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
		float MaxRainWetness = 1.0f;
		float MaxPuddleWetness = 2.5f;
		float MaxShoreWetness = 1.0f;
		uint ShoreRange = 32;
		float PuddleRadius = 1.0f;
		// Runoff Width tuning (keeps original CB slot/order).
		float RunoffWidth = 1.0f;
		float PuddleMaxAngle = 0.95f;
		float PuddleMinWetness = 0.85f;
		float MinRainWetness = 0.65f;
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
		uint EnableVanillaRipples = false;
		uint EnableLegacyRainBehavior = false;
		uint EnableModernWetReflection = true;
		uint EnableLegacyWetReflection = false;
		float WetIndirectSpecularScale = 0.2f;
		float RaindropFxRange = 1000.f;
		float RaindropGridSize = 4.f;
		float RaindropInterval = 1.0f;
		float RaindropChance = 1.0f;
		float SplashesLifetime = 10.0f;
		float SplashesStrength = 1.05f;
		float SplashesMinRadius = .3f;
		float SplashesMaxRadius = .5f;
		float RippleStrength = 1.f;
		float RippleRadius = 1.f;
		float RippleBreadth = .5f;
		float RippleLifetime = .5f;

		// Dev tuning controls (temporary while calibrating wetness behavior in-game)
		float PostRainPuddleWaterStrength = 0.8f;
		// Runoff Strength tuning (reuses legacy field slot to keep CB layout stable).
		float CloseRangeWetnessBoost = 0.0f;
		float RaindropTransitionFalloff = 2.0f;
		uint EnableDualPuddleModel = true;
		float PuddleDepthBlend = 0.5f;
		float WetDarkeningStrength = 1.0f;
		float WetColorSaturation = 1.0f;
		float WetHighlightReduction = 1.0f;
		// Runoff Speed tuning (keeps original CB slot/order).
		float RunoffSpeed = 1.0f;
		// Optional compatibility profile and tuning controls (opt-in).
		uint EnableHostilesWetProfile = false;
		uint EnableMarch3WetnessProfile = false;
		uint EnableExtendedLegacyReflectionRange = false;
		uint EnableForwardReflectionBias = false;
		uint EnableVanillaReflectionCompensation = false;
		float PuddlePatternDominance = 1.0f;
		uint EnablePuddleInfluenceDebugReadout = false;
		uint EnableLodSafeWetDarkening = false;
	};
	static_assert(sizeof(Settings) == 208, "WetnessEffects::Settings layout changed; update wetness shader/CB contract.");
	static_assert(offsetof(Settings, WeatherTransitionSpeed) == 44, "WetnessEffects::Settings WeatherTransitionSpeed offset changed.");
	static_assert(offsetof(Settings, EnableRaindropFx) == 60, "WetnessEffects::Settings EnableRaindropFx offset changed.");
	static_assert(offsetof(Settings, WetIndirectSpecularScale) == 88, "WetnessEffects::Settings WetIndirectSpecularScale offset changed.");
	static_assert(offsetof(Settings, RaindropGridSize) == 96, "WetnessEffects::Settings RaindropGridSize offset changed.");
	static_assert(offsetof(Settings, RippleLifetime) == 136, "WetnessEffects::Settings RippleLifetime offset changed.");
	static_assert(offsetof(Settings, EnableDualPuddleModel) == 152, "WetnessEffects::Settings EnableDualPuddleModel offset changed.");
	static_assert(offsetof(Settings, RunoffSpeed) == 172, "WetnessEffects::Settings RunoffSpeed offset changed.");
	static_assert(offsetof(Settings, EnableHostilesWetProfile) == 176, "WetnessEffects::Settings EnableHostilesWetProfile offset changed.");

	// Shader-facing wetness settings layout.
	// Keep this binary-compatible with Settings while exposing shader semantics directly.
	struct ShaderSettings
	{
		uint EnableWetnessEffects = true;
		float MaxRainWetness = 1.0f;
		float MaxPuddleWetness = 2.5f;
		float MaxShoreWetness = 1.0f;
		uint ShoreRange = 32;
		float PuddleRadius = 1.0f;
		float RunoffWidth = 1.0f;
		float PuddleMaxAngle = 0.95f;
		float PuddleMinWetness = 0.85f;
		float MinRainWetness = 0.65f;
		float SkinWetness = 0.95f;
		float PuddleLayout = 2.0f;
		float StoneDryingMultiplier = 6.0f;
		float DirtDryingMultiplier = 12.0f;
		float GrassDryingMultiplier = 3.0f;

		uint EnableRaindropFx = true;
		uint EnableSplashes = true;
		uint EnableRipples = true;
		uint EnableVanillaRipples = false;
		uint EnableLegacyRainBehavior = false;
		uint EnableModernWetReflection = true;
		uint EnableLegacyWetReflection = false;
		float WetIndirectSpecularScale = 0.2f;
		float RaindropFxRange = 1000.f;
		float RaindropGridSize = 4.f;
		float RaindropInterval = 1.0f;
		float RaindropChance = 1.0f;
		float SplashesLifetime = 10.0f;
		float SplashesStrength = 1.05f;
		float SplashesMinRadius = .3f;
		float SplashesMaxRadius = .5f;
		float RippleStrength = 1.f;
		float RippleRadius = 1.f;
		float RippleBreadth = .5f;
		float RippleLifetime = .5f;

		float PostRainPuddleWaterStrength = 0.8f;
		float CloseRangeWetnessBoost = 0.0f;
		float RaindropTransitionFalloff = 2.0f;
		uint EnableDualPuddleModel = true;
		float PuddleDepthBlend = 0.5f;
		float WetDarkeningStrength = 1.0f;
		float WetColorSaturation = 1.0f;
		float WetHighlightReduction = 1.0f;
		float RunoffSpeed = 1.0f;
		uint EnableHostilesWetProfile = false;
		uint EnableMarch3WetnessProfile = false;
		uint EnableExtendedLegacyReflectionRange = false;
		uint EnableForwardReflectionBias = false;
		uint EnableVanillaReflectionCompensation = false;
		float PuddlePatternDominance = 1.0f;
		uint EnablePuddleInfluenceDebugReadout = false;
		uint EnableLodSafeWetDarkening = false;
	};
	static_assert(sizeof(ShaderSettings) == sizeof(Settings), "WetnessEffects::ShaderSettings must stay binary-compatible with Settings.");
	static_assert(offsetof(ShaderSettings, PuddleLayout) == offsetof(Settings, WeatherTransitionSpeed),
		"WetnessEffects::ShaderSettings::PuddleLayout must match Settings::WeatherTransitionSpeed offset.");
	static_assert(offsetof(ShaderSettings, EnableRaindropFx) == offsetof(Settings, EnableRaindropFx),
		"WetnessEffects::ShaderSettings toggle offsets must match Settings.");
	static_assert(offsetof(ShaderSettings, WetIndirectSpecularScale) == offsetof(Settings, WetIndirectSpecularScale),
		"WetnessEffects::ShaderSettings reflection offsets must match Settings.");

	struct alignas(16) PerFrame
	{
		REX::W32::XMFLOAT4X4 OcclusionViewProj;
		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;
		ShaderSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(offsetof(PerFrame, settings) == 80, "WetnessEffects::PerFrame settings offset changed.");
	static_assert(sizeof(PerFrame) == 288, "WetnessEffects::PerFrame size changed; update wetness shader/CB contract.");
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
	float puddleDryingHours = 18.0f;
	float puddleLayout = 2.0f;
	float modernWetIndirectSpecularScale = 0.2f;
	float legacyWetIndirectSpecularScale = 0.0145f;
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

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

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

	bool splashesOfStormsLoaded = false;

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
