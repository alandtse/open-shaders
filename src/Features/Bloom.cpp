#include "Bloom.h"

#include <cmath>

#include "../I18n/I18n.h"
#include "../Utils/UI.h"

#define I18N_KEY_PREFIX "feature.bloom."

namespace
{
	constexpr float kStrengthMax = 6.0f;
	constexpr float kRadiusMax = 32.0f;
	constexpr float kSaturationMax = 2.0f;
	constexpr float kMaxContributionMax = 8.0f;

	void DrawTooltip(const char* a_text)
	{
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", a_text);
	}
}

void Bloom::DrawSettings(Settings& settings, bool a_isInterior)
{
	bool enabled = settings.Enabled != 0;
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable Vanilla Bloom Enhanced"), &enabled)) {
		settings.Enabled = enabled;
	}

	if (ImGui::Button(T(TKEY("preset_default"), "Default"))) {
		settings = Settings{};
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_fantasy"), "Fantasy"))) {
		settings.Enabled = true;
		if (a_isInterior) {
			settings.Strength = 2.0f;
			settings.Radius = 19.0f;
			settings.Scatter = 0.3f;
			settings.Saturation = 1.1f;
			settings.Tint = { 236.0f / 255.0f, 160.0f / 255.0f, 160.0f / 255.0f };
			settings.MaxContribution = 1.10f;
			settings.GlowThreshold = 0.14f;
		} else {
			settings.Strength = 3.25f;
			settings.Radius = 20.0f;
			settings.Scatter = 1.0f;
			settings.Saturation = 1.15f;
			settings.Tint = { 1.0f, 0.98f, 0.94f };
			settings.MaxContribution = kMaxContributionMax;
			settings.GlowThreshold = 0.0f;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_dreamy"), "Dreamy"))) {
		settings.Enabled = true;
		settings.Strength = 1.5f;
		settings.Radius = 28.0f;
		settings.Scatter = 0.72f;
		settings.Saturation = 0.85f;
		settings.Tint = { 185.0f / 255.0f, 215.0f / 255.0f, 1.0f };
		settings.MaxContribution = 0.9f;
		settings.GlowThreshold = 0.08f;
	}

	ImGui::BeginDisabled(!settings.Enabled);
	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, kStrengthMax, "%.2f");
	DrawTooltip(T(TKEY("strength_tooltip"), "Multiplies the bloom signal before Glow Threshold and Glow Ceiling are applied. Raise it to exaggerate weak bloom, such as the sky."));
	ImGui::SliderFloat(T(TKEY("radius"), "Radius"), &settings.Radius, 0.0f, kRadiusMax, "%.1f");
	DrawTooltip(T(TKEY("radius_tooltip"), "Controls how far bloom spreads from bright sources. Higher values create wider halos."));
	ImGui::SliderFloat(T(TKEY("scatter"), "Scatter"), &settings.Scatter, 0.0f, 1.0f, "%.2f");
	DrawTooltip(T(TKEY("scatter_tooltip"), "Blends between the source pixel and the wider bloom samples. Higher values make the bloom softer and more spread out."));
	ImGui::SliderFloat(T(TKEY("saturation"), "Saturation"), &settings.Saturation, 0.0f, kSaturationMax, "%.2f");
	DrawTooltip(T(TKEY("saturation_tooltip"), "Controls how much color the bloom keeps. Lower values make it whiter; higher values preserve or exaggerate its tint."));
	ImGui::ColorEdit3(T(TKEY("tint"), "Tint"), reinterpret_cast<float*>(&settings.Tint));
	DrawTooltip(T(TKEY("tint_tooltip"), "Colors the bloom halo without changing the underlying scene lighting."));
	ImGui::SliderFloat(T(TKEY("max_contribution"), "Glow Ceiling"), &settings.MaxContribution, 0.0f, kMaxContributionMax, "%.2f");
	DrawTooltip(T(TKEY("glow_ceiling_tooltip"), "The soft limiter's maximum glow level. Bloom above the threshold approaches this value instead of continuing to scale. Set to 0 to remove added bloom."));
	ImGui::SliderFloat(T(TKEY("glow_threshold"), "Glow Threshold"), &settings.GlowThreshold, 0.0f, settings.MaxContribution, "%.2f");
	DrawTooltip(T(TKEY("glow_threshold_tooltip"), "The post-Strength bloom level where soft compression starts. Bloom below it is unchanged; bloom above it rolls toward Glow Ceiling. Set it equal to Glow Ceiling for a hard cap."));
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

	settings.Enabled = settings.Enabled != 0;
	settings.Strength = clampFiniteOrDefault(settings.Strength, 0.0f, kStrengthMax, defaults.Strength);
	settings.Radius = clampFiniteOrDefault(settings.Radius, 0.0f, kRadiusMax, defaults.Radius);
	settings.Scatter = clampFiniteOrDefault(settings.Scatter, 0.0f, 1.0f, defaults.Scatter);
	settings.Saturation = clampFiniteOrDefault(settings.Saturation, 0.0f, kSaturationMax, defaults.Saturation);
	settings.Tint.x = clampFiniteOrDefault(settings.Tint.x, 0.0f, 1.0f, defaults.Tint.x);
	settings.Tint.y = clampFiniteOrDefault(settings.Tint.y, 0.0f, 1.0f, defaults.Tint.y);
	settings.Tint.z = clampFiniteOrDefault(settings.Tint.z, 0.0f, 1.0f, defaults.Tint.z);
	settings.MaxContribution = clampFiniteOrDefault(settings.MaxContribution, 0.0f, kMaxContributionMax, defaults.MaxContribution);
	settings.GlowThreshold = clampFiniteOrDefault(settings.GlowThreshold, 0.0f, settings.MaxContribution, defaults.GlowThreshold);
}

#undef I18N_KEY_PREFIX
