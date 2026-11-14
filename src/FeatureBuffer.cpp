#include "FeatureBuffer.h"

#include <array>

#include "Features/Bloom.h"
#include "Features/CSUtility.h"
#include "Features/CloudRelight.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/FoliageLighting.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Features/ProceduralSun.h"
#include "Features/RainRefraction.h"
#include "Features/Skin.h"
#include "Features/Skylighting.h"
#include "Features/SnowCover.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/VanillaFresnel.h"
#include "Features/WetnessEffects.h"
#include "Features/Wind/Wind.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/Game.h"

template <class... Ts>
std::pair<unsigned char*, size_t> _GetFeatureBufferData(Ts... feat_datas)
{
	// The packed size is a compile-time constant, so reuse one aligned, thread-local buffer
	// instead of allocating/freeing every UpdateSharedData call. The returned pointer is
	// non-owning and must NOT be deleted by the caller.
	constexpr size_t totalSize = (... + sizeof(Ts));
	alignas(16) static thread_local std::array<unsigned char, totalSize> storage;
	size_t offset = 0;

	([&] {
		*reinterpret_cast<decltype(feat_datas)*>(storage.data() + offset) = feat_datas;
		offset += sizeof(decltype(feat_datas));
	}(),
		...);

	return std::make_pair(storage.data(), storage.size());
}

std::pair<unsigned char*, size_t> GetFeatureBufferData(bool a_inWorld)
{
	const auto& bloomSettings = globals::features::csUtility.settings.bloomEnhancement;
	const auto linearLightingData = globals::features::linearLighting.GetCommonBufferData();
	const auto utilityData = globals::features::csUtility.GetCommonBufferData();
	const auto iblData = globals::features::ibl.PrepareFrameData();
	globals::state->effectAmbientIBLUploaded = utilityData.useAmbientEffectLighting &&
	                                           globals::features::ibl.loaded && iblData.EnableIBL &&
	                                           iblData.DALCMode == 0 && iblData.DALCAmount == 0.0f;
	auto& lightingFlags = globals::state->deferredLightingShaderFlags;
	lightingFlags &= ~SIE::ShaderCache::kLightingLinearACEScg;
	if (linearLightingData.enableLinearLighting && linearLightingData.enableACEScg)
		lightingFlags |= SIE::ShaderCache::kLightingLinearACEScg;

	return _GetFeatureBufferData(
		globals::features::grassLighting.settings,
		globals::features::extendedMaterials.settings,
		globals::features::dynamicCubemaps.settings,
		globals::features::terrainShadows.GetCommonBufferData(),
		globals::features::lightLimitFix.GetCommonBufferData(),
		globals::features::wetnessEffects.PrepareFrameData(),
		globals::features::skylighting.GetCommonBufferData(a_inWorld),
		globals::features::cloudShadows.GetCommonBufferData(),
		globals::features::cloudRelight.GetCommonBufferData(),
		globals::features::proceduralSun.GetCommonBufferData(),
		globals::features::lodBlending.settings,
		globals::features::hairSpecular.settings,
		globals::features::terrainVariation.settings,
		iblData,
		globals::features::extendedTranslucency.GetCommonBufferData(),
		utilityData,
		globals::features::wind.GetCommonBufferData(),
		linearLightingData,
#if defined(ENABLE_EFFECTS11)
		globals::features::effects11.GetCommonBufferData(),
#else
		// Effects11::PerFrame keeps this cbuffer slot's size/offset stable across the
		// ENABLE_EFFECTS11 build gate; the type is declared unconditionally in
		// Effects11.h even though the feature instance only exists when the gate is on.
		Effects11::PerFrame{},
#endif
		globals::features::terrainBlending.settings,
		globals::features::exponentialHeightFog.GetCommonBufferData(),
		globals::features::truePBR.settings,
		globals::features::foliageLighting.GetCommonBufferData(),
		globals::features::skin.GetCommonBufferData(),
		globals::features::vanillaFresnel.settings,
		Bloom::GetCommonBufferData(bloomSettings),
		globals::features::postProcessing.GetCommonBufferData(),
		globals::features::grassCollision.GetCommonBufferData(),
		globals::features::horizonFix.GetCommonBufferData(),
		globals::features::rainRefraction.GetCommonBufferData(),
		globals::features::snowCover.PrepareFrameData(a_inWorld));
}
