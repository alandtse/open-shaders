#include "PostProcessing.h"

#include "GpuPass.h"
#include "LinearLighting.h"
#include "PostProcessing/RasterPass.h"

#include "CSEditor/EditorWindow.h"
#include "IconsTabler.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/PresetsPageRenderer.h"
#include "PostProcessingMode.h"
#include "Presets/PostProcessingPresets.h"
#include "Presets/PresetProvider.h"
#include "Utils/SettingsControl.h"
#include "imgui_stdlib.h"

#include "Globals.h"
#include "Menu.h"
#include "Profiler.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/SettingsPatch.h"

#include "Features/PostProcessing/PostProcessingUI.h"
#include "Features/Upscaling.h"

#include <format>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PostProcessing::Settings,
	DisableVanillaTonemapping)

namespace
{
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
		case Index::LUT:
		case Index::Camera:
		case Index::Border:
		case Index::COUNT:
			return false;
		}
		return false;
	}
}

void PostProcessing::DrawSettings()
{
	// Presets live on the Presets page like every other pack; a scene replica of this page edits a
	// scene layer rather than the base settings a preset sets, so it leaves the row out.
	if (!SettingsControl::IsReplicating()) {
		PresetsPageRenderer::DrawOpenButton("##PostProcessingOpenPresets",
			T("feature.post_processing.open_presets_tooltip",
				"Apply Post Processing presets on the Presets page. To save your current settings as one, use "
				"Export Preset in the OS Editor with Post Processing ticked."));

		// Inside the CS Editor (its Base Settings window draws this page too) the editor is already open.
		if (const auto* editor = EditorWindow::GetSingleton(); !editor || !editor->open)
			PresetsPageRenderer::DrawOpenCSEditorButton("##PostProcessingOpenCSEditor");

		ImGui::Separator();
	}

	PostProcessingMode::DrawSelector();
	ImGui::BeginDisabled(PostProcessingMode::Get() == PostProcessingMode::Mode::Effects11);
	Util::CheckboxFlag(T("feature.post_processing.disable_vanilla_tonemapping", "Disable Vanilla Tonemapping"), settings.DisableVanillaTonemapping);
	ImGui::EndDisabled();

	ImGui::Separator();

	// Effects 11 discards the pipeline output, so its controls would do nothing.
	const Util::LockedSection effects11Lock(PostProcessingMode::Get() == PostProcessingMode::Mode::Effects11,
		T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));

	{
		auto& cam = cinematicCamera;
		ImGui::Checkbox(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"), &cinematicCamera.settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T("feature.post_processing.cinematic_camera.description",
								  "Controls lens, focus, exposure and FOV. Exposure processing runs automatically while the camera is active; other effects must be enabled separately."));
		ImGui::SameLine();
		if (Icons::LabeledButton("##CinematicCameraSettings", Icons::TI(ICON_TI_MENU_2),
				T("feature.post_processing.cinematic_camera.settings", "Settings")))
			activeSettingsPage = SettingsPage::CinematicCamera;

		if (const auto* state = cam.GetState()) {
			const auto isoText = state->Exposure == CinematicCamera::ExposureMode::AutoISO ?
			                         std::format("{} {:.0f}-{:.0f}", T("feature.post_processing.cinematic_camera.exposure_auto_iso", "Auto ISO"), state->MinISO, state->MaxISO) :
			                         std::format("ISO {:.0f}", state->ISO);
			ImGui::TextDisabled(T("feature.post_processing.cinematic_camera.summary_exposure",
									"%s - %.1f mm - f/%.1f - %.0f deg - %s - FOV %.1f deg - %s"),
				cam.GetFilmbackPresetName(),
				state->FocalLengthMM,
				state->FNumber,
				cam.settings.Exposure.ShutterAngleDeg,
				isoText.c_str(),
				state->HorizontalFOVDeg,
				cam.GetFovStateText());
		} else if (cam.settings.Enabled) {
			ImGui::TextDisabled("%s - %s", cam.GetFilmbackPresetName(), cam.GetFovStateText());
		}
	}

	ImGui::Separator();

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

	if (activeSettingsPage == SettingsPage::Pipeline) {
		for (int i = 0; i < pipeline.size(); ++i) {
			auto& feat = pipeline[i];
			if (feat && feat->IsVisible()) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());
				if (!drawForcedEnabled("##Enabled", *feat))
					ImGui::Checkbox("##Enabled", &feat->enabled);
				ImGui::SameLine();
				if (Icons::Button("##Bars", Icons::TI(ICON_TI_MENU_2))) {
					activePipelineFeature = i;
					activeSettingsPage = SettingsPage::SubFeature;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T("feature.post_processing.edit_settings_for_this_feature", "Edit settings for this feature."));
				ImGui::SameLine();
				ImGui::Text("%s", displayName.c_str());
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", description.c_str());
				ImGui::PopID();
			}
		}
	} else if (activeSettingsPage == SettingsPage::SubFeature) {
		if (Icons::LabeledButton("##BackToPipeline", Icons::TI(ICON_TI_ARROW_LEFT),
				T("feature.post_processing.back_to_pipeline", "Back to Pipeline"))) {
			activeSettingsPage = SettingsPage::Pipeline;
		}
		ImGui::Separator();
		if (activePipelineFeature >= 0 && activePipelineFeature < pipeline.size()) {
			auto& feat = pipeline[activePipelineFeature];
			if (feat) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());

				ImGui::SeparatorText(displayName.c_str());
				ImGui::TextWrapped("%s", description.c_str());

				ImGui::Spacing();
				if (Icons::LabeledButton("##RecompileShaders", Icons::TI(ICON_TI_REFRESH),
						T("feature.post_processing.recompile_shaders", "Recompile Shaders"))) {
					feat->ClearShaderCache();
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T("feature.post_processing.recompile_shaders_for_this_sub_feature_only", "Recompile shaders for this sub-feature only."));
				ImGui::Separator();
				ImGui::Spacing();
				if (!drawForcedEnabled(T("feature.post_processing.enabled", "Enabled"), *feat))
					ImGui::Checkbox(T("feature.post_processing.enabled", "Enabled"), &feat->enabled);
				if (feat->IsActive()) {
					ImGui::Indent();
					feat->DrawSettings();
					ImGui::Unindent();
				} else {
					ImGui::TextDisabled("%s", T("feature.post_processing.enable_the_feature_to_see_its_settings", "Enable the feature to see its settings."));
				}

				ImGui::PopID();
			} else {
				ImGui::TextDisabled("%s", T("feature.post_processing.selected_feature_is_not_valid", "Selected feature is not valid."));
				activeSettingsPage = SettingsPage::Pipeline;
			}
		} else {
			ImGui::TextDisabled("%s", T("feature.post_processing.invalid_feature_selected_returning_to_list", "Invalid feature selected. Returning to list."));
			activeSettingsPage = SettingsPage::Pipeline;
		}
	} else if (activeSettingsPage == SettingsPage::CinematicCamera) {
		if (Icons::LabeledButton("##BackToPipeline", Icons::TI(ICON_TI_ARROW_LEFT),
				T("feature.post_processing.back_to_pipeline", "Back to Pipeline"))) {
			activeSettingsPage = SettingsPage::Pipeline;
		}
		ImGui::Separator();
		ImGui::SeparatorText(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"));

		ImGui::TextWrapped("%s", T("feature.post_processing.cinematic_camera.description",
									 "Controls lens, focus, exposure and FOV. Exposure processing runs automatically while the camera is active; other effects must be enabled separately."));
		ImGui::Spacing();

		cinematicCamera.DrawSettings();
		if (cinematicCamera.settings.Enabled) {
			ImGui::Indent();
			if (auto* exposure = GetPipelineFeature<HistogramAutoExposure>(FeaturePipelineIndex::AutoExposure)) {
				ImGui::SeparatorText(T("feature.post_processing.cinematic_camera.exposure_meter", "Exposure Meter"));
				exposure->DrawCameraExposureReadout();
			}

			// Linked-effect status: which adapters are currently consuming overrides.
			ImGui::Separator();
			ImGui::Text("%s", T("feature.post_processing.cinematic_camera.linked_effects", "Linked Effects"));
			auto drawLinked = [&](FeaturePipelineIndex idx, const char* fallbackName) {
				auto& pipe = pipeline[static_cast<size_t>(idx)];
				if (!pipe)
					return;
				ImGui::Bullet();
				ImGui::Text("%s: ", pipe->GetDisplayName().empty() ? fallbackName : pipe->GetDisplayName().c_str());
				ImGui::SameLine();
				if (pipe->IsActive())
					ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", T("feature.post_processing.cinematic_camera.linked_active", "Active"));
				else
					ImGui::TextDisabled("%s", T("feature.post_processing.cinematic_camera.linked_disabled", "Disabled - no override"));
			};
			drawLinked(FeaturePipelineIndex::DoF, "Depth of Field");
			drawLinked(FeaturePipelineIndex::LensFlare, "Lens Flare");
			drawLinked(FeaturePipelineIndex::PhysicalGlare, "Physical Glare");
			drawLinked(FeaturePipelineIndex::Vignette, "Vignette");
			drawLinked(FeaturePipelineIndex::MotionBlur, "Motion Blur");
			drawLinked(FeaturePipelineIndex::AutoExposure, "Histogram Auto Exposure");
			drawLinked(FeaturePipelineIndex::Camera, "Camera");
			ImGui::Unindent();
		}
	}

	ImGui::Separator();

	if (Util::ShowDebugSections() && ImGui::TreeNode(T("feature.post_processing.debug", "Debug"))) {
		if (ImGui::TreeNode(T("feature.post_processing.game_imagespace_values", "Game ImageSpace Values"))) {
			ImGui::Text(T("feature.post_processing.base_amount", "Base Amount: %.3f"), imageSpaceManager->gameISData.baseAmount);
			ImGui::Text("%s", T("feature.post_processing.base_data", "Base Data:"));
			ImGui::Text("%s", T("feature.post_processing.cinematic_values", "Cinematic Values:"));
			ImGui::Text(T("feature.post_processing.saturation_brightness_contrast_values", "Saturation: %.3f\nBrightness: %.3f\nContrast: %.3f"),
				imageSpaceManager->gameISData.baseData.cinematic.saturation,
				imageSpaceManager->gameISData.baseData.cinematic.brightness,
				imageSpaceManager->gameISData.baseData.cinematic.contrast);

			ImGui::Text("%s", T("feature.post_processing.hdr_values", "HDR Values:"));
			ImGui::Text(T("feature.post_processing.hdr_values_detail", "Eye Adapt Speed: %.3f\nBloom Blur Radius: %.3f\nBloom Threshold: %.3f\nBloom Scale: %.3f\nReceive Bloom Threshold: %.3f\nWhite: %.3f\nSunlight Scale: %.3f\nSky Scale: %.3f\nEye Adapt Strength: %.3f"),
				imageSpaceManager->gameISData.baseData.hdr.eyeAdaptSpeed,
				imageSpaceManager->gameISData.baseData.hdr.bloomBlurRadius,
				imageSpaceManager->gameISData.baseData.hdr.bloomThreshold,
				imageSpaceManager->gameISData.baseData.hdr.bloomScale,
				imageSpaceManager->gameISData.baseData.hdr.receiveBloomThreshold,
				imageSpaceManager->gameISData.baseData.hdr.white,
				imageSpaceManager->gameISData.baseData.hdr.sunlightScale,
				imageSpaceManager->gameISData.baseData.hdr.skyScale,
				imageSpaceManager->gameISData.baseData.hdr.eyeAdaptStrength);

			ImGui::Text("%s", T("feature.post_processing.tint_values", "Tint Values:"));
			ImGui::Text(T("feature.post_processing.tint_values_detail", "Tint Amount: %.3f\nTint Color: (%.3f, %.3f, %.3f)"),
				imageSpaceManager->gameISData.baseData.tint.amount,
				imageSpaceManager->gameISData.baseData.tint.color.red,
				imageSpaceManager->gameISData.baseData.tint.color.green,
				imageSpaceManager->gameISData.baseData.tint.color.blue);

			ImGui::Text("%s", T("feature.post_processing.depth_of_field_values", "Depth of Field Values:"));
			ImGui::Text(T("feature.post_processing.depth_of_field_values_detail", "DOF Strength: %.3f\nDOF Distance: %.3f\nDOF Range: %.3f\nDOF Flags: %d\nDOF Sky Blur Radius: %d"),
				imageSpaceManager->gameISData.baseData.depthOfField.strength,
				imageSpaceManager->gameISData.baseData.depthOfField.distance,
				imageSpaceManager->gameISData.baseData.depthOfField.range,
				imageSpaceManager->gameISData.baseData.depthOfField.flags,
				static_cast<int>(imageSpaceManager->gameISData.baseData.depthOfField.skyBlurRadius.get()));

			ImGui::Text(T("feature.post_processing.mod_amount", "Mod Amount: %.3f"), imageSpaceManager->gameISData.modAmount);
			ImGui::Text("%s", T("feature.post_processing.mod_data", "Mod Data:"));
			ImGui::Text(T("feature.post_processing.mod_fade_values_detail", "Fade Amount: %.3f\nFade Color: (%.3f, %.3f, %.3f)\nBlur Radius: %.3f\nDouble Vision Strength: %.3f\n"),
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeAmount],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeR],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeG],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeB],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kBlurRadius],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDoubleVisionStrength]);
			ImGui::Text(T("feature.post_processing.radial_blur_values_detail", "Radial Blur Strength: %.3f\nRadial Blur Rampup: %.3f\nRadial Blur Start: %.3f\nRadial Blur Rampdown: %.3f\nRadial Blur Down Start: %.3f\nRadial Blur Center: (%.3f, %.3f)"),
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurStrength],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurRampup],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurStart],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurRampdown],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurDownStart],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurCenterX],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurCenterY]);
			ImGui::Text(T("feature.post_processing.mod_dof_values_detail", "DOF Strength: %.3f\nDOF Distance: %.3f\nDOF Range: %.3f\nDOF Mode: %d"),
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFStrength],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFDistance],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFRange],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFMode]);
			ImGui::Text(T("feature.post_processing.motion_blur_strength", "Motion Blur Strength: %.3f"), imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kMotionBlurStrength]);
			ImGui::TreePop();
		}
		ImGui::TreePop();
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

	json merged{ { "ppsettings", settings } };
	Util::Settings::OverlayRecognizedRootSettings(merged, o_json);
	settings = merged["ppsettings"];
}

void PostProcessing::SaveSettings(json& o_json)
{
	if (!pendingSettings.empty()) {
		o_json = pendingSettings;
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

	o_json["ppsettings"] = settings;
	json cameraSettings;
	cinematicCamera.SaveSettings(cameraSettings);
	o_json["cinematic_camera"] = cameraSettings;
}

void PostProcessing::RestoreDefaultSettings()
{
	pendingSettings = {};
	settings = {};
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
	auto* overrideManager = SettingsOverrideManager::GetSingleton();
	const std::string featureName = GetShortName();
	if (!overrideManager || !overrideManager->HasFeatureOverrides(featureName))
		return false;

	if (!ApplyPendingSettings())
		return false;

	json currentSettings;
	SaveSettings(currentSettings);
	json mergedSettings = currentSettings;
	if (overrideManager->ReapplyFeatureOverrides(featureName, mergedSettings) == 0)
		return false;
	const json overrideSettings = overrideManager->GetMergedOverrideSettings(featureName, json::object());

	const auto rollback = [&]() {
		try {
			ProcessSettings(currentSettings);
		} catch (const std::exception& e) {
			logger::error("Failed to roll back override settings for {}. Error: {}", featureName, e.what());
		}
	};
	const auto rollbackPersistence = [&]() {
		try {
			if (!overrideManager->PersistUserOverride(featureName, currentSettings, overrideSettings))
				logger::error("Failed to roll back persisted override settings for {}", featureName);
		} catch (const std::exception& e) {
			logger::error("Failed to roll back persisted override settings for {}. Error: {}", featureName, e.what());
		}
	};

	if (!HasActivePipelineFeature()) {
		try {
			ProcessSettings(mergedSettings);
		} catch (const std::exception& e) {
			logger::warn("Failed to apply override settings for {}. Error: {}", featureName, e.what());
			rollback();
			return false;
		}
		if (overrideManager->DeleteUserOverride(featureName))
			return true;
		logger::warn("Failed to delete user override settings for {}", featureName);
		rollback();
		rollbackPersistence();
		return false;
	}

	const auto featureType = pipeline[activePipelineFeature]->GetType();
	bool hasScopedOverride = false;
	for (const auto* featureOverride : overrideManager->GetFeatureOverrides(featureName)) {
		if (featureOverride->enabled && featureOverride->overrideData.contains(featureType)) {
			hasScopedOverride = true;
			break;
		}
	}
	if (!hasScopedOverride || !mergedSettings.contains(featureType))
		return false;

	json scopedSettings = json::object();
	scopedSettings[featureType] = mergedSettings[featureType];
	bool persistenceAttempted = false;
	try {
		ProcessSettings(scopedSettings);
		json appliedSettings;
		SaveSettings(appliedSettings);
		persistenceAttempted = true;
		if (overrideManager->PersistUserOverride(featureName, appliedSettings, overrideSettings))
			return true;
		logger::warn("Failed to persist scoped override settings for {}", featureName);
	} catch (const std::exception& e) {
		logger::warn("Failed to apply scoped override settings for {}. Error: {}", featureName, e.what());
	}
	rollback();
	if (persistenceAttempted)
		rollbackPersistence();
	return false;
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
			next->Reset();
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

	for (auto& pipe : pipeline) {
		if (pipe)
			pipe->Reset();
	}
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
		context->CopySubresourceRegion(Util::AsReal(targetRT.texture), 0, 0, 0, 0, srcTex, 0, nullptr);
		return;
	}

	if (!copyPS || !fullscreenVS || !convertTex || !convertTex->rtv || !convertTex->resource)
		return;

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
	if (bypass || IsTonemapOwnedByEffects11() || !IsPipelineReady())
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
	if (bypass || IsTonemapOwnedByEffects11() || !IsPipelineReady())
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

	// go through each fx
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsActive() && !pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && (!pipe->DrawBeforeUpscaling() || !upscaling.loaded)) {
			DrawFeature(*pipe, lastTexColor);
		}
	}

	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsActive() && pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && (!pipe->DrawBeforeUpscaling() || !upscaling.loaded)) {
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
		CopyToRenderTarget(gameTexMainAlt, useMainCopy ? mainConvertTex : mainCopyConvertTex,
			Util::AsReal(gameTexMain.texture), Util::AsReal(gameTexMain.SRV), CopyCB{});
	}

	isrefraction = false;

	globals::state->SetOutputRenderTarget(a_output);
}

void PostProcessing::ClearBorderMotionVectorsForFrameGen()
{
	// Effects11 skips the letterbox, so its motion vectors must remain live scene data.
	if (bypass || IsTonemapOwnedByEffects11() || !IsPipelineReady())
		return;

	auto borderIdx = static_cast<size_t>(FeaturePipelineIndex::Border);
	auto& pipe = pipeline[borderIdx];
	if (pipe && pipe->enabled) {
		auto* border = static_cast<Border*>(pipe.get());
		border->ClearMotionVectorsForFrameGen();
	}
}

bool PostProcessing::WantsTonemapOwnership() const
{
	if (bypass || !IsPipelineReady() || settings.DisableVanillaTonemapping == 0)
		return false;
	const auto* colorGrading = static_cast<const ColorGrading*>(pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get());
	return !colorGrading || !colorGrading->enabled || !colorGrading->settings.enableTonemap || colorGrading->IsReadyForTonemapping();
}

bool PostProcessing::IsTonemapOwnedByEffects11() const
{
	return globals::state->GetTonemapOwner() == State::TonemapOwner::kEffects11;
}

PostProcessing::Settings PostProcessing::GetCommonBufferData() const
{
	Settings data = settings;

	// Only Post Processing may advertise its linear, already-tonemapped output to consumers.
	if (globals::state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing)
		data.DisableVanillaTonemapping = 0;

	return data;
}

void PostProcessing::UpdateVanillaEffects(bool active)
{
	const bool runnable = active && loaded && !bypass && settings.DisableVanillaTonemapping != 0 && IsPipelineReady() && globals::shaderCache->IsEnabled();
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
	if (!loaded || bypass || IsTonemapOwnedByEffects11() || !IsPipelineReady())
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

	// globals::game::imageSpaceManager isn't cached until OnDataLoaded(); skip
	// the update rather than crash if Prepass() runs before that.
	if (!globals::game::imageSpaceManager) {
		return;
	}

	const float aspect = Util::GetCameraAspectRatio();
	const bool runnable = !bypass && !IsTonemapOwnedByEffects11() && IsPipelineReady() && !globals::state->IsMainOrLoadingMenuOpen();
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
			return PostProcessingMode::Set(active ? PostProcessingMode::Mode::PostProcessing : PostProcessingMode::Mode::Vanilla);
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
