#include "Bloom.h"

#include "../I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.bloom."

namespace
{
	constexpr float kStrengthMax = 6.0f;
	constexpr float kRadiusMax = 32.0f;
	constexpr float kSaturationMax = 2.0f;
}

void Bloom::DrawSettings(Settings& settings)
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
		settings.Strength = 3.25f;
		settings.Radius = 20.0f;
		settings.Scatter = 1.0f;
		settings.Saturation = 1.15f;
		settings.Tint = { 1.0f, 0.98f, 0.94f };
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("preset_dreamy"), "Dreamy"))) {
		settings.Enabled = true;
		settings.Strength = 2.2f;
		settings.Radius = 28.0f;
		settings.Scatter = 0.75f;
		settings.Saturation = 0.85f;
		settings.Tint = { 1.0f, 0.93f, 0.82f };
	}

	ImGui::BeginDisabled(!settings.Enabled);
	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, kStrengthMax, "%.2f");
	ImGui::SliderFloat(T(TKEY("radius"), "Radius"), &settings.Radius, 0.0f, kRadiusMax, "%.1f");
	ImGui::SliderFloat(T(TKEY("scatter"), "Scatter"), &settings.Scatter, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("saturation"), "Saturation"), &settings.Saturation, 0.0f, kSaturationMax, "%.2f");
	ImGui::ColorEdit3(T(TKEY("tint"), "Tint"), reinterpret_cast<float*>(&settings.Tint));
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
	settings.Enabled = settings.Enabled != 0;
	settings.Strength = std::clamp(settings.Strength, 0.0f, kStrengthMax);
	settings.Radius = std::clamp(settings.Radius, 0.0f, kRadiusMax);
	settings.Scatter = std::clamp(settings.Scatter, 0.0f, 1.0f);
	settings.Saturation = std::clamp(settings.Saturation, 0.0f, kSaturationMax);
	settings.Tint.x = std::clamp(settings.Tint.x, 0.0f, 1.0f);
	settings.Tint.y = std::clamp(settings.Tint.y, 0.0f, 1.0f);
	settings.Tint.z = std::clamp(settings.Tint.z, 0.0f, 1.0f);
}

#undef I18N_KEY_PREFIX
