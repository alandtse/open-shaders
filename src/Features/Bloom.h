#pragma once

struct Bloom
{
	struct Profile
	{
		float EnhancementIntensity = 1.0f;
		float HaloRadius = 3.5f;
		float HaloSpread = 0.85f;

		float BloomSaturation = 0.9f;
		float3 BloomTint = { 1.0f, 0.98f, 0.94f };
		float CompressionThreshold = 0.0f;
		float CompressionCeiling = 8.0f;
	};

	struct PresetSettings
	{
		uint Enabled = false;
		uint SelectedPreset = 0;
		Profile Default;
		Profile Fantasy = { 4.0f, 14.0f, 1.0f, 1.15f, { 1.0f, 0.98f, 0.94f }, 0.0f, 0.67f };
		Profile Dreamy = { 2.5f, 14.0f, 0.72f, 0.85f, { 165.0f / 255.0f, 205.0f / 255.0f, 1.0f }, 0.08f, 0.9f };
	};

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

	static void DrawSettings(PresetSettings& settings);
	static Settings GetCommonBufferData(const PresetSettings& settings);
	static void SanitizeSettings(PresetSettings& settings);
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Bloom::Profile,
	EnhancementIntensity,
	HaloRadius,
	HaloSpread,
	BloomSaturation,
	BloomTint,
	CompressionThreshold,
	CompressionCeiling)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Bloom::PresetSettings,
	Enabled,
	SelectedPreset,
	Default,
	Fantasy,
	Dreamy)
