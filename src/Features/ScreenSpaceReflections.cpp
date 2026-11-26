#include "ScreenSpaceReflections.h"
#include "NRDPortStereo.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "Effects11.h"
#include "Effects11/SettingManager.h"
#include "I18n/I18n.h"
#include "IBL.h"
#include "NRD.h"
#include "Skylighting.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"
#include "GpuPass.h"
#include "Utils/MathUtils.h"

#define I18N_KEY_PREFIX "feature.screen_space_reflections."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceReflections::Settings,
	Enabled,
	HalfRes,
	SpecularMult,
	MaxTraceRoughness,
	SpecMaxSteps,
	SpecThickness,
	NormalBias,
	BRDFBias,
	OcclusionStrength,
	UseDynamicCubemapsAsFallback,
	SpecCubemapMult,
	HitDistA,
	HitDistB,
	HitDistC,
	EnableREBLUR,
	Reblur,
	SpecularPrepassBlurRadius,
	UsePrepassOnlyForSpecularMotionEstimation,
	E11AmountScale)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceReflections::RestoreDefaultSettings()
{
	const bool wasEnabled = settings.Enabled;
	settings = {};
	if (globals::game::isVR) settings.Enabled = false;
	userSettings = settings;
	appliedE11Overlay = GetE11Overlay();
	if (appliedE11Overlay.active) {
		settings.Enabled = appliedE11Overlay.enabled;
		settings.SpecularMult = appliedE11Overlay.specularMult;
	}
	recompileFlag = true;
	QueueHistoryReset();
	resetReblurHistory = true;
	outputReady = false;
	denoisedThisFrame = false;
	// The SSR define is baked into the deferred composite; DrawSSR rebuilds it on the render thread.
	if (settings.Enabled != wasEnabled)
		queuedEnabledChange = true;
}

void ScreenSpaceReflections::DrawSettings()
{
	static bool showAdvanced;
	const auto syncUserSettings = SKSE::stl::scope_exit([this]() noexcept {
		Settings edited = settings;
		if (appliedE11Overlay.active) {
			edited.Enabled = userSettings.Enabled;
			edited.SpecularMult = userSettings.SpecularMult;
		}
		userSettings = edited;
	});
	const bool e11Managed = appliedE11Overlay.active;

	if (!ShadersOK())
		Util::Text::Error("%s", T(TKEY("compute_shaders_failed_to_compile"), "Compute shaders failed to compile!"));
	if (e11Managed)
		Util::Text::WrappedInfo("%s", T(TKEY("e11_managed"), "The active Effects 11 preset drives Enabled and Specular Multiplier from its [EFFECT] EnableReflection and [REFLECTION] Amount. Hand SSR back on its scene page to use your own values."));
	if (!globals::features::dynamicCubemaps.loaded)
		Util::Text::WrappedWarning("%s", T(TKEY("requires_dynamic_cubemaps"), "Requires Dynamic Cubemaps. Reflections are composited into the cubemap specular, so without it nothing is traced."));

	ImGui::SeparatorText(T(TKEY("toggles"), "Toggles"));

	ImGui::Checkbox(T(TKEY("show_advanced_options"), "Show Advanced Options"), &showAdvanced);

	{
		auto managedGuard = Util::DisableGuard(e11Managed);
		if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled)) {
			queuedEnabledChange = true;
			QueueHistoryReset();
		}
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Enable screen-space reflections. When disabled, the deferred composite falls back to cubemap-only reflections."));
	}

	{
		auto enabledGuard = Util::DisableGuard(!settings.Enabled);

		{
			auto halfResGuard = Util::DisableGuard(!settings.EnableREBLUR);
			if (ImGui::Checkbox(T(TKEY("half_resolution_checkerboard"), "Half Resolution (Checkerboard)"), &settings.HalfRes)) {
				recompileFlag = true;
				resetReblurHistory = true;
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("half_resolution_checkerboard_tooltip"), "Trace half the columns in a checkerboard pattern. NRD reconstructs the missing pixels."));
		}
	}

	ImGui::SeparatorText(T(TKEY("visual"), "Visual"));

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);
		{
			auto managedGuard = Util::DisableGuard(e11Managed);
			ImGui::SliderFloat(T(TKEY("specular_multiplier"), "Specular Multiplier"), &settings.SpecularMult, 0.0f, 5.0f, "%.2f");
		}
		if (ImGui::SliderFloat(T(TKEY("max_trace_roughness"), "Max Trace Roughness"), &settings.MaxTraceRoughness, 0.0f, 1.0f, "%.2f"))
			resetReblurHistory = true;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("max_trace_roughness_tooltip"), "Rougher surfaces skip the screen trace and use the cubemap reflection. Rough rays on grass, rock and dirt are mostly noise; lower this to trace only glossy surfaces."));
		}
		ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps_as_fallback"), "Use Dynamic Cubemaps as Fallback"), &settings.UseDynamicCubemapsAsFallback);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_as_fallback_tooltip"), "When ray marching misses, fall back to dynamic cubemaps for reflections."));
		}
		{
			auto cubemapGuard = Util::DisableGuard(!settings.UseDynamicCubemapsAsFallback);
			ImGui::SliderFloat(T(TKEY("specular_cubemap_multiplier"), "Specular Cubemap Multiplier"), &settings.SpecCubemapMult, 0.0f, 5.0f, "%.2f");
		}

		if (showAdvanced) {
			ImGui::Separator();
			ImGui::SliderInt(T(TKEY("max_steps"), "Max Steps"), (int*)&settings.SpecMaxSteps, 1, 256, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.SpecThickness, 0.0f, 50.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("normal_bias"), "Normal Bias"), &settings.NormalBias, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("normal_bias_tooltip"), "Push ray origin along surface normal to avoid self-intersection artifacts."));
			}
			ImGui::SliderFloat(T(TKEY("brdf_bias"), "BRDF Bias"), &settings.BRDFBias, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("brdf_bias_tooltip"), "Higher values reduce noise but make reflections more glossy."));
			}
			ImGui::SliderFloat(T(TKEY("occlusion_strength"), "Occlusion Strength"), &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("nrd_hit_distance_parameters"), "NRD Hit Distance Parameters"));
			bool hitDistanceChanged = false;
			hitDistanceChanged |= ImGui::SliderFloat(T(TKEY("hit_dist_a"), "Hit Dist A"), &settings.HitDistA, 1.0f, 1000.0f, "%.1f");
			hitDistanceChanged |= ImGui::SliderFloat(T(TKEY("hit_dist_b"), "Hit Dist B"), &settings.HitDistB, 0.0f, 1.0f, "%.3f");
			hitDistanceChanged |= ImGui::SliderFloat(T(TKEY("hit_dist_c"), "Hit Dist C"), &settings.HitDistC, 1.0f, 200.0f, "%.1f");
			resetReblurHistory |= hitDistanceChanged;
		}
	}

	ImGui::SeparatorText(T(TKEY("reblur_denoiser"), "REBLUR Denoiser"));

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::Checkbox(T(TKEY("enable_reblur"), "Enable REBLUR"), &settings.EnableREBLUR)) {
			if (!settings.EnableREBLUR && settings.HalfRes) {
				settings.HalfRes = false;
				recompileFlag = true;
			}
			resetReblurHistory = true;
		}

		if (settings.EnableREBLUR) {
			resetReblurHistory |= NRDPort::Service().DrawReblurSettings(settings.Reblur, showAdvanced, "ssr_reblur");
			if (showAdvanced) {
				resetReblurHistory |= ImGui::SliderFloat(T(TKEY("specular_prepass_blur_radius"), "Specular Pre-pass Radius"), &settings.SpecularPrepassBlurRadius, 0.0f, 75.0f, "%.1f px");
				resetReblurHistory |= ImGui::Checkbox(T(TKEY("specular_prepass_motion_only"), "Use Pre-pass Only for Motion Estimation"), &settings.UsePrepassOnlyForSpecularMotionEstimation);
			}
		}
	}

	#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		ImGui::SeparatorText(T(TKEY("e11_calibration"), "Effects 11 Presets"));
		ImGui::SliderFloat(T(TKEY("e11_amount_scale"), "Preset Reflection Scale"), &settings.E11AmountScale, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("e11_amount_scale_tooltip"), "Specular Multiplier is the preset's [REFLECTION] Amount times this."));
	}
#endif

	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
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
				if (const auto* right = NRDPort::RightSSRDebug())
					right->DrawBufferViewer(debugRescale);
				else
					ImGui::TextUnformatted(T("feature.nrd.eye_buffers_unavailable", "Right-eye buffers are not allocated yet."));
				ImGui::TreePop();
			}
		}

		ImGui::TreePop();
	}
}

void ScreenSpaceReflections::DrawBufferViewer(float debugRescale) const
{
	if (texHiZDepth)
		BUFFER_VIEWER_NODE(texHiZDepth, debugRescale)
	if (texNRDSpecInput)
		BUFFER_VIEWER_NODE(texNRDSpecInput, debugRescale)
	if (texNRDSpecOutput)
		BUFFER_VIEWER_NODE(texNRDSpecOutput, debugRescale)
	if (auto validation = settings.Reblur.EnableValidation ? nrdReblurSpecular.GetValidationSRV() : nullptr) {
		if (ImGui::TreeNode(T(TKEY("nrd_validation"), "NRD Validation"))) {
			ImGui::Image(validation, { nrdReblurSpecular.GetWidth() * debugRescale, nrdReblurSpecular.GetHeight() * debugRescale });
			ImGui::TreePop();
		}
	}
}

#undef I18N_KEY_PREFIX

namespace
{
	void ClampSetting(float& a_value, float a_min, float a_max, float a_default)
	{
		a_value = std::isfinite(a_value) ? std::clamp(a_value, a_min, a_max) : a_default;
	}
}

void ScreenSpaceReflections::LoadSettings(json& o_json)
{
	const auto previous = settings;
	const Settings defaults{};
	settings = o_json;
	if (globals::game::isVR && !o_json.contains("Enabled")) settings.Enabled = false;
	settings.SpecMaxSteps = std::clamp(settings.SpecMaxSteps, 1u, 256u);
	ClampSetting(settings.SpecularMult, 0.0f, 5.0f, defaults.SpecularMult);
	ClampSetting(settings.MaxTraceRoughness, 0.0f, 1.0f, defaults.MaxTraceRoughness);
	ClampSetting(settings.SpecCubemapMult, 0.0f, 5.0f, defaults.SpecCubemapMult);
	ClampSetting(settings.SpecThickness, 0.0f, 50.0f, defaults.SpecThickness);
	ClampSetting(settings.NormalBias, 0.0f, 1.0f, defaults.NormalBias);
	ClampSetting(settings.BRDFBias, 0.0f, 1.0f, defaults.BRDFBias);
	ClampSetting(settings.OcclusionStrength, 0.0f, 1.0f, defaults.OcclusionStrength);
	ClampSetting(settings.HitDistA, 1.0f, 1000.0f, defaults.HitDistA);
	ClampSetting(settings.HitDistB, 0.0f, 1.0f, defaults.HitDistB);
	ClampSetting(settings.HitDistC, 1.0f, 200.0f, defaults.HitDistC);
	ClampSetting(settings.SpecularPrepassBlurRadius, 0.0f, 75.0f, defaults.SpecularPrepassBlurRadius);
	ClampSetting(settings.E11AmountScale, 0.0f, 4.0f, defaults.E11AmountScale);
	if (!settings.EnableREBLUR)
		settings.HalfRes = false;
	userSettings = settings;
	appliedE11Overlay = GetE11Overlay();
	if (appliedE11Overlay.active) {
		settings.Enabled = appliedE11Overlay.enabled;
		settings.SpecularMult = appliedE11Overlay.specularMult;
	}
	recompileFlag = recompileFlag || settings.HalfRes != previous.HalfRes;
	if (settings.Enabled != previous.Enabled && globals::deferred)
		globals::deferred->ClearShaderCache();
	if (recompileFlag ||
		settings.Enabled != previous.Enabled ||
		settings.EnableREBLUR != previous.EnableREBLUR ||
		settings.SpecMaxSteps != previous.SpecMaxSteps ||
		settings.UseDynamicCubemapsAsFallback != previous.UseDynamicCubemapsAsFallback)
		resetReblurHistory = true;
	outputReady = false;
}

void ScreenSpaceReflections::SaveSettings(json& o_json)
{
	o_json = userSettings;
}

bool ScreenSpaceReflections::IsManagedByE11() const
{
	#if defined(ENABLE_EFFECTS11)
	const auto& effects11 = globals::features::effects11;
	return effects11.IsActive() && effects11.OwnsFeature(E11Handoff::Feature::SSR);
#else
	return false;
#endif
}

ScreenSpaceReflections::E11Overlay ScreenSpaceReflections::GetE11Overlay() const
{
	E11Overlay overlay;
	if (!IsManagedByE11())
		return overlay;
#if defined(ENABLE_EFFECTS11)
	auto& settingManager = SettingManager::GetSingleton();
	const bool interior = Util::IsInterior();
	overlay.active = true;
	overlay.enabled = settingManager.GetValue<bool>("EnableReflection", "EFFECT") &&
	                  (!interior || settingManager.GetValue<bool>("EnableInterior", "REFLECTION"));
	const float amount = settingManager.GetValue<float>(interior ? "AmountInterior" : "Amount", "REFLECTION");
	const float specularMult = amount * userSettings.E11AmountScale;
	overlay.specularMult = std::isfinite(specularMult) ? std::clamp(specularMult, 0.0f, 5.0f) : 1.0f;
#endif
	return overlay;
}

void ScreenSpaceReflections::RefreshE11Overlay()
{
	if (GetE11Overlay() == appliedE11Overlay)
		return;
	json document = userSettings;
	LoadSettings(document);
}

void ScreenSpaceReflections::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;
	resetReblurHistory = true;
	outputReady = false;

	logger::debug("ScreenSpaceReflections: creating buffers");
	{
		ssrCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSRCB>(), "SSR::CB");
	}

	logger::debug("ScreenSpaceReflections: creating textures");
	{
		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC texDesc;
		Util::AsReal(mainTex.texture)->GetDesc(&texDesc);
		NRDPort::EyeTextureDesc(texDesc);

		uint32_t fullW = texDesc.Width;
		uint32_t fullH = texDesc.Height;

		// Hi-Z depth pyramid (R32F, raw NDC depth with min-Z mip chain)
		{
			uint minDim = std::min(fullW, fullH);
			numHiZMips = 1;
			while ((minDim >> numHiZMips) >= 16 && numHiZMips < kMaxHiZMips)
				++numHiZMips;

			numHiZMips = std::max(numHiZMips, 7u);

			D3D11_TEXTURE2D_DESC hizDesc{
				.Width = fullW,
				.Height = fullH,
				.MipLevels = numHiZMips,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_FLOAT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			};
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = hizDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = numHiZMips }
			};
			texHiZDepth = eastl::make_unique<Texture2D>(hizDesc, "SSR::HiZDepth");
			texHiZDepth->CreateSRV(srvDesc);

			for (uint i = 0; i < numHiZMips; i++) {
				D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
					.Format = hizDesc.Format,
					.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
				};
				DX::ThrowIfFailed(device->CreateShaderResourceView(texHiZDepth->resource.get(), &mipSrvDesc, hiZDepthSRVs[i].put()));
				Util::SetResourceName(hiZDepthSRVs[i].get(), "SSR::HiZDepth SRV mip%u", i);

				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = hizDesc.Format,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texHiZDepth->resource.get(), &mipUavDesc, hiZDepthUAVs[i].put()));
				Util::SetResourceName(hiZDepthUAVs[i].get(), "SSR::HiZDepth UAV mip%u", i);
			}
		}

		SetupNRDResources(fullW, fullH);
	}

	logger::debug("ScreenSpaceReflections: creating samplers");
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
		Util::SetResourceName(linearClampSampler.get(), "SSR::LinearClampSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
		Util::SetResourceName(pointClampSampler.get(), "SSR::PointClampSampler");
	}

	CompileComputeShaders();
}

void ScreenSpaceReflections::SetupNRDResources(uint32_t a_width, uint32_t a_height)
{
	D3D11_TEXTURE2D_DESC specDesc{
		.Width = a_width,
		.Height = a_height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = specDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = specDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	resetReblurHistory = true;
	outputReady = false;

	texNRDSpecInput = eastl::make_unique<Texture2D>(specDesc, "SSR::NRDInput");
	texNRDSpecInput->CreateSRV(srvDesc);
	texNRDSpecInput->CreateUAV(uavDesc);

	texNRDSpecOutput = eastl::make_unique<Texture2D>(specDesc, "SSR::NRDOutput");
	texNRDSpecOutput->CreateSRV(srvDesc);
	texNRDSpecOutput->CreateUAV(uavDesc);

	nrdReblurSpecular.Shutdown();
	reblurInitAttempted = WantsReblur();
	if (reblurInitAttempted)
		nrdReblurSpecular.Init(a_width, a_height, nrd::Denoiser::REBLUR_SPECULAR, 0);
}

bool ScreenSpaceReflections::WantsReblur() const
{
	return settings.Enabled && settings.EnableREBLUR && globals::features::dynamicCubemaps.loaded;
}

void ScreenSpaceReflections::ClearShaderCache()
{
	NRDPort::InvalidateStereo();
	resetReblurHistory = true;
	outputReady = false;
	denoisedThisFrame = false;
	prefilterHiZDepthCompute = nullptr;
	prefilterHiZMipsCompute = nullptr;
	depthDownsampleCompute = nullptr;
	specularGICompute = nullptr;
	CompileComputeShaders();
}

void ScreenSpaceReflections::CompileComputeShaders()
{
	{
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "prefilterHiZDepth.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")))
			prefilterHiZDepthCompute.attach(rawPtr);
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), { { "FUSED_HIZ_MIPS", "" } }, "cs_5_0")))
			prefilterHiZMipsCompute.attach(rawPtr);
	}
	{
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "depthDownsample.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")))
			depthDownsampleCompute.attach(rawPtr);
	}
	{
		std::vector<std::pair<const char*, const char*>> defines;
		tracedHalfRes = settings.HalfRes && CanDenoise();
		if (tracedHalfRes)
			defines.push_back({ "SSR_HALF", "" });
		if (globals::features::dynamicCubemaps.loaded)
			defines.push_back({ "DYNAMIC_CUBEMAPS", "" });
		if (globals::features::skylighting.loaded)
			defines.push_back({ "SKYLIGHTING", "" });
		if (globals::features::ibl.loaded)
			defines.push_back({ "IBL", "" });
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceReflections") / "specularGI.cs.hlsl";
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0")))
			specularGICompute.attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceReflections::CanDenoise() const
{
	return settings.EnableREBLUR && nrdReblurSpecular.IsValid() && NRDPort::Service().CanDenoise() && NRDPort::Service().AreGuidesReady();
}

bool ScreenSpaceReflections::ShadersOK()
{
	return prefilterHiZDepthCompute && prefilterHiZMipsCompute && depthDownsampleCompute && specularGICompute;
}

void ScreenSpaceReflections::UpdateSB()
{
	float2 res = { (float)texHiZDepth->desc.Width, (float)texHiZDepth->desc.Height };
	float2 dynres = NRDPort::ActiveSize();
	dynres = { floor(dynres.x), floor(dynres.y) };

	SSRCB data;
	data.TexDim = res;
	data.RcpTexDim = float2(1.0f) / res;
	data.FrameDim = dynres;
	data.RcpFrameDim = float2(1.0f) / dynres;
	data.FrameIndex = globals::state->frameCount;

	data.SpecMaxSteps = settings.SpecMaxSteps;
	data.SpecMaxMips = std::min((uint)settings.SpecMaxSteps, numHiZMips);
	data.SpecThickness = settings.SpecThickness;
	data.NormalBias = settings.NormalBias;
	data.BRDFBias = settings.BRDFBias;
	data.OcclusionStrength = settings.OcclusionStrength;
	data.SpecUseDynamicCubemap = (settings.UseDynamicCubemapsAsFallback && globals::features::dynamicCubemaps.loaded) ? 1u : 0u;
	data.SpecCubemapMult = settings.SpecCubemapMult;
	data.HitDistA = settings.HitDistA;
	data.HitDistB = settings.HitDistB;
	data.HitDistC = settings.HitDistC;
	data.MaxTraceRoughness = settings.MaxTraceRoughness;

	ssrCB->Update(data);
}

void ScreenSpaceReflections::DrawSSR()
{
	outputReady = false;
	denoisedThisFrame = false;


	if (queuedEnabledChange.exchange(false)) {
		resetReblurHistory = true;
		if (globals::deferred)
			globals::deferred->ClearShaderCache();
	}
	if (queuedHistoryReset.exchange(false))
		resetReblurHistory = true;

	if (texNRDSpecInput) {
		uint32_t nrdWidth = 0;
		uint32_t nrdHeight = 0;
		const auto& nrdSvc = NRDPort::Service();
		if (nrdSvc.loaded && nrdSvc.GetResourceExtent(nrdWidth, nrdHeight) &&
			(texNRDSpecInput->desc.Width != nrdWidth || texNRDSpecInput->desc.Height != nrdHeight))
			SetupNRDResources(nrdWidth, nrdHeight);

		if (!WantsReblur()) {
			if (nrdReblurSpecular.IsValid()) {
				nrdReblurSpecular.Shutdown();
				resetReblurHistory = true;
			}
			reblurInitAttempted = false;
		} else if (!reblurInitAttempted) {
			SetupNRDResources(texNRDSpecInput->desc.Width, texNRDSpecInput->desc.Height);
		}
	}

	if ((settings.HalfRes && CanDenoise()) != tracedHalfRes) {
		recompileFlag = true;
		resetReblurHistory = true;
	}

	if (recompileFlag)
		ClearShaderCache();

	if (!(settings.Enabled && globals::features::dynamicCubemaps.loaded && ShadersOK() && texHiZDepth && texNRDSpecInput))
		return;

	if (globals::state->isMapMenuOpen)
		return;

	ZoneScoped;
	CS_GPU_PASS("ScreenSpaceReflections::Total");

	const uint frame = globals::state->frameCount;
	if (globals::state->ShouldResetHistory() || frame != lastDrawnFrame + 1u)
		resetReblurHistory = true;
	lastDrawnFrame = frame;


	UpdateSB();
	NRDPort::BindEye();

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;

	float2 size = NRDPort::ActiveSize();
	auto resolution = std::array{ (uint)size.x, (uint)size.y };
	float2 dynres = { floor(size.x), floor(size.y) };

	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssrCB->CB();

	context->CSSetConstantBuffers(1, 1, &cb);
	globals::state->BindSharedDataCS(context);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	auto resetViews = [&]() {
		ID3D11ShaderResourceView* nullSRVs[8] = {};
		ID3D11UnorderedAccessView* nullUAVs[8] = {};
		context->CSSetShaderResources(0, 8, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	};

	// Hi-Z depth pyramid
	{
		CS_GPU_PASS("ScreenSpaceReflections::HiZDepth");

		uint firstDownsampleMip = 0;
		const bool canFuseFirstFiveMips = numHiZMips >= 5 &&
		                                  (texHiZDepth->desc.Width & 15u) == 0 &&
		                                  (texHiZDepth->desc.Height & 15u) == 0;
		if (canFuseFirstFiveMips) {
			// Build mip 0-4 in one pass. Each group covers a 16x16 source tile,
			// then performs the same min-Z reduction in group-shared memory.
			resetViews();
			auto srv = NRDPort::Crop(NRDPort::Input::Depth, Util::GetCurrentSceneDepthSRV(false));
			context->CSSetShaderResources(0, 1, &srv);
			std::array<ID3D11UnorderedAccessView*, 5> uavs;
			for (uint i = 0; i < (uint)uavs.size(); ++i)
				uavs[i] = hiZDepthUAVs[i].get();
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(prefilterHiZMipsCompute.get(), nullptr, 0);
			context->Dispatch(((uint)dynres.x + 15) / 16, ((uint)dynres.y + 15) / 16, 1);
			firstDownsampleMip = 4;
		} else {
			// Small or non-16-aligned render targets keep the generic path so its
			// NPOT edge-reduction behavior remains unchanged.
			resetViews();
			auto srv = NRDPort::Crop(NRDPort::Input::Depth, Util::GetCurrentSceneDepthSRV(false));
			context->CSSetShaderResources(0, 1, &srv);
			ID3D11UnorderedAccessView* uav = hiZDepthUAVs[0].get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(prefilterHiZDepthCompute.get(), nullptr, 0);
			context->Dispatch(((uint)dynres.x + 7) / 8, ((uint)dynres.y + 7) / 8, 1);
		}

		// Min-Z downsample mip chain
		for (uint i = firstDownsampleMip; i < numHiZMips - 1; ++i) {
			// Round up: the ray march addresses cell floor(dynamicSize / 2^mip), which is a partial cell whenever the
			// dynamic size is not a multiple of the mip's texel size. Rounding down left that cell holding depth from
			// whatever render scale last covered it. The shader bounds writes by the mip's real dimensions.
			const uint shift = i + 1;
			uint outW = std::max(1u, ((uint)size.x + (1u << shift) - 1) >> shift);
			uint outH = std::max(1u, ((uint)size.y + (1u << shift) - 1) >> shift);

			resetViews();
			ID3D11ShaderResourceView* inSrv = hiZDepthSRVs[i].get();
			ID3D11UnorderedAccessView* outUav = hiZDepthUAVs[i + 1].get();
			context->CSSetShaderResources(0, 1, &inSrv);
			context->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
			context->CSSetShader(depthDownsampleCompute.get(), nullptr, 0);
			context->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
		}

		resetViews();

	}

	// Specular GI ray march
	{
		CS_GPU_PASS("ScreenSpaceReflections::SpecularGI");

		auto normal = rts[NORMALROUGHNESS];
		auto main = rts[globals::deferred->forwardRenderTargets[0]];

		auto& dynamicCubemaps = globals::features::dynamicCubemaps;
		auto& ibl = globals::features::ibl;
		auto& skylighting = globals::features::skylighting;

		resetViews();
		std::array<ID3D11ShaderResourceView*, 7> specSRVs = { nullptr };
		specSRVs[0] = NRDPort::Crop(NRDPort::Input::Depth, Util::GetCurrentSceneDepthSRV(false));
		specSRVs[1] = NRDPort::Crop(NRDPort::Input::Normal, Util::AsReal(normal.SRV));
		specSRVs[2] = NRDPort::Crop(NRDPort::Input::Main, Util::AsReal(main.SRV));
		specSRVs[3] = texHiZDepth->srv.get();
		specSRVs[4] = dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr;
		specSRVs[5] = dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr;
		specSRVs[6] = dynamicCubemaps.loaded && skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
		ID3D11ShaderResourceView* iblViews[2] = {ibl.loaded && ibl.envIBLTexture ? ibl.envIBLTexture->srv.get() : nullptr, ibl.loaded && ibl.skyIBLTexture ? ibl.skyIBLTexture->srv.get() : nullptr};

		ID3D11UnorderedAccessView* specUAV = texNRDSpecInput->uav.get();

		context->CSSetShaderResources(0, (uint)specSRVs.size(), specSRVs.data());
		context->CSSetShaderResources(76, 2, iblViews);
		context->CSSetUnorderedAccessViews(0, 1, &specUAV, nullptr);
		context->CSSetShader(specularGICompute.get(), nullptr, 0);

		uint specDispatchX = tracedHalfRes ? (resolution[0] + 1) / 2 : resolution[0];
		uint specDispatchY = resolution[1];

		context->Dispatch((specDispatchX + 7u) >> 3, (specDispatchY + 7u) >> 3, 1);


		iblViews[0] = iblViews[1] = nullptr;
		context->CSSetShaderResources(76, 2, iblViews);
		resetViews();

	}

	// REBLUR denoise (specular)
	auto& nrdSvc = NRDPort::Service();
	if (settings.EnableREBLUR && nrdReblurSpecular.IsValid() && nrdSvc.loaded && nrdSvc.AreGuidesReady()) {
		CS_GPU_PASS("ScreenSpaceReflections::ReblurSpecular");


		nrdSvc.RefreshMotionVectors();
		auto commonSettings = nrdSvc.GetCommonSettings();
		commonSettings.splitScreen = settings.Reblur.SplitScreen;
		commonSettings.enableValidation = settings.Reblur.EnableValidation;
		if (resetReblurHistory)
			commonSettings.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
		nrdReblurSpecular.SetCommonSettings(commonSettings);

		nrdSvc.ApplyReblurSettings(reblurSpecularSettings, settings.Reblur,
			tracedHalfRes ? nrd::CheckerboardMode::BLACK : nrd::CheckerboardMode::OFF);
		reblurSpecularSettings.hitDistanceParameters.A = settings.HitDistA;
		reblurSpecularSettings.hitDistanceParameters.B = settings.HitDistB;
		reblurSpecularSettings.hitDistanceParameters.C = settings.HitDistC;
		reblurSpecularSettings.specularPrepassBlurRadius = std::max(settings.SpecularPrepassBlurRadius, 0.0f);
		reblurSpecularSettings.usePrepassOnlyForSpecularMotionEstimation = settings.UsePrepassOnlyForSpecularMotionEstimation;
		nrdReblurSpecular.SetDenoiserSettings(&reblurSpecularSettings);

		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorSRV());
		nrdReblurSpecular.SetNamedUAV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorUAV());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_NORMAL_ROUGHNESS, nrdSvc.GetNormalRoughnessSRV());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_VIEWZ, nrdSvc.GetViewZSRV());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, texNRDSpecInput->srv.get());
		nrdReblurSpecular.SetNamedSRV(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, texNRDSpecOutput->srv.get());
		nrdReblurSpecular.SetNamedUAV(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, texNRDSpecOutput->uav.get());

		denoisedThisFrame = nrdReblurSpecular.Dispatch();
		resetReblurHistory = !denoisedThisFrame;

	}

	// cleanup
	resetViews();
	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
	outputReady = !tracedHalfRes || denoisedThisFrame;

}

ID3D11ShaderResourceView* ScreenSpaceReflections::GetOutputTexture()
{
	if (!outputReady || !loaded || !settings.Enabled)
		return nullptr;
	if (denoisedThisFrame)
		return texNRDSpecOutput->srv.get();
	return texNRDSpecInput->srv.get();
}

ScreenSpaceReflections::SharedData ScreenSpaceReflections::GetCommonBufferData()
{
	SharedData data;
	data.Enabled = loaded && settings.Enabled ? NRDPort::SpecularMask() : 0u;
	data.SpecularMult = settings.SpecularMult;
	data.SpecCubemapMult = settings.UseDynamicCubemapsAsFallback && globals::features::dynamicCubemaps.loaded ? settings.SpecCubemapMult : 0.0f;
	data.pad0 = 0;
	return data;
}
