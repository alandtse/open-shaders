#pragma once

struct Bloom
{
	struct Settings
	{
		uint Enabled = false;
		float EnhancementIntensity = 1.0f;
		float HaloRadius = 3.5f;
		float HaloSpread = 0.85f;

		float BloomSaturation = 0.9f;
		float3 BloomTint = { 1.0f, 0.98f, 0.94f };
		float CompressionThreshold = 0.0f;
		float CompressionCeiling = 8.0f;
		float2 pad{};
	};
	static_assert(sizeof(Settings) == 48);

	static void DrawSettings(Settings& settings);
	static Settings GetCommonBufferData(Settings settings);
	static void SanitizeSettings(Settings& settings);
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Bloom::Settings,
	Enabled,
	EnhancementIntensity,
	HaloRadius,
	HaloSpread,
	BloomSaturation,
	BloomTint,
	CompressionThreshold,
	CompressionCeiling)
