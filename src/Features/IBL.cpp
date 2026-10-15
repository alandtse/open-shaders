#include "IBL.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "LinearLighting.h"
#include "ShaderCache.h"
#include "State.h"

#if defined(ENABLE_EFFECTS11)
#	include "Effects11.h"
#	include "Effects11/SettingManager.h"
#endif
#include "Globals.h"

#include "../I18n/I18n.h"
#include "GpuPass.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>

#define I18N_KEY_PREFIX "feature.ibl."

namespace
{
	constexpr float kIBLScaleMin = 0.0f;
	constexpr float kIBLScaleMax = 2.0f;
	constexpr uint kDALCLuminanceRatioMode = 0;
	constexpr uint kDALCColorRatioMode = 1;
	constexpr uint kDALCPlusSkyMode = 2;
	constexpr uint kDALCPlusSkyDirectionalMode = 3;

	bool IsDALCModeDisabled(const IBL::Settings& settings)
	{
		return settings.DALCAmount <= 0.0f;
	}

	uint GetEffectiveDALCMode(const IBL::Settings& settings)
	{
		if (IsDALCModeDisabled(settings) && settings.DALCMode >= kDALCPlusSkyMode)
			return kDALCLuminanceRatioMode;

		return settings.DALCMode;
	}

	void DrawEnableCheckbox(const char* label, bool& disableSetting)
	{
		bool enableSetting = !disableSetting;
		if (ImGui::Checkbox(label, &enableSetting)) {
			disableSetting = !enableSetting;
		}
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	IBL::Settings,
	EnableIBL,
	PreserveFogLuminance,
	UseStaticIBL,
	DALCAmount,
	EnvIBLScale,
	SkyIBLScale,
	EnvIBLSaturation,
	SkyIBLSaturation,
	FogAmount,
	DALCMode,
	DisableInInteriors,
	DisableInWorldMap,
	DisableInLoadingScreen)

void IBL::DrawSettings()
{
	if (IsManagedByENB()) {
		ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
		return;
	}

	Util::CheckboxFlag(T(TKEY("enable_ibl"), "Enable IBL"), settings.EnableIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_ibl_tooltip"), "Toggle IBL. When enabled, ambient lighting is derived from cubemap spherical harmonics instead of the vanilla system."));
	}
	if (ImGui::TreeNodeEx(T(TKEY("enable_ibl_options"), "Enable IBL Options"), ImGuiTreeNodeFlags_None)) {
		DrawEnableCheckbox(T(TKEY("enable_in_interiors"), "Enable Interior IBL"), settings.DisableInInteriors);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_in_interiors_tooltip"), "Enables IBL in interior cells."));
		}
		DrawEnableCheckbox(T(TKEY("enable_in_world_map"), "Enable World Map IBL"), settings.DisableInWorldMap);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_in_world_map_tooltip"), "Enables IBL while the world map is open."));
		}
		DrawEnableCheckbox(T(TKEY("enable_in_loading_screen"), "Enable Loading Screen IBL"), settings.DisableInLoadingScreen);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_in_loading_screen_tooltip"), "Enables IBL during loading screens and the main menu."));
		}
		ImGui::TreePop();
	}
	ImGui::SliderFloat(T(TKEY("env_ibl_scale"), "Env IBL Scale"), &settings.EnvIBLScale, kIBLScaleMin, kIBLScaleMax, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("env_ibl_scale_tooltip"), "Intensity multiplier for the environment IBL (from Dynamic Cubemaps).\nControls how strongly the surrounding environment contributes to ambient lighting."));
	}
	ImGui::SliderFloat(T(TKEY("sky_ibl_scale"), "Sky IBL Scale"), &settings.SkyIBLScale, kIBLScaleMin, kIBLScaleMax, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("sky_ibl_scale_tooltip"), "Intensity multiplier for the sky IBL (from the game's native reflections cubemap).\nControls how strongly the sky contributes to ambient lighting."));
	}
	ImGui::SliderFloat(T(TKEY("env_ibl_saturation"), "Env IBL Saturation"), &settings.EnvIBLSaturation, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("env_ibl_saturation_tooltip"), "Color saturation of the environment IBL.\nLower values produce more neutral ambient light; higher values produce more vivid color."));
	}
	ImGui::SliderFloat(T(TKEY("sky_ibl_saturation"), "Sky IBL Saturation"), &settings.SkyIBLSaturation, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("sky_ibl_saturation_tooltip"), "Color saturation of the sky IBL.\nLower values produce more neutral ambient light; higher values produce more vivid color."));
	}
	ImGui::SliderFloat(T(TKEY("dalc_amount"), "DALC Amount"), &settings.DALCAmount, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dalc_amount_tooltip"),
							  "Blends the IBL brightness toward the game's vanilla ambient (DALC) level.\n"
							  "0 = no matching (pure IBL brightness), 1 = fully matched to vanilla ambient."));
	}
	{
		struct DALCModeOption
		{
			uint value;
			const char* label;
		};

		const DALCModeOption dalcModeOptions[][2] = {
			{
				{ kDALCLuminanceRatioMode, T(TKEY("dalc_mode_luminance_ratio"), "Luminance Ratio") },
				{ kDALCPlusSkyMode, T(TKEY("dalc_mode_dalc_plus_sky"), "DALC + Sky") },
			},
			{
				{ kDALCColorRatioMode, T(TKEY("dalc_mode_color_ratio"), "Color Ratio") },
				{ kDALCPlusSkyDirectionalMode, T(TKEY("dalc_mode_dalc_plus_sky_directional"), "DALC + Sky (Directional)") },
			},
		};
		int dalcMode = static_cast<int>(settings.DALCMode);

		auto showDALCModeTooltip = []() {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("dalc_mode_tooltip"),
									  "Luminance Ratio: Scalar ratio from overall luminance (loses DALC color tint).\n"
									  "Color Ratio: Per-channel ratio (preserves DALC color tint).\n"
									  "DALC + Sky: Uses vanilla ambient as base, sky IBL on top. Skylighting only affects sky.\n"
									  "DALC + Sky (Directional): Same, but Skylighting also dims vanilla ambient per-direction."));
			}
		};

		const bool dalcModeDisabled = IsDALCModeDisabled(settings);
		const auto disabledGuard = Util::DisableGuard(dalcModeDisabled);
		ImGui::Text("%s:", T(TKEY("dalc_mode"), "DALC Mode"));
		showDALCModeTooltip();
		if (ImGui::BeginTable("##DALCModeOptions", 2, ImGuiTableFlags_SizingStretchSame)) {
			for (const auto& row : dalcModeOptions) {
				for (const auto& option : row) {
					ImGui::TableNextColumn();
					if (ImGui::RadioButton(option.label, &dalcMode, static_cast<int>(option.value))) {
						settings.DALCMode = static_cast<uint>(dalcMode);
					}
					showDALCModeTooltip();
				}
			}
			ImGui::EndTable();
		}
	}
	Util::CheckboxFlag(T(TKEY("use_static_ibl"), "Use Static IBL For Out-of-World Objects"), settings.UseStaticIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_static_ibl_tooltip"), "Uses pre-baked static IBL cubemap textures for objects rendered outside the game world (e.g. inventory items, loading screens)."));
	}
	ImGui::SliderFloat(T(TKEY("fog_mix"), "Fog Mix"), &settings.FogAmount, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("fog_mix_tooltip"), "Blends the fog color toward the IBL ambient color.\n0 = vanilla fog, 1 = fog fully tinted by IBL."));
	}
	Util::CheckboxFlag(T(TKEY("preserve_fog_luminance"), "Preserve Fog Luminance"), settings.PreserveFogLuminance);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("preserve_fog_luminance_tooltip"), "When Fog Mix is active, rescales the IBL-tinted fog to keep the original fog brightness.\nPrevents fog from becoming too bright or too dark."));
	}
}

#undef I18N_KEY_PREFIX

void IBL::LoadSettings(json& o_json)
{
	settings = o_json;
}

void IBL::SaveSettings(json& o_json)
{
	o_json = settings;
}

void IBL::RestoreDefaultSettings()
{
	settings = {};
}

IBL::PerFrame IBL::GetCommonBufferData() const
{
	const bool sceneDisabled = IsDisabledForCurrentScene();
	PerFrame data = {
		.EnableIBL = sceneDisabled ? 0u : settings.EnableIBL,
		.PreserveFogLuminance = settings.PreserveFogLuminance,
		.UseStaticIBL = settings.UseStaticIBL,
		.DALCAmount = settings.DALCAmount,
		.EnvIBLScale = settings.EnvIBLScale,
		.SkyIBLScale = settings.SkyIBLScale,
		.EnvIBLSaturation = settings.EnvIBLSaturation,
		.SkyIBLSaturation = settings.SkyIBLSaturation,
		.FogAmount = settings.FogAmount,
		.DALCMode = GetEffectiveDALCMode(settings)
	};

#if defined(ENABLE_EFFECTS11)
	if (!sceneDisabled && IsManagedByENB()) {
		auto& settingManager = SettingManager::GetSingleton();
		data.EnableIBL = Util::IsInterior() ? 0u : 1u;
		data.EnvIBLScale = 0.0f;
		data.SkyIBLScale = settingManager.GetInterpolatedTimeOfDayValue("MultiplicativeAmount", "IMAGEBASEDLIGHTING");
		data.DALCAmount = 1.0f;
		data.EnvIBLSaturation = 1.0f;
		data.SkyIBLSaturation = 1.0f;
		data.DALCMode = kDALCPlusSkyDirectionalMode;
		data.FogAmount = 0.0f;
	}
#endif

	const bool useStatic = settings.UseStaticIBL && !globals::state->inWorld && !globals::state->activeReflections;
	if (!useStatic && !dynamicIBLValid)
		data.EnableIBL = 0;

	return data;
}

bool IBL::IsManagedByENB() const
{
#if defined(ENABLE_EFFECTS11)
	auto& enb = globals::features::effects11;
	if (!enb.loaded || !enb.enableEffect)
		return false;
	return SettingManager::GetSingleton().GetValue<bool>("EnableImageBasedLighting", "EFFECT");
#else
	return false;
#endif
}

bool IBL::IsDisabledForCurrentScene() const
{
	const auto state = globals::state;
	if (!state)
		return false;

	if (state->IsMainOrLoadingMenuOpen())
		return settings.DisableInLoadingScreen;

	if (state->isMapMenuOpen)
		return settings.DisableInWorldMap;

	return settings.DisableInInteriors && Util::IsInterior();
}

void IBL::ReflectionsPrepass()
{
	if (loaded) {
		auto context = globals::d3d::context;

		bool sceneDisabled = IsDisabledForCurrentScene();
		// dynamicIBLValid is false when the diffuse IBL compute shader failed to
		// compile -- envIBLTexture/skyIBLTexture were never written this frame.
		bool bindDynamic = !sceneDisabled && dynamicIBLValid;

		// Set PS shader resource
		{
			std::array<ID3D11ShaderResourceView*, 4> srvs = {
				bindDynamic ? envIBLTexture->srv.get() : nullptr,
				bindDynamic ? skyIBLTexture->srv.get() : nullptr,
				staticDiffuseIBLTexture ? staticDiffuseIBLTexture->srv.get() : nullptr,
				staticSpecularIBLTexture ? staticSpecularIBLTexture->srv.get() : nullptr
			};
			context->PSSetShaderResources(76, 4, srvs.data());
		}
	}
}

std::function<void()> IBL::OnReflectionsRenderBegin()
{
	if (!reflectionRadiance || !globals::shaderCache->IsEnabled() || IsDisabledForCurrentScene() ||
		Util::IsInterior() || (!settings.EnableIBL && !IsManagedByENB())) {
		validFaces = 0;
		return nullptr;
	}

	const auto lighting = globals::features::linearLighting.GetCommonBufferData();
	const std::array<uint, 2> colorSpace{ lighting.enableLinearLighting, lighting.enableACEScg };
	if (captureColorSpace != colorSpace) {
		validFaces = 0;
		captureColorSpace = colorSpace;
	}
	clearedFaces = 0;
	captureFailed = false;
	capturingRadiance = true;
	const auto flag = static_cast<uint32_t>(State::ExtraShaderDescriptors::CaptureReflectionRadiance);
	globals::state->permutationData.ExtraShaderDescriptor |= flag;
	return [this, flag]() {
		RestoreCaptureTargets();
		globals::state->permutationData.ExtraShaderDescriptor &= ~flag;
		capturingRadiance = false;
		validFaces = captureFailed ? 0 : validFaces | clearedFaces;
	};
}

void IBL::OnSceneTransitionReset(bool)
{
	validFaces = 0;
}

json IBL::GetDiagnostics()
{
	return {
		{ "radianceValidFaces", validFaces },
		{ "radianceCaptureFailed", captureFailed },
		{ "radianceSize", reflectionRadiance ? reflectionRadiance->desc.Width : 0 },
		{ "dynamicIBLValid", dynamicIBLValid }
	};
}

std::function<void()> IBL::OnRenderPassBegin(const RE::BSRenderPass*)
{
	if (!capturingRadiance)
		return nullptr;
	return [this]() { RestoreCaptureTargets(); };
}

void IBL::OnDraw()
{
	if (!capturingRadiance)
		return;
	auto* context = globals::d3d::context;
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
	std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> currentTargets;
	context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
	for (UINT index = 0; index < targets.size(); ++index)
		currentTargets[index].attach(targets[index]);
	const auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
	for (UINT face = 0; face < radianceViews.size(); ++face) {
		if (targets[0] != Util::AsReal(reflections.cubeSideRTV[face]))
			continue;
		auto* currentShader = globals::state->currentShader;
		if (!currentShader || (currentShader->shaderType.get() != RE::BSShader::Type::Lighting &&
								  currentShader->shaderType.get() != RE::BSShader::Type::Sky &&
								  currentShader->shaderType.get() != RE::BSShader::Type::DistantTree &&
								  currentShader->shaderType.get() != RE::BSShader::Type::Effect)) {
			captureFailed = true;
			return;
		}
		auto* shader = globals::shaderCache->GetPixelShader(*currentShader, globals::state->modifiedPixelDescriptor);
		winrt::com_ptr<ID3D11PixelShader> boundShader;
		context->PSGetShader(boundShader.put(), nullptr, nullptr);
		if (!shader || Util::AsReal(shader->shader) != boundShader.get()) {
			captureFailed = true;
			return;
		}
		winrt::com_ptr<ID3D11BlendState> currentBlend;
		std::array<float, 4> currentBlendFactor{};
		UINT currentSampleMask = 0;
		context->OMGetBlendState(currentBlend.put(), currentBlendFactor.data(), &currentSampleMask);
		const bool radianceTargetBound = captureTargetBound && targets[kRadianceTarget] == radianceViews[face].get();
		if (radianceTargetBound && currentBlend.get() == boundRadianceBlend)
			return;
		if (currentBlend.get() == boundRadianceBlend)
			currentBlend = savedBlend;
		if (!radianceTargetBound)
			RestoreCaptureTargets();
		if (targets[kRadianceTarget] && !radianceTargetBound) {
			captureFailed = true;
			return;
		}
		savedBlend = currentBlend;
		savedBlendFactor = currentBlendFactor;
		savedSampleMask = currentSampleMask;
		auto& entry = captureBlendStates[savedBlend.get()];
		if (!entry.capture) {
			entry.source = savedBlend;
			D3D11_BLEND_DESC desc{};
			if (savedBlend)
				savedBlend->GetDesc(&desc);
			else
				desc.RenderTarget[0] = { FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
					D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
			if (!desc.IndependentBlendEnable)
				for (auto& target : desc.RenderTarget)
					target = desc.RenderTarget[0];
			desc.IndependentBlendEnable = TRUE;
			desc.RenderTarget[kRadianceTarget] = desc.RenderTarget[0];
			if (FAILED(globals::d3d::device->CreateBlendState(&desc, entry.capture.put()))) {
				captureFailed = true;
				return;
			}
			Util::SetResourceName(entry.capture.get(), "IBL::ReflectionRadiance Blend");
		}
		const UINT faceBit = 1u << face;
		if (!(clearedFaces & faceBit)) {
			CS_GPU_PASS("IBL::ClearReflectionRadiance");
			context->ClearRenderTargetView(radianceViews[face].get(), globals::game::renderer->GetRendererData().clearColor);
			clearedFaces |= faceBit;
		}
		savedTargets = currentTargets;
		savedTargets[kRadianceTarget] = nullptr;
		savedDepth = nullptr;
		context->OMGetRenderTargets(0, nullptr, savedDepth.put());
		targets[kRadianceTarget] = radianceViews[face].get();
		context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), savedDepth.get());
		context->OMSetBlendState(entry.capture.get(), savedBlendFactor.data(), savedSampleMask);
		boundRadianceBlend = entry.capture.get();
		captureTargetBound = true;
		return;
	}
}

void IBL::RestoreCaptureTargets()
{
	if (captureTargetBound) {
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		globals::d3d::context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
		const bool captureStillBound = std::any_of(radianceViews.begin(), radianceViews.end(),
			[&](const auto& view) { return view.get() == targets[kRadianceTarget]; });
		for (auto* target : targets)
			if (target)
				target->Release();
		if (captureStillBound) {
			for (UINT index = 0; index < targets.size(); ++index)
				targets[index] = savedTargets[index].get();
			globals::d3d::context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), savedDepth.get());
			globals::d3d::context->OMSetBlendState(savedBlend.get(), savedBlendFactor.data(), savedSampleMask);
		}
	}
	captureTargetBound = false;
	boundRadianceBlend = nullptr;
	for (auto& target : savedTargets)
		target = nullptr;
	savedDepth = nullptr;
	savedBlend = nullptr;
}

void IBL::Prepass()
{
	if (IsDisabledForCurrentScene())
		return;

	auto context = globals::d3d::context;

	auto& dynamicCubemaps = globals::features::dynamicCubemaps;

	auto& envTexture = dynamicCubemaps.envTexture;

	// Unset PS shader resource
	{
		ID3D11ShaderResourceView* views[2]{ nullptr, nullptr };
		context->PSSetShaderResources(76, 2, views);
	}

	std::array<ID3D11ShaderResourceView*, 1> srvs = { dynamicCubemaps.loaded && envTexture ? envTexture->srv.get() : nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { envIBLTexture->uav.get() };
	std::array<ID3D11SamplerState*, 1> samplers = { Deferred::GetSingleton()->linearSampler };

	auto* diffuseIBLShader = GetDiffuseIBLCS();
	dynamicIBLValid = diffuseIBLShader != nullptr;

	// IBL - Environment cubemap SH projection (skip for DALC-based modes that don't use EnvIBL)
	if (GetEffectiveDALCMode(settings) < kDALCPlusSkyMode) {
		samplers[0] = Deferred::GetSingleton()->linearSampler;

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		if (diffuseIBLShader) {
			context->CSSetShader(diffuseIBLShader, nullptr, 0);
			{
				CS_GPU_PASS("IBL::EnvDiffuseIBL");
				context->Dispatch(1, 1, 1);
			}
		}
	} else {
		// Still need to set sampler and shader for sky IBL dispatch below
		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		if (diffuseIBLShader)
			context->CSSetShader(diffuseIBLShader, nullptr, 0);
	}

	// Reflections retain their shadows while ambient uses the same unshadowed scene radiance.
	{
		const auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		srvs.at(0) = validFaces == kAllCubeFaces ? reflectionRadiance->srv.get() : Util::AsReal(reflections.SRV);
		uavs.at(0) = skyIBLTexture->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		if (diffuseIBLShader) {
			context->CSSetShader(diffuseIBLShader, nullptr, 0);
			CS_GPU_PASS("IBL::SkyDiffuseIBL");
			context->Dispatch(1, 1, 1);
		}
	}

	// Reset
	{
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		samplers.fill(nullptr);

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// Set PS shader resource
	{
		ID3D11ShaderResourceView* views[2]{ envIBLTexture->srv.get(), skyIBLTexture->srv.get() };
		context->PSSetShaderResources(76, 2, views);
	}
}

void IBL::SetupResources()
{
	GetDiffuseIBLCS();
	validFaces = 0;
	capturingRadiance = false;
	{
		const auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		D3D11_TEXTURE2D_DESC desc{};
		Util::AsReal(reflections.texture)->GetDesc(&desc);
		desc.MipLevels = 1;
		desc.ArraySize = kCubeFaces;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		reflectionRadiance = eastl::make_unique<Texture2D>(desc, "IBL::ReflectionRadiance");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = desc.Format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
		srv.TextureCube.MipLevels = 1;
		reflectionRadiance->CreateSRV(srv);
		for (UINT face = 0; face < radianceViews.size(); ++face) {
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = desc.Format;
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.FirstArraySlice = face;
			rtv.Texture2DArray.ArraySize = 1;
			DX::ThrowIfFailed(globals::d3d::device->CreateRenderTargetView(reflectionRadiance->resource.get(), &rtv, radianceViews[face].put()));
			Util::SetResourceName(radianceViews[face].get(), "IBL::ReflectionRadiance Face %u RTV", face);
		}
	}

	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 3,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		envIBLTexture = new Texture2D(texDesc, "IBL::EnvironmentIrradiance");
		envIBLTexture->CreateSRV(srvDesc);
		envIBLTexture->CreateUAV(uavDesc);
		skyIBLTexture = new Texture2D(texDesc, "IBL::SkyIrradiance");
		skyIBLTexture->CreateSRV(srvDesc);
		skyIBLTexture->CreateUAV(uavDesc);
	}

	auto device = globals::d3d::device;

	logger::debug("Loading static Diffuse IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\DiffuseIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		staticDiffuseIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "IBL::StaticDiffuse");

		staticDiffuseIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticDiffuseIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		staticDiffuseIBLTexture->CreateSRV(srvDesc);
	}

	logger::debug("Loading static Specular IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\SpecIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		staticSpecularIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "IBL::StaticSpecular");

		staticSpecularIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticSpecularIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 8 }
		};
		staticSpecularIBLTexture->CreateSRV(srvDesc);
	}
}

void IBL::ClearShaderCache()
{
	diffuseIBLCS.Reset();
	dynamicIBLValid = false;
	validFaces = 0;
}

ID3D11ComputeShader* IBL::GetDiffuseIBLCS()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });
	return diffuseIBLCS.Get(L"Data\\Shaders\\IBL\\DiffuseIBLCS.hlsl", defines, "cs_5_0");
}
