#include "LinearLighting.h"

#include "../I18n/I18n.h"
#include "Features/PostProcessing.h"
#include "GpuPass.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Util.h"

#if defined(ENABLE_EFFECTS11)
#	include "Effects11.h"
#	include "Effects11/SettingManager.h"
#endif
#include "Globals.h"
#include "Utils/Game.h"

#include <algorithm>
#include <cmath>

#undef GetObject

#define I18N_KEY_PREFIX "feature.linear_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
	enableACEScg,
	ambientMult,
	vanillaDiffuseColorMult)

namespace
{
	constexpr float kAuthoredColorGamma = 1.8f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;

	float ClampFiniteOrDefault(float a_value, float a_default)
	{
		if (!std::isfinite(a_value))
			return a_default;
		return std::clamp(a_value, kMultiplierMin, kMultiplierMax);
	}

	void SanitizeSettings(LinearLighting::Settings& a_settings)
	{
		const LinearLighting::Settings defaults{};
		a_settings.ambientMult = ClampFiniteOrDefault(a_settings.ambientMult, defaults.ambientMult);
		a_settings.vanillaDiffuseColorMult = ClampFiniteOrDefault(a_settings.vanillaDiffuseColorMult, defaults.vanillaDiffuseColorMult);
	}
}

void LinearLighting::DrawSettings()
{
#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
			return;
		}
	}
#endif

	if (Util::CheckboxFlag(T(TKEY("enable"), "Enable Linear Lighting"), settings.enableLinearLighting))
		weatherLightingColorsInitialized = false;
	Util::CheckboxFlag(T(TKEY("enable_acescg"), "Enable ACEScg Wide Gamut"), settings.enableACEScg);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enable_acescg_tooltip"),
							  "Render in ACEScg color space for wider gamut and more accurate lighting.\n"
							  "Requires Linear Lighting and Post Processing enabled.\n"
							  "All sRGB-gamut textures and colors will be converted to ACEScg during shading."));

	ImGui::SliderFloat(T(TKEY("vanilla_diffuse_color_multiplier"), "Vanilla Diffuse Color Multiplier"), &settings.vanillaDiffuseColorMult, kMultiplierMin, kMultiplierMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, kMultiplierMin, kMultiplierMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
}

void LinearLighting::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings(settings);
	weatherLightingColorsInitialized = false;
}

void LinearLighting::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
}

void LinearLighting::RestoreDefaultSettings()
{
	settings = {};
	weatherLightingColorsInitialized = false;
}

void LinearLighting::SetupResources()
{
	PerGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometryData>(), "LinearLighting::PerGeometryCB");
	CompileSceneGammaDecodeShader();
	sceneGammaActive = false;
	weatherLightingColorsInitialized = false;
}

void LinearLighting::ClearShaderCache()
{
	CompileSceneGammaDecodeShader();
}

void LinearLighting::CompileSceneGammaDecodeShader()
{
	sceneGammaDecodeCS = nullptr;
	if (!globals::game::renderer) {
		logger::warn("[LinearLighting] Renderer unavailable; scene gamma decode disabled");
		return;
	}

	auto& mainTarget = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	if (!mainTarget.UAV) {
		logger::warn("[LinearLighting] kMAIN has no UAV; scene gamma decode disabled");
		return;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC mainUAVDesc{};
	Util::AsReal(mainTarget.UAV)->GetDesc(&mainUAVDesc);
	if (!State::SupportsTypedUAVLoad(mainUAVDesc.Format)) {
		logger::warn("[LinearLighting] kMAIN format lacks typed UAV load support; scene gamma decode disabled");
		return;
	}

	sceneGammaDecodeCS.attach(static_cast<ID3D11ComputeShader*>(
		Util::CompileShader(L"Data\\Shaders\\LinearLighting\\SceneGammaDecodeCS.hlsl", {}, "cs_5_0")));
}

void LinearLighting::OnWorldRenderBegin()
{
	sceneGammaActive = false;
	globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);

	if (!globals::state->inWorld || !IsLinearLightingActive())
		return;

	sceneGammaActive = true;
	globals::state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
}

void LinearLighting::OnWorldRenderEnd(RE::RENDER_TARGET a_renderTarget)
{
	if (!globals::state) {
		sceneGammaActive = false;
		return;
	}
	constexpr auto gammaRenderTarget = static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	if (!sceneGammaActive && (globals::state->permutationData.ExtraShaderDescriptor & gammaRenderTarget) == 0)
		return;
	const SKSE::stl::scope_exit completeSceneGamma([this]() noexcept {
		sceneGammaActive = false;
		globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	});
	if (!sceneGammaActive || (globals::state->permutationData.ExtraShaderDescriptor & gammaRenderTarget) == 0)
		return;

	const auto targetIndex = static_cast<size_t>(a_renderTarget);
	if (!sceneGammaDecodeCS || !globals::game::renderer || targetIndex >= static_cast<size_t>(Util::GetRenderTargetCount()))
		return;

	auto& target = globals::game::renderer->GetRuntimeData().renderTargets[targetIndex];
	auto* targetUAV = Util::AsReal(target.UAV);
	if (!targetUAV)
		return;
	if (!globals::d3d::context || !globals::state->sharedDataCB || !globals::state->featureDataCB)
		return;

	uint32_t width = 0;
	uint32_t height = 0;
	if (a_renderTarget == RE::RENDER_TARGET::kMAIN) {
		const auto screenSize = Util::ConvertToDynamic(globals::state->screenSize, true);
		if (!std::isfinite(screenSize.x) || !std::isfinite(screenSize.y) || screenSize.x <= 0.0f || screenSize.y <= 0.0f)
			return;
		width = static_cast<uint32_t>(std::ceil(screenSize.x));
		height = static_cast<uint32_t>(std::ceil(screenSize.y));
	} else {
		if (!target.texture)
			return;
		D3D11_TEXTURE2D_DESC textureDesc{};
		Util::AsReal(target.texture)->GetDesc(&textureDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		targetUAV->GetDesc(&uavDesc);
		if (uavDesc.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D)
			return;
		const auto mipLevel = uavDesc.Texture2D.MipSlice;
		if (mipLevel >= 32 || (textureDesc.MipLevels != 0 && mipLevel >= textureDesc.MipLevels))
			return;
		width = std::max(textureDesc.Width >> mipLevel, 1u);
		height = std::max(textureDesc.Height >> mipLevel, 1u);
	}
	if (width == 0 || height == 0)
		return;

	CS_GPU_PASS("LinearLighting::DecodeScene");
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CSSetShader(sceneGammaDecodeCS.get(), nullptr, 0);
	globals::state->BindSharedDataCS(context);
	context->CSSetUnorderedAccessViews(0, 1, &targetUAV, nullptr);

	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void LinearLighting::OnBeforePostProcessing(RE::RENDER_TARGET a_renderTarget)
{
	if (a_renderTarget == RE::RENDER_TARGET::kMAIN)
		OnWorldRenderEnd(a_renderTarget);
}

std::function<void()> LinearLighting::OnReflectionsRenderBegin()
{
	constexpr auto gammaRenderTarget = static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	auto* const state = globals::state;
	if ((state->permutationData.ExtraShaderDescriptor & gammaRenderTarget) == 0)
		return nullptr;

	state->permutationData.ExtraShaderDescriptor &= ~gammaRenderTarget;
	return [state]() { state->permutationData.ExtraShaderDescriptor |= gammaRenderTarget; };
}

void LinearLighting::Prepass()
{
	dirLightMult = 1.0f;
	if (!IsLinearLightingActive())
		return;

	lodProjectedMaterialColorScales.fill(kNoProjectedMaterialColorScale);
	if (auto* defaultObjects = RE::BGSDefaultObjectManager::GetSingleton(); defaultObjects && globals::features::truePBR.loaded) {
		constexpr std::array materialObjects{ RE::DefaultObjectID::kSnowLODMaterial, RE::DefaultObjectID::kSnowLODMaterialHD };
		for (size_t index = 0; index < materialObjects.size(); ++index) {
			auto** materialObject = defaultObjects->GetObject<RE::BGSMaterialObject>(materialObjects[index]);
			auto* materialData = globals::features::truePBR.GetPBRMaterialObjectData(materialObject ? *materialObject : nullptr);
			if (materialData && std::ranges::all_of(materialData->baseColorScale, [](float value) { return std::isfinite(value); })) {
				std::ranges::transform(materialData->baseColorScale, lodProjectedMaterialColorScales[index].begin(), [](float value) { return std::max(value, 0.0f); });
			}
		}
	}

	auto imageSpaceManager = globals::game::imageSpaceManager;
	if (!imageSpaceManager)
		return;

	dirLightMult = imageSpaceManager->GetImageSpaceData().baseData.hdr.sunlightScale;
}

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::linearLighting.BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[LinearLighting] Installed lighting shader hook");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
	if (!loaded) {
		auto data = PerFrameData{};
		data.enableLinearLighting = false;
		return data;
	}
	auto data = PerFrameData{};
	data.enableLinearLighting = IsLinearLightingActive();
	data.enableACEScg = settings.enableACEScg && data.enableLinearLighting && globals::features::postProcessing.loaded;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
	data.authoredColorGamma = kAuthoredColorGamma;

	Settings sanitizedSettings = settings;
	SanitizeSettings(sanitizedSettings);
	data.vanillaDiffuseColorMult = sanitizedSettings.vanillaDiffuseColorMult;
	data.ambientMult = sanitizedSettings.ambientMult;
	if (data.enableLinearLighting && !weatherLightingColorsInitialized)
		OnWeatherColorsUpdated(globals::game::sky);
	data.effectLightingColor = effectLightingColor;
	data.skyStaticsColor = skyStaticsColor;

	// Override multipliers to neutral values when ENB PP is active
#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.ambientMult = 1.0f;
			data.vanillaDiffuseColorMult = 1.0f;
			data.dirLightMult = 1.0f;
		}
	}
#endif
	return data;
}

bool LinearLighting::IsLinearLightingActive() const
{
	if (!loaded || !settings.enableLinearLighting || !sceneGammaDecodeCS || !globals::state || !globals::shaderCache ||
		!globals::shaderCache->IsEnabled() || globals::state->IsMainOrLoadingMenuOpen())
		return false;

#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded && globals::features::effects11.enableEffect)
		return false;
#endif

	return true;
}

void LinearLighting::OnWeatherColorsUpdated(RE::Sky* a_sky)
{
	if (!a_sky || !IsLinearLightingActive()) {
		weatherLightingColorsInitialized = false;
		return;
	}

	const auto effectLightingSource =
		a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kEffectLighting)];
	const auto skyStaticsSource =
		a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSkyStatics)];
	if (weatherLightingColorsInitialized &&
		effectLightingSource == weatherEffectLightingSource &&
		skyStaticsSource == weatherSkyStaticsSource)
		return;

	weatherEffectLightingSource = effectLightingSource;
	weatherSkyStaticsSource = skyStaticsSource;
	effectLightingColor = DecodeAuthoredColor(effectLightingSource);
	skyStaticsColor = DecodeAuthoredColor(skyStaticsSource);
	weatherLightingColorsInitialized = true;
}

RE::NiColor LinearLighting::DecodeAuthoredColor(RE::NiColor inColor)
{
	RE::NiColor outColor;
	outColor.red = std::pow(inColor.red, kAuthoredColorGamma);
	outColor.green = std::pow(inColor.green, kAuthoredColorGamma);
	outColor.blue = std::pow(inColor.blue, kAuthoredColorGamma);
	return outColor;
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!IsLinearLightingActive() || !PerGeometryCB || !globals::d3d::context)
		return;

	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().shaderProperty;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;
	if (!lightProperty)
		return;

	PerGeometryData perGeometryData{};
	perGeometryData.emissiveMult = lightProperty->emissiveMult;
	const auto descriptor = static_cast<RE::BSLightingShader*>(a_pass->shader)->currentRawTechnique;
	using LightingFlags = SIE::ShaderCache::LightingShaderFlags;
	using enum SIE::ShaderCache::LightingShaderTechniques;
	if ((descriptor & static_cast<uint32_t>(LightingFlags::ProjectedUV)) != 0 && (descriptor & static_cast<uint32_t>(LightingFlags::TruePbr)) == 0) {
		const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((descriptor >> 24) & 0x3F);
		if (technique == LODObjects || technique == LODObjectHD)
			perGeometryData.projectedMaterialColorScale = lodProjectedMaterialColorScales[technique == LODObjectHD ? 1 : 0];
	}
	PerGeometryCB->Update(perGeometryData);

	ID3D11Buffer* buffer = PerGeometryCB->CB();
	globals::d3d::context->PSSetConstantBuffers(8, 1, &buffer);
}

#undef I18N_KEY_PREFIX
