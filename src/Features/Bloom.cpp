#include "Bloom.h"

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
}

void Bloom::DrawSettings(Settings& settings)
{
	bool enabled = settings.Enabled != 0;
	if (ImGui::Checkbox(T(TKEY("enable_enhancement"), "Enable Bloom Enhancement"), &enabled)) {
		settings.Enabled = enabled;
	}

	if (ImGui::Button(T(TKEY("preset_default"), "Default"))) {
		settings = Settings{};
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_fantasy"), "Fantasy"))) {
		settings.Enabled = true;
		settings.EnhancementIntensity = 3.25f;
		settings.HaloRadius = 20.0f;
		settings.HaloSpread = 1.0f;
		settings.BloomSaturation = 1.15f;
		settings.BloomTint = { 1.0f, 0.98f, 0.94f };
		settings.CompressionCeiling = kCompressionCeilingMax;
		settings.CompressionThreshold = 0.0f;
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_dreamy"), "Dreamy"))) {
		settings.Enabled = true;
		settings.EnhancementIntensity = 1.5f;
		settings.HaloRadius = 28.0f;
		settings.HaloSpread = 0.72f;
		settings.BloomSaturation = 0.85f;
		settings.BloomTint = { 185.0f / 255.0f, 215.0f / 255.0f, 1.0f };
		settings.CompressionCeiling = 0.9f;
		settings.CompressionThreshold = 0.08f;
	}

	ImGui::BeginDisabled(!settings.Enabled);
	ImGui::SliderFloat(T(TKEY("enhancement_intensity"), "Enhancement Intensity"), &settings.EnhancementIntensity, 0.0f, kEnhancementIntensityMax, "%.2f");
	DrawTooltip(T(TKEY("enhancement_intensity_tooltip"), "Multiplies the generated vanilla bloom signal before compression. Raise it to exaggerate weak bloom, such as the sky."));
	ImGui::SliderFloat(T(TKEY("halo_radius"), "Halo Radius"), &settings.HaloRadius, 0.0f, kHaloRadiusMax, "%.1f");
	DrawTooltip(T(TKEY("halo_radius_tooltip"), "Controls the radius of the enhancement's additional bloom samples. Higher values create wider halos."));
	ImGui::SliderFloat(T(TKEY("halo_spread"), "Halo Spread"), &settings.HaloSpread, 0.0f, 1.0f, "%.2f");
	DrawTooltip(T(TKEY("halo_spread_tooltip"), "Blends between the original bloom and the widened halo samples. Higher values make the halo softer and more spread out."));
	ImGui::SliderFloat(T(TKEY("bloom_saturation"), "Bloom Saturation"), &settings.BloomSaturation, 0.0f, kBloomSaturationMax, "%.2f");
	DrawTooltip(T(TKEY("bloom_saturation_tooltip"), "Controls the color saturation of the enhanced bloom. Lower values make it whiter; higher values preserve or exaggerate its tint."));
	ImGui::ColorEdit3(T(TKEY("bloom_tint"), "Bloom Tint"), reinterpret_cast<float*>(&settings.BloomTint));
	DrawTooltip(T(TKEY("bloom_tint_tooltip"), "Colors the bloom halo without changing the underlying scene lighting."));
	ImGui::SliderFloat(T(TKEY("compression_ceiling"), "Compression Ceiling"), &settings.CompressionCeiling, 0.0f, kCompressionCeilingMax, "%.2f");
	DrawTooltip(T(TKEY("compression_ceiling_tooltip"), "The soft limiter's maximum bloom level. Bloom above the compression threshold approaches this value instead of continuing to scale. Set to 0 to remove added bloom."));
	ImGui::SliderFloat(T(TKEY("compression_threshold"), "Compression Threshold"), &settings.CompressionThreshold, 0.0f, settings.CompressionCeiling, "%.2f");
	DrawTooltip(T(TKEY("compression_threshold_tooltip"), "The post-enhancement bloom level where soft compression starts. Bloom below it is unchanged; bloom above it rolls toward Compression Ceiling. Set it equal to Compression Ceiling for a hard cap."));
	ImGui::EndDisabled();

	SanitizeSettings(settings);
}

Bloom::Settings Bloom::GetCommonBufferData(Settings settings)
{
	SanitizeSettings(settings);
	return settings;
}

void Bloom::SanitizeSettings(Settings& settings)
{
	const Settings defaults{};
	auto clampFiniteOrDefault = [](float value, float min, float max, float defaultValue) {
		return std::isfinite(value) ? std::clamp(value, min, max) : defaultValue;
	};

	settings.pad = {};
	settings.Enabled = settings.Enabled != 0;
	settings.EnhancementIntensity = clampFiniteOrDefault(settings.EnhancementIntensity, 0.0f, kEnhancementIntensityMax, defaults.EnhancementIntensity);
	settings.HaloRadius = clampFiniteOrDefault(settings.HaloRadius, 0.0f, kHaloRadiusMax, defaults.HaloRadius);
	settings.HaloSpread = clampFiniteOrDefault(settings.HaloSpread, 0.0f, 1.0f, defaults.HaloSpread);
	settings.BloomSaturation = clampFiniteOrDefault(settings.BloomSaturation, 0.0f, kBloomSaturationMax, defaults.BloomSaturation);
	settings.BloomTint.x = clampFiniteOrDefault(settings.BloomTint.x, 0.0f, 1.0f, defaults.BloomTint.x);
	settings.BloomTint.y = clampFiniteOrDefault(settings.BloomTint.y, 0.0f, 1.0f, defaults.BloomTint.y);
	settings.BloomTint.z = clampFiniteOrDefault(settings.BloomTint.z, 0.0f, 1.0f, defaults.BloomTint.z);
	settings.CompressionCeiling = clampFiniteOrDefault(settings.CompressionCeiling, 0.0f, kCompressionCeilingMax, defaults.CompressionCeiling);
	settings.CompressionThreshold = clampFiniteOrDefault(settings.CompressionThreshold, 0.0f, settings.CompressionCeiling, defaults.CompressionThreshold);
}

#undef I18N_KEY_PREFIX
