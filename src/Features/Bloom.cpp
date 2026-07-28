#include "Bloom.h"

#include <algorithm>
#include <cmath>

#include "../I18n/I18n.h"
#include "../Utils/UI.h"

#define I18N_KEY_PREFIX "feature.bloom."

namespace
{
	constexpr float kEnhancementIntensityMax = 6.0f;
	constexpr float kHaloRadiusMax = 32.0f;
	constexpr float kBloomSaturationMax = 2.0f;
	constexpr float kCompressionCeilingMax = 8.0f;

	void DrawTooltip(const char* a_text)
	{
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", a_text);
	}

	Bloom::Profile& GetSelectedProfile(Bloom::PresetSettings& settings)
	{
		switch (settings.SelectedPreset) {
		case 1:
			return settings.Fantasy;
		case 2:
			return settings.Dreamy;
		default:
			return settings.Default;
		}
	}

	const Bloom::Profile& GetSelectedProfile(const Bloom::PresetSettings& settings)
	{
		switch (settings.SelectedPreset) {
		case 1:
			return settings.Fantasy;
		case 2:
			return settings.Dreamy;
		default:
			return settings.Default;
		}
	}

	Bloom::Profile GetPresetDefaults(uint preset)
	{
		const Bloom::PresetSettings defaults{};
		switch (preset) {
		case 1:
			return defaults.Fantasy;
		case 2:
			return defaults.Dreamy;
		default:
			return defaults.Default;
		}
	}
}

void Bloom::DrawSettings(PresetSettings& settings)
{
	bool enabled = settings.Enabled != 0;
	if (ImGui::Checkbox(T(TKEY("enable_enhancement"), "Enable Bloom Enhancement"), &enabled)) {
		settings.Enabled = enabled;
	}

	if (ImGui::Button(T(TKEY("preset_default"), "Default")))
		settings.SelectedPreset = 0;
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_fantasy"), "Fantasy")))
		settings.SelectedPreset = 1;
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_dreamy"), "Dreamy")))
		settings.SelectedPreset = 2;

	const char* resetLabel = T(TKEY("preset_reset"), "Reset Preset");
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(resetLabel).x - ImGui::GetStyle().FramePadding.x * 2.0f);
	if (ImGui::Button(resetLabel))
		GetSelectedProfile(settings) = GetPresetDefaults(settings.SelectedPreset);

	Profile& profile = GetSelectedProfile(settings);
	ImGui::BeginDisabled(!settings.Enabled);
	ImGui::SliderFloat(T(TKEY("enhancement_intensity"), "Enhancement Intensity"), &profile.EnhancementIntensity, 0.0f, kEnhancementIntensityMax, "%.2f");
	DrawTooltip(T(TKEY("enhancement_intensity_tooltip"), "Multiplies the generated vanilla bloom signal before compression. Raise it to exaggerate weak bloom, such as the sky."));
	ImGui::SliderFloat(T(TKEY("halo_radius"), "Halo Radius"), &profile.HaloRadius, 0.0f, kHaloRadiusMax, "%.1f");
	DrawTooltip(T(TKEY("halo_radius_tooltip"), "Controls the radius of the enhancement's additional bloom samples. Higher values create wider halos."));
	ImGui::SliderFloat(T(TKEY("halo_spread"), "Halo Spread"), &profile.HaloSpread, 0.0f, 1.0f, "%.2f");
	DrawTooltip(T(TKEY("halo_spread_tooltip"), "Blends between the original bloom and the widened halo samples. Higher values make the halo softer and more spread out."));
	ImGui::SliderFloat(T(TKEY("bloom_saturation"), "Bloom Saturation"), &profile.BloomSaturation, 0.0f, kBloomSaturationMax, "%.2f");
	DrawTooltip(T(TKEY("bloom_saturation_tooltip"), "Controls the color saturation of the enhanced bloom. Lower values make it whiter; higher values preserve or exaggerate its tint."));
	ImGui::ColorEdit3(T(TKEY("bloom_tint"), "Bloom Tint"), reinterpret_cast<float*>(&profile.BloomTint));
	DrawTooltip(T(TKEY("bloom_tint_tooltip"), "Colors the bloom halo without changing the underlying scene lighting."));
	ImGui::SliderFloat(T(TKEY("compression_ceiling"), "Compression Ceiling"), &profile.CompressionCeiling, 0.0f, kCompressionCeilingMax, "%.2f");
	DrawTooltip(T(TKEY("compression_ceiling_tooltip"), "The soft limiter's maximum bloom level. Bloom above the compression threshold approaches this value instead of continuing to scale. Set to 0 to remove added bloom."));
	ImGui::SliderFloat(T(TKEY("compression_threshold"), "Compression Threshold"), &profile.CompressionThreshold, 0.0f, profile.CompressionCeiling, "%.2f");
	DrawTooltip(T(TKEY("compression_threshold_tooltip"), "The post-enhancement bloom level where soft compression starts. Bloom below it is unchanged; bloom above it rolls toward Compression Ceiling. Set it equal to Compression Ceiling for a hard cap."));
	ImGui::EndDisabled();

	SanitizeSettings(settings);
}

Bloom::Settings Bloom::GetCommonBufferData(const PresetSettings& settings)
{
	const auto& profile = GetSelectedProfile(settings);
	return { settings.Enabled, profile.EnhancementIntensity, profile.HaloRadius, profile.HaloSpread, profile.BloomSaturation, profile.BloomTint, profile.CompressionThreshold, profile.CompressionCeiling };
}

void Bloom::SanitizeSettings(PresetSettings& settings)
{
	const PresetSettings defaults{};
	auto clampFiniteOrDefault = [](float value, float min, float max, float defaultValue) {
		return std::isfinite(value) ? std::clamp(value, min, max) : defaultValue;
	};
	auto sanitizeProfile = [&](Profile& profile, const Profile& defaultProfile) {
		profile.EnhancementIntensity = clampFiniteOrDefault(profile.EnhancementIntensity, 0.0f, kEnhancementIntensityMax, defaultProfile.EnhancementIntensity);
		profile.HaloRadius = clampFiniteOrDefault(profile.HaloRadius, 0.0f, kHaloRadiusMax, defaultProfile.HaloRadius);
		profile.HaloSpread = clampFiniteOrDefault(profile.HaloSpread, 0.0f, 1.0f, defaultProfile.HaloSpread);
		profile.BloomSaturation = clampFiniteOrDefault(profile.BloomSaturation, 0.0f, kBloomSaturationMax, defaultProfile.BloomSaturation);
		profile.BloomTint.x = clampFiniteOrDefault(profile.BloomTint.x, 0.0f, 1.0f, defaultProfile.BloomTint.x);
		profile.BloomTint.y = clampFiniteOrDefault(profile.BloomTint.y, 0.0f, 1.0f, defaultProfile.BloomTint.y);
		profile.BloomTint.z = clampFiniteOrDefault(profile.BloomTint.z, 0.0f, 1.0f, defaultProfile.BloomTint.z);
		profile.CompressionCeiling = clampFiniteOrDefault(profile.CompressionCeiling, 0.0f, kCompressionCeilingMax, defaultProfile.CompressionCeiling);
		const float thresholdDefault = std::min(defaultProfile.CompressionThreshold, profile.CompressionCeiling);
		profile.CompressionThreshold = clampFiniteOrDefault(profile.CompressionThreshold, 0.0f, profile.CompressionCeiling, thresholdDefault);
	};

	settings.Enabled = settings.Enabled != 0;
	settings.SelectedPreset = std::min(settings.SelectedPreset, 2u);
	sanitizeProfile(settings.Default, defaults.Default);
	sanitizeProfile(settings.Fantasy, defaults.Fantasy);
	sanitizeProfile(settings.Dreamy, defaults.Dreamy);
}

#undef I18N_KEY_PREFIX
