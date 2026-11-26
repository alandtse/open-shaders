#include "ScreenSpaceGI.h"
#include "NRDPortStereo.h"
#include "ScreenSpaceReflections.h"

#include <DirectXTex.h>

#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "Effects11.h"
#include "Effects11/SettingManager.h"
#include "I18n/I18n.h"
#include "IBL.h"
#include "NRD.h"
#include "ReverseZ.h"
#include "Skylighting.h"
#include "State.h"
#include "TerrainBlending.h"
#include "Upscaling.h"
#include "Util.h"
#include "GpuPass.h"
#include "Utils/MathUtils.h"
#include "Utils/DebugSection.h"

namespace { bool AllowsAmbientFallback(); }

#define I18N_KEY_PREFIX "feature.screen_space_gi."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableVanillaSSAO,
	EnableSH,
	NumSteps,
	HalfRes,
	QuarterRes,
	Thickness,
	AOPower,
	GIStrength,
	ILSaturation,
	EnableMultiBounce,
	UseDynamicCubemapsAsFallback,
	DiffuseCubemapMult,
	EnableREBLUR,
	Reblur,
	DiffusePrepassBlurRadius,
	E11AOScale,
	E11ILScale,
	E11ILSaturation)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
	if (globals::game::isVR) settings.EnableGI = false;
	userSettings = settings;
	appliedE11Overlay = GetE11Overlay();
	if (appliedE11Overlay.active) {
		settings.Enabled = appliedE11Overlay.enabled;
		settings.EnableGI = appliedE11Overlay.indirectLighting;
		settings.AOPower = appliedE11Overlay.aoPower;
		settings.GIStrength = appliedE11Overlay.giStrength;
	}
	recompileFlag = true;
	QueueHistoryReset();
	globals::features::screenSpaceReflections.QueueHistoryReset();
	resetReblurHistory = true;
	outputReady = false;
	denoisedThisFrame = false;
	hasMultiBounceHistory = false;
	hasFullResolutionMultiBounceHistory = false;
	multiBounceHistoryUsesNRDOutput = false;
}

void ScreenSpaceGI::DrawSettings()
{
	static bool showAdvanced;
	const auto syncUserSettings = SKSE::stl::scope_exit([this]() noexcept {
		Settings edited = settings;
		if (appliedE11Overlay.active) {
			edited.Enabled = userSettings.Enabled;
			edited.EnableGI = userSettings.EnableGI;
			edited.AOPower = userSettings.AOPower;
			edited.GIStrength = userSettings.GIStrength;
		}
		userSettings = edited;
	});

	if (!ShadersOK())
		Util::Text::Error("%s", T(TKEY("compute_shaders_failed_to_compile"), "Compute shaders failed to compile!"));

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("toggles"), "Toggles"));

	ImGui::Checkbox(T(TKEY("show_advanced_options"), "Show Advanced Options"), &showAdvanced);

	const bool e11Managed = appliedE11Overlay.active;
	if (e11Managed)
		Util::Text::WrappedInfo("%s", T(TKEY("e11_managed"), "The active Effects 11 preset drives SSGI: its Ambient Occlusion and Indirect Lighting settings set Enabled, Indirect Lighting, AO Power and IL Brightness. Hand SSGI back on its scene page to use your own values."));

	if (ImGui::BeginTable("Toggles", 4)) {
		ImGui::TableNextColumn();
		{
			auto enabledGuard = Util::DisableGuard(e11Managed);
			if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled)) {
				QueueHistoryReset();
				globals::features::screenSpaceReflections.QueueHistoryReset();
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Enable Screen Space Global Illumination. When disabled, all other settings are ignored."));
		}

		ImGui::TableNextColumn();
		{
			auto ilToggleGuard = Util::DisableGuard(!settings.Enabled || e11Managed);
			if (ImGui::Checkbox(T(TKEY("indirect_lighting"), "Indirect Lighting (IL)"), &settings.EnableGI)) {
				recompileFlag = true;
				SetupNRDResources();
			}
		}
		ImGui::TableNextColumn();
		{
			auto shGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI);
			if (ImGui::Checkbox(T(TKEY("sh_mode"), "SH Mode"), &settings.EnableSH)) {
				recompileFlag = true;
				SetupNRDResources();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("sh_mode_tooltip"), "Use Spherical Harmonics for directional GI. Higher quality but more expensive."));
			}
		}
		ImGui::TableNextColumn();
		{
			ImGui::Checkbox(T(TKEY("vanilla_ssao"), "Vanilla SSAO"), &settings.EnableVanillaSSAO);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("vanilla_ssao_tooltip"), "Enable Skyrim's built-in SSAO. Usually disabled when using SSGI to avoid double-darkening."));
			}
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("quality_performance"), "Quality/Performance"));

	{
		auto qualityGuard = Util::DisableGuard(!settings.Enabled);

		int resolutionMode = settings.QuarterRes ? 2 : (settings.HalfRes ? 1 : 0);
		bool resolutionChanged = false;
		if (!SceneWidgetInterceptor::IsArmed() && ImGui::BeginTable("SSGI Resolution", 3)) {
			ImGui::TableNextColumn();
			resolutionChanged |= ImGui::RadioButton(T(TKEY("full_resolution"), "Full Resolution"), &resolutionMode, 0);

			ImGui::TableNextColumn();
			{
				auto halfResGuard = Util::DisableGuard(!settings.EnableREBLUR);
				resolutionChanged |= ImGui::RadioButton(T(TKEY("half_resolution"), "1/2 Resolution"), &resolutionMode, 1);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("half_resolution_checkerboard_tooltip"), "Trace half the columns in a checkerboard pattern. NRD reconstructs the missing pixels."));
				}
			}

			ImGui::TableNextColumn();
			{
				auto quarterResGuard = Util::DisableGuard(!settings.EnableREBLUR);
				resolutionChanged |= ImGui::RadioButton(T(TKEY("quarter_resolution"), "1/4 Resolution"), &resolutionMode, 2);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("quarter_resolution_tooltip"), "Trace one pixel per 2x2 block with a rotating pattern. REBLUR reconstructs the missing pixels."));
				}
			}

			ImGui::EndTable();
		}
		if (resolutionChanged) {
			settings.HalfRes = resolutionMode == 1;
			settings.QuarterRes = resolutionMode == 2;
			recompileFlag = true;
			resetReblurHistory = true;
			hasMultiBounceHistory = false;
		}

		if (showAdvanced) {
			if (ImGui::SliderInt(T(TKEY("steps_per_slice"), "Steps Per Slice"), (int*)&settings.NumSteps, 1, 32, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				resetReblurHistory = true;
				hasMultiBounceHistory = false;
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("visual"), "Visual"));

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);

		{
			auto aoGuard = Util::DisableGuard(e11Managed);
			if (ImGui::SliderFloat(T(TKEY("ao_power"), "AO Power"), &settings.AOPower, 0.f, 6.f, "%.2f"))
				resetReblurHistory = true;
		}

		{
			auto ilGuard = Util::DisableGuard(!settings.EnableGI);
			{
				auto strengthGuard = Util::DisableGuard(e11Managed);
				if (ImGui::SliderFloat(T(TKEY("il_source_brightness"), "IL Source Brightness"), &settings.GIStrength, 0.f, 6.f, "%.2f")) {
					resetReblurHistory = true;
					hasMultiBounceHistory = false;
				}
				ImGui::SliderFloat(T(TKEY("il_saturation"), "IL Saturation"), &settings.ILSaturation, 0.f, 2.f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("il_saturation_tooltip"), "Colour of the bounce light. Below 1 keeps sunlit grass and ground from tinting shade, so it keeps the sky's cool fill; 1 is unchanged."));
			}
			if (ImGui::Checkbox(T(TKEY("multi_bounce"), "Multi-Bounce"), &settings.EnableMultiBounce)) {
				resetReblurHistory = true;
				hasMultiBounceHistory = false;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("multi_bounce_tooltip"), "Feed the previous frame's reprojected, post-albedo indirect radiance back into the source radiance pyramid."));
			}

			{
				const Util::LockedSection enbLock(!AllowsAmbientFallback(),
					T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));
				if (ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps_as_fallback"), "Use Dynamic Cubemaps as Fallback"), &settings.UseDynamicCubemapsAsFallback)) {
					resetReblurHistory = true;
					hasMultiBounceHistory = false;
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_as_fallback_tooltip"), "Where indirect rays miss the screen, sample dynamic cubemaps for diffuse fallback."));
				}
				{
					auto cubemapGuard = Util::DisableGuard(!settings.UseDynamicCubemapsAsFallback);
					if (ImGui::SliderFloat(T(TKEY("diffuse_cubemap_multiplier"), "Diffuse Cubemap Multiplier"), &settings.DiffuseCubemapMult, 0.0f, 5.0f, "%.2f")) {
						resetReblurHistory = true;
						hasMultiBounceHistory = false;
					}
				}
			}
		}

		if (showAdvanced) {
			ImGui::Separator();
			if (ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.Thickness, 0.f, 0.2f, "%.3f")) {
				resetReblurHistory = true;
				hasMultiBounceHistory = false;
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("reblur_denoiser"), "REBLUR Denoiser"));

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::Checkbox(T(TKEY("enable_reblur"), "Enable REBLUR"), &settings.EnableREBLUR)) {
			// Reduced-resolution traces are checkerboard/phase packed and are only
			// full-resolution consumer inputs after REBLUR reconstruction.
			if (!settings.EnableREBLUR && (settings.HalfRes || settings.QuarterRes)) {
				settings.HalfRes = false;
				settings.QuarterRes = false;
				recompileFlag = true;
			}
			resetReblurHistory = true;
			hasMultiBounceHistory = false;
		}

		if (settings.EnableREBLUR) {
			resetReblurHistory |= NRDPort::Service().DrawReblurSettings(settings.Reblur, showAdvanced, "ssgi_reblur");
			if (showAdvanced) {
				resetReblurHistory |= ImGui::SliderFloat(T(TKEY("diffuse_prepass_blur_radius"), "Diffuse Pre-pass Radius"), &settings.DiffusePrepassBlurRadius, 0.0f, 75.0f, "%.1f px");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("diffuse_prepass_blur_radius_tooltip"), "Spatial reuse before temporal accumulation. It is the only spatial filtering the AO term gets, so lowering it makes AO and dim indirect light noisier."));
			}
		}
	}

	#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		ImGui::SeparatorText(T(TKEY("e11_calibration"), "Effects 11 Presets"));
		ImGui::SliderFloat(T(TKEY("e11_ao_scale"), "Preset AO Scale"), &settings.E11AOScale, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("e11_ao_scale_tooltip"), "AO Power is the preset's AOAmount times this. ENB's occlusion is weaker than SSGI's, so presets set it high."));
		ImGui::SliderFloat(T(TKEY("e11_il_scale"), "Preset IL Scale"), &settings.E11ILScale, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("e11_il_scale_tooltip"), "IL Brightness is the preset's ILAmount times this. ENB's indirect light is faint, so presets often set ILAmount to 3 or 5."));
		ImGui::SliderFloat(T(TKEY("e11_il_saturation"), "Preset IL Saturation"), &settings.E11ILSaturation, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("e11_il_saturation_tooltip"), "Colour of the bounce light while a preset drives SSGI. Below 1 keeps strongly coloured surfaces from tinting the preset's grade."));
	}
#endif
}

void ScreenSpaceGI::DrawDebugSection()
{
	// The buffer viewer has no scene-context meaning, so hide it while a Scene Manager replica
	// is borrowing this panel to author overrides.
	if (SceneWidgetInterceptor::IsArmed())
		return;

	Util::DebugSection section;
	if (!section)
		return;

	static float debugRescale = .3f;
	ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.f, 1.f);

	if (!globals::game::isVR) {
		DrawBufferViewer(debugRescale);
	} else {
		if (ImGui::TreeNode(T("feature.nrd.left_eye", "Left eye"))) {
			DrawBufferViewer(debugRescale);
			ImGui::TreePop();
		}
		if (ImGui::TreeNode(T("feature.nrd.right_eye", "Right eye"))) {
			if (const auto* right = NRDPort::RightGIDebug())
				right->DrawBufferViewer(debugRescale);
			else
				ImGui::TextUnformatted(T("feature.nrd.eye_buffers_unavailable", "Right-eye buffers are not allocated yet."));
			ImGui::TreePop();
		}
	}
}

void ScreenSpaceGI::DrawBufferViewer(float debugRescale) const
{
	BUFFER_VIEWER_NODE(texNoise, debugRescale)
	BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
	BUFFER_VIEWER_NODE(texHistoryGeo[0], debugRescale)
	BUFFER_VIEWER_NODE(texHistoryGeo[1], debugRescale)
	BUFFER_VIEWER_NODE(texRadiance, debugRescale)
	BUFFER_VIEWER_NODE(texNRDInput, debugRescale)
	BUFFER_VIEWER_NODE(texNRDOutput, debugRescale)
	if (texNRDInputSH1)
		BUFFER_VIEWER_NODE(texNRDInputSH1, debugRescale)
	if (texNRDOutputSH1)
		BUFFER_VIEWER_NODE(texNRDOutputSH1, debugRescale)
	if (auto validation = settings.Reblur.EnableValidation ? nrdReblur.GetValidationSRV() : nullptr) {
		if (ImGui::TreeNode(T(TKEY("nrd_validation"), "NRD Validation"))) {
			ImGui::Image(validation, { nrdReblur.GetWidth() * debugRescale, nrdReblur.GetHeight() * debugRescale });
			ImGui::TreePop();
		}
	}
}

namespace
{
	void ResetIfOutOfRange(float& a_value, float a_min, float a_max, float a_default)
	{
		if (!std::isfinite(a_value) || a_value < a_min || a_value > a_max)
			a_value = a_default;
	}

	bool IsLegacySSGISchema(const json& a_json)
	{
		constexpr std::array kLegacyOnlyKeys{
			"ResolutionMode", "EnableExperimentalSpecularGI", "NumSlices", "MinScreenRadius", "AORadius", "GIRadius",
			"DepthFadeRange", "GISaturation", "GIDistanceCompensation", "SpecularStrength", "EnableTemporalDenoiser",
			"EnableBlur", "DepthDisocclusion", "NormalDisocclusion", "MaxAccumFrames", "BlurRadius", "DistanceNormalisation"
		};
		return a_json.is_object() && std::ranges::any_of(kLegacyOnlyKeys, [&](const char* key) { return a_json.contains(key); });
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	const auto previous = settings;
	const Settings defaults{};

	settings = o_json;
	if (globals::game::isVR && !o_json.contains("EnableGI")) settings.EnableGI = false;

	if (IsLegacySSGISchema(o_json)) {
		Settings migrated{};
		migrated.Enabled = settings.Enabled;
		migrated.EnableGI = settings.EnableGI;
		migrated.EnableVanillaSSAO = settings.EnableVanillaSSAO;
		if (const auto resolutionMode = o_json.find("ResolutionMode"); resolutionMode != o_json.end() && resolutionMode->is_number_integer()) {
			migrated.HalfRes = false;
			migrated.QuarterRes = resolutionMode->get<int>() != 0;
		}
		settings = migrated;
		logger::info("[SSGI] Migrated settings from the previous Screen Space GI schema; tuning values use the new defaults");
	}

	settings.NumSteps = std::clamp(settings.NumSteps, 1u, 32u);
	ResetIfOutOfRange(settings.Thickness, 0.0f, 0.2f, defaults.Thickness);
	ResetIfOutOfRange(settings.AOPower, 0.0f, 6.0f, defaults.AOPower);
	ResetIfOutOfRange(settings.GIStrength, 0.0f, 6.0f, defaults.GIStrength);
	ResetIfOutOfRange(settings.ILSaturation, 0.0f, 2.0f, defaults.ILSaturation);
	ResetIfOutOfRange(settings.DiffuseCubemapMult, 0.0f, 5.0f, defaults.DiffuseCubemapMult);
	ResetIfOutOfRange(settings.DiffusePrepassBlurRadius, 0.0f, 75.0f, defaults.DiffusePrepassBlurRadius);
	if (settings.QuarterRes)
		settings.HalfRes = false;
	if (!settings.EnableREBLUR) {
		settings.HalfRes = false;
		settings.QuarterRes = false;
	}
	settings.E11AOScale = Util::ClampFinite(settings.E11AOScale, 0.0f, 4.0f, defaults.E11AOScale);
	settings.E11ILScale = Util::ClampFinite(settings.E11ILScale, 0.0f, 4.0f, defaults.E11ILScale);
	settings.E11ILSaturation = Util::ClampFinite(settings.E11ILSaturation, 0.0f, 2.0f, defaults.E11ILSaturation);
	userSettings = settings;
	appliedE11Overlay = GetE11Overlay();
	if (appliedE11Overlay.active) {
		settings.Enabled = appliedE11Overlay.enabled;
		settings.EnableGI = appliedE11Overlay.indirectLighting;
		settings.AOPower = appliedE11Overlay.aoPower;
		settings.GIStrength = appliedE11Overlay.giStrength;
	}

	recompileFlag = recompileFlag ||
	                settings.HalfRes != previous.HalfRes ||
	                settings.QuarterRes != previous.QuarterRes ||
	                settings.EnableGI != previous.EnableGI ||
	                settings.EnableSH != previous.EnableSH;
	const bool historyInvalidated = recompileFlag ||
	                                settings.Enabled != previous.Enabled ||
	                                settings.NumSteps != previous.NumSteps ||
	                                settings.EnableMultiBounce != previous.EnableMultiBounce ||
	                                settings.UseDynamicCubemapsAsFallback != previous.UseDynamicCubemapsAsFallback ||
	                                settings.EnableREBLUR != previous.EnableREBLUR;
	outputReady = false;
	denoisedThisFrame = false;
	if (historyInvalidated) {
		QueueHistoryReset();
		resetReblurHistory = true;
		hasMultiBounceHistory = false;
		hasFullResolutionMultiBounceHistory = false;
		multiBounceHistoryUsesNRDOutput = false;
	}
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	o_json = userSettings;
}

bool ScreenSpaceGI::IsManagedByE11() const
{
	#if defined(ENABLE_EFFECTS11)
	const auto& effects11 = globals::features::effects11;
	return effects11.IsActive() && effects11.OwnsFeature(E11Handoff::Feature::SSGI);
#else
	return false;
#endif
}

ScreenSpaceGI::E11Overlay ScreenSpaceGI::GetE11Overlay() const
{
	E11Overlay overlay;
	if (!IsManagedByE11())
		return overlay;
#if defined(ENABLE_EFFECTS11)
	auto& settingManager = SettingManager::GetSingleton();
	const bool interior = Util::IsInterior();
	overlay.active = true;
	overlay.enabled = settingManager.GetValue<bool>("EnableAmbientOcclusion", "EFFECT");
	overlay.indirectLighting = settingManager.GetValue<bool>("UseIndirectLighting", "SSAO_SSIL");
	const float aoAmount = settingManager.GetValue<float>(interior ? "AOAmountInterior" : "AOAmount", "SSAO_SSIL");
	const float ilAmount = settingManager.GetValue<float>(interior ? "ILAmountInterior" : "ILAmount", "SSAO_SSIL");
	overlay.aoPower = Util::ClampFinite(aoAmount * userSettings.E11AOScale, 0.0f, 6.0f, 1.0f);
	overlay.giStrength = Util::ClampFinite(ilAmount * userSettings.E11ILScale, 0.0f, 6.0f, 1.0f);
#endif
	return overlay;
}

void ScreenSpaceGI::RefreshE11Overlay()
{
	if (GetE11Overlay() == appliedE11Overlay)
		return;
	json document = userSettings;
	LoadSettings(document);
}

RE::BSEventNotifyControl ScreenSpaceGI::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
		globals::features::screenSpaceGI.queuedResetHistory = true;

	return RE::BSEventNotifyControl::kContinue;
}

bool ScreenSpaceGI::MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = globals::game::ui;

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);
	return true;
}

void ScreenSpaceGI::PostPostLoad()
{
	MenuOpenCloseEventHandler::Register();
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;
	resetReblurHistory = true;
	outputReady = false;
	denoisedThisFrame = false;
	hasMultiBounceHistory = false;
	hasFullResolutionMultiBounceHistory = false;
	multiBounceHistoryUsesNRDOutput = false;
	historyGeoWriteIndex = 0;

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>(), "SSGI::CB");
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
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

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		Util::AsReal(mainTex.texture)->GetDesc(&texDesc);
		NRDPort::EyeTextureDesc(texDesc);
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const uint mipCount = std::clamp<uint>(static_cast<uint>(std::bit_width(std::min(texDesc.Width, texDesc.Height))), 1u, 5u);
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = mipCount;
		for (uint i = 0; i < 5; ++i)
			uavRadiance[i] = uavWorkingDepth[i] = uavNormal[i] = nullptr;

		{
			texRadiance = eastl::make_unique<Texture2D>(texDesc, "SSGI::Radiance");
			texRadiance->CreateSRV(srvDesc);

			for (uint i = 0; i < mipCount; ++i) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texRadiance->resource.get(), &mipUavDesc, uavRadiance[i].put()));
				Util::SetResourceName(uavRadiance[i].get(), "SSGI::Radiance UAV mip%u", i);
			}
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = globals::features::reverseZ.IsActive() ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16_FLOAT;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc, "SSGI::WorkingDepth");
			texWorkingDepth->CreateSRV(srvDesc);
			for (uint i = 0; i < mipCount; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
				Util::SetResourceName(uavWorkingDepth[i].get(), "SSGI::WorkingDepth UAV mip%u", i);
			}
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		{
			texNormal = eastl::make_unique<Texture2D>(texDesc, "SSGI::Normal");
			texNormal->CreateSRV(srvDesc);
			for (uint i = 0; i < mipCount; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texNormal->resource.get(), &uavDesc, uavNormal[i].put()));
				Util::SetResourceName(uavNormal[i].get(), "SSGI::Normal UAV mip%u", i);
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		for (uint i = 0; i < 2; ++i) {
			texHistoryGeo[i] = eastl::make_unique<Texture2D>(texDesc, i == 0 ? "SSGI::HistoryGeo0" : "SSGI::HistoryGeo1");
			texHistoryGeo[i]->CreateSRV(srvDesc);
			texHistoryGeo[i]->CreateUAV(uavDesc);
		}

		SetupNRDResources();
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };
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

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "SSGI::Noise");

		D3D11_SHADER_RESOURCE_VIEW_DESC noiseSrvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(noiseSrvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
		Util::SetResourceName(linearClampSampler.get(), "SSGI::LinearClampSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
		Util::SetResourceName(pointClampSampler.get(), "SSGI::PointClampSampler");
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::SetupNRDResources()
{
	outputReady = false;
	denoisedThisFrame = false;
	resetReblurHistory = true;
	hasMultiBounceHistory = false;
	hasFullResolutionMultiBounceHistory = false;
	multiBounceHistoryUsesNRDOutput = false;
	const bool useSH = settings.EnableGI && settings.EnableSH;
	const bool modeChanged = nrdReblurUsesSH != useSH || !texNRDInput;
	if (nrdReblurUsesSH != useSH)
		recompileFlag = true;

	uint32_t fullW = 0;
	uint32_t fullH = 0;
	GetNRDResourceSize(fullW, fullH);

	D3D11_TEXTURE2D_DESC texDesc{
		.Width = fullW,
		.Height = fullH,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	nrdReblur.Shutdown();

	texNRDInput = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDInput");
	texNRDInput->CreateSRV(srvDesc);
	texNRDInput->CreateUAV(uavDesc);

	texNRDOutput = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDOutput");
	texNRDOutput->CreateSRV(srvDesc);
	texNRDOutput->CreateUAV(uavDesc);

	if (useSH) {
		texNRDInputSH1 = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDInputSH1");
		texNRDInputSH1->CreateSRV(srvDesc);
		texNRDInputSH1->CreateUAV(uavDesc);

		texNRDOutputSH1 = eastl::make_unique<Texture2D>(texDesc, "SSGI::NRDOutputSH1");
		texNRDOutputSH1->CreateSRV(srvDesc);
		texNRDOutputSH1->CreateUAV(uavDesc);

		const float clearColor[4] = {};
		globals::d3d::context->ClearUnorderedAccessViewFloat(texNRDInputSH1->uav.get(), clearColor);
		globals::d3d::context->ClearUnorderedAccessViewFloat(texNRDOutputSH1->uav.get(), clearColor);
	} else {
		texNRDInputSH1.reset();
		texNRDOutputSH1.reset();
	}

	reblurInitAttempted = settings.Enabled && settings.EnableREBLUR;
	if (reblurInitAttempted)
		nrdReblur.Init(fullW, fullH, useSH ? nrd::Denoiser::REBLUR_DIFFUSE_SH : nrd::Denoiser::REBLUR_DIFFUSE, 0);
	nrdReblurUsesSH = useSH;

	if (modeChanged)
		globals::deferred->ClearShaderCache();
}

void ScreenSpaceGI::GetNRDResourceSize(uint32_t& a_width, uint32_t& a_height) const
{
	const auto& nrdSvc = NRDPort::Service();
	const bool multiBounce = settings.EnableGI && settings.EnableMultiBounce;
	if (!multiBounce && nrdSvc.loaded && nrdSvc.GetResourceExtent(a_width, a_height))
		return;
	if (texRadiance) {
		a_width = texRadiance->desc.Width;
		a_height = texRadiance->desc.Height;
	} else {
		auto mainTex = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC mainDesc;
		Util::AsReal(mainTex.texture)->GetDesc(&mainDesc);
		NRDPort::EyeTextureDesc(mainDesc);
		a_width = mainDesc.Width;
		a_height = mainDesc.Height;
	}
}

void ScreenSpaceGI::ClearShaderCache()
{
	NRDPort::InvalidateStereo();
	queuedResetHistory = true;
	outputReady = false;
	denoisedThisFrame = false;
	const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&prefilterDepthsCompute, &prefilterRadianceCompute, &prefilterNormalCompute, &giCompute, &compositeCompute
	};

	for (auto shader : shaderPtrs)
		*shader = nullptr;

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", { { "LINEAR_FILTER", "" } } },
			{ &prefilterRadianceCompute, "prefilterRadiance.cs.hlsl", {} },
			{ &prefilterNormalCompute, "prefilterNormal.cs.hlsl", {} },
			{ &giCompute, "diffuseGI.cs.hlsl", {} },
		};
	if (settings.EnableGI)
		shaderInfos.push_back({ &compositeCompute, "composite.cs.hlsl", {} });

	const bool canDenoise = CanDenoise();
	tracedHalfRes = settings.HalfRes && canDenoise;
	tracedQuarterRes = settings.QuarterRes && canDenoise;

	for (auto& info : shaderInfos) {
		if (info.programPtr == &compositeCompute) {
			if (settings.EnableSH && settings.EnableGI)
				info.defines.push_back({ "SSGI_SH", "" });
			if (globals::features::terrainBlending.loaded)
				info.defines.push_back({ "TERRAIN_BLENDING", "" });
			if (globals::features::ibl.loaded) info.defines.push_back({ "IBL", "" });
			if (globals::features::skylighting.loaded) info.defines.push_back({ "SKYLIGHTING", "" });
			continue;
		}

		if (settings.EnableGI)
			info.defines.push_back({ "GI", "" });
		if (settings.EnableSH && settings.EnableGI)
			info.defines.push_back({ "SSGI_SH", "" });
		if (tracedQuarterRes)
			info.defines.push_back({ "SSGI_QUARTER", "" });
		else if (tracedHalfRes)
			info.defines.push_back({ "SSGI_HALF", "" });
		if (globals::features::dynamicCubemaps.loaded)
			info.defines.push_back({ "DYNAMIC_CUBEMAPS", "" });
		if (globals::features::skylighting.loaded)
			info.defines.push_back({ "SKYLIGHTING", "" });
		if (globals::features::ibl.loaded)
			info.defines.push_back({ "IBL", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::CanDenoise() const
{
	return settings.EnableREBLUR && nrdReblur.IsValid() && NRDPort::Service().CanDenoise() && NRDPort::Service().AreGuidesReady();
}

bool ScreenSpaceGI::ShadersOK()
{
	const bool useSH = settings.EnableGI && settings.EnableSH;
	return texNoise && texWorkingDepth && texRadiance && texNormal && texNRDInput &&
	       prefilterDepthsCompute && prefilterRadianceCompute && prefilterNormalCompute && giCompute &&
	       (!settings.EnableGI || compositeCompute) && (!useSH || texNRDInputSH1);
}

void ScreenSpaceGI::UpdateSB()
{
	float2 res = { (float)texRadiance->desc.Width, (float)texRadiance->desc.Height };
	float2 dynres = NRDPort::ActiveSize();
	dynres = { floor(dynres.x), floor(dynres.y) };

	SSGICB data;
	{
		const auto& projMat = globals::game::frameBufferCached.GetCameraProj(NRDPort::Eye());

		data.NDCToViewMul = { 2.0f / projMat(0, 0), -2.0f / projMat(1, 1) };
		data.NDCToViewAdd = { -1.0f / projMat(0, 0), 1.0f / projMat(1, 1) };

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = globals::state->frameCount;

		data.NumSteps = settings.NumSteps;

		data.Thickness = settings.Thickness;
		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;
		data.DiffuseCubemapMult = settings.DiffuseCubemapMult;
		data.UseDynamicCubemap = UsesCubemapFallback() ? 1u : 0u;
		const bool hasContinuousHistory = hasMultiBounceHistory &&
		                                  globals::state->frameCount == lastMultiBounceHistoryFrame + 1;
		const bool trackMultiBounce = settings.EnableGI && settings.EnableMultiBounce;
		const bool injectMultiBounce = trackMultiBounce && hasContinuousHistory && hasFullResolutionMultiBounceHistory;
		// 0: disabled, 1: populate history, 2: populate and inject valid history.
		data.MultiBounceMode = injectMultiBounce ? 2u : (trackMultiBounce ? 1u : 0u);
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI()
{
	outputReady = false;
	denoisedThisFrame = false;


	auto context = globals::d3d::context;

	if (auto* setting = RE::GetINISetting("bSAOEnable:Display"))
		setting->data.b = settings.EnableVanillaSSAO;
	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISSAOBlurH, imageSpaceManager);
	if (auto* sao = BSImagespaceShaderISSAOBlurH)
		sao->enableSAO = settings.EnableVanillaSSAO;

	const bool useSH = settings.EnableGI && settings.EnableSH;
	const bool trackMultiBounce = settings.EnableGI && settings.EnableMultiBounce;
	uint32_t nrdWidth = 0;
	uint32_t nrdHeight = 0;
	GetNRDResourceSize(nrdWidth, nrdHeight);
	if (nrdReblurUsesSH != useSH || !texNRDInput || texNRDInput->desc.Width != nrdWidth || texNRDInput->desc.Height != nrdHeight)
		SetupNRDResources();

	if (!settings.Enabled || !settings.EnableREBLUR) {
		if (nrdReblur.IsValid()) {
			nrdReblur.Shutdown();
			resetReblurHistory = true;
		}
		reblurInitAttempted = false;
	} else if (!reblurInitAttempted) {
		SetupNRDResources();
	}

	if (const bool canDenoise = CanDenoise(); (settings.HalfRes && canDenoise) != tracedHalfRes || (settings.QuarterRes && canDenoise) != tracedQuarterRes) {
		recompileFlag = true;
		queuedResetHistory = true;
	}

	// Compile first: enabling GI may require creating the composite shader, so
	// checking the old shader set before recompilation can otherwise deadlock the
	// feature in an unavailable state.
	if (recompileFlag)
		ClearShaderCache();

	if (!(settings.Enabled && ShadersOK())) {
		return;
	}

	if (globals::state->isMapMenuOpen) {
		queuedResetHistory = true;
		return;
	}

	ZoneScoped;
	CS_GPU_PASS("ScreenSpaceGI::Total");

	// Restarting accumulation drives the denoiser's lerp factor to 1, dropping stale
	// history in one frame instead of fading it over MaxAccumFrames.
	if (globals::state->ShouldResetHistory())
		queuedResetHistory = true;
	if (const bool usesFallback = UsesCubemapFallback(); usesFallback != lastUsedCubemapFallback) {
		lastUsedCubemapFallback = usesFallback;
		queuedResetHistory = true;
	}
	if (queuedResetHistory.exchange(false)) {
		resetReblurHistory = true;
		hasMultiBounceHistory = false;
		hasFullResolutionMultiBounceHistory = false;
		multiBounceHistoryUsesNRDOutput = false;
	}

	//////////////////////////////////////////////////////

	UpdateSB();
	NRDPort::BindEye();

	//////////////////////////////////////////////////////

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = globals::deferred;

	float2 size = NRDPort::ActiveSize();
	auto resolution = std::array{ (uint)size.x, (uint)size.y };

	std::array<ID3D11ShaderResourceView*, 11> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 6> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	globals::state->BindSharedDataCS(context);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		CS_GPU_PASS("ScreenSpaceGI::PrefilterDepths");

		srvs.at(0) = NRDPort::Crop(NRDPort::Input::Depth, Util::GetCurrentSceneDepthSRV(false));
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);

		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);

	}

	// Prefilter normals and write the current full-resolution geometry history.
	{
		CS_GPU_PASS("ScreenSpaceGI::PrefilterNormals");

		resetViews();
		srvs.at(0) = NRDPort::Crop(NRDPort::Input::Normal, Util::AsReal(rts[NORMALROUGHNESS].SRV));
		uavs.at(0) = uavNormal[0].get();
		uavs.at(1) = uavNormal[1].get();
		uavs.at(2) = uavNormal[2].get();
		uavs.at(3) = uavNormal[3].get();
		uavs.at(4) = uavNormal[4].get();
		if (trackMultiBounce) {
			srvs.at(1) = texWorkingDepth->srv.get();
			uavs.at(5) = texHistoryGeo[historyGeoWriteIndex]->uav.get();
		}

		context->CSSetShaderResources(0, trackMultiBounce ? 2u : 1u, srvs.data());
		context->CSSetUnorderedAccessViews(0, trackMultiBounce ? 6u : 5u, uavs.data(), nullptr);
		context->CSSetShader(prefilterNormalCompute.get(), nullptr, 0);

		context->Dispatch((resolution[0] + 15u) >> 4, (resolution[1] + 15u) >> 4, 1);

	}

	// Prefilter source radiance. Multi-bounce history is reprojected, converted
	// from illumination to post-albedo outgoing radiance, and injected here once.
	{
		CS_GPU_PASS("ScreenSpaceGI::PrefilterRadiance");

		resetViews();
		srvs.at(0) = NRDPort::Crop(NRDPort::Input::Main, Util::AsReal(rts[deferred->forwardRenderTargets[0]].SRV));
		if (trackMultiBounce) {
			srvs.at(1) = (multiBounceHistoryUsesNRDOutput ? texNRDOutput : texNRDInput)->srv.get();
			srvs.at(2) = texHistoryGeo[historyGeoWriteIndex]->srv.get();
			srvs.at(3) = NRDPort::Crop(NRDPort::Input::Motion, Util::AsReal(rts[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV));
			srvs.at(4) = texHistoryGeo[historyGeoWriteIndex ^ 1]->srv.get();
			srvs.at(5) = NRDPort::Crop(NRDPort::Input::Albedo, Util::AsReal(rts[ALBEDO].SRV));
		}
		uavs.at(0) = uavRadiance[0].get();
		uavs.at(1) = uavRadiance[1].get();
		uavs.at(2) = uavRadiance[2].get();
		uavs.at(3) = uavRadiance[3].get();
		uavs.at(4) = uavRadiance[4].get();

		context->CSSetShaderResources(0, trackMultiBounce ? 6u : 1u, srvs.data());
		context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
		context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);

		context->Dispatch((resolution[0] + 15u) >> 4, (resolution[1] + 15u) >> 4, 1);

	}

	// GI → NRD input
	{
		CS_GPU_PASS("ScreenSpaceGI::GI");

		auto& dynamicCubemaps = globals::features::dynamicCubemaps;
		auto& ibl = globals::features::ibl;
		auto& skylighting = globals::features::skylighting;

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = NRDPort::Crop(NRDPort::Input::Masks, Util::AsReal(rts[MASKS].SRV));
		srvs.at(2) = texRadiance->srv.get();
		srvs.at(3) = texNoise->srv.get();
		if (dynamicCubemaps.loaded) {
			auto& reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
			srvs.at(4) = dynamicCubemaps.envTexture->srv.get();
			srvs.at(5) = Util::AsReal(reflections.SRV);
		}
		if (dynamicCubemaps.loaded && skylighting.loaded && skylighting.texProbeArray)
			srvs.at(6) = skylighting.texProbeArray->srv.get();
		srvs.at(8) = texNormal->srv.get();
		ID3D11ShaderResourceView* iblViews[2] = {ibl.loaded && ibl.envIBLTexture ? ibl.envIBLTexture->srv.get() : nullptr, ibl.loaded && ibl.skyIBLTexture ? ibl.skyIBLTexture->srv.get() : nullptr};

		uavs.at(0) = texNRDInput->uav.get();
		if (useSH && texNRDInputSH1)
			uavs.at(2) = texNRDInputSH1->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(76, 2, iblViews);
		if (tracedQuarterRes) {
			const float clearColor[4] = {};
			context->ClearUnorderedAccessViewFloat(texNRDInput->uav.get(), clearColor);
			if (useSH && texNRDInputSH1)
				context->ClearUnorderedAccessViewFloat(texNRDInputSH1->uav.get(), clearColor);
		}
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(giCompute.get(), nullptr, 0);

		uint dispatchX = (tracedHalfRes || tracedQuarterRes) ? (resolution[0] + 1) / 2 : resolution[0];
		uint dispatchY = tracedQuarterRes ? (resolution[1] + 1) / 2 : resolution[1];

		context->Dispatch((dispatchX + 7u) >> 3, (dispatchY + 7u) >> 3, 1);


		iblViews[0] = iblViews[1] = nullptr;
		context->CSSetShaderResources(76, 2, iblViews);
		resetViews();
	}

	// REBLUR diffuse denoising via core NRD service
	auto& nrdSvc = NRDPort::Service();
	bool producedDenoisedHistory = false;
	if (settings.EnableREBLUR && nrdReblur.IsValid() && nrdSvc.loaded && nrdSvc.AreGuidesReady()) {
		CS_GPU_PASS("ScreenSpaceGI::Reblur");

		nrdSvc.RefreshMotionVectors();
		auto commonSettings = nrdSvc.GetCommonSettings();
		commonSettings.splitScreen = settings.Reblur.SplitScreen;
		commonSettings.enableValidation = settings.Reblur.EnableValidation;
		if (resetReblurHistory)
			commonSettings.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
		nrdReblur.SetCommonSettings(commonSettings);

		nrdSvc.ApplyReblurSettings(reblurSettings, settings.Reblur,
			tracedHalfRes ? nrd::CheckerboardMode::BLACK : nrd::CheckerboardMode::OFF);
		reblurSettings.hitDistanceParameters.A = 3.0f / Util::Units::GAME_UNIT_TO_M;
		reblurSettings.hitDistanceParameters.B = 0.1f;
		reblurSettings.diffusePrepassBlurRadius = settings.DiffusePrepassBlurRadius;
		if (tracedQuarterRes) {
			if (reblurSettings.hitDistanceReconstructionMode == nrd::HitDistanceReconstructionMode::OFF)
				reblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
			// Probabilistic inputs require spatial reuse to redistribute the
			// probability-compensated radiance before temporal accumulation.
			reblurSettings.diffusePrepassBlurRadius = std::max({ reblurSettings.diffusePrepassBlurRadius, reblurSettings.maxBlurRadius, 1.0f });
		}
		nrdReblur.SetDenoiserSettings(&reblurSettings);

		nrdReblur.SetNamedSRV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorSRV());
		nrdReblur.SetNamedUAV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorUAV());
		nrdReblur.SetNamedSRV(nrd::ResourceType::IN_NORMAL_ROUGHNESS, nrdSvc.GetNormalRoughnessSRV());
		nrdReblur.SetNamedSRV(nrd::ResourceType::IN_VIEWZ, nrdSvc.GetViewZSRV());

		if (useSH && texNRDInputSH1) {
			nrdReblur.SetNamedSRV(nrd::ResourceType::IN_DIFF_SH0, texNRDInput->srv.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::IN_DIFF_SH1, texNRDInputSH1->srv.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::OUT_DIFF_SH0, texNRDOutput->srv.get());
			nrdReblur.SetNamedUAV(nrd::ResourceType::OUT_DIFF_SH0, texNRDOutput->uav.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::OUT_DIFF_SH1, texNRDOutputSH1->srv.get());
			nrdReblur.SetNamedUAV(nrd::ResourceType::OUT_DIFF_SH1, texNRDOutputSH1->uav.get());
		} else {
			nrdReblur.SetNamedSRV(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, texNRDInput->srv.get());
			nrdReblur.SetNamedSRV(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, texNRDOutput->srv.get());
			nrdReblur.SetNamedUAV(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, texNRDOutput->uav.get());
		}

		producedDenoisedHistory = nrdReblur.Dispatch();
		denoisedThisFrame = producedDenoisedHistory;
		resetReblurHistory = !producedDenoisedHistory;

	}

	if (trackMultiBounce) {
		hasMultiBounceHistory = true;
		hasFullResolutionMultiBounceHistory = (!tracedHalfRes && !tracedQuarterRes) || producedDenoisedHistory;
		multiBounceHistoryUsesNRDOutput = producedDenoisedHistory;
		lastMultiBounceHistoryFrame = globals::state->frameCount;
		historyGeoWriteIndex ^= 1;
	} else {
		hasMultiBounceHistory = false;
		hasFullResolutionMultiBounceHistory = false;
		multiBounceHistoryUsesNRDOutput = false;
	}

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	outputReady = (!tracedHalfRes && !tracedQuarterRes) || denoisedThisFrame;
}

void ScreenSpaceGI::Composite()
{
	if (!(outputReady && settings.Enabled && settings.EnableGI && compositeCompute))
		return;

	ZoneScoped;
	CS_GPU_PASS("ScreenSpaceGI::Composite");

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& rts = renderer->GetRuntimeData().renderTargets;
	auto& main = rts[globals::deferred->forwardRenderTargets[0]];

	std::array<ID3D11ShaderResourceView*, 7> srvs = {
		NRDPort::Crop(NRDPort::Input::Albedo, Util::AsReal(rts[ALBEDO].SRV)),
		NRDPort::Crop(NRDPort::Input::Normal, Util::AsReal(rts[NORMALROUGHNESS].SRV)),
		NRDPort::Crop(NRDPort::Input::Depth, Util::GetCurrentSceneDepthSRV(false)),
		GetDiffuseOutputTexture(),
		GetDiffuseSH1Texture(),
		NRDPort::Crop(NRDPort::Input::Masks, Util::AsReal(rts[MASKS].SRV)),
		globals::features::skylighting.loaded && globals::features::skylighting.texProbeArray ? globals::features::skylighting.texProbeArray->srv.get() : nullptr
	};

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());

	auto& ibl = globals::features::ibl;
    ID3D11ShaderResourceView* iblSRVs[2] = {ibl.loaded && ibl.envIBLTexture ? ibl.envIBLTexture->srv.get() : nullptr,ibl.loaded && ibl.skyIBLTexture ? ibl.skyIBLTexture->srv.get() : nullptr};
    context->CSSetShaderResources(76,2,iblSRVs);
    auto& sky=globals::features::skylighting;
    ID3D11ShaderResourceView* skySRVs[2]={sky.loaded && sky.texProbeCoverage ? sky.texProbeCoverage->srv.get() : nullptr,sky.loaded && sky.texZenithOcclusion ? sky.texZenithOcclusion->srv.get() : nullptr};
    context->CSSetShaderResources(18,2,skySRVs);
    ID3D11UnorderedAccessView* mainUAV = Util::AsReal(main.UAV);
	context->CSSetUnorderedAccessViews(0, 1, &mainUAV, nullptr);

	globals::state->BindSharedDataCS(context);
	context->CSSetShader(compositeCompute.get(), nullptr, 0);

	auto active = NRDPort::ActiveSize();
	NRDPort::BindEye(UsesCubemapFallback(), appliedE11Overlay.active ? userSettings.E11ILSaturation : settings.ILSaturation);

	context->Dispatch((uint(active.x)+7)/8, (uint(active.y)+7)/8, 1);


	srvs.fill(nullptr);
	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());

	iblSRVs[0]=iblSRVs[1]=nullptr; context->CSSetShaderResources(76,2,iblSRVs);
	skySRVs[0]=skySRVs[1]=nullptr; context->CSSetShaderResources(18,2,skySRVs);
	mainUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &mainUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
}

ID3D11ShaderResourceView* ScreenSpaceGI::GetDiffuseOutputTexture()
{
	if (!outputReady)
		return nullptr;
	if (denoisedThisFrame)
		return texNRDOutput->srv.get();
	else if (loaded && settings.Enabled)
		return texNRDInput->srv.get();
	return nullptr;
}

ID3D11ShaderResourceView* ScreenSpaceGI::GetDiffuseSH1Texture()
{
	if (!outputReady || !loaded || !settings.Enabled || !settings.EnableGI || !settings.EnableSH)
		return nullptr;
	if (denoisedThisFrame && texNRDOutputSH1)
		return texNRDOutputSH1->srv.get();
	else if (texNRDInputSH1)
		return texNRDInputSH1->srv.get();
	return nullptr;
}

namespace { bool AllowsAmbientFallback() {
#if defined(ENABLE_EFFECTS11)
    const auto& effects11 = globals::features::effects11;
    return !(effects11.IsActive() && !effects11.IsHandedOff(E11Handoff::Feature::IBL));
#else
    return true;
#endif
} }
bool ScreenSpaceGI::UsesCubemapFallback() const
{
	if (!settings.UseDynamicCubemapsAsFallback || settings.DiffuseCubemapMult <= 0.0f || !globals::features::dynamicCubemaps.loaded)
		return false;
	return AllowsAmbientFallback();
}

ScreenSpaceGI::SharedData ScreenSpaceGI::GetCommonBufferData()
{
	SharedData data{};
	data.Enabled = loaded && settings.Enabled ? NRDPort::DiffuseMask() : 0u;
	data.EnableIL = settings.EnableGI ? data.Enabled : 0u;
	data.ReplaceAmbient = UsesCubemapFallback() ? data.EnableIL : 0u; // early composite subtracts existing ambient from target masks
	data.AOPower = settings.AOPower;
	data.ILSaturation = appliedE11Overlay.active ? userSettings.E11ILSaturation : settings.ILSaturation;
	return data;
}

#undef I18N_KEY_PREFIX
