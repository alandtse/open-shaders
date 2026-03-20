#include "VolumetricLighting.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "RE/B/BSSkyShaderProperty.h"
#include "RE/N/NiDirectionalLight.h"
#include "RE/S/Sun.h"
#include "InteriorSun.h"
#include "ShaderCache.h"
#include "State.h"

namespace
{
	constexpr float kWeatherTransitionEpsilon = 0.001f;
	constexpr float kGodrayIntensityMax = 3.0f;
	constexpr float kGodrayShaftIntensityMax = 3.0f;
	constexpr float kGodrayOpacityMax = 2.0f;
	constexpr float kGodraySaturationMax = 4.0f;
	constexpr float kColorLumaR = 0.2126f;
	constexpr float kColorLumaG = 0.7152f;
	constexpr float kColorLumaB = 0.0722f;
	constexpr float kDefaultGodrayIntensity = 1.0f;
	constexpr float kDefaultGodrayShaftIntensity = 1.0f;
	constexpr float kDefaultGodrayOpacity = 1.0f;
	constexpr float kDefaultGodraySaturation = 1.0f;
	constexpr float kDefaultCustomContribution = 0.0f;
	constexpr float kFloatEpsilon = 1e-4f;
	constexpr float kSunGlareOcclusionCutoff = 0.01f;

	struct GodrayRuntimeParams
	{
		float shaftIntensity = 1.0f;
		float opacity = 1.0f;
		float saturation = 1.0f;
		float customContribution = 0.0f;
		RE::NiColor customColor = { 1.0f, 1.0f, 1.0f };
	};

	struct WeatherSunGlareState
	{
		std::uint8_t baseline = 0;
		std::uint8_t lastApplied = 0;
		bool initialized = false;
	};

	float ClampFinite(float value, float minValue, float maxValue, float fallback)
	{
		if (!std::isfinite(value))
			value = fallback;
		return std::clamp(value, minValue, maxValue);
	}

	bool IsNear(float value, float target, float epsilon = kFloatEpsilon)
	{
		return std::abs(value - target) <= epsilon;
	}

	bool IsImageSpaceReplacementEnabled()
	{
		auto* state = globals::state;
		if (!state)
			return true;

		const int classCount = static_cast<int>(sizeof(state->enabledClasses) / sizeof(state->enabledClasses[0]));
		const int imageSpaceClassIndex = static_cast<int>(RE::BSShader::Type::ImageSpace) - 1;
		if (imageSpaceClassIndex >= 0 && imageSpaceClassIndex < classCount && !state->enabledClasses[imageSpaceClassIndex]) {
			return false;
		}

		return state->enablePShaders;
	}

	std::unordered_map<RE::TESWeather*, WeatherSunGlareState>& GetSunGlareStateMap()
	{
		static std::unordered_map<RE::TESWeather*, WeatherSunGlareState> states{};
		return states;
	}

	void ApplyScaledSunGlareToWeather(RE::TESWeather* weather, float glareScale)
	{
		if (!weather)
			return;

		auto& states = GetSunGlareStateMap();
		auto [it, inserted] = states.try_emplace(weather);
		WeatherSunGlareState& state = it->second;
		if (inserted || !state.initialized) {
			state.baseline = weather->data.sunGlare;
			state.lastApplied = weather->data.sunGlare;
			state.initialized = true;
		}

		// Track live game/weather edits unless this is the value we wrote.
		if (weather->data.sunGlare != state.lastApplied) {
			state.baseline = weather->data.sunGlare;
		}

		glareScale = ClampFinite(glareScale, 0.0f, kGodrayIntensityMax, kDefaultGodrayIntensity);
		const float base = static_cast<float>(state.baseline);
		const auto scaled = static_cast<long>(std::lround(base * glareScale));
		const auto applied = static_cast<std::uint8_t>(std::clamp(scaled, 0L, 255L));
		weather->data.sunGlare = applied;
		state.lastApplied = applied;
	}

	void RestoreWeatherSunGlare(RE::TESWeather* weather)
	{
		if (!weather)
			return;

		auto& states = GetSunGlareStateMap();
		const auto it = states.find(weather);
		if (it == states.end() || !it->second.initialized)
			return;

		weather->data.sunGlare = it->second.baseline;
		it->second.lastApplied = it->second.baseline;
	}

	bool IsRainWeatherActive(const RE::TESWeather* a_weather, float a_weight)
	{
		return a_weather &&
		       a_weather->precipitationData &&
		       a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy) &&
		       a_weight > kWeatherTransitionEpsilon;
	}

	bool IsRainTransitionActive()
	{
		auto* sky = globals::game::sky;
		if (!sky || !sky->precip || sky->mode.get() != RE::Sky::Mode::kFull)
			return false;

		const float currentWeight = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
		const float lastWeight = 1.0f - currentWeight;
		return IsRainWeatherActive(sky->currentWeather, currentWeight) ||
		       IsRainWeatherActive(sky->lastWeather, lastWeight);
	}

	RE::NiColor ClampColor01(const RE::NiColor& color)
	{
		const auto clampChannel = [](float value) {
			return ClampFinite(value, 0.0f, 1.0f, 0.0f);
		};
		return {
			clampChannel(color.red),
			clampChannel(color.green),
			clampChannel(color.blue)
		};
	}

	float GetLuminance(const RE::NiColor& color)
	{
		return color.red * kColorLumaR + color.green * kColorLumaG + color.blue * kColorLumaB;
	}

	RE::NiColor SaturateColor(const RE::NiColor& color, float saturation)
	{
		saturation = ClampFinite(saturation, 0.0f, kGodraySaturationMax, 1.0f);
		const float luminance = GetLuminance(color);
		return ClampColor01({
			luminance + (color.red - luminance) * saturation,
			luminance + (color.green - luminance) * saturation,
			luminance + (color.blue - luminance) * saturation
		});
	}

	RE::NiColor LerpColor(const RE::NiColor& a, const RE::NiColor& b, float t)
	{
		t = ClampFinite(t, 0.0f, 1.0f, 0.0f);
		return {
			a.red + (b.red - a.red) * t,
			a.green + (b.green - a.green) * t,
			a.blue + (b.blue - a.blue) * t
		};
	}

	RE::NiColor GetDescriptorColor(const RE::BSVolumetricLightingRenderData& descriptor)
	{
		return ClampColor01({ descriptor.red, descriptor.green, descriptor.blue });
	}

	void SetDescriptorColor(RE::BSVolumetricLightingRenderData& descriptor, const RE::NiColor& color)
	{
		const RE::NiColor clamped = ClampColor01(color);
		descriptor.red = clamped.red;
		descriptor.green = clamped.green;
		descriptor.blue = clamped.blue;
	}

	void ApplyGodrayOpacity(RE::BSVolumetricLightingRenderData& descriptor, float opacity)
	{
		opacity = ClampFinite(opacity, 0.0f, kGodrayOpacityMax, 1.0f);
		descriptor.intensity *= opacity;
		descriptor.density.contribution *= opacity;
	}

	void ApplyGodrayShaftIntensity(RE::BSVolumetricLightingRenderData& descriptor, float intensity)
	{
		intensity = ClampFinite(intensity, 0.0f, kGodrayShaftIntensityMax, 1.0f);
		descriptor.intensity *= intensity;
	}

	RE::NiColor GetCurrentWeatherSunColor(const RE::NiColor& fallbackColor)
	{
		auto* sky = globals::game::sky;
		if (sky && sky->sun && sky->sun->light) {
			return ClampColor01(sky->sun->light->GetLightRuntimeData().diffuse);
		}

		return ClampColor01(fallbackColor);
	}

	GodrayRuntimeParams BuildGodrayRuntimeParams(const VolumetricLighting::Settings& settings)
	{
		GodrayRuntimeParams params{};
		params.shaftIntensity = ClampFinite(settings.GodrayShaftIntensity, 0.0f, kGodrayShaftIntensityMax, 1.0f);
		params.opacity = ClampFinite(settings.GodrayOpacity, 0.0f, kGodrayOpacityMax, 1.0f);
		params.saturation = ClampFinite(settings.GodraySaturation, 0.0f, kGodraySaturationMax, 1.0f);
		params.customContribution = ClampFinite(settings.CustomColorContribution, 0.0f, 1.0f, 0.0f);
		params.customColor = ClampColor01({ settings.CustomColorRed, settings.CustomColorGreen, settings.CustomColorBlue });
		return params;
	}

	bool HasDescriptorTuning(const GodrayRuntimeParams& params)
	{
		return !IsNear(params.shaftIntensity, kDefaultGodrayShaftIntensity) ||
		       !IsNear(params.opacity, kDefaultGodrayOpacity) ||
		       !IsNear(params.saturation, kDefaultGodraySaturation) ||
		       !IsNear(params.customContribution, kDefaultCustomContribution);
	}

	float GetSunGlareBlendValue(const RE::Sun* sun)
	{
		if (!sun || !sun->sunGlare)
			return 1.0f;

		const auto* shaderProp = skyrim_cast<const RE::BSSkyShaderProperty*>(
			static_cast<const RE::BSShaderProperty*>(sun->sunGlare->GetGeometryRuntimeData().shaderProperty.get()));
		if (!shaderProp)
			return 1.0f;

		return ClampFinite(shaderProp->fBlendValue, 0.0f, 1.0f, 1.0f);
	}

	void ApplySunGlareOcclusionGate(RE::Sun* sun)
	{
		if (!sun)
			return;

		if (!sun->doOcclusionTests) {
			sun->glareScale = kDefaultGodrayIntensity;
			return;
		}

		const float blend = GetSunGlareBlendValue(sun);
		sun->glareScale = blend <= kSunGlareOcclusionCutoff ? 0.0f : kDefaultGodrayIntensity;
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricLighting::TextureSize,
	Width,
	Height,
	Depth);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricLighting::Settings,
	ExteriorEnabled,
	DisableWeatherInteractionDuringRain,
	GodrayIntensity,
	GodrayShaftIntensity,
	GodrayOpacity,
	GodraySaturation,
	CustomColorContribution,
	CustomColorRed,
	CustomColorGreen,
	CustomColorBlue,
	ExteriorQuality,
	ExteriorCustomSize,
	InteriorEnabled,
	InteriorQuality,
	InteriorCustomSize);

void VolumetricLighting::DrawSettings()
{
	if (REL::Module::IsVR()) {
		if (ImGui::Checkbox("Disable Weather-Driven VL During Rain", &settings.DisableWeatherInteractionDuringRain))
			SetupVL();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Rain-only mode. Automatically disables weather-driven VL while rain is active and restores it after rain.");
		ImGui::Separator();
	}

	DrawGodrayTuningSettings();

	if (ImGui::Checkbox("Enable Volumetric Lighting in Exteriors", &settings.ExteriorEnabled))
		SetupVL();

	if (settings.ExteriorEnabled)
		DrawVolumetricLightingSettings(settings.ExteriorQuality, settings.ExteriorCustomSize, false, !inInterior);

	if (ImGui::Checkbox("Enable Volumetric Lighting in Interiors", &settings.InteriorEnabled))
		SetupVL();

	if (settings.InteriorEnabled)
		DrawVolumetricLightingSettings(settings.InteriorQuality, settings.InteriorCustomSize, true, inInterior);
}

void VolumetricLighting::DrawGodrayTuningSettings()
{
	auto drawSlider = [](const char* label, float& value, float minValue, float maxValue, const char* tooltip) {
		const bool changed = ImGui::SliderFloat(label, &value, minValue, maxValue, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(tooltip);
		return changed;
	};

	ImGui::SeparatorText("Godray Tuning");
	const bool glareChanged = drawSlider("Sun Glare Intensity", settings.GodrayIntensity, 0.0f, kGodrayIntensityMax, "Scales the sun glare sprite brightness.");
	drawSlider("Godray Intensity", settings.GodrayShaftIntensity, 0.0f, kGodrayShaftIntensityMax, "Scales volumetric godray shaft brightness.");
	drawSlider("Godray Opacity", settings.GodrayOpacity, 0.0f, kGodrayOpacityMax, "Controls shaft strength and visibility. 1.0 is default; values above 1.0 boost presence.");
	drawSlider("Godray Saturation", settings.GodraySaturation, 0.0f, kGodraySaturationMax, "Adjusts weather-driven godray color richness. 1.0 is default.");

	drawSlider("Custom Color Contribution", settings.CustomColorContribution, 0.0f, 1.0f, "Blends your custom color into the weather godray color.");
	drawSlider("Custom Color Red", settings.CustomColorRed, 0.0f, 1.0f, "Red channel for custom volumetric color.");
	drawSlider("Custom Color Green", settings.CustomColorGreen, 0.0f, 1.0f, "Green channel for custom volumetric color.");
	drawSlider("Custom Color Blue", settings.CustomColorBlue, 0.0f, 1.0f, "Blue channel for custom volumetric color.");

	if (glareChanged) {
		const float glareScale = ClampFinite(settings.GodrayIntensity, 0.0f, kGodrayIntensityMax, kDefaultGodrayIntensity);
		if (IsImageSpaceReplacementEnabled() && !IsNear(glareScale, kDefaultGodrayIntensity)) {
			ApplySunGlareTuning();
		} else {
			RestoreSunGlareTuning();
		}
	}
}

void VolumetricLighting::DrawVolumetricLightingSettings(int32_t& quality, TextureSize& customSize, const bool isInterior, const bool inLocationType)
{
	auto& [Width, Height, Depth] = FetchCurrentSizeInUnits(isInterior);

	if (ImGui::SliderInt(isInterior ? "Interior Quality" : "Exterior Quality", &quality, 0, static_cast<uint8_t>(Quality::Count) - 1, QualityNames[quality])) {
		if (inLocationType)
			SetupVL();
	}

	const bool isCustomQuality = static_cast<Quality>(quality) == Quality::Custom;
	if (!isCustomQuality)
		ImGui::BeginDisabled();

	if (ImGui::SliderInt(isInterior ? "Interior Width" : "Exterior Width", &Width, 1, 20, FromUnits(Width, 32), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		customSize.Width = Width * 32;
		if (inLocationType)
			SetupVL();
	}

	if (ImGui::SliderInt(isInterior ? "Interior Height" : "Exterior Height", &Height, 1, 20, FromUnits(Height, 32), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		customSize.Height = Height * 32;
		if (inLocationType)
			SetupVL();
	}

	if (ImGui::SliderInt(isInterior ? "Interior Depth" : "Exterior Depth", &Depth, 1, 64, FromUnits(Depth, 10), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		customSize.Depth = Depth * 10;
		if (inLocationType)
			SetupVL();
	}

	if (!isCustomQuality)
		ImGui::EndDisabled();
}

inline const char* VolumetricLighting::FromUnits(const int32_t value, const int32_t unitScale)
{
	static std::string s;
	s = std::to_string(value * unitScale);
	return s.c_str();
}

VolumetricLighting::TextureSize& VolumetricLighting::FetchCurrentSizeInUnits(const bool interior)
{
	auto& size = interior ? interiorSizeInUnits : exteriorSizeInUnits;
	if (interior) {
		switch (static_cast<Quality>(settings.InteriorQuality)) {
		case Quality::Low:
			size = *gVolumetricLightingSizeLow;
			break;
		case Quality::Medium:
			size = *gVolumetricLightingSizeMedium;
			break;
		case Quality::High:
			size = defaultSizeHigh;
			break;
		case Quality::Custom:
			size = settings.InteriorCustomSize;
			break;
		default:
			break;
		}
	} else {
		switch (static_cast<Quality>(settings.ExteriorQuality)) {
		case Quality::Low:
			size = *gVolumetricLightingSizeLow;
			break;
		case Quality::Medium:
			size = *gVolumetricLightingSizeMedium;
			break;
		case Quality::High:
			size = defaultSizeHigh;
			break;
		case Quality::Custom:
			size = settings.ExteriorCustomSize;
			break;
		default:
			break;
		}
	}

	size.Height /= 32;
	size.Width /= 32;
	size.Depth /= 10;

	return size;
}

void VolumetricLighting::LoadSettings(json& o_json)
{
	settings = o_json;

	// Backward-compat migration: older configs only had GodrayIntensity.
	if (!o_json.contains("GodrayShaftIntensity")) {
		settings.GodrayShaftIntensity = settings.GodrayIntensity;
	}

	// Sanitize persisted values once on load for stable runtime behavior.
	settings.GodrayIntensity = ClampFinite(settings.GodrayIntensity, 0.0f, kGodrayIntensityMax, 1.0f);
	settings.GodrayShaftIntensity = ClampFinite(settings.GodrayShaftIntensity, 0.0f, kGodrayShaftIntensityMax, 1.0f);
	settings.GodrayOpacity = ClampFinite(settings.GodrayOpacity, 0.0f, kGodrayOpacityMax, 1.0f);
	settings.GodraySaturation = ClampFinite(settings.GodraySaturation, 0.0f, kGodraySaturationMax, 1.0f);
	settings.CustomColorContribution = ClampFinite(settings.CustomColorContribution, 0.0f, 1.0f, 0.0f);
	settings.CustomColorRed = ClampFinite(settings.CustomColorRed, 0.0f, 1.0f, 1.0f);
	settings.CustomColorGreen = ClampFinite(settings.CustomColorGreen, 0.0f, 1.0f, 1.0f);
	settings.CustomColorBlue = ClampFinite(settings.CustomColorBlue, 0.0f, 1.0f, 1.0f);
}

void VolumetricLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VolumetricLighting::RestoreDefaultSettings()
{
	settings = {};
	if (globals::game::isVR)
	{
		Util::ResetGameSettingsToDefaults(hiddenVREnableSettings);
		Util::ResetGameSettingsToDefaults(hiddenVRWeatherUpdateSettings);
	}
}

bool VolumetricLighting::IsExteriorEnabled() const
{
	return settings.ExteriorEnabled;
}

void VolumetricLighting::SetExteriorEnabled(bool enabled)
{
	settings.ExteriorEnabled = enabled;

	if (initialised && !inInterior && bEnableVolumetricLighting && gVolumetricLightingSizeHigh) {
		SetupVL();
	}
}

void VolumetricLighting::DataLoaded()
{
	auto shaderCache = globals::shaderCache;
	const static auto address = REL::Offset{ 0x1ec6b88 }.address();
	bool& bDepthBufferCulling = *reinterpret_cast<bool*>(address);

	if (REL::Module::IsVR() && bDepthBufferCulling && shaderCache->IsDiskCache()) {
		// clear cache to fix bug caused by bDepthBufferCulling
		logger::info("Force clearing cache due to bDepthBufferCulling");
		shaderCache->Clear();
	}
}

void VolumetricLighting::PostPostLoad()
{
	if (REL::Module::IsVR()) {
		if (settings.ExteriorEnabled || settings.InteriorEnabled) {
			EnableBooleanSettings(hiddenVREnableSettings, GetName());
			const bool weatherInteractionEnabled = !(settings.DisableWeatherInteractionDuringRain && IsRainTransitionActive());
			SetBooleanSettings(hiddenVRWeatherUpdateSettings, GetName(), weatherInteractionEnabled);
		}
		auto address = REL::RelocationID(100475, 0).address() + 0x45b;  // AE not needed, VR only hook
		logger::info("[{}] Hooking CopyResource at {:x}", GetName(), address);
		REL::safe_fill(address, REL::NOP, 7);
		stl::write_thunk_call<CopyResource>(address);

		// Skip volumetric lighting rendering
		REL::safe_write(REL::RelocationID(35560, 0).address() + REL::Relocate(0x254, 0), &REL::JMP8, 1);
		// Move it to render after depth to ensure camera matches rest of scene
		stl::write_thunk_call<RenderDepth>(REL::RelocationID(35560, 0).address() + REL::Relocate(0x2EE, 0));
	}

	stl::write_thunk_call<ApplyVolumetricLighting_VolumetricLightingDescriptor_Get>(REL::RelocationID(100475, 107193).address() + 0x354);

	bEnableVolumetricLighting = reinterpret_cast<bool*>(REL::RelocationID(527940, 414913).address());
	gVolumetricLightingSizeLow = reinterpret_cast<TextureSize*>(REL::RelocationID(527970, 414916).address());
	gVolumetricLightingSizeMedium = reinterpret_cast<TextureSize*>(REL::RelocationID(527973, 414919).address());
	gVolumetricLightingSizeHigh = reinterpret_cast<TextureSize*>(REL::RelocationID(527976, 414922).address());
	defaultSizeHigh = *gVolumetricLightingSizeHigh;

	// Ensure the VL raymarch compute shader is only dispatched once, rather than once for every level of depth
	// The updated raymarch shader iterates through the depth now instead
	// Skip the first call, the second call has read/write texture setup in the correct order
	REL::safe_fill(REL::RelocationID(100309, 107023).address() + REL::Relocate(0xA4, 0x406), REL::NOP, 3);
	// Exit the loop after the first iteration
	REL::safe_fill(REL::RelocationID(100309, 107023).address() + REL::Relocate(0x147, 0x4A9), REL::NOP, 6);
}

void VolumetricLighting::SetupResources()
{
	vlDataCB = new ConstantBuffer(ConstantBufferDesc<VLData>());
}

void VolumetricLighting::EarlyPrepass()
{
	auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);

	int32_t width = static_cast<int32_t>(renderSize.x);
	int32_t height = static_cast<int32_t>(renderSize.y);

	if (width != vlData.screenX || height != vlData.screenY) {
		blurHCS = nullptr;
		blurVCS = nullptr;
	}

	vlData.screenX = width;
	vlData.screenY = height;
	vlData.screenXMin1 = width - 1;
	vlData.screenYMin1 = height - 1;
	vlDataCB->Update(vlData);

	const float glareScale = ClampFinite(settings.GodrayIntensity, 0.0f, kGodrayIntensityMax, kDefaultGodrayIntensity);
	if (IsImageSpaceReplacementEnabled() && !IsNear(glareScale, kDefaultGodrayIntensity)) {
		ApplySunGlareTuning();
	} else {
		RestoreSunGlareTuning();
	}

	const auto interiorCell = RE::TES::GetSingleton()->interiorCell;
	const bool currentlyInInterior = interiorCell != nullptr;
	const bool nextRainSuppressionActive =
		settings.DisableWeatherInteractionDuringRain &&
		!currentlyInInterior &&
		IsRainTransitionActive();

	if (initialised &&
	    currentlyInInterior == inInterior &&
	    nextRainSuppressionActive == rainOnlySuppressionActive)
		return;

	initialised = true;
	inInterior = currentlyInInterior;
	inInteriorWithSun = InteriorSun::IsInteriorWithSun(interiorCell);
	rainOnlySuppressionActive = nextRainSuppressionActive;
	SetupVL();
}

void VolumetricLighting::SetupVL()
{
	if (!gVolumetricLightingSizeHigh || (!globals::game::isVR && !bEnableVolumetricLighting)) {
		return;
	}

	const bool runtimeEnabled = inInterior ? (settings.InteriorEnabled && inInteriorWithSun) : settings.ExteriorEnabled;
	const int32_t quality = inInterior ? settings.InteriorQuality : settings.ExteriorQuality;
	const TextureSize& customSize = inInterior ? settings.InteriorCustomSize : settings.ExteriorCustomSize;

	if (globals::game::isVR) {
		rainOnlySuppressionActive =
			settings.DisableWeatherInteractionDuringRain &&
			!inInterior &&
			IsRainTransitionActive();
		const bool weatherInteractionEnabled = !rainOnlySuppressionActive;
		const bool effectiveWeatherUpdateEnabled = runtimeEnabled && weatherInteractionEnabled;
		SetBooleanSettings(hiddenVREnableSettings, GetName(), runtimeEnabled);
		SetBooleanSettings(hiddenVRWeatherUpdateSettings, GetName(), effectiveWeatherUpdateEnabled);
		if (runtimeEnabled && !effectiveWeatherUpdateEnabled) {
			// Drop stale volumetric history immediately when weather updates are suppressed.
			ClearVolumetricLightingTargets();
		}
	} else {
		*bEnableVolumetricLighting = runtimeEnabled;
	}

	*gVolumetricLightingSizeHigh = static_cast<Quality>(quality) == Quality::Custom ? customSize : defaultSizeHigh;
	SetVLQuality(GetVLDescriptor(), quality);

	if (!runtimeEnabled) {
		ClearVolumetricLightingTargets();
		RestoreSunGlareTuning();
	}
}

void VolumetricLighting::ClearVolumetricLightingTargets()
{
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer) {
		return;
	}

	const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto clearRT = [&](RE::RENDER_TARGET index) {
		auto& target = renderer->GetRuntimeData().renderTargets[index];
		if (target.RTV) {
			context->ClearRenderTargetView(target.RTV, clearColor);
		}
		if (target.UAV) {
			context->ClearUnorderedAccessViewFloat(target.UAV, clearColor);
		}
	};

	clearRT(RE::RENDER_TARGETS::kIMAGESPACE_VOLUMETRIC_LIGHTING);
	clearRT(RE::RENDER_TARGETS::kIMAGESPACE_VOLUMETRIC_LIGHTING_PREVIOUS);
	clearRT(RE::RENDER_TARGETS::kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY);
	clearRT(RE::RENDER_TARGETS::kVOLUMETRIC_LIGHTING_HALF_RES);
	clearRT(RE::RENDER_TARGETS::kVOLUMETRIC_LIGHTING_BLUR_HALF_RES);
	clearRT(RE::RENDER_TARGETS::kVOLUMETRIC_LIGHTING_QUARTER_RES);
	clearRT(RE::RENDER_TARGETS::kVOLUMETRIC_LIGHTING_BLUR_QUARTER_RES);
}

VolumetricLighting::VolumetricLightingDescriptor& VolumetricLighting::GetVLDescriptor()
{
	using func_t = decltype(&VolumetricLighting::GetVLDescriptor);
	static REL::Relocation<func_t> func{ REL::RelocationID(100297, 107014) };
	return func();
}

void VolumetricLighting::SetVLQuality(VolumetricLightingDescriptor& descriptor, const uint32_t quality)
{
	using func_t = decltype(&VolumetricLighting::SetVLQuality);
	static REL::Relocation<func_t> func{ REL::RelocationID(100299, 107016).address() };
	func(descriptor, std::clamp<uint32_t>(quality, 0, 2));
}

void VolumetricLighting::RenderVolumetricLighting(VolumetricLightingDescriptor* descriptor, RE::NiCamera* camera, bool flag)
{
	using func_t = decltype(&VolumetricLighting::RenderVolumetricLighting);
	static REL::Relocation<func_t> func{ REL::RelocationID(100306, 0) };
	func(descriptor, camera, flag);
}

void VolumetricLighting::RenderDepth::thunk()
{
	func();
	if (globals::features::volumetricLighting.bEnableVolumetricLighting)
		RenderVolumetricLighting(&GetVLDescriptor(), RE::Main::WorldRootCamera(), false);
}

void VolumetricLighting::ApplySunGlareTuning() const
{
	auto* sky = globals::game::sky;
	if (!sky || !sky->sun)
		return;

	const float glareScale = ClampFinite(settings.GodrayIntensity, 0.0f, kGodrayIntensityMax, kDefaultGodrayIntensity);
	ApplyScaledSunGlareToWeather(sky->currentWeather, glareScale);
	if (sky->lastWeather != sky->currentWeather)
		ApplyScaledSunGlareToWeather(sky->lastWeather, glareScale);

	// Keep the tuning weather-driven, but hard-gate glare when the sun is fully occluded.
	ApplySunGlareOcclusionGate(sky->sun);
}

void VolumetricLighting::RestoreSunGlareTuning() const
{
	auto* sky = globals::game::sky;
	if (!sky || !sky->sun)
		return;

	RestoreWeatherSunGlare(sky->currentWeather);
	if (sky->lastWeather != sky->currentWeather)
		RestoreWeatherSunGlare(sky->lastWeather);
	sky->sun->glareScale = kDefaultGodrayIntensity;
}

VolumetricLighting::VolumetricLightingDescriptor* VolumetricLighting::ApplyVolumetricLighting_VolumetricLightingDescriptor_Get::thunk()
{
	auto* descriptor = func();
	if (!descriptor)
		return nullptr;

	auto& feature = globals::features::volumetricLighting;
	const bool imageSpaceReplacementEnabled = IsImageSpaceReplacementEnabled();
	const auto& runtimeSettings = feature.settings;
	const GodrayRuntimeParams params = BuildGodrayRuntimeParams(runtimeSettings);
	const float glareScale = ClampFinite(runtimeSettings.GodrayIntensity, 0.0f, kGodrayIntensityMax, kDefaultGodrayIntensity);

	// Keep sun glare tuning active without touching descriptor data when no shaft/color overrides are set.
	if (imageSpaceReplacementEnabled && !IsNear(glareScale, kDefaultGodrayIntensity)) {
		feature.ApplySunGlareTuning();
	} else {
		feature.RestoreSunGlareTuning();
	}

	// If image-space replacement is disabled, keep vanilla descriptor behavior untouched.
	if (!imageSpaceReplacementEnabled) {
		return descriptor;
	}

	if (!HasDescriptorTuning(params)) {
		return descriptor;
	}

	if (!IsNear(params.shaftIntensity, kDefaultGodrayShaftIntensity))
		ApplyGodrayShaftIntensity(*descriptor, params.shaftIntensity);
	if (!IsNear(params.opacity, kDefaultGodrayOpacity))
		ApplyGodrayOpacity(*descriptor, params.opacity);

	if (!IsNear(params.saturation, kDefaultGodraySaturation) || !IsNear(params.customContribution, kDefaultCustomContribution)) {
		const RE::NiColor weatherColor = SaturateColor(GetCurrentWeatherSunColor(GetDescriptorColor(*descriptor)), params.saturation);
		const RE::NiColor finalColor = LerpColor(weatherColor, params.customColor, params.customContribution);

		// Use explicit final color each frame so saturation and custom contribution are independent.
		descriptor->customColor.contribution = 1.0f;
		SetDescriptorColor(*descriptor, finalColor);
	}

	return descriptor;
}

RE::BSImagespaceShader* VolumetricLighting::CreateShader(const std::string_view& name, const std::string_view& fileName, RE::BSComputeShader* computeShader)
{
	auto shader = RE::BSImagespaceShader::Create();
	shader->shaderType = RE::BSShader::Type::ImageSpace;
	shader->fxpFilename = fileName.data();
	shader->name = name.data();
	shader->originalShaderName = fileName.data();
	shader->computeShader = computeShader;
	shader->isComputeShader = true;
	return shader;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateGenerateCS(RE::BSComputeShader* computeShader)
{
	if (generateCS == nullptr)
		generateCS = CreateShader("BSImagespaceShaderVolumetricLightingGenerateCS", "ISVolumetricLightingGenerateCS", computeShader);
	return generateCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateRaymarchCS(RE::BSComputeShader* computeShader)
{
	if (raymarchCS == nullptr)
		raymarchCS = CreateShader("BSImagespaceShaderVolumetricLightingRaymarchCS", "ISVolumetricLightingRaymarchCS", computeShader);
	return raymarchCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateBlurHCS(RE::BSComputeShader* computeShader)
{
	if (blurHCS == nullptr)
		blurHCS = CreateShader("BSImagespaceShaderVolumetricLightingBlurHCS", "ISVolumetricLightingBlurHCS", computeShader);
	return blurHCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateBlurVCS(RE::BSComputeShader* computeShader)
{
	if (blurVCS == nullptr)
		blurVCS = CreateShader("BSImagespaceShaderVolumetricLightingBlurVCS", "ISVolumetricLightingBlurVCS", computeShader);
	return blurVCS;
}

void VolumetricLighting::SetDimensionsCB() const
{
	auto cb = vlDataCB->CB();
	globals::d3d::context->CSSetConstantBuffers(1, 1, &cb);
}

void VolumetricLighting::SetGroupCountsHCS(uint32_t& threadGroupCountX) const
{
	threadGroupCountX = (vlData.screenX + BlurThreadGroupSizeX - BlurWindow * 2u - 1u) / (BlurThreadGroupSizeX - BlurWindow * 2u);
}

void VolumetricLighting::SetGroupCountsVCS(uint32_t& threadGroupCountY) const
{
	threadGroupCountY = (vlData.screenY + BlurThreadGroupSizeY - BlurWindow * 2u - 1u) / (BlurThreadGroupSizeY - BlurWindow * 2u);
}

void VolumetricLighting::CopyResource::thunk(ID3D11DeviceContext* a_this, ID3D11Resource* a_renderTarget, ID3D11Resource* a_renderTargetSource)
{
	// In VR with dynamic resolution enabled, there's a bug with the depth stencil.
	// The depth stencil passed to IsFullScreenVR is scaled down incorrectly.
	// The fix is to stop a CopyResource from replacing kMAIN_COPY with kMAIN after
	// ISApplyVolumetricLighting because it clobbers a properly scaled kMAIN_COPY.
	// The kMAIN_COPY does not appear to be used in the remaining frame after
	// ISApplyVolumetricLighting except for IsFullScreenVR.
	// But, the copy might have to be done manually later after IsFullScreenVR if
	// used in the next frame.

	auto& singleton = globals::features::volumetricLighting;
	if (!(Util::IsDynamicResolution() && singleton.bEnableVolumetricLighting)) {
		a_this->CopyResource(a_renderTarget, a_renderTargetSource);
	}
}
