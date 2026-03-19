#include "FeatureBuffer.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassLighting.h"
#include "Features/HairSpecular.h"
#include "Features/IBL.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/WetnessEffects.h"

#include "TruePBR.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <tuple>
#include <type_traits>

namespace
{
	using GrassLightingSettingsCB = GrassLighting::Settings;
	using ExtendedMaterialsSettingsCB = ExtendedMaterials::Settings;
	using DynamicCubemapsSettingsCB = DynamicCubemaps::Settings;
	using TerrainShadowsSettingsCB = TerrainShadows::PerFrame;
	using LightLimitFixSettingsCB = LightLimitFix::PerFrame;
	using WetnessEffectsSettingsCB = WetnessEffects::PerFrame;
	using SkylightingSettingsCB = Skylighting::SkylightingCB;
	using CloudShadowsSettingsCB = CloudShadows::Settings;
	using LODBlendingSettingsCB = LODBlending::Settings;
	using HairSpecularSettingsCB = HairSpecular::Settings;
	using TerrainVariationSettingsCB = TerrainVariation::Settings;
	using IBLSettingsCB = IBL::CommonBufferData;
	using ExtendedTranslucencySettingsCB = ExtendedTranslucency::PerFrame;
	using LinearLightingSettingsCB = LinearLighting::PerFrameData;
	using TerrainBlendingSettingsCB = TerrainBlending::Settings;

	// Keep these in lock-step with package/Shaders/Common/SharedData.hlsli::FeatureData.
	struct FeatureDataLayout
	{
		GrassLightingSettingsCB grassLightingSettings;
		ExtendedMaterialsSettingsCB extendedMaterialSettings;
		DynamicCubemapsSettingsCB cubemapCreatorSettings;
		TerrainShadowsSettingsCB terraOccSettings;
		LightLimitFixSettingsCB lightLimitFixSettings;
		WetnessEffectsSettingsCB wetnessEffectsSettings;
		SkylightingSettingsCB skylightingSettings;
		CloudShadowsSettingsCB cloudShadowsSettings;
		LODBlendingSettingsCB lodBlendingSettings;
		HairSpecularSettingsCB hairSpecularSettings;
		TerrainVariationSettingsCB terrainVariationSettings;
		IBLSettingsCB iblSettings;
		ExtendedTranslucencySettingsCB extendedTranslucencySettings;
		LinearLightingSettingsCB linearLightingSettings;
		TerrainBlendingSettingsCB terrainBlendingSettings;
	};

	using FeatureDataTuple = std::tuple<
		GrassLightingSettingsCB,
		ExtendedMaterialsSettingsCB,
		DynamicCubemapsSettingsCB,
		TerrainShadowsSettingsCB,
		LightLimitFixSettingsCB,
		WetnessEffectsSettingsCB,
		SkylightingSettingsCB,
		CloudShadowsSettingsCB,
		LODBlendingSettingsCB,
		HairSpecularSettingsCB,
		TerrainVariationSettingsCB,
		IBLSettingsCB,
		ExtendedTranslucencySettingsCB,
		LinearLightingSettingsCB,
		TerrainBlendingSettingsCB>;

	static_assert(sizeof(GrassLightingSettingsCB) == 32);
	static_assert(sizeof(ExtendedMaterialsSettingsCB) == 32);
	static_assert(sizeof(DynamicCubemapsSettingsCB) == 32);
	static_assert(sizeof(TerrainShadowsSettingsCB) == 32);
	static_assert(sizeof(LightLimitFixSettingsCB) == 32);
	static_assert(sizeof(WetnessEffectsSettingsCB) == 192);
	static_assert(sizeof(SkylightingSettingsCB) == 160);
	static_assert(sizeof(CloudShadowsSettingsCB) == 16);
	static_assert(sizeof(LODBlendingSettingsCB) == 32);
	static_assert(sizeof(HairSpecularSettingsCB) == 80);
	static_assert(sizeof(TerrainVariationSettingsCB) == 16);
	static_assert(sizeof(IBLSettingsCB) == 32);
	static_assert(sizeof(ExtendedTranslucencySettingsCB) == 16);
	static_assert(sizeof(LinearLightingSettingsCB) == 112);
	static_assert(sizeof(TerrainBlendingSettingsCB) == 16);

	static_assert(std::is_standard_layout_v<FeatureDataLayout>);
	static_assert(std::is_trivially_copyable_v<FeatureDataLayout>);
	static_assert(sizeof(FeatureDataLayout) % 16 == 0);
	static_assert(offsetof(FeatureDataLayout, grassLightingSettings) == 0);
	static_assert(offsetof(FeatureDataLayout, extendedMaterialSettings) == sizeof(GrassLightingSettingsCB));
	static_assert(offsetof(FeatureDataLayout, cubemapCreatorSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB));
	static_assert(offsetof(FeatureDataLayout, terraOccSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB));
	static_assert(offsetof(FeatureDataLayout, lightLimitFixSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB));
	static_assert(offsetof(FeatureDataLayout, wetnessEffectsSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB));
	static_assert(offsetof(FeatureDataLayout, skylightingSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB));
	static_assert(offsetof(FeatureDataLayout, cloudShadowsSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB));
	static_assert(offsetof(FeatureDataLayout, lodBlendingSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB));
	static_assert(offsetof(FeatureDataLayout, hairSpecularSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB));
	static_assert(offsetof(FeatureDataLayout, terrainVariationSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB) + sizeof(HairSpecularSettingsCB));
	static_assert(offsetof(FeatureDataLayout, iblSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB) + sizeof(HairSpecularSettingsCB) + sizeof(TerrainVariationSettingsCB));
	static_assert(offsetof(FeatureDataLayout, extendedTranslucencySettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB) + sizeof(HairSpecularSettingsCB) + sizeof(TerrainVariationSettingsCB) + sizeof(IBLSettingsCB));
	static_assert(offsetof(FeatureDataLayout, linearLightingSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB) + sizeof(HairSpecularSettingsCB) + sizeof(TerrainVariationSettingsCB) + sizeof(IBLSettingsCB) + sizeof(ExtendedTranslucencySettingsCB));
	static_assert(offsetof(FeatureDataLayout, terrainBlendingSettings) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB) + sizeof(HairSpecularSettingsCB) + sizeof(TerrainVariationSettingsCB) + sizeof(IBLSettingsCB) + sizeof(ExtendedTranslucencySettingsCB) + sizeof(LinearLightingSettingsCB));
	static_assert(sizeof(FeatureDataLayout) == sizeof(GrassLightingSettingsCB) + sizeof(ExtendedMaterialsSettingsCB) + sizeof(DynamicCubemapsSettingsCB) + sizeof(TerrainShadowsSettingsCB) + sizeof(LightLimitFixSettingsCB) + sizeof(WetnessEffectsSettingsCB) + sizeof(SkylightingSettingsCB) + sizeof(CloudShadowsSettingsCB) + sizeof(LODBlendingSettingsCB) + sizeof(HairSpecularSettingsCB) + sizeof(TerrainVariationSettingsCB) + sizeof(IBLSettingsCB) + sizeof(ExtendedTranslucencySettingsCB) + sizeof(LinearLightingSettingsCB) + sizeof(TerrainBlendingSettingsCB));

	template <class T>
	void PackField(unsigned char* a_dst, size_t& a_offset, const T& a_value)
	{
		static_assert(std::is_standard_layout_v<T>);
		static_assert(std::is_trivially_copyable_v<T>);
		std::memcpy(a_dst + a_offset, std::addressof(a_value), sizeof(T));
		a_offset += sizeof(T);
	}

	template <class... Ts>
	std::pair<unsigned char*, size_t> BuildFeatureBufferData(const Ts&... a_fields)
	{
		using PackedTuple = std::tuple<std::remove_cv_t<std::remove_reference_t<Ts>>...>;
		static_assert(std::is_same_v<PackedTuple, FeatureDataTuple>, "FeatureData packing order/type mismatch");

		constexpr size_t totalSize = (sizeof(Ts) + ...);
		static_assert(totalSize % 16 == 0);
		static_assert(totalSize == sizeof(FeatureDataLayout));

		auto data = std::make_unique<unsigned char[]>(totalSize);
		// Start from a deterministic payload; this avoids stale bytes in any untouched padding.
		std::memset(data.get(), 0, totalSize);

		size_t offset = 0;
		(PackField(data.get(), offset, a_fields), ...);

		return std::make_pair(data.release(), totalSize);
	}
}


std::pair<unsigned char*, size_t> GetFeatureBufferData(bool a_inWorld)
{
	return BuildFeatureBufferData(
		globals::features::grassLighting.settings,
		globals::features::extendedMaterials.settings,
		globals::features::dynamicCubemaps.settings,
		globals::features::terrainShadows.GetCommonBufferData(),
		globals::features::lightLimitFix.GetCommonBufferData(),
		globals::features::wetnessEffects.GetCommonBufferData(),
		globals::features::skylighting.GetCommonBufferData(a_inWorld),
		globals::features::cloudShadows.settings,
		globals::features::lodBlending.settings,
		globals::features::hairSpecular.settings,
		globals::features::terrainVariation.settings,
		globals::features::ibl.GetCommonBufferData(),
		globals::features::extendedTranslucency.GetCommonBufferData(),
		globals::features::linearLighting.GetCommonBufferData(),
		globals::features::terrainBlending.settings);
}
