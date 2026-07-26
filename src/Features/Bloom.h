#pragma once

struct Bloom
{
	struct Settings
	{
		uint Enabled = true;
		float Strength = 1.0f;
		float Radius = 3.5f;
		float Scatter = 0.85f;

		float Saturation = 0.9f;
		float3 Tint = { 1.0f, 0.98f, 0.94f };
	};
	static_assert(sizeof(Settings) == 32);

	static void DrawSettings(Settings& settings);
	static Settings GetCommonBufferData(Settings settings);
	static void SanitizeSettings(Settings& settings);
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Bloom::Settings,
	Enabled,
	Strength,
	Radius,
	Scatter,
	Saturation,
	Tint)
