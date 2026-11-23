#include "PostProcessing.h"

#include "GpuPass.h"
#include "LinearLighting.h"
#include "PostProcessing/RasterPass.h"

#include "IconsTabler.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "PostProcessingMode.h"
#include "Presets/PostProcessingPresets.h"
#include "Presets/PresetProvider.h"
#include "imgui_stdlib.h"

#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "Globals.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "Profiler.h"
#include "State.h"
#include "Util.h"
#include "Utils/RenderExtent.h"
#include "Utils/SettingsControl.h"
#include "Utils/SettingsPatch.h"

#include "Features/PostProcessing/PostProcessingUI.h"
#include "Features/Upscaling.h"

#include <algorithm>
#include <array>

namespace
{
	constexpr std::array kPointerButtons{ ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle };
	constexpr std::array kActivateKeys{ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown };

	/** @brief Whether this frame's input could activate a control, so idle frames skip the settings diff. */
	bool InputMayEditSettings()
	{
		return ImGui::IsAnyItemActive() ||
		       std::ranges::any_of(kPointerButtons, [](auto button) { return ImGui::IsMouseClicked(button) || ImGui::IsMouseReleased(button); }) ||
		       std::ranges::any_of(kActivateKeys, [](auto key) { return ImGui::IsKeyPressed(key); });
	}

	bool skipVanillaDoF = false;
	bool skipVanillaBloom = false;
	std::optional<bool> savedVanillaLensFlare;

	struct VanillaDoF_IsActive
	{
		static bool thunk(RE::ImageSpaceEffectDepthOfField* effect)
		{
			return !skipVanillaDoF && func(effect);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VanillaBlur_Render
	{
		static void thunk(RE::ImageSpaceEffect* effect, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
		{
			if (skipVanillaBloom &&
				effect->effectTextures.capacity() > 0 && effect->effectTextures[0] &&
				effect->effectTextures[0]->renderTarget == RE::RENDER_TARGETS::kHDR_BLOOM)
				return;
			func(effect, shape, param);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace
{
	constexpr float kLegacySceneGamma = 1.6f;

	constexpr bool IsPipelineFeatureEnabledByDefault(PostProcessing::FeaturePipelineIndex a_index)
	{
		using Index = PostProcessing::FeaturePipelineIndex;
		switch (a_index) {
		case Index::DoF:
		case Index::LocalExposure:
		case Index::LensFlare:
		case Index::Vignette:
		case Index::AutoExposure:
		case Index::CODBloom:
		case Index::Composite:
		case Index::ColorGrading:
			return true;
		case Index::MotionBlur:
		case Index::PhysicalGlare:
		case Index::PhysicalLens:
		case Index::LUT:
		case Index::Camera:
		case Index::Border:
		case Index::COUNT:
			return false;
		}
		return false;
	}
}

PostProcessFeature* PostProcessing::GetOpenEffectWithDebug() const
{
	if (activeSettingsPage != SettingsPage::SubFeature || activePipelineFeature >= pipeline.size())
		return nullptr;
	auto* feat = pipeline[activePipelineFeature].get();
	return feat && feat->IsActive() && feat->HasDebugSection() ? feat : nullptr;
}

void PostProcessing::DrawSettings()
{
	json before;
	// Unbound widgets (buttons, popup combos) bypass the interceptor, so a diff is their only edit signal.
	const bool watchEdits = !SettingsControl::IsReplicating() && InputMayEditSettings();
	if (watchEdits)
		SaveSettings(before);

	PostProcessingMode::DrawSelector();

	ImGui::Spacing();

	// The pipeline only runs in its own mode, so its controls would do nothing in the others.
	const auto mode = PostProcessingMode::Get();
	const Util::LockedSection modeLock(mode != PostProcessingMode::Mode::PostProcessing,
		mode == PostProcessingMode::Mode::Effects11 ?
			T("common.settings_managed_by_enb", "This setting is managed by Effects 11.") :
			T("feature.post_processing.off_in_vanilla", "Vanilla mode is active. Select Post Processing above to use these settings."));

	// Draws a read-only checkbox for a forced-on sub-feature and returns true. Otherwise callers bind
	// `&feat->enabled` themselves: the scene catalog generator only recognises that form as a scene toggle.
	auto drawForcedEnabled = [this](const char* label, PostProcessFeature& feature) {
		const bool automatic = feature.IsAutoEnabled() ||
		                       (&feature == GetPipelineFeature<HistogramAutoExposure>(FeaturePipelineIndex::AutoExposure) && GetActivePhysicalCameraState());
		if (!automatic)
			return false;
		bool active = feature.IsActive();
		ImGui::BeginDisabled();
		ImGui::Checkbox(label, &active);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.post_processing.cinematic_camera.exposure_required", "Exposure processing is required by Cinematic Camera. Disable Cinematic Camera to restore the saved enable state."));
		return true;
	};

	// Sub-page header: the way back, then the page title and its description.
	auto drawSubPageHeader = [&](const char* title, const char* description) {
		if (Util::LabeledButton("##BackToPipeline", Icons::TI(ICON_TI_ARROW_LEFT),
				T("feature.post_processing.back_to_pipeline", "Back to Pipeline")))
			activeSettingsPage = SettingsPage::Pipeline;
		ImGui::Spacing();
		{
			MenuFonts::FontRoleGuard heading(Menu::FontRole::Heading);
			ImGui::TextUnformatted(title);
		}
		if (description && description[0])
			Util::SettingHint(description, false);
		ImGui::Separator();
		ImGui::Spacing();
	};

	if (activeSettingsPage == SettingsPage::Pipeline) {
		ImGui::PushID("CinematicCamera");
		ImGui::Checkbox("##Enabled", &cinematicCamera.settings.Enabled);
		ImGui::SameLine();
		if (Util::LabeledButton("##Bars", Icons::TI(ICON_TI_MENU_2)))
			activeSettingsPage = SettingsPage::CinematicCamera;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.post_processing.edit_settings_for_this_feature", "Edit settings for this feature."));
		ImGui::SameLine();
		ImGui::TextUnformatted(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.post_processing.cinematic_camera.description",
				"Controls lens, focus, exposure and FOV. Exposure processing runs automatically while the camera is active; other effects must be enabled separately."));
		ImGui::PopID();

		for (int i = 0; i < pipeline.size(); ++i) {
			auto& feat = pipeline[i];
			if (feat && feat->IsVisible()) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());
				if (!drawForcedEnabled("##Enabled", *feat))
					ImGui::Checkbox("##Enabled", &feat->enabled);
				ImGui::SameLine();
				if (Util::LabeledButton("##Bars", Icons::TI(ICON_TI_MENU_2))) {
					activePipelineFeature = i;
					activeSettingsPage = SettingsPage::SubFeature;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T("feature.post_processing.edit_settings_for_this_feature", "Edit settings for this feature."));
				ImGui::SameLine();
				ImGui::Text("%s", displayName.c_str());
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", description.c_str());
				if (feat->IsExperimental()) {
					ImGui::SameLine();
					Util::Badge(T("feature.post_processing.experimental_badge", "Experimental"), Util::Colors::GetWarning(), { .rowHeight = ImGui::GetFrameHeight() });
				}
				ImGui::PopID();
			}
		}
	} else if (activeSettingsPage == SettingsPage::SubFeature) {
		if (activePipelineFeature >= 0 && activePipelineFeature < pipeline.size() && pipeline[activePipelineFeature]) {
			auto& feat = pipeline[activePipelineFeature];
			auto displayName = feat->GetDisplayName();
			auto description = feat->GetDesc();
			ImGui::PushID(feat->GetType().c_str());

			// Recompiling is a troubleshooting action, so it sits quietly at the header's right
			const float headerTop = ImGui::GetCursorPosY();
			drawSubPageHeader(displayName.c_str(), description.c_str());
			const float headerBottom = ImGui::GetCursorPosY();
			ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - Util::IconButtonSize(), headerTop));
			if (Util::IconButton("##RecompileShaders", Icons::TI(ICON_TI_REFRESH),
					T("feature.post_processing.recompile_shaders_for_this_sub_feature_only", "Recompile shaders for this sub-feature only.")))
				feat->ClearShaderCache();
			ImGui::SetCursorPosY(headerBottom);

			if (!drawForcedEnabled(T("feature.post_processing.enabled", "Enabled"), *feat))
				ImGui::Checkbox(T("feature.post_processing.enabled", "Enabled"), &feat->enabled);
			ImGui::Spacing();
			if (feat->IsActive())
				feat->DrawSettings();
			else
				Util::SettingHint(T("feature.post_processing.enable_the_feature_to_see_its_settings", "Enable the feature to see its settings."), false);

			ImGui::PopID();
		} else {
			ImGui::TextDisabled("%s", T("feature.post_processing.invalid_feature_selected_returning_to_list", "Invalid feature selected. Returning to list."));
			activeSettingsPage = SettingsPage::Pipeline;
		}
	} else if (activeSettingsPage == SettingsPage::CinematicCamera) {
		drawSubPageHeader(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"),
			T("feature.post_processing.cinematic_camera.description",
				"Controls lens, focus, exposure and FOV. Exposure processing runs automatically while the camera is active; other effects must be enabled separately."));

		ImGui::Checkbox(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"), &cinematicCamera.settings.Enabled);
		ImGui::Spacing();
		if (!cinematicCamera.settings.Enabled)
			Util::SettingHint(T("feature.post_processing.enable_the_feature_to_see_its_settings", "Enable the feature to see its settings."), false);

		cinematicCamera.DrawSettings(GetPipelineFeature<PhysicalLens>(FeaturePipelineIndex::PhysicalLens));
		if (cinematicCamera.settings.Enabled) {
			if (auto* exposure = GetPipelineFeature<HistogramAutoExposure>(FeaturePipelineIndex::AutoExposure)) {
				ImGui::SeparatorText(T("feature.post_processing.cinematic_camera.exposure_meter", "Exposure Meter"));
				exposure->DrawCameraExposureReadout();
			}

			// Linked-effect status: which adapters are currently consuming overrides.
			ImGui::SeparatorText(T("feature.post_processing.cinematic_camera.linked_effects", "Linked Effects"));
			if (ImGui::BeginTable("##LinkedEffects", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
				auto drawLinked = [&](FeaturePipelineIndex idx, const char* fallbackName) {
					auto& pipe = pipeline[static_cast<size_t>(idx)];
					if (!pipe)
						return;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(pipe->GetDisplayName().empty() ? fallbackName : pipe->GetDisplayName().c_str());
					ImGui::TableNextColumn();
					if (pipe->IsActive())
						ImGui::TextColored(Util::Colors::GetSuccess(), "%s", T("feature.post_processing.cinematic_camera.linked_active", "Active"));
					else
						ImGui::TextDisabled("%s", T("feature.post_processing.cinematic_camera.linked_disabled", "Disabled - no override"));
				};
				drawLinked(FeaturePipelineIndex::DoF, "Depth of Field");
				drawLinked(FeaturePipelineIndex::LensFlare, "Lens Flare");
				drawLinked(FeaturePipelineIndex::PhysicalLens, "Physical Lens");
				drawLinked(FeaturePipelineIndex::PhysicalGlare, "Physical Glare");
				drawLinked(FeaturePipelineIndex::Vignette, "Vignette");
				drawLinked(FeaturePipelineIndex::MotionBlur, "Motion Blur");
				drawLinked(FeaturePipelineIndex::AutoExposure, "Histogram Auto Exposure");
				drawLinked(FeaturePipelineIndex::Camera, "Camera");
				ImGui::EndTable();
			}
		}
	}

	if (watchEdits) {
		json after;
		SaveSettings(after);
		if (before != after)
			globals::sceneSettingsManager->CaptureExternalFeatureChanges(this, true);
	}
}

void PostProcessing::LoadSettings(json& o_json)
{
	if (!pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)])
		pendingSettings = o_json;
	else
		ProcessSettings(o_json);
}

void PostProcessing::ProcessSettings(json& o_json)
{
	if (o_json.contains("cinematic_camera")) {
		json cameraSettings;
		cinematicCamera.SaveSettings(cameraSettings);
		json merged{ { "cinematic_camera", std::move(cameraSettings) } };
		Util::Settings::OverlayRecognizedRootSettings(merged, o_json);
		cinematicCamera.LoadSettings(merged["cinematic_camera"]);
	}

	logger::debug("Loading post processing settings...");

	for (auto& feat : pipeline) {
		if (feat && o_json.contains(feat->GetType())) {
			json featureSettings;
			feat->SaveSettings(featureSettings);
			json merged{ { "enabled", feat->enabled }, { "settings", std::move(featureSettings) } };
			Util::Settings::OverlayRecognizedRootSettings(merged, o_json[feat->GetType()]);
			if (!feat->IsAutoEnabled())
				feat->enabled = merged["enabled"].get<bool>();
			feat->LoadSettings(merged["settings"]);
		}
	}
}

void PostProcessing::SaveSettings(json& o_json)
{
	if (!pendingSettings.empty()) {
		o_json = pendingSettings;
		o_json.erase("ppsettings");
		return;
	}

	SaveActiveSettings(o_json);
}

void PostProcessing::SaveActiveSettings(json& o_json)
{
	const bool built = pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)] != nullptr;
	if (!built)
		BuildPipeline();
	const SKSE::stl::scope_exit dropDefaults([&] {
		if (!built)
			pipeline = {};
	});
	for (auto& pipe : pipeline) {
		if (pipe) {
			json featureSetting{};
			pipe->SaveSettings(featureSetting);
			o_json[pipe->GetType()] = {
				{ "enabled", pipe->enabled },
				{ "settings", featureSetting }
			};
		}
	}

	json cameraSettings;
	cinematicCamera.SaveSettings(cameraSettings);
	o_json["cinematic_camera"] = cameraSettings;
	o_json.erase("ppsettings");
}

void PostProcessing::ApplyContextualDefaults(json& a_defaults) const
{
	for (const auto& pipe : pipeline) {
		if (!pipe)
			continue;
		if (auto section = a_defaults.find(pipe->GetType()); section != a_defaults.end() && section->contains("settings"))
			pipe->ApplyContextualDefaults((*section)["settings"]);
	}
}

void PostProcessing::RestoreDefaultSettings()
{
	pendingSettings = {};
	cinematicCamera.RestoreDefaultSettings();
	if (!pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)])
		return;
	RestorePipelineDefaultEnablement();
	for (auto& pipe : pipeline)
		if (pipe)
			pipe->RestoreDefaultSettings();
}

bool PostProcessing::HasActivePipelineFeature() const
{
	return activeSettingsPage == SettingsPage::SubFeature &&
	       activePipelineFeature < pipeline.size() &&
	       pipeline[activePipelineFeature] != nullptr;
}

bool PostProcessing::HasScopedDefaultSettings() const
{
	return HasActivePipelineFeature();
}

bool PostProcessing::HasScopedOverrideSettings() const
{
	return HasActivePipelineFeature();
}

void PostProcessing::RestoreCurrentPageDefaultSettings()
{
	if (!HasActivePipelineFeature()) {
		RestoreDefaultSettings();
		return;
	}
	auto& feature = pipeline[activePipelineFeature];
	if (!feature->IsAutoEnabled())
		feature->enabled = IsPipelineFeatureEnabledByDefault(static_cast<FeaturePipelineIndex>(activePipelineFeature));
	feature->RestoreDefaultSettings();
}

bool PostProcessing::ReapplyCurrentPageOverrideSettings()
{
	if (!ApplyPendingSettings())
		return false;
	return Feature::ReapplyCurrentPageOverrideSettings();
}

std::vector<std::string> PostProcessing::GetCurrentPageSettingKeys() const
{
	return HasActivePipelineFeature() ? std::vector<std::string>{ pipeline[activePipelineFeature]->GetType() } : std::vector<std::string>{};
}

bool PostProcessing::ApplyPendingSettings()
{
	if (!resourcesReady)
		return false;
	if (pendingSettings.empty())
		return true;

	json settingsToApply = std::move(pendingSettings);
	pendingSettings = {};
	try {
		ProcessSettings(settingsToApply);
		return true;
	} catch (const std::exception& e) {
		logger::warn("Failed to apply pending Post Processing settings. Error: {}", e.what());
		RestoreDefaultSettings();
		return false;
	}
}

void PostProcessing::RestorePipelineDefaultEnablement()
{
	for (size_t i = 0; i < pipeline.size(); ++i) {
		auto& feature = pipeline[i];
		if (!feature || feature->IsAutoEnabled())
			continue;
		feature->enabled = IsPipelineFeatureEnabledByDefault(static_cast<FeaturePipelineIndex>(i));
	}
}

void PostProcessing::CompileCopyShaders()
{
	fullscreenVS = nullptr;
	copyPS = nullptr;
	if (auto rawPtr = reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\fullscreen.hlsli", {}, "vs_5_0", "FullscreenTriangleVS")))
		fullscreenVS.attach(rawPtr);
	if (auto rawPtr = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\copy.ps.hlsl", {}, "ps_5_0")))
		copyPS.attach(rawPtr);
	if (!fullscreenVS || !copyPS)
		logger::error("Post Processing input/output shaders unavailable; using the game pipeline");
}

void PostProcessing::ClearShaderCache()
{
	CompileCopyShaders();
	for (size_t i = 0; i < pipeline.size(); ++i) {
		if (pipeline[i])
			pipeline[i]->ClearShaderCache();
		if (alternatePipeline.effects[i] && alternatePipeline.effects[i] != pipeline[i])
			alternatePipeline.effects[i]->ClearShaderCache();
	}
}

void PostProcessing::SetupResources()
{
	resourcesReady = false;
	postProcessingOutput = nullptr;
	D3D11_TEXTURE2D_DESC mainDesc;
	globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(Util::AsW32(&mainDesc));
	if (Util::IsMinimizedExtent(mainDesc.Width, mainDesc.Height))
		return;
	json activeSettings;
	SaveActiveSettings(activeSettings);
	alternatePipeline = {};
	D3D11_TEXTURE2D_DESC engineDesc{};

	try {
		{
			auto renderer = globals::game::renderer;
			auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

			D3D11_TEXTURE2D_DESC texDesc;
			D3D11_TEXTURE2D_DESC texMainDesc;
			D3D11_TEXTURE2D_DESC texMainCopyDesc;
			gameTexMain.texture->GetDesc(Util::AsW32(&texMainDesc));
			gameTexMainCopy.texture->GetDesc(Util::AsW32(&texMainCopyDesc));
			engineDesc = texMainCopyDesc;
			pipelineTextureDesc = texMainCopyDesc;
			inputProvider = Feature::FindLoadedFeature([&](Feature* feature) {
				const auto input = feature->GetPostProcessingInput();
				if (!input.width || !input.height)
					return false;
				pipelineTextureDesc.Width = input.width;
				pipelineTextureDesc.Height = input.height;
				return true;
			});
			texDesc = texMainDesc;

			texDesc.MipLevels = 1;
			texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
			texDesc.MiscFlags = 0;

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};

			texCopyMain = eastl::make_unique<Texture2D>(texDesc, "PostProcessing::Copy Target");
			texCopyMain->CreateRTV(rtvDesc);

			if (texMainCopyDesc.Format != texMainDesc.Format) {
				texDesc = texMainCopyDesc;
				rtvDesc.Format = texDesc.Format;
				texDesc.MipLevels = 1;
				texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
				texDesc.MiscFlags = 0;

				texCopyMainCopy = eastl::make_unique<Texture2D>(texDesc, "PostProcessing::Copy Target");
				texCopyMainCopy->CreateRTV(rtvDesc);
			} else {
				texCopyMainCopy = nullptr;
			}
		}

		{
			copyCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CopyCB>(), "PostProcessing::Copy Constants");
			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.MaxAnisotropy = 1;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			copySampler = nullptr;
			DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, copySampler.put()));
			Util::SetResourceName(copySampler.get(), "PostProcessing::Copy Sampler");
		}

		CompileCopyShaders();

		CreatePipelineResources(false);
		if (pipelineTextureDesc.Width != engineDesc.Width || pipelineTextureDesc.Height != engineDesc.Height) {
			SwapPipelineResources();
			pipelineTextureDesc = engineDesc;
			CreatePipelineResources(true);
			// Pre-upscale effects always read the engine-sized scene, so both pipelines use the engine-sized instance.
			for (size_t i = 0; i < pipeline.size(); ++i) {
				if (pipeline[i] && pipeline[i]->DrawBeforeUpscaling())
					alternatePipeline.effects[i] = pipeline[i];
			}
			SwapPipelineResources();
		}

		bokehResources.Setup();
		Util::RequestTargetLockAPI();

		ProcessSettings(activeSettings);
		resourcesReady = true;
		ApplyPendingSettings();
	} catch (const DX::com_exception& e) {
		resourcesReady = false;
		if (pendingSettings.empty())
			pendingSettings = std::move(activeSettings);
		else if (pendingSettings.is_object()) {
			activeSettings.update(pendingSettings, true);
			pendingSettings = std::move(activeSettings);
		}
		pipeline.fill(nullptr);
		alternatePipeline = {};
		texCopyMain = nullptr;
		texCopyMainCopy = nullptr;
		texInput = nullptr;
		texOutput = nullptr;
		copyCB = nullptr;
		copySampler = nullptr;
		fullscreenVS = nullptr;
		copyPS = nullptr;
		logger::error("Post Processing resource setup failed; using the game pipeline: {}", e.what());
	}
}

void PostProcessing::CreatePipelineResources(bool fallback)
{
	auto desc = pipelineTextureDesc;
	desc.MipLevels = 1;
	desc.MiscFlags = 0;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	texInput = std::make_unique<Texture2D>(desc, "PostProcessing::Linear Input");
	texInput->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
	texInput->CreateRTV({ .Format = desc.Format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
	texOutput = nullptr;
	if (inputProvider && !fallback) {
		texOutput = std::make_unique<Texture2D>(desc, "PostProcessing::Output");
		texOutput->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
		texOutput->CreateRTV({ .Format = desc.Format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
	}

	BuildPipeline();
	if (fallback) {
		pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)] = alternatePipeline.effects[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)];
		pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)] = alternatePipeline.effects[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)];
	}

	for (size_t i = 0; i < pipeline.size(); ++i) {
		auto& pipe = pipeline[i];
		if (pipe && (!fallback || pipe != alternatePipeline.effects[i])) {
			pipe->owner = this;
			pipe->SetupResources();
		}
	}
}

void PostProcessing::SwapPipelineResources()
{
	pipeline.swap(alternatePipeline.effects);
	texInput.swap(alternatePipeline.input);
	texOutput.swap(alternatePipeline.output);
	std::swap(pipelineTextureDesc, alternatePipeline.desc);
}

bool PostProcessing::SelectPipelineResources(ID3D11Texture2D* texture)
{
	D3D11_TEXTURE2D_DESC desc;
	texture->GetDesc(&desc);
	if (desc.Width == pipelineTextureDesc.Width && desc.Height == pipelineTextureDesc.Height)
		return true;
	if (desc.Width != alternatePipeline.desc.Width || desc.Height != alternatePipeline.desc.Height)
		return false;

	for (size_t i = 0; i < pipeline.size(); ++i) {
		auto& current = pipeline[i];
		auto& next = alternatePipeline.effects[i];
		if (current && next && current != next) {
			json featureSettings;
			current->SaveSettings(featureSettings);
			next->LoadSettings(featureSettings);
			next->enabled = current->enabled;
		}
	}
	SwapPipelineResources();
	return true;
}

void PostProcessing::Reset()
{
	skipVanillaDoF = false;
	skipVanillaBloom = false;
	postProcessingOutput = nullptr;
	// PreProcess may not run while bypassed or owned by Effects11, so clear refraction each frame.
	isrefraction = false;
}

void PostProcessing::CopyToRenderTarget(
	RE::BSGraphics::RenderTargetData& targetRT,
	Texture2D* convertTex,
	ID3D11Texture2D* srcTex,
	ID3D11ShaderResourceView* srcSRV, const CopyCB& conversion)
{
	const bool convert = conversion.gamma != 1.f || conversion.inputGamut != conversion.outputGamut;
	if (!targetRT.texture || !srcTex || (!convert && Util::AsReal(targetRT.texture) == srcTex))
		return;

	auto context = globals::d3d::context;

	D3D11_TEXTURE2D_DESC srcDesc;
	srcTex->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC targetDesc;
	targetRT.texture->GetDesc(Util::AsW32(&targetDesc));

	if (!convert && srcDesc.Format == targetDesc.Format && srcDesc.Width == targetDesc.Width && srcDesc.Height == targetDesc.Height) {
		CS_GPU_PASS("PostProcessing::CopyToTarget");
		context->CopySubresourceRegion(Util::AsReal(targetRT.texture), 0, 0, 0, 0, srcTex, 0, nullptr);
		return;
	}

	if (!copyPS || !fullscreenVS || !convertTex || !convertTex->rtv || !convertTex->resource)
		return;

	CS_GPU_PASS("PostProcessing::ConvertToTarget");
	DrawCopy(*convertTex, srcTex, srcSRV, conversion);

	context->CopySubresourceRegion(Util::AsReal(targetRT.texture), 0, 0, 0, 0, convertTex->resource.get(), 0, nullptr);
}

void PostProcessing::DrawCopy(Texture2D& target, ID3D11Texture2D* source, ID3D11ShaderResourceView* srv, CopyCB conversion)
{
	CS_GPU_PASS("PostProcessing::Copy");
	auto* context = globals::d3d::context;
	PostProcessingRaster::RasterPass pass(context);
	D3D11_TEXTURE2D_DESC sourceDesc;
	source->GetDesc(&sourceDesc);
	conversion.resample = sourceDesc.Width != target.desc.Width || sourceDesc.Height != target.desc.Height;
	copyCB->Update(conversion);
	ID3D11Buffer* cb = copyCB->CB();
	ID3D11SamplerState* sampler = copySampler.get();
	context->PSSetConstantBuffers(1, 1, &cb);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetShaderResources(0, 1, &srv);
	pass.SetTargets({ target.rtv.get() }, (float)target.desc.Width, (float)target.desc.Height);
	pass.SetShaders(fullscreenVS.get(), copyPS.get());
	pass.Draw();
}

void PostProcessing::BeginLinearProcessing(PostProcessFeature::TextureInfo& texture, bool scene)
{
	const auto& ll = globals::features::linearLighting;
	const bool linear = ll.IsLinearLightingActive();
	if (scene && linear)
		texture.gamut = ll.settings.enableACEScg ? Gamut::ACEScg : Gamut::Rec709;
	D3D11_TEXTURE2D_DESC desc;
	texture.tex->GetDesc(&desc);
	const auto matchesSource = [&](const Texture2D* candidate) {
		return candidate && candidate->desc.Width == desc.Width && candidate->desc.Height == desc.Height;
	};
	auto* input = matchesSource(texInput.get()) || !matchesSource(alternatePipeline.input.get()) ? texInput.get() : alternatePipeline.input.get();
	const float gamma = scene && !linear ? kLegacySceneGamma : 1.0f;
	if (gamma == 1.0f && matchesSource(input))
		return;

	DrawCopy(*input, texture.tex, texture.srv, CopyCB{ .inputGamut = texture.gamut, .outputGamut = texture.gamut, .gamma = gamma });
	texture = { input->resource.get(), input->srv.get(), texture.gamut };
}

void PostProcessing::DrawFeature(PostProcessFeature& feature, PostProcessFeature::TextureInfo& lastTexColor)
{
	if (feature.WritesToMainTexture()) {
		const auto gamut = lastTexColor.gamut;
		feature.Draw(lastTexColor);
		if (&feature != pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get())
			lastTexColor.gamut = gamut;
	} else {
		PostProcessFeature::TextureInfo inTex = lastTexColor;
		feature.Draw(inTex);
	}
}

void PostProcessing::DrawBeforeUpscaling()
{
	if (!IsPipelineRunning())
		return;

	auto& upscaling = globals::features::upscaling;
	if (!upscaling.loaded)
		return;

	auto renderer = globals::game::renderer;
	auto state = globals::state;

	bool inMainLoadingMenu = state->IsMainOrLoadingMenuOpen();
	auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { Util::AsReal(gameTexMain.texture), Util::AsReal(gameTexMain.SRV) };

	CS_GPU_PASS("PostProcessing::PreUpscale");
	bool processing = false;

	// update auto-enabled features
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsAutoEnabled())
			pipe->UpdateAutoEnabled();
	}

	// go through each fx
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsActive() && !pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && pipe->DrawBeforeUpscaling()) {
			if (!processing) {
				globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
				globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				if (!inMainLoadingMenu)
					BeginLinearProcessing(lastTexColor);
				processing = true;
			}
			DrawFeature(*pipe, lastTexColor);
		}
	}

	if (processing)
		CopyToRenderTarget(gameTexMain, texCopyMain.get(), lastTexColor.tex, lastTexColor.srv,
			CopyCB{ .inputGamut = lastTexColor.gamut, .outputGamut = lastTexColor.gamut, .gamma = inMainLoadingMenu || globals::features::linearLighting.IsLinearLightingActive() ? 1.0f : 1.0f / kLegacySceneGamma });
}

void PostProcessing::PreProcess(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output)
{
	CS_GPU_PASS("PostProcessing::Pipeline");
	postProcessingOutput = nullptr;
	if (!IsPipelineRunning())
		return;

	auto renderer = globals::game::renderer;

	auto& upscaling = globals::features::upscaling;

	// ISRefraction can leave kMAIN_COPY bound, which makes D3D11 null its SRV when sampled.
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	bool inMainLoadingMenu = globals::state->IsMainOrLoadingMenuOpen();

	auto& gameTexMainRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& gameTexMainCopyRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

	// The tonemap hook hands us the pass input directly, so no need to probe the bound RTV.
	// Refraction still routes through kMAIN_COPY without that being reflected in a_input.
	bool useMainCopy = isrefraction || a_input == RE::RENDER_TARGETS::kMAIN_COPY;

	auto gameTexMain = useMainCopy ? gameTexMainCopyRT : gameTexMainRT;
	PostProcessFeature::TextureInfo lastTexColor = { Util::AsReal(gameTexMain.texture), Util::AsReal(gameTexMain.SRV) };
	const auto input = inputProvider && inputProvider->loaded ? inputProvider->GetPostProcessingInput() : PostProcessingInput{};
	const bool hasInput = input.texture && input.srv;
	if (hasInput)
		lastTexColor = { input.texture, input.srv };
	if (!SelectPipelineResources(lastTexColor.tex))
		return;
	BeginLinearProcessing(lastTexColor, !inMainLoadingMenu);
	auto gameTexMainAlt = useMainCopy ? gameTexMainRT : gameTexMainCopyRT;

	// update auto-enabled features
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsAutoEnabled())
			pipe->UpdateAutoEnabled();
	}

	const auto canDraw = [&](const auto& pipe) {
		return pipe && pipe->IsActive() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) &&
		       (!pipe->DrawBeforeUpscaling() || !upscaling.loaded);
	};
	const auto gradingIndex = static_cast<size_t>(FeaturePipelineIndex::ColorGrading);
	const auto firstLaterFeature = pipeline.begin() + gradingIndex + 1;
	const auto canDrawAfterGrading = [&](const auto& pipe) { return canDraw(pipe) && pipe->DrawAfterColorGrading(); };
	const bool directTarget = !hasInput && !inMainLoadingMenu &&
	                          std::none_of(firstLaterFeature, pipeline.end(), canDraw) &&
	                          std::none_of(pipeline.begin(), firstLaterFeature, canDrawAfterGrading);
	bool gradingWroteAlternate = false;

	// go through each fx
	for (auto& pipe : pipeline) {
		if (!canDraw(pipe) || pipe->DrawAfterColorGrading())
			continue;
		if (directTarget && pipe == pipeline[gradingIndex]) {
			const auto gradingTarget = PostProcessFeature::TextureInfo{
				Util::AsReal(gameTexMain.texture), Util::AsReal(gameTexMain.SRV)
			};
			gradingWroteAlternate = static_cast<ColorGrading&>(*pipe).DrawToTarget(
				lastTexColor, gradingTarget, Util::AsReal(gameTexMain.RTV),
				Util::AsReal(gameTexMainAlt.texture), Util::AsReal(gameTexMainAlt.RTV));
			continue;
		}
		DrawFeature(*pipe, lastTexColor);
	}

	for (auto& pipe : pipeline) {
		if (canDraw(pipe) && pipe->DrawAfterColorGrading()) {
			DrawFeature(*pipe, lastTexColor);
		}
	}

	Texture2D* mainConvertTex = texCopyMain.get();
	Texture2D* mainCopyConvertTex = texCopyMainCopy ? texCopyMainCopy.get() : texCopyMain.get();

	const bool sceneOutput = globals::state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing;
	const auto& ll = globals::features::linearLighting;
	const bool linear = ll.IsLinearLightingActive();
	const auto sceneGamut = linear && ll.settings.enableACEScg ? Gamut::ACEScg : Gamut::Rec709;
	const auto* grading = GetPipelineFeature<ColorGrading>(FeaturePipelineIndex::ColorGrading);
	const auto displayGamut = grading ? grading->GetDisplayGamut() : Gamut::Rec709;
	const CopyCB conversion{
		.inputGamut = lastTexColor.gamut,
		.outputGamut = inMainLoadingMenu ? lastTexColor.gamut : (sceneOutput ? sceneGamut : displayGamut),
		.gamma = !inMainLoadingMenu && sceneOutput && !linear ? 1.0f / kLegacySceneGamma : 1.0f
	};
	if (hasInput) {
		if (conversion.gamma != 1.0f || conversion.inputGamut != conversion.outputGamut) {
			DrawCopy(*texOutput, lastTexColor.tex, lastTexColor.srv, conversion);
			postProcessingOutput = texOutput->srv.get();
		} else {
			postProcessingOutput = lastTexColor.srv;
		}
	} else {
		CopyToRenderTarget(gameTexMain, useMainCopy ? mainCopyConvertTex : mainConvertTex, lastTexColor.tex, lastTexColor.srv, conversion);
		if (!gradingWroteAlternate || conversion.gamma != 1.f || conversion.inputGamut != conversion.outputGamut)
			CopyToRenderTarget(gameTexMainAlt, useMainCopy ? mainConvertTex : mainCopyConvertTex,
				Util::AsReal(gameTexMain.texture), Util::AsReal(gameTexMain.SRV), CopyCB{});
	}

	isrefraction = false;

	globals::state->SetOutputRenderTarget(a_output);
}

void PostProcessing::ClearBorderMotionVectorsForFrameGen()
{
	// Effects11 skips the letterbox, so its motion vectors must remain live scene data.
	if (!IsPipelineRunning())
		return;

	auto borderIdx = static_cast<size_t>(FeaturePipelineIndex::Border);
	auto& pipe = pipeline[borderIdx];
	if (pipe && pipe->IsActive() && !(globals::state->IsMainOrLoadingMenuOpen() && pipe->DisableInMainLoadingMenu())) {
		auto* border = static_cast<Border*>(pipe.get());
		border->ClearMotionVectorsForFrameGen();
	}
}

bool PostProcessing::WantsTonemapOwnership() const
{
	if (bypass || globals::state->IsFlatWorldMapOpen() || !IsPipelineReady())
		return false;
	const auto* colorGrading = static_cast<const ColorGrading*>(pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get());
	return !colorGrading || !colorGrading->enabled || !colorGrading->settings.enableTonemap || colorGrading->IsReadyForTonemapping();
}

bool PostProcessing::IsPipelineRunning() const
{
	return !bypass && IsPipelineReady() && globals::state->GetTonemapOwner() == State::TonemapOwner::kPostProcessing;
}

PostProcessing::PerFrame PostProcessing::GetCommonBufferData() const
{
	// Only Post Processing may advertise its linear, already-tonemapped output to consumers.
	return { .DisableVanillaTonemapping = globals::state->GetTonemapOwner() == State::TonemapOwner::kPostProcessing };
}

void PostProcessing::UpdateVanillaEffects(bool active)
{
	const bool runnable = active && loaded && !bypass && IsPipelineReady() && globals::shaderCache->IsEnabled();
	const bool inMainLoadingMenu = globals::state->IsMainOrLoadingMenuOpen();
	const bool linear = globals::features::linearLighting.IsLinearLightingActive();
	const auto replaces = [&](FeaturePipelineIndex idx) {
		const auto& feature = pipeline[static_cast<size_t>(idx)];
		return runnable && feature && feature->IsActive() &&
		       (!inMainLoadingMenu || (!linear && !feature->DisableInMainLoadingMenu()));
	};

	skipVanillaDoF = replaces(FeaturePipelineIndex::DoF);
	skipVanillaBloom = globals::state->enablePShaders && globals::state->ShaderEnabled(RE::BSShader::Type::ImageSpace) &&
	                   (replaces(FeaturePipelineIndex::PhysicalGlare) || replaces(FeaturePipelineIndex::CODBloom));
	if (skipVanillaBloom) {
		using enum RE::ImageSpaceManager::ImageSpaceEffectEnum;
		static const std::array<uint32_t, 2> tonemapShaders{
			RE::ImageSpaceManager::GetCurrentIndex(ISHDRTonemapBlendCinematic),
			RE::ImageSpaceManager::GetCurrentIndex(ISHDRTonemapBlendCinematicFade)
		};
		skipVanillaBloom = globals::shaderCache->blockedKeyIndex == -1 &&
		                   globals::shaderCache->HasPixelShaders(RE::BSShader::Type::ImageSpace, tonemapShaders);
	}

	static auto* lensFlare = RE::GetINISetting("bLensFlare:ImageSpace");
	if (lensFlare) {
		if (replaces(FeaturePipelineIndex::LensFlare)) {
			if (!savedVanillaLensFlare) {
				savedVanillaLensFlare = lensFlare->data.b;
				lensFlare->data.b = false;
			}
		} else if (savedVanillaLensFlare) {
			lensFlare->data.b = *savedVanillaLensFlare;
			savedVanillaLensFlare.reset();
		}
	}
}

bool PostProcessing::GetSceneExposure(SceneExposure& a_out) const
{
	if (!loaded || !IsPipelineRunning())
		return false;

	const auto* exposure = GetPipelineFeature<HistogramAutoExposure>(FeaturePipelineIndex::AutoExposure);
	if (!exposure || !exposure->HasActiveAdaptation())
		return false;

	const auto parameters = exposure->GetExposureParameters();
	a_out.adaptedLuminance = exposure->GetAdaptationSRV();
	a_out.luminanceRange = parameters.LuminanceRange;
	a_out.compensationScale = parameters.CompensationScale();
	return true;
}

json PostProcessing::GetDiagnostics()
{
	SceneExposure exposure;
	if (!GetSceneExposure(exposure))
		return json{ { "sceneExposurePublished", false } };

	return json{
		{ "sceneExposurePublished", true },
		{ "sceneExposureLuminanceRange", json::array({ exposure.luminanceRange.x, exposure.luminanceRange.y }) },
		{ "sceneExposureCompensationScale", exposure.compensationScale },
	};
}

void PostProcessing::Prepass()
{
	if (auto* lut = GetPipelineFeature<::LUT>(FeaturePipelineIndex::LUT))
		lut->UpdateTexture();
	if (!pendingSettings.empty()) {
		ApplyPendingSettings();
		UpdateVanillaEffects(globals::state->GetTonemapOwner() == State::TonemapOwner::kPostProcessing);
	}
	if (auto* grading = GetPipelineFeature<ColorGrading>(FeaturePipelineIndex::ColorGrading); grading && grading->recompileFlag)
		grading->ReloadActiveShaders();

	// globals::game::imageSpaceManager isn't cached until OnDataLoaded(); skip
	// the update rather than crash if Prepass() runs before that.
	if (!globals::game::imageSpaceManager) {
		return;
	}

	const float aspect = Util::GetCameraAspectRatio();
	const bool runnable = IsPipelineRunning() && !globals::state->IsMainOrLoadingMenuOpen();
	cinematicCamera.Update(runnable, aspect);

	// Update gameISData. GetRuntimeData() and GetVRRuntimeData() return
	// differently-laid-out structs (VR_RUNTIME_DATA has two extra leading
	// fields), so calling the wrong one for the current runtime silently
	// misreads unrelated bytes as pointers.
	const auto updateGameISData = [this](const auto& iSRuntimeData) {
		imageSpaceManager->gameISData = iSRuntimeData.data;
		if (const auto& overrideBaseData = iSRuntimeData.overrideBaseData) {
			imageSpaceManager->gameISData.baseData = *overrideBaseData;
		} else if (const auto& currentBaseData = iSRuntimeData.currentBaseData) {
			imageSpaceManager->gameISData.baseData = *currentBaseData;
		}
	};
	if (globals::game::isVR) {
		// Guaranteed non-null: GetVRRuntimeData() only returns null when IsVR()
		// is false, which this branch already excludes.
		updateGameISData(*globals::game::imageSpaceManager->GetVRRuntimeData());
	} else {
		updateGameISData(globals::game::imageSpaceManager->GetRuntimeData());
	}
}

void PostProcessing::PostPostLoad()
{
	logger::info("Hooking preprocess passes");
	stl::write_vfunc<0x6, VanillaDoF_IsActive>(RE::VTABLE_ImageSpaceEffectDepthOfField[0]);
	stl::write_vfunc<0x1, VanillaBlur_Render>(RE::VTABLE_ImageSpaceEffectBlur[0]);
	stl::write_vfunc<0x2, BSImagespaceShaderRefraction_SetupTechnique>(RE::VTABLE_BSImagespaceShaderRefraction[0]);
}

void PostProcessing::BuildPipeline()
{
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LocalExposure)] = std::make_shared<LocalExposure>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)] = std::make_shared<HistogramAutoExposure>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)] = std::make_shared<ColorGrading>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LUT)] = std::make_shared<LUT>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::MotionBlur)] = std::make_shared<MotionBlur>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::DoF)] = std::make_shared<DoF>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::PhysicalGlare)] = std::make_shared<PhysicalGlare>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::CODBloom)] = std::make_shared<CODBloom>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LensFlare)] = std::make_shared<LensFlare>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::PhysicalLens)] = std::make_shared<PhysicalLens>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Composite)] = std::make_shared<Composite>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Vignette)] = std::make_shared<Vignette>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Camera)] = std::make_shared<Camera>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Border)] = std::make_shared<Border>();
	ApplyDefaultEnabledStates();
}

void PostProcessing::ApplyDefaultEnabledStates()
{
	RestorePipelineDefaultEnablement();
}

bool PostProcessing::WantsAutoHDR() const
{
	if (globals::state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing)
		return false;

	auto* colorGrading = static_cast<const ColorGrading*>(pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get());
	return colorGrading && colorGrading->IsActive() && colorGrading->WantsAutoHDR();
}

namespace
{
	class ProcessingPresetProvider final : public PresetProvider
	{
	public:
		void PrepareDiscovery() override { PostProcessingPresets::MigrateLegacyPresets(); }
		bool IsDefaultPipelineProvider() const override { return true; }
		bool SetPipelineActive(bool active) override
		{
			return PostProcessingMode::Set(active ? PostProcessingMode::Mode::PostProcessing : PostProcessingMode::Mode::Vanilla, false);
		}
		std::string_view GetDedicatedBaselineFeature() const override { return PostProcessingPresets::kFeatureShortName; }
		bool CanExportBaseline(std::string_view type) const override
		{
			return globals::features::postProcessing.loaded && type != "E11";
		}
		bool ReplacesBaselineTree() const override { return true; }
	};
}

PresetProvider* PostProcessing::GetPresetProvider()
{
	static ProcessingPresetProvider provider;
	return &provider;
}
