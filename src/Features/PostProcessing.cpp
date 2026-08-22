#include "PostProcessing.h"

#include "IconsFontAwesome5.h"
#include "imgui_stdlib.h"

#include "Globals.h"
#include "Menu.h"
#include "Profiler.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"

#include "Features/PostProcessing/PostProcessingUI.h"
#include "Features/Upscaling.h"

#include <format>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PostProcessing::Settings,
	DisableVanillaTonemapping)

namespace
{
	constexpr bool IsPipelineFeatureEnabledByDefault(PostProcessing::FeaturePipelineIndex a_index)
	{
		using Index = PostProcessing::FeaturePipelineIndex;
		switch (a_index) {
		case Index::Vignette:
		case Index::AutoExposure:
		case Index::CODBloom:
		case Index::Composite:
		case Index::ColorGrading:
			return true;
		case Index::DoF:
		case Index::LocalExposure:
		case Index::MotionBlur:
		case Index::PhysicalGlare:
		case Index::LensFlare:
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
	static int presetIdx = -1;

	ImGui::BeginGroup();
	std::string currentPreset = (presetIdx >= 0 && presetIdx < presets.size()) ? presets[presetIdx] : T("feature.post_processing.select_a_preset", "Select a preset");

	if (ImGui::BeginCombo("##PresetCombo", currentPreset.c_str())) {
		presets = LoadPresets();

		for (int i = 0; i < presets.size(); ++i) {
			bool isSelected = presetIdx == i;
			if (ImGui::Selectable(presets[i].c_str(), isSelected))
				presetIdx = i;
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine();
	if (PostProcessingUI::ActionButton(T("feature.post_processing.load", "Load"))) {
		if (presetIdx >= 0 && presetIdx < presets.size()) {
			LoadPresetFrom(presets[presetIdx]);
		}
	}

	ImGui::EndGroup();
	ImGui::BeginGroup();
	static std::string newPresetName = "";
	ImGui::InputText("##NewPresetName", &newPresetName);

	ImGui::SameLine();
	if (PostProcessingUI::ActionButton(T("feature.post_processing.save", "Save"))) {
		if (!newPresetName.empty())
			SavePresetTo(newPresetName);
	}

	ImGui::EndGroup();

	ImGui::Separator();

	// Effects11 replaces the whole tonemap pass, so these toggles would have no effect while
	// it owns the frame. Disable them rather than let them silently do nothing.
	const bool tonemapTakenByEffects11 = IsTonemapOwnedByEffects11();

	ImGui::BeginDisabled(tonemapTakenByEffects11);

	// A disabled checkbox never reports a click, so bypass keeps its stored value while forced on.
	bool bypassDisplay = bypass || tonemapTakenByEffects11;
	if (ImGui::Checkbox(T("feature.post_processing.bypass", "Bypass"), &bypassDisplay))
		bypass = bypassDisplay;

	ImGui::SameLine();
	ImGui::Checkbox(T("feature.post_processing.disable_vanilla_tonemapping", "Disable Vanilla Tonemapping"), (bool*)&settings.DisableVanillaTonemapping);
	ImGui::EndDisabled();

	if (tonemapTakenByEffects11) {
		ImGui::PushStyleColor(ImGuiCol_Text, Menu::GetSingleton()->GetTheme().StatusPalette.Warning);
		const SKSE::stl::scope_exit restoreTextColor([]() noexcept { ImGui::PopStyleColor(); });
		ImGui::TextWrapped("%s", T("feature.post_processing.tonemap_owned_by_effects11",
									 "Tonemapping is currently handled by Effects 11, so the Post Processing pipeline is "
									 "paused. To use Post Processing instead, disable Effects 11 or enable its "
									 "\"UseOriginalPostProcessing\" setting."));
	}

	ImGui::Separator();

	if (activeSettingsPage == SettingsPage::Pipeline) {
		for (size_t i = 0; i < pipeline.size(); ++i) {
			auto& feat = pipeline[i];
			if (feat && feat->IsVisible()) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());
				ImGui::Checkbox("##Enabled", &feat->enabled);
				ImGui::SameLine();
				if (PostProcessingUI::IconButton(ICON_FA_BARS)) {
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
		auto backLabel = std::format("{} {}", ICON_FA_ARROW_LEFT, T("feature.post_processing.back_to_pipeline", "Back to Pipeline"));
		if (PostProcessingUI::ActionButton(backLabel.c_str())) {
			activeSettingsPage = SettingsPage::Pipeline;
		}
		ImGui::Separator();
		if (activePipelineFeature < pipeline.size()) {
			auto& feat = pipeline[activePipelineFeature];
			if (feat) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());

				ImGui::SeparatorText(displayName.c_str());
				ImGui::TextWrapped("%s", description.c_str());

				ImGui::Spacing();
				auto recompileLabel = std::format("{} {}", ICON_FA_SYNC, T("feature.post_processing.recompile_shaders", "Recompile Shaders"));
				if (PostProcessingUI::ActionButton(recompileLabel.c_str())) {
					feat->ClearShaderCache();
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T("feature.post_processing.recompile_shaders_for_this_sub_feature_only", "Recompile shaders for this sub-feature only."));
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::Checkbox(T("feature.post_processing.enabled", "Enabled"), &feat->enabled);
				if (feat->enabled) {
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
	}

	ImGui::Separator();

	if (ImGui::TreeNode(T("feature.post_processing.debug", "Debug"))) {
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
	pendingSettings = o_json;
}

void PostProcessing::ProcessSettings(json& o_json)
{
	logger::debug("Loading post processing settings...");

	for (auto& feat : pipeline) {
		if (feat && o_json.contains(feat->GetType())) {
			if (!feat->IsAutoEnabled())
				feat->enabled = o_json.value(feat->GetType(), json::object()).value("enabled", true);
			json featSettings = o_json.value(feat->GetType(), json::object()).value("settings", json::object());
			feat->LoadSettings(featSettings);
		}
	}

	if (o_json.contains("ppsettings")) {
		settings = o_json["ppsettings"];
	}
}

void PostProcessing::SaveSettings(json& o_json)
{
	if (!pendingSettings.empty()) {
		o_json = pendingSettings;
		return;
	}

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
}

std::vector<std::string> PostProcessing::LoadPresets()
{
	std::vector<std::string> o_presets = {};

	try {
		std::filesystem::create_directories(ppPresetPath);
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error creating preset directory during Load ({}) : {}\n", ppPresetPath, e.what());
		return o_presets;
	}

	for (const auto& entry : std::filesystem::directory_iterator(ppPresetPath)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json") {
			o_presets.push_back(entry.path().stem().string());
		}
	}

	return o_presets;
}

bool PostProcessing::LoadPresetFrom(std::string a_name)
{
	json a_presets = {};

	// if the name has .json, remove it
	if (a_name.ends_with(".json"))
		a_name = a_name.substr(0, a_name.size() - 5);

	try {
		logger::info("Loading preset: {}", a_name);
		std::ifstream i{ std::format("{}\\{}.json", ppPresetPath, a_name) };
		i >> a_presets;
	} catch (const std::exception& e) {
		logger::warn("Failed to load preset: {}. Error: {}", a_name, e.what());
		return false;
	}

	pendingSettings = {};
	json previousSettings;
	SaveSettings(previousSettings);
	try {
		ProcessSettings(a_presets);
		return true;
	} catch (const std::exception& e) {
		logger::warn("Failed to apply preset: {}. Error: {}", a_name, e.what());
		try {
			ProcessSettings(previousSettings);
		} catch (const std::exception& rollbackError) {
			logger::error("Failed to roll back preset: {}. Error: {}", a_name, rollbackError.what());
		}
		return false;
	}
}

void PostProcessing::SavePresetTo(std::string a_name)
{
	// Check if the name is valid
	if (a_name.empty()) {
		logger::warn("Invalid preset name.");
		return;
	}

	json a_presets = {};
	SaveSettings(a_presets);
	a_presets["preset_name"] = a_name;

	try {
		std::filesystem::create_directories(ppPresetPath);
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error creating preset directory during Save ({}) : {}\n", ppPresetPath, e.what());
		return;
	}

	std::string presetPath = std::format("{}\\{}.json", ppPresetPath, a_name);
	std::ofstream o{ presetPath };
	if (!o.is_open() || !o.good()) {
		logger::warn("Failed to open preset file for writing: {}", presetPath);
		return;
	}

	try {
		o << std::setw(4) << a_presets;
		logger::info("Saving preset to {}", presetPath);
	} catch (const std::exception& e) {
		logger::warn("Failed to write preset to file: {}. Error: {}", presetPath, e.what());
	}
}

void PostProcessing::RestoreDefaultSettings()
{
	bypass = false;

	// Defer the default preset until SetupResources creates the pipeline.
	bool pipelineReady = pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)] != nullptr;
	if (!pipelineReady) {
		try {
			std::ifstream i{ std::format("{}\\{}.json", ppPresetPath, "default") };
			json defaultPreset;
			i >> defaultPreset;
			pendingSettings = defaultPreset;
			logger::info("Pipeline not ready, loaded default preset into pending settings");
		} catch (const std::exception& e) {
			logger::info("No default preset available during early load, C++ defaults will be used. Error: {}", e.what());
			pendingSettings = {};
		}
		return;
	}

	pendingSettings = {};
	if (LoadPresetFrom("default"))
		return;

	logger::warn("Falling back to built-in Post Processing defaults");
	settings = {};
	RestorePipelineDefaultEnablement();

	for (auto& pipe : pipeline) {
		if (pipe) {
			pipe->RestoreDefaultSettings();
		}
	}
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
	const auto restoreBuiltInDefaults = [&]() {
		if (!feature->IsAutoEnabled())
			feature->enabled = IsPipelineFeatureEnabledByDefault(static_cast<FeaturePipelineIndex>(activePipelineFeature));
		feature->RestoreDefaultSettings();
	};

	try {
		json defaultPreset;
		std::ifstream input{ std::format("{}\\{}.json", ppPresetPath, "default") };
		input >> defaultPreset;

		const auto featureType = feature->GetType();
		const auto defaults = defaultPreset.find(featureType);
		if (defaults == defaultPreset.end()) {
			logger::warn("Default preset has no settings for Post Processing subfeature {}", featureType);
			restoreBuiltInDefaults();
			return;
		}

		if (!feature->IsAutoEnabled())
			feature->enabled = defaults->value("enabled", IsPipelineFeatureEnabledByDefault(static_cast<FeaturePipelineIndex>(activePipelineFeature)));
		json featureSettings = defaults->value("settings", json::object());
		feature->LoadSettings(featureSettings);
	} catch (const std::exception& e) {
		logger::warn("Failed to apply scoped Post Processing defaults. Error: {}", e.what());
		restoreBuiltInDefaults();
	}
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

void PostProcessing::ClearShaderCache()
{
	for (auto& pipe : pipeline) {
		if (pipe)
			pipe->ClearShaderCache();
	}
}

void PostProcessing::SetupResources()
{
	{
		auto renderer = globals::game::renderer;
		auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		D3D11_TEXTURE2D_DESC texMainDesc;
		D3D11_TEXTURE2D_DESC texMainCopyDesc;
		gameTexMain.texture->GetDesc(&texMainDesc);
		gameTexMainCopy.texture->GetDesc(&texMainCopyDesc);
		texDesc = texMainDesc;

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

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texCopyMain = eastl::make_unique<Texture2D>(texDesc, "Post Processing Main Copy");
		texCopyMain->CreateUAV(uavDesc);

		if (texMainCopyDesc.Format != texMainDesc.Format) {
			texDesc = texMainCopyDesc;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;
			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			texDesc.MiscFlags = 0;

			texCopyMainCopy = eastl::make_unique<Texture2D>(texDesc, "Post Processing Main Copy Conversion");
			texCopyMainCopy->CreateUAV(uavDesc);
		} else {
			texCopyMainCopy = nullptr;
		}

		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		texAfterTAA = eastl::make_unique<Texture2D>(texDesc, "Post Processing After TAA");
		texAfterTAA->CreateSRV(srvDesc);
		texAfterTAA->CreateUAV(uavDesc);
	}

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\copy.cs.hlsl", {}, "cs_5_0")))
		copyCS.attach(rawPtr);

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

	RestorePipelineDefaultEnablement();

	for (auto& pipe : pipeline) {
		if (pipe) {
			pipe->owner = this;
			pipe->SetupResources();
		}
	}

	bokehResources.Setup();

	ApplyPendingSettings();
}

void PostProcessing::Reset()
{
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
	ID3D11ShaderResourceView* srcSRV)
{
	// D3D11 rejects a copy whose source and destination are the same resource, which happens
	// whenever the pipeline left the image in the buffer we are writing back to.
	if (targetRT.texture == srcTex)
		return;

	auto context = globals::d3d::context;

	D3D11_TEXTURE2D_DESC srcDesc;
	srcTex->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC targetDesc;
	targetRT.texture->GetDesc(&targetDesc);

	if (srcDesc.Format == targetDesc.Format) {
		context->CopySubresourceRegion(targetRT.texture, 0, 0, 0, 0, srcTex, 0, nullptr);
		return;
	}

	if (!copyCS || !convertTex || !convertTex->uav || !convertTex->resource)
		return;

	ID3D11ShaderResourceView* srv = srcSRV;
	ID3D11UnorderedAccessView* uav = convertTex->uav.get();

	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetShader(copyCS.get(), nullptr, 0);
	context->Dispatch((convertTex->desc.Width + 7) >> 3, (convertTex->desc.Height + 7) >> 3, 1);

	srv = nullptr;
	uav = nullptr;

	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetShader(nullptr, nullptr, 0);

	context->CopySubresourceRegion(targetRT.texture, 0, 0, 0, 0, convertTex->resource.get(), 0, nullptr);
}

void PostProcessing::DrawFeature(PostProcessFeature& feature, PostProcessFeature::TextureInfo& lastTexColor)
{
	if (feature.WritesToMainTexture()) {
		feature.Draw(lastTexColor);
	} else {
		PostProcessFeature::TextureInfo inTex = lastTexColor;
		feature.Draw(inTex);
	}
}

void PostProcessing::DrawBeforeUpscaling()
{
	if (bypass || IsTonemapOwnedByEffects11())
		return;

	auto& upscaling = globals::features::upscaling;
	if (!upscaling.loaded)
		return;

	auto renderer = globals::game::renderer;
	auto state = globals::state;

	bool inMainLoadingMenu = state->IsMainOrLoadingMenuOpen();
	auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };

	state->BeginPerfEvent("[Post Processing] Pre-Upscale");

	// update auto-enabled features
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsAutoEnabled())
			pipe->UpdateAutoEnabled();
	}

	// go through each fx
	for (auto& pipe : pipeline) {
		if (pipe && pipe->enabled && !pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && pipe->DrawBeforeUpscaling()) {
			DrawFeature(*pipe, lastTexColor);
		}
	}

	CopyToRenderTarget(gameTexMain, texCopyMain.get(), lastTexColor.tex, lastTexColor.srv);

	state->EndPerfEvent();
}

void PostProcessing::PreProcess(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output)
{
	if (bypass)
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
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };
	auto gameTexMainAlt = useMainCopy ? gameTexMainRT : gameTexMainCopyRT;

	// update auto-enabled features
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsAutoEnabled())
			pipe->UpdateAutoEnabled();
	}

	// go through each fx
	for (auto& pipe : pipeline) {
		if (pipe && pipe->enabled && !pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && (!pipe->DrawBeforeUpscaling() || !upscaling.loaded)) {
			DrawFeature(*pipe, lastTexColor);
		}
	}

	for (auto& pipe : pipeline) {
		if (pipe && pipe->enabled && pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && (!pipe->DrawBeforeUpscaling() || !upscaling.loaded)) {
			DrawFeature(*pipe, lastTexColor);
		}
	}

	Texture2D* mainConvertTex = texCopyMain.get();
	Texture2D* mainCopyConvertTex = texCopyMainCopy ? texCopyMainCopy.get() : texCopyMain.get();

	CopyToRenderTarget(gameTexMain, useMainCopy ? mainCopyConvertTex : mainConvertTex, lastTexColor.tex, lastTexColor.srv);
	CopyToRenderTarget(gameTexMainAlt, useMainCopy ? mainConvertTex : mainCopyConvertTex, lastTexColor.tex, lastTexColor.srv);

	isrefraction = false;

	globals::state->SetOutputRenderTarget(a_output);
}

void PostProcessing::ClearBorderMotionVectorsForFrameGen()
{
	// Effects11 skips the letterbox, so its motion vectors must remain live scene data.
	if (bypass || IsTonemapOwnedByEffects11())
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
	return !bypass && settings.DisableVanillaTonemapping != 0;
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

void PostProcessing::Prepass()
{
	if (!pendingSettings.empty())
		ApplyPendingSettings();

	// globals::game::imageSpaceManager isn't cached until OnDataLoaded(); skip
	// the update rather than crash if Prepass() runs before that.
	if (!globals::game::imageSpaceManager) {
		return;
	}

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
	stl::write_vfunc<0x2, BSImagespaceShaderRefraction_SetupTechnique>(RE::VTABLE_BSImagespaceShaderRefraction[0]);
}
