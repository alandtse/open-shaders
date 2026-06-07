#pragma once

#include "Utils/BootSnapshot.h"

struct VolumetricLighting : Feature
{
public:
	struct TextureSize
	{
		int32_t Width = 320;
		int32_t Height = 192;
		int32_t Depth = 90;
	};

	struct Settings
	{
		bool ExteriorEnabled = true;
		int32_t ExteriorQuality = 2;
		TextureSize ExteriorCustomSize;
		bool InteriorEnabled = true;
		int32_t InteriorQuality = 2;
		TextureSize InteriorCustomSize;
	};

	Settings settings;

	inline static constexpr Util::Settings::RestartTable<Settings, 2> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, ExteriorEnabled, "Volumetric Lighting (Exterior)"),
		UTIL_RESTART_FIELD(Settings, InteriorEnabled, "Volumetric Lighting (Interior)"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override
	{
		// VR-only: enabling VL relies on startup-only game setting initialization.
		return globals::game::isVR ? std::span<const Util::Settings::RestartFieldInfo>{ kRestartFields.data(), kRestartFields.size() } : std::span<const Util::Settings::RestartFieldInfo>{};
	}
	const void* GetBootValue(std::string_view jsonKey) const override { return bootSnapshot.RawBoot(jsonKey); }
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }

	virtual inline std::string GetName() override { return "Volumetric Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.volumetric_lighting.name", "Volumetric Lighting"); }
	virtual inline std::string GetShortName() override { return "VolumetricLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.volumetric_lighting.description", "Volumetric Lighting creates realistic light scattering effects through fog, dust, and atmospheric particles.\nThis adds dramatic god rays and atmospheric depth to both interior and exterior environments."),
			{ T("feature.volumetric_lighting.key_feature_1", "Realistic light scattering"),
				T("feature.volumetric_lighting.key_feature_2", "God rays and atmospheric effects"),
				T("feature.volumetric_lighting.key_feature_3", "Separate interior/exterior settings"),
				T("feature.volumetric_lighting.key_feature_4", "Configurable quality levels"),
				T("feature.volumetric_lighting.key_feature_5", "Enhanced atmospheric immersion") } };
	};

	virtual void SaveSettings(json&) override;
	virtual void LoadSettings(json&) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void DataLoaded() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;
	virtual void EarlyPrepass() override;

	virtual bool IsCore() const override { return true; };

	static RE::BSImagespaceShader* CreateShader(const std::string_view& name, const std::string_view& fileName, RE::BSComputeShader* computeShader);
	RE::BSImagespaceShader* GetOrCreateGenerateCS(RE::BSComputeShader* computeShader);
	RE::BSImagespaceShader* GetOrCreateRaymarchCS(RE::BSComputeShader* computeShader);
	RE::BSImagespaceShader* GetOrCreateBlurHCS(RE::BSComputeShader* computeShader);
	RE::BSImagespaceShader* GetOrCreateBlurVCS(RE::BSComputeShader* computeShader);
	void SetDimensionsCB() const;
	void SetGroupCountsHCS(uint32_t& threadGroupCountX) const;
	void SetGroupCountsVCS(uint32_t& threadGroupCountY) const;

private:
	struct VolumetricLightingDescriptor
	{};

	static const char* FromUnits(int32_t value, int32_t unitScale);
	static VolumetricLightingDescriptor& GetVLDescriptor();
	static void SetVLQuality(VolumetricLightingDescriptor& descriptor, std::uint32_t quality);
	void DrawVolumetricLightingSettings(int32_t& quality, TextureSize& customSize, bool isInterior, bool inLocationType);
	TextureSize& FetchCurrentSizeInUnits(bool interior);
	void SetupVL();

	enum class Quality : uint8_t
	{
		Low,
		Medium,
		High,
		Custom,
		Count
	};

	const char* QualityNames[static_cast<uint8_t>(Quality::Count)] = { "Low", "Medium", "High", "Custom" };

	TextureSize exteriorSizeInUnits;
	TextureSize interiorSizeInUnits;
	TextureSize defaultSizeHigh;

	bool* bEnableVolumetricLighting = nullptr;
	TextureSize* gVolumetricLightingSizeHigh = nullptr;
	TextureSize* gVolumetricLightingSizeMedium = nullptr;
	TextureSize* gVolumetricLightingSizeLow = nullptr;

	bool initialised = false;
	bool inInterior = false;
	bool inInteriorWithSun = false;

	struct VLData
	{
		int32_t screenX;
		int32_t screenY;
		int32_t screenXMin1;
		int32_t screenYMin1;
	};
	VLData vlData = VLData();
	ConstantBuffer* vlDataCB = nullptr;

	static constexpr int32_t BlurThreadGroupSizeX = 256;
	static constexpr int32_t BlurThreadGroupSizeY = 256;
	static constexpr int32_t BlurWindow = 12;

	RE::BSImagespaceShader* generateCS = nullptr;
	RE::BSImagespaceShader* raymarchCS = nullptr;
	RE::BSImagespaceShader* blurHCS = nullptr;
	RE::BSImagespaceShader* blurVCS = nullptr;
};
