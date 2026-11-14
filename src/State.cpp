#include "State.h"
#include <algorithm>

#include <codecvt>

#include <pystring/pystring.h>

#include "BaselineManager.h"
#include "CSEditor/FormEditSources.h"
#include "CSEditor/SceneManager/SceneSettingsUI.h"
#include "Deferred.h"
#include "EngineFixes/LODBlendRingFix.h"
#include "EngineFixes/LandscapeSeamsFix.h"
#include "EngineFixes/SunShadowMaskFix.h"
#include "FeatureIssues.h"
#include "Features/CSEditor.h"
#include "Features/CSUtility.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#if defined(ENABLE_EFFECTS11)
#	include "Features/Effects11.h"
#	include "Features/Effects11/EffectManager.h"
#	include "Features/Effects11/SettingManager.h"
#endif
#include "Features/ExponentialHeightFog.h"
#include "Features/FoveatedCommon.h"
#include "Features/HDRDisplay.h"
#include "Features/InteriorSun.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PostProcessing.h"
#include "Features/RainRefraction.h"
#include "Features/RemoteControl/NvPerf.h"
#include "Features/SceneManager.h"
#include "Features/SceneSelector.h"
#include "Features/Skin.h"
#include "Features/SkySync.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainHelper.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "Features/VRStereoOptimizations.h"
#include "Features/VolumetricShadows.h"
#include "Features/Wind/Wind.h"
#include "Menu.h"
#include "PostProcessingMode.h"
#include "Presets/BaseSettingsCustomization.h"
#include "Presets/PresetPreferences.h"
#include "Presets/PresetState.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneSettingsManager.h"
#include "SettingsOverrideManager.h"
#include "ShaderCache.h"
#include "TruePBR.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/SettingsPatch.h"
#include "Utils/SphericalHarmonics.h"
#include "Utils/TracyZones.h"
#include "VRAPI/CSpluginapi.h"
#include "VanillaBinding.h"

struct State::SettingsLayerGuard::Impl
{
	SceneSettingsManager::SceneLayerGuard guard;
};

State::SettingsLayerGuard::SettingsLayerGuard() : impl(std::make_unique<Impl>()) {}
State::SettingsLayerGuard::~SettingsLayerGuard() = default;

#ifdef TRACY_ENABLE
static thread_local std::vector<TracyCZoneCtx> s_tracyPerfZones;
#endif
static thread_local std::vector<bool> s_perfEventEmitted;

void State::UpdateLightingShaderPermutation(RE::BSRenderPass* a_pass)
{
	constexpr auto additiveLighting = static_cast<uint32_t>(State::ExtraShaderDescriptors::AdditiveLighting);
	permutationData.ExtraShaderDescriptor &= ~additiveLighting;

	if (!a_pass || !a_pass->geometry)
		return;

	auto* alphaProperty = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
	if (alphaProperty && alphaProperty->GetAlphaBlending() &&
		alphaProperty->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kOne) {
		permutationData.ExtraShaderDescriptor |= additiveLighting;
	}
}

void State::UpdateSkyShaderPermutation(RE::BSRenderPass* a_pass)
{
	permutationData.ExtraShaderDescriptor &= ~(static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSun) | static_cast<uint32_t>(State::ExtraShaderDescriptors::IsMoon) | static_cast<uint32_t>(State::ExtraShaderDescriptors::NoSkyScattering));

	if (!a_pass || !a_pass->shaderProperty)
		return;

	auto* skyProperty = static_cast<const RE::BSSkyShaderProperty*>(a_pass->shaderProperty);
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS) {
		// Bottled Shaders keeps cloud layer 28 out of the scattering
		constexpr std::uint16_t kNoScatteringCloudLayer = 28;
		auto* sky = globals::game::sky;
		if (sky && sky->clouds && kNoScatteringCloudLayer < sky->clouds->numLayers &&
			sky->clouds->clouds[kNoScatteringCloudLayer].get() == a_pass->geometry)
			permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::NoSkyScattering);
	}
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN ||
		skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN_GLARE) {
		permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSun);
	}
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON)
		permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsMoon);
}

void State::UpdateEffectShaderPermutation(RE::BSRenderPass* a_pass)
{
	constexpr auto isAurora = static_cast<uint32_t>(ExtraShaderDescriptors::IsAurora);
	permutationData.ExtraShaderDescriptor &= ~isAurora;

	if (!a_pass || !a_pass->geometry)
		return;
	if (!(currentVertexDescriptor & static_cast<uint32_t>(SIE::ShaderCache::EffectShaderFlags::SkyObject)))
		return;

	const auto sky = globals::game::sky;
	if (!sky || !sky->auroraRoot)
		return;

	for (const RE::NiAVObject* node = a_pass->geometry; node; node = node->parent) {
		if (node == sky->auroraRoot.get()) {
			permutationData.ExtraShaderDescriptor |= isAurora;
			return;
		}
	}
}

void State::UpdatePermutationBuffer()
{
	const auto windContribution = globals::features::wind.GetPermutationContribution();
	permutationData.WindIntensityOverride = windContribution.windIntensityOverride;
	permutationData.OverrideWindIntensity = windContribution.overrideWindIntensity;
	permutationData.EnableGrassWindSpringBend = windContribution.enableGrassWindSpringBend;
	const auto treeBendDescriptor = static_cast<uint32_t>(ExtraShaderDescriptors::TreeBend);
	if ((permutationData.ExtraShaderDescriptor & treeBendDescriptor) == 0) {
		permutationData.TreeTransientWindInfluence = windContribution.treeTransientWindInfluenceDefault;
		permutationData.TreeLeafTransientWindInfluence = windContribution.treeLeafTransientWindInfluenceDefault;
		permutationData.TreeLeafTransientFlutterMaximum = windContribution.treeLeafTransientFlutterMaximumDefault;
		permutationData.TreeTransientMaximumBendMultiplier = windContribution.treeTransientMaximumBendMultiplierDefault;
	}
	permutationData.TrunkWindBendSensitivity = windContribution.trunkWindBendSensitivity;
	permutationData.TreeLeafBaseWindFlutterGain = windContribution.treeLeafBaseWindFlutterGain;
	permutationData.EnableAmbientGrassWind = windContribution.enableAmbientGrassWind;
	permutationData.GrassWindSensitivity = windContribution.grassWindSensitivity;
	permutationData.GrassWindCompressionToBend = windContribution.grassWindCompressionToBend;
	permutationData.GrassWindFlutterStrength = windContribution.grassWindFlutterStrength;
	permutationData.GrassWindFlutterFrequency = windContribution.grassWindFlutterFrequency;
	if (permutationData != permutationDataPrevious) {
		permutationCB->Update(permutationData);
		permutationDataPrevious = permutationData;
	}
}

void State::BindVertexPermutationData(const RE::BSShader* a_shader)
{
	constexpr UINT kPermutationVertexRegister = 4;

	if (!a_shader)
		a_shader = currentShader;
	if (!a_shader || !globals::shaderCache || !globals::shaderCache->IsEnabled() || !globals::d3d::context)
		return;

	const auto shaderType = a_shader->shaderType.get();
	if (shaderType != RE::BSShader::Type::Lighting && shaderType != RE::BSShader::Type::Utility &&
		shaderType != RE::BSShader::Type::Grass && shaderType != RE::BSShader::Type::Sky)
		return;

	ID3D11Buffer* buffers[] = {
		permutationCB->CB(),
		sharedDataCB->CB(),
		featureDataCB->CB(),
	};
	globals::d3d::context->VSSetConstantBuffers(kPermutationVertexRegister, ARRAYSIZE(buffers), buffers);
}

void State::BindSharedDataCS(ID3D11DeviceContext* a_context, bool a_withFeatureData) const
{
	constexpr UINT kSharedDataRegister = 5;
	ID3D11Buffer* buffers[]{ sharedDataCB->CB(), featureDataCB->CB() };
	a_context->CSSetConstantBuffers(kSharedDataRegister, a_withFeatureData ? ARRAYSIZE(buffers) : 1, buffers);
}

void State::Draw()
{
	TracyZones::Count(TracyZones::Family::StateDraw);
	CS_DRAW_ZONE("State::Draw");
	UpdateGrassGpuPass();
	if (globals::features::sceneManager.loaded)
		globals::features::sceneManager.Update();

	auto shaderCache = globals::shaderCache;
	auto& terrainBlending = globals::features::terrainBlending;
	auto& terrainHelper = globals::features::terrainHelper;
	auto& cloudShadows = globals::features::cloudShadows;
	auto& skin = globals::features::skin;
	auto& truePBR = globals::features::truePBR;
	auto context = globals::d3d::context;
	auto& volumetricShadows = globals::features::volumetricShadows;

	if (shaderCache->IsEnabled()) {
		if (terrainBlending.loaded && terrainBlending.settings.Enabled) {
			CS_DRAW_ZONE("TerrainBlending::TerrainShaderHacks");
			terrainBlending.TerrainShaderHacks();
		}

		if (cloudShadows.loaded) {
			CS_DRAW_ZONE("CloudShadows::SkyShaderHacks");
			cloudShadows.SkyShaderHacks();
		}

		if (globals::features::rainRefraction.loaded) {
			CS_DRAW_ZONE("RainRefraction::ParticleShaderHacks");
			globals::features::rainRefraction.ParticleShaderHacks();
		}

		if (terrainHelper.loaded) {
			CS_DRAW_ZONE("TerrainHelper::SetShaderResources");
			terrainHelper.SetShaderResources(context);
		}

		if (skin.loaded) {
			CS_DRAW_ZONE("Skin::SetShaderResources");
			skin.SetShaderResources(context);
		}

		if (truePBR.loaded) {
			CS_DRAW_ZONE("TruePBR::SetShaderResources");
			truePBR.SetShaderResources(context);
		}

		if (volumetricShadows.loaded) {
			CS_DRAW_ZONE("VolumetricShadows::SetShaderResources");
			volumetricShadows.SetShaderResources(context);
		}

		UpdatePermutationBuffer();

		if (currentShader && updateShader) {
			if (currentShader->shaderType.get() == RE::BSShader::Type::Utility) {
				if (currentPixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmask)) {
					if (globals::features::exponentialHeightFog.loaded)
						globals::features::exponentialHeightFog.CaptureDirectionalShadowMap();
				}
			}
		}

		if (currentShader)
			Feature::ApplyDrawHooks();

		if (globals::menu->overlayVisible && globals::features::performanceOverlay.loaded && globals::features::performanceOverlay.IsOverlayVisible())
			Debug();

		// Active Shaders counts draws per frame whether or not the performance overlay is up
		if (IsDeveloperMode() && shaderTrackingFrameChecker.IsNewFrame())
			globals::shaderCache->ResetFrameShaderTracking();

		updateShader = false;
	}
}

void State::UpdateGrassGpuPass()
{
	const bool isGrassDraw = currentShader && currentShader->shaderType.get() == RE::BSShader::Type::Grass;
	if (isGrassDraw) {
		if (!grassGpuPass)
			grassGpuPass.emplace("Grass::Draw", GpuPassSpan::Spanning);
	} else {
		grassGpuPass.reset();
	}
}

void State::Debug()
{
	auto lock = Lock();

	if (frameChecker.IsNewFrame()) {
		// Smooth draw calls and frame times for all shader types
		for (int i = 0; i < magic_enum::enum_integer(RE::BSShader::Type::Total) + 1; ++i) {
			smoothDrawCalls[i] = smoothDrawCalls[i] * static_cast<float>(0.95) + drawCalls[i] * static_cast<float>(0.05);
			smoothFrameTimePerType[i] = smoothFrameTimePerType[i] * static_cast<float>(0.95) + frameTimePerType[i] * static_cast<float>(0.05);
		}
		// Reset counters for next frame
		for (auto& c : drawCalls)
			c = 0;
		for (auto& ft : frameTimePerType)
			ft = 0.0f;

		// Start timing for this frame
		if (frameTimingFrequency.QuadPart == 0) {
			QueryPerformanceFrequency(&frameTimingFrequency);
		}
		QueryPerformanceCounter(&frameStartTime);
		frameTimingActive = true;
	}

	// Track time for current shader type if timing is active
	if (frameTimingActive && currentShader) {
		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);

		// Calculate elapsed time in milliseconds
		float elapsed = (currentTime.QuadPart - frameStartTime.QuadPart) * 1000.0f / frameTimingFrequency.QuadPart;

		// Add elapsed time to the current shader type
		frameTimePerType[magic_enum::enum_integer(currentShader->shaderType.get())] += elapsed;
		frameTimePerType[magic_enum::enum_integer(RE::BSShader::Type::Total)] += elapsed;

		// Update start time for next measurement
		frameStartTime = currentTime;
	}

	if (currentShader) {
		drawCalls[magic_enum::enum_integer(currentShader->shaderType.get())]++;
		drawCalls[magic_enum::enum_integer(RE::BSShader::Type::Total)]++;
	}

	if (currentShader && updateShader && drawAnnotationsActive) {
		// Per-draw (thousands/frame): D3D-capture marker only, never a Tracy zone --
		// a per-draw dynamic Tracy zone allocs a source location per call and OOMs
		// Tracy. Matches BeginDrawEvent's rationale.
		BeginDrawEvent("Draw: OS {}::{:x}::{}", magic_enum::enum_name(currentShader->shaderType.get()), permutationData.PixelShaderDescriptor, currentShader->fxpFilename);
		SetPerfMarker("Defines: {}", SIE::ShaderCache::GetDefinesString(*currentShader, permutationData.PixelShaderDescriptor));
		EndDrawEvent();
	}
}

State::TonemapOwner State::GetTonemapOwner()
{
	static Util::FrameChecker tonemapOwnerFrameChecker;
	static TonemapOwner cachedOwner = TonemapOwner::kVanilla;

	if (!tonemapOwnerFrameChecker.IsNewFrame())
		return cachedOwner;

	auto& postProcessing = globals::features::postProcessing;

#if defined(ENABLE_EFFECTS11)
	auto& effects11 = globals::features::effects11;
	if (effects11.loaded && effects11.WantsTonemapOwnership())
		cachedOwner = TonemapOwner::kEffects11;
	else
#endif
		if (postProcessing.loaded && postProcessing.WantsTonemapOwnership())
		cachedOwner = TonemapOwner::kPostProcessing;
	else
		cachedOwner = TonemapOwner::kVanilla;

	postProcessing.UpdateVanillaEffects(cachedOwner == TonemapOwner::kPostProcessing);
	return cachedOwner;
}

bool State::HandlePostProcessing(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output)
{
#if defined(ENABLE_EFFECTS11)
	// VR-only: world-space menus need vanilla's kMENUBG compositor handoff, which
	// Effects11's tonemap doesn't replicate; flatrim's blur should keep grading.
	if (globals::game::isVR && a_output == RE::RENDER_TARGETS::kMENUBG)
		return false;

	if (GetTonemapOwner() != TonemapOwner::kEffects11 ||
		!globals::features::effects11.RenderTonemap(a_input, a_output))
		return false;

	SetOutputRenderTarget(a_output);

	return true;
#else
	(void)a_input;
	(void)a_output;
	return false;
#endif
}

void State::SetOutputRenderTarget(RE::RENDER_TARGET a_output)
{
	auto renderer = globals::game::renderer;
	auto& outputRT = renderer->GetRuntimeData().renderTargets[a_output];
	globals::d3d::context->OMSetRenderTargets(1, Util::AsReal(&outputRT.RTV), nullptr);

	auto shadowState = globals::game::shadowState;
	auto applyStateData = [a_output](auto& stateData) {
		stateData.renderTargets[0] = a_output;
		stateData.setRenderTargetMode[0] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
		for (int i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			stateData.renderTargets[i] = RE::RENDER_TARGET::kNONE;
			stateData.setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
		}
		stateData.depthStencil = static_cast<uint32_t>(-1);
	};
	// VR's fields sit at different offsets (e.g. setRenderTargetMode at flat 0x48 vs VR 0x50);
	// the flat accessor on VR corrupts adjacent RendererShadowState fields.
	CALL_WITH_RUNTIME_DATA(shadowState, applyStateData);
}

void State::SetGrassFlags(bool sphereNormal, bool pbr)
{
	constexpr auto sphereNormalFlag = static_cast<uint32_t>(ExtraShaderDescriptors::GrassSphereNormal);
	constexpr auto pbrFlag = static_cast<uint32_t>(ExtraShaderDescriptors::GrassPBR);
	permutationData.ExtraShaderDescriptor = (permutationData.ExtraShaderDescriptor & ~(sphereNormalFlag | pbrFlag)) |
	                                        (sphereNormal ? sphereNormalFlag : 0) | (pbr ? pbrFlag : 0);
}

/**
 * @brief Resets per-frame state and publishes frame counter to off-thread readers.
 *
 * Ends the current profiler frame, resets all loaded features, updates the
 * timer if the game is not paused, caches current menu open states, clears
 * shader descriptor tracking, increments and atomically publishes the frame
 * count, and disables reflection and improved snow shaders.
 */
void State::Reset()
{
	grassGpuPass.reset();

	// Land staged SKSE API setter writes before features consume settings this frame.
	CSPluginAPI::ProcessStagedSettings();

	// frameCount, not frameCount+1: EndFrame stamps the frame whose work this
	// call just closed out, i.e. the current (pre-increment) count -- results
	// only surface kFrameLatency frames later, so that lag is expected, not
	// an off-by-one to correct for.
	globals::profiler->EndFrame(frameCount);

	Feature::ForEachLoadedFeature("Reset", [](Feature* feature) { feature->Reset(); });
	LandscapeSeamsFix::GetInstance().Reset();
	LODBlendRingFix::GetInstance().Update();
	SunShadowMaskFix::Update();

	worldRenderedThisFrame = false;

	// Cache menu open states once per frame to avoid repeated IsMenuOpen calls
	// (each call constructs a BSFixedString, which is expensive at scale).
	if (auto ui = globals::game::ui) {
		if (!ui->GameIsPaused())
			timer += RE::GetSecondsSinceLastFrame();

		isMainMenuOpen = ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
		isLoadingMenuOpen = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		isMapMenuOpen = ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
		isStatsMenuOpen = ui->IsMenuOpen(RE::StatsMenu::MENU_NAME);
	} else {
		isMainMenuOpen = false;
		isLoadingMenuOpen = false;
		isMapMenuOpen = false;
		isStatsMenuOpen = false;
	}

	lastModifiedPixelDescriptor = 0;
	lastModifiedVertexDescriptor = 0;
	lastPixelDescriptor = 0;
	lastVertexDescriptor = 0;
	std::memset(&permutationDataPrevious, 0xFF, sizeof(PermutationCB));
	frameCount++;
	// Publish for off-thread readers (e.g. the MCP listener thread).
	frameCountAtomic.store(frameCount, std::memory_order_relaxed);

	globals::shaderCache->TickActiveShaderCapture(globals::menu->ShouldSwallowInput());
	globals::shaderCache->ProcessPendingClear();

	if (auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton()) {
		GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderApplyReflections, imageSpaceManager);

		// Disable reflections being applied to things other than water
		if (BSImagespaceShaderApplyReflections.get()) {
			BSImagespaceShaderApplyReflections->active = false;
		}
	}

	// Disable "improved" snow shader, unsupported
	if (!globals::game::isVR) {
		static RE::Setting* improvedSnow = nullptr;
		if (!improvedSnow)
			improvedSnow = RE::GetINISetting("bEnableImprovedSnow:Display");
		if (improvedSnow)
			improvedSnow->data.b = false;
	}

	activeReflections = false;
}

void State::Setup()
{
	globals::features::truePBR.SetupResources();
	SetupResources();

	// Probe typed UAV load support before features set up their resources, so any
	// gating logic that wants to read the log can run during feature SetupResources.
	CheckTypedUAVLoadSupport();

	Feature::ForEachLoadedFeature("SetupResources", [](Feature* feature) { feature->SetupResources(); });
	globals::deferred->SetupResources();
	Feature::ForEachLoadedFeature("PostSetupResources", [](Feature* feature) { feature->PostSetupResources(); });
	PostProcessingMode::ApplyPending();
	if (!featureResourcesReady && !settingsChanged && !savedSettingsSnapshot.is_null())
		MarkSettingsSaved(true);
	featureResourcesReady = true;
}

static std::string GetConfigPath(State::ConfigMode a_configMode)
{
	switch (a_configMode) {
	case State::ConfigMode::USER:
		return Util::PathHelpers::GetSettingsUserPath().string();
	case State::ConfigMode::TEST:
		return Util::PathHelpers::GetSettingsTestPath().string();
	case State::ConfigMode::THEME:
		return Util::PathHelpers::GetSettingsThemePath().string();
	case State::ConfigMode::DEFAULT:
	default:
		return Util::PathHelpers::GetSettingsDefaultPath().string();
	}
}

static bool WriteConfigFile(const std::filesystem::path& a_configPath, std::string_view a_contents)
{
	std::ofstream output{ a_configPath, std::ios::binary | std::ios::trunc };
	if (!output.is_open()) {
		return false;
	}

	output.write(a_contents.data(), static_cast<std::streamsize>(a_contents.size()));
	output.close();
	return !output.fail();
}

static bool WriteConfigAtomically(const std::filesystem::path& a_configPath, std::string_view a_contents)
{
	auto temporaryPath = a_configPath;
	temporaryPath += std::format(".{}.{}.tmp", GetCurrentProcessId(), GetCurrentThreadId());

	if (!WriteConfigFile(temporaryPath, a_contents)) {
		logger::warn("Failed to write temporary config file: {}", temporaryPath.string());
		std::error_code cleanupError;
		std::filesystem::remove(temporaryPath, cleanupError);
		return false;
	}

	if (!MoveFileExW(temporaryPath.c_str(), a_configPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		const auto error = GetLastError();
		logger::warn("Failed to replace config file {}: Windows error {}; retrying with a direct write", a_configPath.string(), error);
		std::error_code cleanupError;
		std::filesystem::remove(temporaryPath, cleanupError);

		// Virtual filesystems can reject replacement while still allowing direct writes.
		if (!WriteConfigFile(a_configPath, a_contents)) {
			logger::warn("Failed to write config file directly: {}", a_configPath.string());
			return false;
		}
		logger::info("Saved config directly after atomic replacement failed: {}", a_configPath.string());
	}

	return true;
}

static json BuildPresetBootSettings()
{
	json result = json::object();
	const auto& catalog = UnifiedPresetCatalog::GetSingleton();
	for (const auto& id : catalog.GetBaselinePackIds()) {
		const auto* pack = catalog.FindPack(id);
		if (!pack || !pack->valid)
			continue;
		for (auto* feature : Feature::GetFeatureList()) {
			const auto preference = pack->disableAtBoot.find(feature->GetShortName());
			if (!feature->IsAlwaysEnabled() && preference != pack->disableAtBoot.end())
				result[feature->GetShortName()] = preference->second;
		}
	}
	return result;
}

static json PrepareUserFeatureSettings(json& settings, const json& userSettings, const json& baselineMask, bool normalized)
{
	// Feature migrations must distinguish missing fields from explicit defaults.
	if (!normalized) {
		auto mask = baselineMask;
		mask.update(userSettings, true);
		settings = Util::Settings::SelectSettings(settings, mask);
	}
	return Util::Settings::SelectSettings(settings, userSettings);
}

void State::Load(ConfigMode a_configMode, bool a_allowReload)
{
	if (auto* manager = SceneSettingsManager::GetSingleton()) {
		if (a_configMode == ConfigMode::DEFAULT)
			manager->DiscardContextEdit();
		else if (manager->HasPendingContextEdit()) {
			SceneSettingsUI::WarnPendingContextEdit();
			return;
		}
	}
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	const bool restoring = a_configMode == ConfigMode::USER && !defaultSettingsBaseline.is_null();
	const bool restoringDefaults = a_configMode == ConfigMode::DEFAULT && !defaultSettingsBaseline.is_null();
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto previousFormRoots = catalog.GetActiveFormPackRoots();
	const bool reloadFormEdits = a_configMode == ConfigMode::USER && (!restoring || FormEditSources::CanChangePacks());
	auto* overrideManager = SettingsOverrideManager::GetSingleton();
	const json previousCatalog{ { UnifiedPresetCatalog::kUserSettingsKey, catalog.SerializeActiveState() } };
	const auto previousFeatureBases = featureSettingsBases;
	const auto previousUserEdits = userFeatureEdits;
	const auto previousBootPreferences = userBootPreferences;
	const auto previousOverrideBase = overrideManager->GetBaseSettings();
	const auto previousApplied = overrideManager->GetAppliedSettings();
	json previousSettings;
	std::vector<std::pair<Feature*, bool>> previousFeatureLoadStates;
	std::vector<std::pair<PresetProvider*, std::string>> previousPayloads;
	const auto previousMode = PostProcessingMode::Get();
	if (restoring) {
		SaveToJson(previousSettings);
		for (auto* feature : Feature::GetFeatureList()) {
			previousFeatureLoadStates.emplace_back(feature, feature->loaded);
			if (featureResourcesReady && feature->loaded)
				if (auto* provider = feature->GetPresetProvider())
					if (const auto selected = provider->GetActivePresetId())
						previousPayloads.emplace_back(provider, *selected);
		}
	}
	json settings;
	bool errorDetected = false;

	auto configFolderPath = std::filesystem::path(GetConfigPath(a_configMode)).parent_path().string();
	auto defaultConfigFilePath = GetConfigPath(ConfigMode::DEFAULT);
	auto userConfigFilePath = GetConfigPath(ConfigMode::USER);

	try {
		std::filesystem::create_directories(configFolderPath);
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error creating directory during Load ({}) : {}\n", configFolderPath, e.what());
		errorDetected = true;
	}

	// Load ordinary settings before presets and explicit preset customizations.

	// Step 1: Generate the baseline before any user settings can reach live state
	if (defaultSettingsBaseline.is_null()) {
		std::fill(enabledClasses, enabledClasses + magic_enum::enum_integer(RE::BSShader::Type::Total) - 1, true);
		json generatedDefaults;
		std::string serializedDefaults;
		try {
			SaveToJson(generatedDefaults);
			auto& disabledByDefault = generatedDefaults["Disable at Boot"];
			for (auto* feature : Feature::GetFeatureList())
				disabledByDefault[feature->GetShortName()] = feature->IsDisabledByDefault();
			serializedDefaults = generatedDefaults.dump(1);
		} catch (const std::exception& e) {
			logger::error("Failed to generate current default settings: {}", e.what());
			return;
		}
		defaultSettingsBaseline = std::move(generatedDefaults);

		if (!WriteConfigAtomically(defaultConfigFilePath, serializedDefaults)) {
			logger::warn("Failed to persist current default settings to: {}", defaultConfigFilePath);
		} else {
			logger::info("Generated current default settings at: {}", defaultConfigFilePath);
		}
	}
	settings = defaultSettingsBaseline;
	json userSettings = json::object();
	if (a_configMode == ConfigMode::USER) {
		std::ifstream userFile(userConfigFilePath);
		if (userFile.is_open()) {
			try {
				json parsed;
				userFile >> parsed;
				if (!parsed.is_object()) {
					if (restoring)
						return;
				} else
					userSettings = std::move(parsed);
			} catch (const json::exception& e) {
				logger::warn("Error parsing user config file: {}", e.what());
				if (restoring)
					return;
			}
		}
	}
	if (a_configMode == ConfigMode::USER)
		CaptureSavedSettingsFields(userSettings);
	if (a_configMode == ConfigMode::USER)
		userSettings["Presets"] = PresetPreferences::Restore(userSettings.value("Presets", json::object()));
	Feature::MigrateConfigSettings(userSettings);
	if (const auto boot = userSettings.find("Disable at Boot"); boot != userSettings.end())
		*boot = PresetState::NormalizeBootPreferences(*boot, defaultSettingsBaseline["Disable at Boot"],
			userSettings.value(UnifiedPresetCatalog::kUserSettingsKey, json::object()));
	if (a_configMode == ConfigMode::USER)
		catalog.LoadActiveState(userSettings);
	catalog.Discover();
	if (restoring && !reloadFormEdits && previousFormRoots != catalog.GetActiveFormPackRoots()) {
		catalog.LoadActiveState(previousCatalog);
		catalog.SaveActiveState();
		logger::warn("Save or discard unsaved OS Editor record edits before restoring different preset record sources");
		return;
	}
	auto* baselines = BaselineManager::GetSingleton();
	baselines->DiscoverFiles();
	featureSettingsBases = defaultSettingsBaseline;
	userFeatureEdits = json::object();
	userBootPreferences = json::object();
	pendingBaseSettingsResets = json::object();
	json baselineMasks = json::object();
	if (a_configMode == ConfigMode::USER || restoringDefaults) {
		baselineMasks["Disable at Boot"] = BuildPresetBootSettings();
		featureSettingsBases["Disable at Boot"].update(baselineMasks["Disable at Boot"], true);
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->UsesMainSettings())
				continue;
			const auto name = feature->GetName();
			auto patch = baselines->BuildFeatureBaseline(feature->GetShortName(), settings[name]);
			featureSettingsBases[name].update(patch, true);
			baselineMasks[name] = std::move(patch);
		}
	}
	if (a_configMode == ConfigMode::USER) {
		const auto presetState = catalog.SerializeActiveState();
		const auto reconciled = BaseSettingsCustomization::Reconcile(
			presetState.value(PresetState::kBaseSettingsCustomizationsKey, json::object()),
			baselines->BuildCustomizationSources(), baselineMasks);
		baseSettingsInvalidations = reconciled.removed;
		Util::Settings::RestoreSettings(userSettings, json::object(), reconciled.removed);
		catalog.SetBaseSettingsCustomizations(reconciled.state);
		Util::Settings::OverlayRecognizedRootSettings(settings, userSettings);
		Util::Settings::MergeSettings(settings, baselineMasks);
		Util::Settings::MergeSettings(settings, reconciled.state.at("edits"));
		const auto personal = BaseSettingsCustomization::UserLayer(userSettings, reconciled.state);
		userBootPreferences = personal.value("Disable at Boot", json::object());
		if (const auto menu = userSettings.find("Menu"); menu != userSettings.end() && menu->is_object())
			Menu::OverlayInputSettings(settings["Menu"], defaultSettingsBaseline["Menu"], *menu);
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->HasPersonalSettings())
				continue;
			const auto name = feature->GetName();
			if (const auto edits = personal.find(name); edits != personal.end() && edits->is_object() && settings.contains(name))
				userFeatureEdits[name] = PrepareUserFeatureSettings(settings[name], *edits,
					baselineMasks.value(name, json::object()), PresetState::HasNormalizedFeatureEdits(presetState, name));
		}
		logger::info("Applied user settings from: {}", userConfigFilePath);
	}

	overrideManager->CaptureBaseSettings(settings);

	try {
		// Load core settings (Menu, Advanced, General, Replace Original Shaders)
		logger::info("Loading core settings");
		LoadFromJson(settings);
		for (auto* feature : Feature::GetFeatureList()) {
			try {
				const std::string featureName = feature->GetShortName();
				// Resolved here rather than in Feature::Load so features disabled at boot,
				// which never reach Load, still report their install state to the UI.
				std::error_code ec;
				const bool iniExists = std::filesystem::exists(Util::PathHelpers::GetFeatureIniPath(featureName), ec);
				// exists() reports false on error, so treat an unreadable path as installed rather than letting a probe failure hide the feature.
				if (ec)
					logger::warn("Could not determine install state for feature '{}': {}", featureName, ec.message());
				feature->installed = ec || iniExists;
				bool isDisabled = !feature->IsAlwaysEnabled() && ((disabledFeatures.contains(featureName) && disabledFeatures[featureName]) || !feature->conflictMessage.empty());
				if (!isDisabled) {
					logger::info("Loading Feature: '{}'", featureName);

					// Load base feature settings from merged config (default + user)
					feature->Load(settings);

				} else {
					logger::info("Feature '{}' is disabled at boot.", featureName);
				}
			} catch (const std::exception& e) {
				feature->failedLoadedMessage = feature->failedLoadedMessage.empty() ?
				                                   (feature->GetDisplayName() + " failed to load. Check OpenShaders.log") :
				                                   (feature->failedLoadedMessage + "\n" + feature->GetDisplayName() + " failed to load. Check OpenShaders.log");
				logger::warn("Error loading setting for feature '{}': {}", feature->GetShortName(), e.what());
			}
		}

		json appliedSettings;
		SaveToJson(appliedSettings);
		if (restoringDefaults)
			Util::Settings::RestoreSettings(appliedSettings, previousApplied, baselineMasks);
		overrideManager->CaptureAppliedSettings(appliedSettings);

		if (settings["Version"].is_string() && settings["Version"].get<std::string>() != Plugin::VERSION.string()) {
			logger::info("Loaded config for version {}; version {} will be written on save", (std::string)settings["Version"], Plugin::VERSION.string());
		}

		FeatureIssues::ScanForOrphanedFeatureINIs();
		std::optional<FormEditSources::ReloadHistoryGuard> reloadHistory;
		if (reloadFormEdits)
			reloadHistory.emplace();
		bool payloadsLoaded = true;
		if (restoring && featureResourcesReady) {
			try {
				for (auto* feature : Feature::GetFeatureList()) {
					if (!feature->loaded)
						continue;
					if (auto* provider = feature->GetPresetProvider())
						if (const auto selected = catalog.GetActivePayloadPresetId(*provider); selected && !provider->Apply(*selected, false)) {
							payloadsLoaded = false;
							break;
						}
				}
			} catch (const std::exception& error) {
				logger::error("Could not restore preset payload selection: {}", error.what());
				payloadsLoaded = false;
			} catch (...) {
				logger::error("Could not restore preset payload selection after an unknown exception");
				payloadsLoaded = false;
			}
		}
#if defined(ENABLE_EFFECTS11)
		// Effects11's ENB state lives in enbseries.ini, not the Settings JSON -- refresh
		// it here too so a genuine reload picks up on-disk changes.
		if (payloadsLoaded && a_configMode == ConfigMode::USER && EffectManager::GetSingleton().IsPresetLoaded()) {
			SettingManager::GetSingleton().Load();
			EffectManager::GetSingleton().Load();
		}
#endif
		if (payloadsLoaded)
			PostProcessingMode::ApplyReloaded();

		if (a_configMode == ConfigMode::USER) {
			try {
				payloadsLoaded = payloadsLoaded && SceneSettingsManager::GetSingleton()->ReloadSceneSettings() &&
				                 (!reloadFormEdits || FormEditSources::ReloadActivePack());
			} catch (const std::exception& error) {
				logger::error("Could not reload preset payloads: {}", error.what());
				payloadsLoaded = false;
			} catch (...) {
				logger::error("Could not reload preset payloads after an unknown exception");
				payloadsLoaded = false;
			}
		}
		if (payloadsLoaded && a_configMode == ConfigMode::USER && !catalog.SaveActiveState())
			throw std::runtime_error("Could not persist Base Settings customizations");

		if (!payloadsLoaded) {
			logger::error("Preset payload reload failed; restoring the prior selection");
			const auto restore = [](const char* layer, auto&& action) {
				try {
					if (!action())
						logger::error("Could not restore previous {}", layer);
				} catch (const std::exception& error) {
					logger::error("Could not restore previous {}: {}", layer, error.what());
				} catch (...) {
					logger::error("Could not restore previous {} after an unknown exception", layer);
				}
			};
			restore("preset selection", [&] { catalog.LoadActiveState(previousCatalog); return true; });
			restore("personal preset layers", [&] {
				featureSettingsBases = previousFeatureBases;
				userFeatureEdits = previousUserEdits;
				userBootPreferences = previousBootPreferences;
				return true;
			});
			for (const auto& [feature, loaded] : previousFeatureLoadStates)
				feature->loaded = loaded;
			for (const auto& [provider, selected] : previousPayloads)
				restore("preset payload", [&] { return provider->Apply(selected, false); });
			if (restoring && featureResourcesReady)
				restore("pipeline", [&] { return PostProcessingMode::Set(previousMode); });
			if (restoring)
				restore("live settings", [&] { LoadFromJson(previousSettings); return true; });
			restore("settings bases", [&] { overrideManager->CaptureBaseSettings(previousOverrideBase); return true; });
			restore("applied settings", [&] { overrideManager->CaptureAppliedSettings(previousApplied); return true; });
			restore("scene settings", [] { return SceneSettingsManager::GetSingleton()->ReloadSceneSettings(); });
			if (reloadFormEdits)
				restore("editor form sources", [] { return FormEditSources::ReloadActivePack(); });
			return;
		}
		if (restoringDefaults)
			CheckSettingsChanged();
		else
			MarkSettingsSaved();
		if (reloadHistory)
			reloadHistory->Commit();
		logger::info("Loading Settings Complete");
	} catch (const json::exception& e) {
		logger::warn("General JSON error accessing settings: {}", e.what());
		errorDetected = true;
	} catch (const std::exception& e) {
		logger::warn("General error accessing settings: {}", e.what());
		errorDetected = true;
	}
	if (errorDetected && a_allowReload)
		Load(a_configMode, false);
}

const json* State::GetFeatureDefaults(std::string_view featureName) const
{
	const auto found = defaultSettingsBaseline.find(featureName);
	return found != defaultSettingsBaseline.end() && found->is_object() ? &*found : nullptr;
}

std::filesystem::path State::DumpFeatureDefaults() const
{
	json document = json::object();
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->HasPersonalSettings())
			if (const auto* defaults = GetFeatureDefaults(feature->GetName()))
				document[feature->GetName()] = *defaults;
	}
	const auto path = Util::PathHelpers::GetCommunityShaderPath() / "FeatureDefaults.json";
	return Util::FileHelpers::WriteJsonAtomically(path, document, 2, "feature defaults") ? path : std::filesystem::path{};
}

const json* State::GetFeatureBase(std::string_view featureName) const
{
	const auto found = featureSettingsBases.find(featureName);
	return found != featureSettingsBases.end() && found->is_object() ? &*found : nullptr;
}

void State::PreparePresetSettingsSave(json& settings) const
{
	Util::Settings::RestoreSettings(settings, json::object(), baseSettingsInvalidations);
}

void State::MarkPresetSettingsSaved(const json& settings)
{
	CaptureSavedSettingsFields(settings);
	if (!savedSettingsSnapshot.is_null())
		CheckSettingsChanged(baseSettingsInvalidations);
	baseSettingsInvalidations = json::object();
}

json State::BuildSettingsSnapshot()
{
	json current;
	SaveToJson(current);
	// These menu preferences are saved independently of the global Save action.
	for (const auto path : SELF_SAVING_SETTINGS) {
		const json::json_pointer pointer{ std::string(path) };
		if (current.contains(pointer))
			current.at(pointer.parent_pointer()).erase(pointer.back());
	}
	current["Menu"].erase("ShowSettingInformation");
	auto* overrides = SettingsOverrideManager::GetSingleton();
	const auto pending = overrides->GetPendingEdits(current);
	auto personal = current;

	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->HasPersonalSettings())
			continue;
		const auto name = feature->GetName();
		const auto* base = GetFeatureBase(name);
		if (!base)
			continue;
		const auto saved = userFeatureEdits.find(name);
		auto edits = Util::Settings::BuildUserOverride(personal[name], *base, saved != userFeatureEdits.end() ? *saved : json::object());
		if (edits.empty())
			personal.erase(name);
		else
			personal[name] = std::move(edits);
	}
	if (const auto base = featureSettingsBases.find("Disable at Boot"); base != featureSettingsBases.end()) {
		auto edits = Util::Settings::BuildUserOverride(personal["Disable at Boot"], *base, userBootPreferences);
		personal["Disable at Boot"] = std::move(edits);
	}
	personal.erase(UnifiedPresetCatalog::kUserSettingsKey);
	return { { "personal", std::move(personal) }, { "pending", pending } };
}

void State::MarkSettingsSaved(bool captureApplied)
{
	baselineOverlapCache.clear();
	userEditsCache = nullptr;
	try {
		if (captureApplied) {
			json applied;
			SaveToJson(applied);
			SettingsOverrideManager::GetSingleton()->CaptureAppliedSettings(applied);
		}
		savedSettingsSnapshot = BuildSettingsSnapshot();
		settingsChanged = false;
	} catch (const std::exception& e) {
		settingsChanged = true;
		logger::error("Could not capture saved settings: {}", e.what());
	}
}

void State::CheckSettingsChanged(const json& persistedFields)
{
	baselineOverlapCache.clear();
	userEditsCache = nullptr;
	try {
		if (savedSettingsSnapshot.is_null())
			return;
		const auto current = BuildSettingsSnapshot();
		if (!persistedFields.empty())
			for (auto& [layer, snapshot] : savedSettingsSnapshot.items()) {
				const auto persisted = Util::Settings::SelectSettings(current.at(layer), persistedFields);
				Util::Settings::RestoreSettings(snapshot, persisted, persistedFields);
			}
		settingsChanged = current != savedSettingsSnapshot;
	} catch (const std::exception& e) {
		settingsChanged = true;
		logger::error("Could not check unsaved settings: {}", e.what());
	}
}

json State::GetUnsavedSettingsChanges()
{
	return savedSettingsSnapshot.is_null() ? json::array() : json::diff(savedSettingsSnapshot, BuildSettingsSnapshot());
}

bool State::ReapplyPresetLayers()
{
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	json previous;
	SaveToJson(previous);
	auto personal = previous;
	auto* overrides = SettingsOverrideManager::GetSingleton();
	auto pending = overrides->GetPendingEdits(previous);

	auto next = personal;
	auto nextBases = featureSettingsBases;
	auto* baselines = BaselineManager::GetSingleton();
	baselines->Refresh();
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto previousState = catalog.SerializeActiveState().value(PresetState::kBaseSettingsCustomizationsKey, json::object());
	json active = json::object();
	for (auto* feature : Feature::GetFeatureList())
		if (feature->UsesMainSettings())
			active[feature->GetName()] = baselines->BuildFeatureBaseline(feature->GetShortName(), defaultSettingsBaseline[feature->GetName()]);
	active["Disable at Boot"] = BuildPresetBootSettings();
	const auto reconciled = BaseSettingsCustomization::Reconcile(previousState, baselines->BuildCustomizationSources(), active);
	Util::Settings::RestoreSettings(personal, json::object(), reconciled.removed);
	Util::Settings::RestoreSettings(pending, json::object(), reconciled.removed);
	auto retainedEdits = userFeatureEdits;
	retainedEdits["Disable at Boot"] = userBootPreferences;
	Util::Settings::RestoreSettings(retainedEdits, json::object(), reconciled.removed);
	auto workingCustomizations = reconciled.state;
	Util::Settings::RestoreSettings(workingCustomizations["edits"], json::object(), pendingBaseSettingsResets);
	const auto topEdits = BaseSettingsCustomization::UserLayer(retainedEdits, workingCustomizations);
	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->loaded || !feature->UsesMainSettings())
			continue;
		const auto name = feature->GetName();
		const auto oldBase = featureSettingsBases.find(name);
		const auto defaults = defaultSettingsBaseline.find(name);
		if (oldBase == featureSettingsBases.end() || defaults == defaultSettingsBaseline.end())
			continue;
		auto base = *defaults;
		base.update(baselines->BuildFeatureBaseline(feature->GetShortName(), base), true);
		auto layered = base;
		auto edits = topEdits.value(name, json::object());
		Util::Settings::MergeSettings(edits, Util::Settings::BuildUserOverride(personal[name], *oldBase));
		Util::Settings::RestoreSettings(edits, json::object(), active[name]);
		Util::Settings::MergeSettings(edits, workingCustomizations.at("edits").value(name, json::object()));
		layered.update(edits, true);
		next[name] = std::move(layered);
		nextBases[name] = std::move(base);
	}
	auto bootBase = defaultSettingsBaseline["Disable at Boot"];
	bootBase.update(active["Disable at Boot"], true);
	auto bootEdits = topEdits.value("Disable at Boot", json::object());
	nextBases["Disable at Boot"] = bootBase;
	bootBase.update(bootEdits, true);
	next["Disable at Boot"] = std::move(bootBase);
	const auto underlying = next;
	const auto appliedLayers = next;
	next.update(pending, true);
	json applied;
	try {
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->loaded || !feature->UsesMainSettings())
				continue;
			const auto name = feature->GetName();
			auto values = next[name];
			feature->LoadSettings(values);
		}
		SaveToJson(applied);
		applied["Disable at Boot"] = next["Disable at Boot"];
		Util::Settings::RestoreSettings(applied, appliedLayers, pending);
	} catch (const std::exception& e) {
		logger::error("Could not apply preset Baselines: {}", e.what());
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->loaded || !feature->UsesMainSettings())
				continue;
			try {
				feature->LoadSettings(previous[feature->GetName()]);
			} catch (...) {
				logger::error("Could not restore {} after a rejected preset", feature->GetShortName());
			}
		}
		return false;
	}
	featureSettingsBases = std::move(nextBases);
	bool bootChanged = false;
	for (auto* feature : Feature::GetFeatureList()) {
		const auto name = feature->GetShortName();
		if (!feature->IsAlwaysEnabled() && next["Disable at Boot"].contains(name)) {
			const bool disabled = next["Disable at Boot"][name].get<bool>();
			bootChanged |= IsFeatureDisabled(name) != disabled;
			SetFeatureDisabled(name, disabled);
		}
	}
	if (bootChanged)
		globals::shaderCache->MarkExpectedFeatureFlip();
	overrides->CaptureBaseSettings(underlying);
	overrides->CaptureAppliedSettings(applied);
	userFeatureEdits = topEdits;
	userBootPreferences = std::move(bootEdits);
	baseSettingsInvalidations = reconciled.removed;
	catalog.SetBaseSettingsCustomizations(reconciled.state);
	baselineOverlapCache.clear();
	userEditsCache = nullptr;
	return true;
}

void State::ReapplySettingsLayers()
{
	if (!ReapplyPresetLayers())
		throw std::runtime_error("Could not apply preset settings layers");
}

void State::InvalidateEditsUnderBaselines(Feature* feature)
{
	++userEditsRevision;
	if (feature)
		baselineOverlapCache.erase(feature->GetName());
	else
		baselineOverlapCache.clear();
	userEditsCache = nullptr;
}

void State::RestoreFeatureLayers(Feature* feature, bool currentPage)
{
	if (!feature || !feature->loaded || !feature->HasPersonalSettings())
		return;
	if (auto* manager = SceneSettingsManager::GetSingleton(); manager && manager->IsContextEditing(feature->GetShortName()))
		manager->DiscardContextEdit();
	SceneSettingsManager::SceneLayerGuard guard;
	json previous;
	feature->SaveSettings(previous);
	try {
		if (currentPage)
			feature->RestoreCurrentPageDefaultSettings();
		else
			feature->RestoreDefaultSettings();
		if (const auto* defaults = GetFeatureDefaults(feature->GetName())) {
			auto values = *defaults;
			if (currentPage && feature->HasScopedDefaultSettings()) {
				values = previous;
				values.update(feature->SelectCurrentPageSettings(*defaults));
			}
			feature->LoadSettings(values);
		}
		if (currentPage && feature->HasScopedDefaultSettings()) {
			if (auto edits = userFeatureEdits.find(feature->GetName()); edits != userFeatureEdits.end() && edits->is_object())
				for (const auto& key : feature->GetCurrentPageSettingKeys())
					edits->erase(key);
		} else
			userFeatureEdits[feature->GetName()] = json::object();
	} catch (...) {
		feature->LoadSettings(previous);
		throw;
	}
	InvalidateEditsUnderBaselines(feature);
	CheckSettingsChanged();
}

State::SavedFeatureSettings State::ReadSavedFeatureSettings(Feature& feature) const
{
	const auto name = feature.GetName();
	const auto shortName = feature.GetShortName();
	const auto* base = GetFeatureBase(name);
	if (!base)
		throw std::runtime_error("Feature has no captured settings base");
	json userSettings = json::object();
	const auto configPath = GetConfigPath(ConfigMode::USER);
	std::error_code error;
	const bool exists = std::filesystem::exists(configPath, error);
	if (error)
		throw std::runtime_error("Could not inspect the saved settings file");
	if (exists) {
		std::ifstream input(configPath);
		if (!input.is_open())
			throw std::runtime_error("Could not open the saved settings file");
		input >> userSettings;
		if (!userSettings.is_object())
			throw std::runtime_error("Saved settings must be a JSON object");
	}
	Feature::MigrateConfigSettings(userSettings);
	const auto currentState = UnifiedPresetCatalog::GetSingleton().SerializeActiveState().value(PresetState::kBaseSettingsCustomizationsKey, json::object());
	const auto savedState = BaseSettingsCustomization::Reconcile(
		userSettings.value(UnifiedPresetCatalog::kUserSettingsKey, json::object()).value(PresetState::kBaseSettingsCustomizationsKey, json::object()),
		currentState.value("sources", json::object()), currentState.value("active", json::object()));
	userSettings = BaseSettingsCustomization::UserLayer(std::move(userSettings),
		savedState.state);
	json settings{ { name, *base } };
	Util::Settings::OverlayRecognizedRootSettings(settings, userSettings);
	json savedEdits = json::object();
	if (const auto saved = userSettings.find(name); saved != userSettings.end()) {
		if (!saved->is_object())
			throw std::runtime_error("Saved feature settings must be a JSON object");
		const auto baselineMask = BaselineManager::GetSingleton()->BuildFeatureBaseline(shortName, *base);
		const auto presetState = userSettings.value(UnifiedPresetCatalog::kUserSettingsKey, json::object());
		savedEdits = PrepareUserFeatureSettings(settings[name], *saved, baselineMask,
			PresetState::HasNormalizedFeatureEdits(presetState, name));
	}
	const auto underlying = settings.at(name);
	if (!settings.at(name).is_object())
		throw std::runtime_error("Saved feature layers must be a JSON object");
	std::vector<std::string> invalidSettings;
	Util::Settings::EraseMismatchedSettings(settings[name], *base, "", invalidSettings);
	if (!invalidSettings.empty())
		throw std::runtime_error(std::format("Saved feature layers have an invalid value for '{}'", invalidSettings.front()));

	return { std::move(settings.at(name)), underlying, std::move(savedEdits) };
}

bool State::CopyFeatureSettingsToUser(Feature* feature, const json& settings)
{
	if (!feature || !feature->loaded || !feature->HasPersonalSettings() || !savedSettingsSnapshot.is_object() || !settings.is_object() || settings.empty() ||
		SceneSettingsManager::GetSingleton()->HasPendingContextEdit())
		return false;
	SceneSettingsManager::SceneLayerGuard guard;
	const auto name = feature->GetName();
	auto* overrides = SettingsOverrideManager::GetSingleton();
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	json previous;
	feature->SaveSettings(previous);
	const auto previousBase = overrides->GetBaseSettings();
	const auto previousApplied = overrides->GetAppliedSettings();
	const auto previousEdits = userFeatureEdits;
	const auto previousSnapshot = savedSettingsSnapshot;
	const json previousPresets{ { UnifiedPresetCatalog::kUserSettingsKey, catalog.SerializeActiveState() } };
	const bool previousChanged = settingsChanged;
	try {
		std::vector<std::string> invalid;
		Util::Settings::CollectUnknownSettingKeys(settings, previous, "", invalid);
		if (!invalid.empty())
			throw std::runtime_error(std::format("Invalid copied setting '{}'", invalid.front()));
		auto saved = ReadSavedFeatureSettings(*feature);
		const auto configPath = GetConfigPath(ConfigMode::USER);
		json config = json::object();
		if (std::filesystem::exists(configPath)) {
			std::ifstream input(configPath);
			input >> config;
			if (!config.is_object())
				throw std::runtime_error("Saved settings must be a JSON object");
		}
		auto values = previous;
		Util::Settings::MergeSettings(values, settings);
		feature->LoadSettings(values);
		json normalized;
		feature->SaveSettings(normalized);
		const auto copied = Util::Settings::SelectSettings(normalized, settings);
		Util::Settings::MergeSettings(saved.personalEdits, copied);
		config[name] = std::move(saved.personalEdits);
		auto presetState = catalog.SerializeActiveState();
		PresetState::MarkNormalizedFeatureEdits(presetState, name);
		auto customizations = presetState.value(PresetState::kBaseSettingsCustomizationsKey, json::object());
		Util::Settings::MergeSettings(customizations["edits"], Util::Settings::SelectSettings(json{ { name, copied } }, customizations.value("active", json::object())));
		presetState[PresetState::kBaseSettingsCustomizationsKey] = std::move(customizations);
		config[UnifiedPresetCatalog::kUserSettingsKey] = std::move(presetState);
		Util::Settings::MergeSettings(userFeatureEdits[name], copied);
		auto nextBase = previousBase;
		auto nextApplied = previousApplied;
		Util::Settings::MergeSettings(nextBase[name], copied);
		Util::Settings::MergeSettings(nextApplied[name], copied);
		overrides->CaptureBaseSettings(nextBase);
		overrides->CaptureAppliedSettings(nextApplied);
		const auto current = BuildSettingsSnapshot();
		for (const char* layer : { "personal", "pending" }) {
			auto& savedLayer = savedSettingsSnapshot[layer];
			auto savedFeature = savedLayer.value(name, json::object());
			Util::Settings::RestoreSettings(savedFeature, current.at(layer).value(name, json::object()), copied);
			if (savedFeature.empty())
				savedLayer.erase(name);
			else
				savedLayer[name] = std::move(savedFeature);
		}
		settingsChanged = current != savedSettingsSnapshot;
		catalog.LoadActiveState(config);
		const auto destination = std::filesystem::exists(configPath) ? Util::FileHelpers::ResolveExistingFile(configPath) : std::filesystem::path(configPath);
		if (destination.empty() || !PresetPreferences::Save(config.at(UnifiedPresetCatalog::kUserSettingsKey), [&] { return Util::FileHelpers::WriteJsonAtomically(destination, config, 1, "copied personal settings"); }))
			throw std::runtime_error("Could not save copied personal settings");
		CaptureSavedSettingsFields(config);
		Util::Settings::RestoreSettings(pendingBaseSettingsResets[name], json::object(), copied);
		InvalidateEditsUnderBaselines(feature);
		return true;
	} catch (const std::exception& error) {
		overrides->CaptureBaseSettings(previousBase);
		overrides->CaptureAppliedSettings(previousApplied);
		userFeatureEdits = previousEdits;
		savedSettingsSnapshot = previousSnapshot;
		settingsChanged = previousChanged;
		catalog.LoadActiveState(previousPresets);
		try {
			feature->LoadSettings(previous);
		} catch (const std::exception& restoreError) {
			logger::error("Could not restore {} after a failed preset copy: {}", name, restoreError.what());
		}
		InvalidateEditsUnderBaselines(feature);
		logger::error("Could not copy preset settings for {}: {}", name, error.what());
		return false;
	}
}

bool State::RestoreSavedFeatureSettings(Feature* feature, bool currentPage)
{
	if (!feature || !feature->loaded || !feature->HasPersonalSettings() || !savedSettingsSnapshot.is_object())
		return false;
	const auto name = feature->GetName();
	const auto shortName = feature->GetShortName();
	const bool scoped = currentPage && feature->HasScopedDefaultSettings();
	const auto keys = scoped ? feature->GetCurrentPageSettingKeys() : std::vector<std::string>{};
	const auto replaceScope = [&](json& target, const json& source) {
		if (!scoped) {
			target = source;
			return;
		}
		if (!target.is_object())
			target = json::object();
		for (const auto& key : keys) {
			target.erase(key);
			if (const auto value = source.find(key); value != source.end())
				target[key] = *value;
		}
	};
	if (auto* manager = SceneSettingsManager::GetSingleton(); manager && manager->IsContextEditing(shortName) && manager->HasPendingContextEdit()) {
		SceneSettingsUI::WarnPendingContextEdit();
		return false;
	}
	auto* overrides = SettingsOverrideManager::GetSingleton();
	try {
		auto saved = ReadSavedFeatureSettings(*feature);

		if (auto* manager = SceneSettingsManager::GetSingleton(); manager && manager->IsContextEditing(shortName))
			manager->DiscardContextEdit();
		SceneSettingsManager::SceneLayerGuard guard;
		json previous;
		feature->SaveSettings(previous);
		const auto previousBase = overrides->GetBaseSettings();
		const auto previousApplied = overrides->GetAppliedSettings();
		const auto previousEdits = userFeatureEdits;
		const auto previousSnapshot = savedSettingsSnapshot;
		const bool previousChanged = settingsChanged;
		try {
			auto restored = previous;
			replaceScope(restored, saved.values);
			if (!scoped)
				feature->RestoreDefaultSettings();
			feature->LoadSettings(restored);
			json normalized;
			feature->SaveSettings(normalized);
			auto nextBase = previousBase;
			replaceScope(nextBase[name], saved.underlying);
			auto nextApplied = previousApplied;
			nextApplied[name] = std::move(normalized);
			overrides->CaptureBaseSettings(nextBase);
			overrides->CaptureAppliedSettings(nextApplied);
			replaceScope(userFeatureEdits[name], saved.personalEdits);
			const auto currentSnapshot = BuildSettingsSnapshot();
			for (const char* layer : { "personal", "pending" }) {
				auto& savedLayer = savedSettingsSnapshot[layer];
				const auto restoredLayer = currentSnapshot.at(layer).value(name, json::object());
				replaceScope(savedLayer[name], restoredLayer);
				if (savedLayer[name].empty())
					savedLayer.erase(name);
			}
			settingsChanged = currentSnapshot != savedSettingsSnapshot;
			InvalidateEditsUnderBaselines(feature);
			replaceScope(pendingBaseSettingsResets[name], json::object());
			return true;
		} catch (...) {
			overrides->CaptureBaseSettings(previousBase);
			overrides->CaptureAppliedSettings(previousApplied);
			userFeatureEdits = previousEdits;
			savedSettingsSnapshot = previousSnapshot;
			settingsChanged = previousChanged;
			try {
				feature->LoadSettings(previous);
			} catch (const std::exception& restoreError) {
				logger::error("Could not roll back {} after rejected saved settings: {}", shortName, restoreError.what());
			}
			throw;
		}
	} catch (const std::exception& error) {
		logger::error("Could not restore saved settings for {}: {}", shortName, error.what());
		return false;
	} catch (...) {
		logger::error("Could not restore saved settings for {} after an unknown exception", shortName);
		return false;
	}
}

void State::CaptureSavedSettingsFields(const json& document)
{
	hasSavedUserSettings = document.is_object() && !document.empty();
	savedFeatureFields.clear();
	for (auto* feature : Feature::GetFeatureList()) {
		const auto name = feature->GetName();
		const auto found = document.find(name);
		if (found == document.end() || !found->is_object())
			continue;
		for (const auto& [key, value] : found->items())
			savedFeatureFields[name].push_back(key);
	}
}

bool State::HasSavedSettings(Feature* feature, bool currentPage) const
{
	if (!feature)
		return hasSavedUserSettings;
	const auto found = savedFeatureFields.find(feature->GetName());
	if (found == savedFeatureFields.end() || found->second.empty())
		return false;
	if (!currentPage || !feature->HasScopedDefaultSettings())
		return true;
	const auto keys = feature->GetCurrentPageSettingKeys();
	return std::ranges::any_of(keys, [&](const auto& key) { return std::ranges::find(found->second, key) != found->second.end(); });
}

bool State::ReapplyBaseSettings()
{
	if (!DropPresetEdits({}, nullptr))
		return false;
	const auto boot = BuildPresetBootSettings();
	Util::Settings::RestoreSettings(userBootPreferences, json::object(), boot);
	Util::Settings::MergeSettings(pendingBaseSettingsResets["Disable at Boot"], boot);
	for (const auto& [name, disabled] : boot.items())
		SetFeatureDisabled(name, disabled.get<bool>());
	CheckSettingsChanged();
	return true;
}

std::vector<Feature*> State::FindEditsUnderPack(const std::string& packId)
{
	std::vector<Feature*> result;
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	json current;
	SaveToJson(current);
	auto personal = current;
	auto* overrides = SettingsOverrideManager::GetSingleton();

	const auto pending = overrides->GetPendingEdits(current);
	auto* baselines = BaselineManager::GetSingleton();
	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->loaded || !feature->UsesMainSettings())
			continue;
		const auto name = feature->GetName();
		const auto* base = GetFeatureBase(name);
		if (!base)
			continue;
		const auto mask = baselines->BuildFeatureBaseline(feature->GetShortName(), *base, packId);
		const auto saved = userFeatureEdits.find(name);
		auto edits = Util::Settings::BuildUserOverride(personal[name], *base, saved != userFeatureEdits.end() ? *saved : json::object());
		if (const auto live = pending.find(name); live != pending.end())
			edits.update(*live, true);
		if (!Util::Settings::SelectSettings(edits, mask).empty())
			result.push_back(feature);
	}
	return result;
}

bool State::HasEditsUnderBaselines(Feature* feature)
{
	if (!feature)
		return false;
	const auto revision = UnifiedPresetCatalog::GetSingleton().GetRevision();
	if (baselineOverlapCatalogRevision != revision) {
		baselineOverlapCache.clear();
		userEditsCache = nullptr;
		baselineOverlapCatalogRevision = revision;
	}
	if (baselineOverlapCache.empty()) {
		for (auto* candidate : Feature::GetFeatureList())
			baselineOverlapCache[candidate->GetName()] = false;
		try {
			for (auto* candidate : FindEditsUnderPack({}))
				baselineOverlapCache[candidate->GetName()] = true;
		} catch (const std::exception& e) {
			logger::error("Could not check personal edits under Baselines: {}", e.what());
		}
	}
	return baselineOverlapCache[feature->GetName()];
}

bool State::ClearEditsUnderPack(const std::string& packId)
{
	return UnifiedPresetCatalog::GetSingleton().IsBaselineEnabled(packId) && DropPresetEdits(packId, nullptr);
}

bool State::ClearFeatureEditsUnderBaselines(Feature* feature, std::span<const std::string_view> keys)
{
	return feature && feature->loaded && feature->UsesMainSettings() && DropPresetEdits({}, feature, keys);
}

bool State::DropPresetEdits(const std::string& packId, Feature* onlyFeature, std::span<const std::string_view> keys)
{
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	json previous;
	SaveToJson(previous);
	auto* overrides = SettingsOverrideManager::GetSingleton();
	auto underlying = previous;

	auto pending = overrides->GetPendingEdits(previous);
	auto savedEdits = userFeatureEdits;
	auto resets = pendingBaseSettingsResets;
	std::vector<Feature*> changed;
	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->loaded || !feature->UsesMainSettings() || (onlyFeature && onlyFeature != feature))
			continue;
		const auto name = feature->GetName();
		const auto* base = GetFeatureBase(name);
		if (!base)
			continue;
		auto mask = BaselineManager::GetSingleton()->BuildFeatureBaseline(feature->GetShortName(), *base, packId);
		if (!keys.empty()) {
			for (auto item = mask.begin(); item != mask.end();)
				if (std::ranges::find(keys, std::string_view(item.key())) == keys.end())
					item = mask.erase(item);
				else
					++item;
		}
		if (mask.empty())
			continue;
		Util::Settings::RestoreSettings(underlying[name], *base, mask);
		if (auto edits = pending.find(name); edits != pending.end())
			Util::Settings::RestoreSettings(*edits, json::object(), mask);
		if (auto edits = savedEdits.find(name); edits != savedEdits.end())
			Util::Settings::RestoreSettings(*edits, json::object(), mask);
		Util::Settings::MergeSettings(resets[name], mask);
		changed.push_back(feature);
	}
	if (changed.empty())
		return true;
	auto next = underlying;
	const auto appliedLayers = next;
	next.update(pending, true);
	json applied;
	try {
		for (auto* feature : changed)
			feature->LoadSettings(next[feature->GetName()]);
		SaveToJson(applied);
		Util::Settings::RestoreSettings(applied, appliedLayers, pending);
	} catch (const std::exception& e) {
		logger::error("Could not restore preset values: {}", e.what());
		for (auto* feature : changed) {
			try {
				feature->LoadSettings(previous[feature->GetName()]);
			} catch (...) {
				logger::error("Could not restore {} after rejected preset values", feature->GetShortName());
			}
		}
		return false;
	}
	pendingBaseSettingsResets = std::move(resets);
	userFeatureEdits = std::move(savedEdits);
	overrides->CaptureBaseSettings(underlying);
	overrides->CaptureAppliedSettings(applied);
	CheckSettingsChanged();
	return true;
}

void State::SaveCoreToJson(json& settings)
{
	const auto shaderCache = globals::shaderCache;

	globals::menu->Save(settings["Menu"]);

	json advanced;
	advanced["Dump Shaders"] = shaderCache->IsDump();
	advanced["Log Level"] = logLevel;
	advanced["Developer Mode"] = enableDeveloperMode;
	advanced["Shader Defines"] = GetDefinesString();
	advanced["Compiler Threads"] = shaderCache->compilationThreadCount;
	advanced["Background Compiler Threads"] = shaderCache->backgroundCompilationThreadCount;
	advanced["Use FileWatcher"] = shaderCache->UseFileWatcher();
	advanced["Frame Annotations"] = frameAnnotations;
	advanced["Tracy Per-Draw Zones"] = TracyZones::PerDrawZonesEnabled();
	advanced["Partial Precision"] = enablePartialPrecision.load(std::memory_order_relaxed);
	advanced["Content Store"] = enableContentStore.load(std::memory_order_relaxed);
	advanced["Content Store Max MB"] = contentStoreMaxMB.load(std::memory_order_relaxed);
	advanced["Refraction Scale"] = refractionScale;
	settings["Advanced"] = advanced;

	json general;
	general["Enable Shaders"] = shaderCache->IsEnabled();
	general["Enable Disk Cache"] = shaderCache->IsDiskCache();
	general["Skip Unchanged Shaders"] = shaderCache->IsSkipUnchangedShaders();
	general["Enable Async"] = shaderCache->IsAsync();
	general["Language"] = I18n::GetSingleton()->GetCurrentLocale();
	PostProcessingMode::Save(general);

	settings["General"] = general;

	auto& upscaling = globals::features::upscaling;
	auto& upscalingJson = settings[upscaling.GetShortName()];
	upscaling.SaveSettings(upscalingJson);

	json originalShaders;
	ForEachShaderTypeWithIndex([&](auto type, int classIndex) {
		originalShaders[magic_enum::enum_name(type)] = enabledClasses[classIndex];
	});
	settings["Replace Original Shaders"] = originalShaders;

	json disabledFeaturesJson;
	for (const auto& [featureName, isDisabled] : disabledFeatures) {
		disabledFeaturesJson[featureName] = isDisabled;
	}
	settings["Disable at Boot"] = disabledFeaturesJson;
	settings["Favorites"] = favoriteFeatures;

	settings["Version"] = Plugin::VERSION.string();
}

json State::CaptureCoreSettings()
{
	json settings;
	std::lock_guard<std::mutex> lock(m_mutex);
	SaveCoreToJson(settings);
	return settings;
}

void State::SaveToJson(nlohmann::json& settings)
{
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	std::lock_guard<std::mutex> lock(m_mutex);
	SaveCoreToJson(settings);

	// Save feature settings
	for (auto* feature : Feature::GetFeatureList()) {
		const std::string featureSettingsName = feature->GetName();
		if (feature->loaded || !settings.contains(featureSettingsName) || !settings[featureSettingsName].is_object()) {
			feature->Save(settings);
		}
	}
}

void State::LoadFromJson(nlohmann::json& settings)
{
	Feature::MigrateConfigSettings(settings);
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto shaderCache = globals::shaderCache;

	disabledFeatures.clear();
	if (const auto disabled = settings.find("Disable at Boot"); disabled != settings.end() && disabled->is_object()) {
		for (const auto& [featureName, featureStatus] : disabled->items()) {
			if (featureStatus.is_boolean())
				disabledFeatures[featureName] = featureStatus.get<bool>();
			else
				logger::warn("Invalid entry for feature '{}' in 'Disable at Boot', expected boolean.", featureName);
		}
	}
	for (auto* feature : Feature::GetFeatureList()) {
		const auto featureName = feature->GetShortName();
		if (feature->IsAlwaysEnabled())
			disabledFeatures.erase(featureName);
		else if (!disabledFeatures.contains(featureName) && feature->IsDisabledByDefault())
			disabledFeatures[featureName] = true;
	}

	favoriteFeatures.clear();
	if (const auto favorites = settings.find("Favorites"); favorites != settings.end() && favorites->is_object()) {
		for (const auto& [featureName, favorite] : favorites->items()) {
			if (favorite.is_boolean())
				favoriteFeatures[featureName] = favorite.get<bool>();
		}
	}

	// Load Menu settings
	if (settings.contains("Menu") && settings["Menu"].is_object()) {
		globals::menu->Load(settings["Menu"]);
	}

	if (settings.contains("Advanced") && settings["Advanced"].is_object()) {
		json& advanced = settings["Advanced"];
		if (advanced.contains("Dump Shaders") && advanced["Dump Shaders"].is_boolean())
			shaderCache->SetDump(advanced["Dump Shaders"]);
		if (advanced.contains("Log Level") && advanced["Log Level"].is_number_integer()) {
			const auto rawLogLevel = advanced["Log Level"].get<int64_t>();
			const auto newLogLevel = (rawLogLevel >= 0 && rawLogLevel <= static_cast<int64_t>(spdlog::level::off)) ?
			                             magic_enum::enum_cast<spdlog::level::level_enum>(static_cast<int>(rawLogLevel)).value_or(spdlog::level::info) :
			                             spdlog::level::info;
			if (newLogLevel != logLevel)
				SetLogLevel(newLogLevel);
		}
		if (advanced.contains("Developer Mode") && advanced["Developer Mode"].is_boolean())
			enableDeveloperMode = advanced["Developer Mode"];
		if (advanced.contains("Shader Defines") && advanced["Shader Defines"].is_string())
			SetDefines(advanced["Shader Defines"]);
		if (advanced.contains("Compiler Threads") && advanced["Compiler Threads"].is_number_integer())
			shaderCache->compilationThreadCount = std::clamp(advanced["Compiler Threads"].get<int32_t>(), 1, static_cast<int32_t>(std::thread::hardware_concurrency()));
		if (advanced.contains("Background Compiler Threads") && advanced["Background Compiler Threads"].is_number_integer())
			shaderCache->backgroundCompilationThreadCount = std::clamp(advanced["Background Compiler Threads"].get<int32_t>(), 1, static_cast<int32_t>(std::thread::hardware_concurrency()));
		if (advanced.contains("Use FileWatcher") && advanced["Use FileWatcher"].is_boolean())
			shaderCache->SetFileWatcher(advanced["Use FileWatcher"]);
		if (advanced.contains("Frame Annotations") && advanced["Frame Annotations"].is_boolean())
			frameAnnotations = advanced["Frame Annotations"];
		if (advanced.contains("Tracy Per-Draw Zones") && advanced["Tracy Per-Draw Zones"].is_boolean())
			TracyZones::SetPerDrawZones(advanced["Tracy Per-Draw Zones"]);
		if (advanced.contains("Partial Precision") && advanced["Partial Precision"].is_boolean())
			enablePartialPrecision.store(advanced["Partial Precision"].get<bool>(), std::memory_order_relaxed);
		if (advanced.contains("Content Store") && advanced["Content Store"].is_boolean())
			enableContentStore.store(advanced["Content Store"].get<bool>(), std::memory_order_relaxed);
		if (advanced.contains("Content Store Max MB") && advanced["Content Store Max MB"].is_number_unsigned())
			contentStoreMaxMB.store(static_cast<uint32_t>(std::clamp<uint64_t>(advanced["Content Store Max MB"].get<uint64_t>(), kContentStoreMinMB, kContentStoreMaxMB)), std::memory_order_relaxed);
		if (advanced.contains("Refraction Scale") && advanced["Refraction Scale"].is_number())
			refractionScale = std::clamp(advanced["Refraction Scale"].get<float>(), 0.0f, 2.0f);
	}

	if (settings.contains("General") && settings["General"].is_object()) {
		json& general = settings["General"];
		PostProcessingMode::Load(general);
		if (general.contains("Enable Shaders") && general["Enable Shaders"].is_boolean())
			shaderCache->SetEnabled(general["Enable Shaders"]);
		if (general.contains("Enable Disk Cache") && general["Enable Disk Cache"].is_boolean())
			shaderCache->SetDiskCache(general["Enable Disk Cache"]);
		if (general.contains("Skip Unchanged Shaders") && general["Skip Unchanged Shaders"].is_boolean())
			shaderCache->SetSkipUnchangedShaders(general["Skip Unchanged Shaders"]);
		if (general.contains("Enable Async") && general["Enable Async"].is_boolean())
			shaderCache->SetAsync(general["Enable Async"]);

		// Load i18n locale preference
		if (general.contains("Language") && general["Language"].is_string()) {
			auto locale = general["Language"].get<std::string>();
			auto* i18n = I18n::GetSingleton();
			if (locale != i18n->GetCurrentLocale()) {
				i18n->SetLocale(locale);
			}
		} else {
			// No saved language preference — auto-detect from system locale on first launch
			auto* i18n = I18n::GetSingleton();
			auto detected = i18n->DetectSystemLocale();
			if (detected != "en" && detected != i18n->GetCurrentLocale()) {
				i18n->SetLocale(detected);
				logger::info("[I18n] Auto-detected system locale: '{}'", detected);
			}
		}
	}

	if (settings.contains("Replace Original Shaders") && settings["Replace Original Shaders"].is_object()) {
		json& originalShaders = settings["Replace Original Shaders"];
		ForEachShaderTypeWithIndex([&](auto type, int classIndex) {
			auto name = magic_enum::enum_name(type);
			if (originalShaders.contains(name) && originalShaders[name].is_boolean()) {
				enabledClasses[classIndex] = originalShaders[name];
			} else {
				logger::warn("Invalid entry for shader class '{}', using current value", name);
			}
		});
	}

	// Load feature settings (only for already-loaded features)
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			feature->Load(settings);
		}
	}
	InvalidateEditsUnderBaselines();
}

void State::Save(ConfigMode a_configMode, bool a_isExplicitUserSave)
{
	std::string configPath = GetConfigPath(a_configMode);

	try {
		std::filesystem::create_directories(Util::PathHelpers::GetCommunityShaderPath());
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("Error creating directory during Save ({}) : {}\n", Util::PathHelpers::GetCommunityShaderPath().string(), e.what());
		return;
	}

	json settings = json::object();
	std::ifstream existingConfig{ configPath };
	if (existingConfig.is_open()) {
		try {
			json existingSettings;
			existingConfig >> existingSettings;
			for (auto* feature : Feature::GetFeatureList()) {
				const std::string featureSettingsName = feature->GetName();
				if (!feature->loaded && existingSettings.contains(featureSettingsName) && existingSettings[featureSettingsName].is_object()) {
					settings[featureSettingsName] = existingSettings[featureSettingsName];
				}
			}
		} catch (const nlohmann::json::exception& e) {
			logger::warn("Failed to preserve inactive feature settings from {}: {}", configPath, e.what());
		}
		existingConfig.close();
	}

	std::string serializedSettings;
	json userBaseSettings;
	auto* overrideManager = SettingsOverrideManager::GetSingleton();
	try {
		SaveToJson(settings);
		if (a_configMode == ConfigMode::USER) {
			userBaseSettings = settings;
			auto presetState = UnifiedPresetCatalog::GetSingleton().SerializeActiveState();
			Util::Settings::RestoreSettings(presetState[PresetState::kBaseSettingsCustomizationsKey]["edits"], json::object(), pendingBaseSettingsResets);
			auto customizationValues = settings;
			for (auto* feature : Feature::GetFeatureList())
				if (!feature->loaded)
					customizationValues.erase(feature->GetName());
			presetState[PresetState::kBaseSettingsCustomizationsKey] = BaseSettingsCustomization::SaveEdits(
				presetState.value(PresetState::kBaseSettingsCustomizationsKey, json::object()), customizationValues, overrideManager->GetAppliedSettings());
			presetState[PresetState::kBootPreferencesVersionKey] = PresetState::kBootPreferencesVersion;
			for (auto* feature : Feature::GetFeatureList()) {
				const auto name = feature->GetName();
				const auto base = featureSettingsBases.find(name);
				if (!feature->loaded || !feature->HasPersonalSettings() || base == featureSettingsBases.end())
					continue;
				const auto saved = userFeatureEdits.find(name);
				settings[name] = Util::Settings::BuildUserOverride(settings[name], *base, saved != userFeatureEdits.end() ? *saved : json::object());
				if (settings[name].empty())
					settings.erase(name);
				PresetState::MarkNormalizedFeatureEdits(presetState, name);
			}
			if (const auto bootBase = featureSettingsBases.find("Disable at Boot"); bootBase != featureSettingsBases.end())
				settings["Disable at Boot"] = Util::Settings::BuildUserOverride(settings["Disable at Boot"], *bootBase, userBootPreferences);
			settings = BaseSettingsCustomization::UserLayer(std::move(settings), presetState.at(PresetState::kBaseSettingsCustomizationsKey));
			settings[UnifiedPresetCatalog::kUserSettingsKey] = std::move(presetState);
		}
		serializedSettings = settings.dump(1);
	} catch (const std::exception& e) {
		logger::warn("Failed to serialize settings for {}: {}", configPath, e.what());
		return;
	}

	const auto saveConfig = [&] { return WriteConfigAtomically(configPath, serializedSettings); };
	if (!(a_configMode == ConfigMode::USER ? PresetPreferences::Save(settings.at(UnifiedPresetCatalog::kUserSettingsKey), saveConfig) : saveConfig())) {
		logger::error("Could not save settings");
		return;
	}
	logger::info("Saving settings to {}", configPath);

	// A real user save is the only signal that a Disable-at-Boot change (restart-
	// gated) was actually intentional; record it so next boot's disk-cache mismatch
	// can auto-resolve instead of holding for the "Rebuild Cache" menu action.
	if (a_configMode == ConfigMode::USER) {
		UnifiedPresetCatalog::GetSingleton().LoadActiveState(settings);
		pendingBaseSettingsResets = json::object();
		CaptureSavedSettingsFields(settings);
		overrideManager->CaptureBaseSettings(userBaseSettings);
		overrideManager->CaptureAppliedSettings(userBaseSettings);
		userFeatureEdits = json::object();
		userBootPreferences = settings.value("Disable at Boot", json::object());
		for (auto* feature : Feature::GetFeatureList()) {
			const auto name = feature->GetName();
			if (const auto saved = settings.find(name); saved != settings.end() && saved->is_object())
				userFeatureEdits[name] = *saved;
		}
		if (auto* shaderCache = globals::shaderCache)
			shaderCache->MarkExpectedFeatureFlip();

#if defined(ENABLE_EFFECTS11)
		// Effects11's ENB state lives in enbseries.ini, not the Settings JSON -- persist
		// it here too, not just via MenuManager's own "Save"/"Save & Apply" buttons.
		if (a_isExplicitUserSave && EffectManager::GetSingleton().IsPresetLoaded()) {
			SettingManager::GetSingleton().Save();
			EffectManager::GetSingleton().Save();
		}
#endif
	}
	if (a_configMode == ConfigMode::USER)
		MarkSettingsSaved();
}

bool State::SaveCoreSettings(std::span<const std::string_view> paths)
{
	const auto configPath = GetConfigPath(ConfigMode::USER);
	std::error_code ec;
	const bool fileExists = std::filesystem::exists(configPath, ec);
	if (ec)
		return false;
	const auto saved = fileExists ? Util::FileHelpers::ReadJsonObject(configPath, "user config") : std::optional<json>(json::object());
	if (!saved)
		return false;
	const auto current = CaptureCoreSettings();
	json patch = json::object();
	for (const auto path : paths) {
		const json::json_pointer pointer{ std::string(path) };
		if (current.contains(pointer) && (!saved->contains(pointer) || saved->at(pointer) != current.at(pointer)))
			patch[pointer] = current.at(pointer);
	}
	return patch.empty() || SaveFeaturePreference(patch, false);
}

bool State::SaveFeaturePreference(const json& patch, bool trackInSnapshot)
{
	const auto configPath = GetConfigPath(ConfigMode::USER);
	try {
		json settings = json::object();
		if (std::filesystem::exists(configPath)) {
			std::ifstream input(configPath);
			input >> settings;
			if (!settings.is_object())
				return false;
		}
		if (patch.contains("Disable at Boot")) {
			const auto& catalog = UnifiedPresetCatalog::GetSingleton();
			auto presetState = catalog.SerializeActiveState();
			if (presetState.value(PresetState::kBootPreferencesVersionKey, json{}) != PresetState::kBootPreferencesVersion) {
				settings["Disable at Boot"] = userBootPreferences;
				presetState[PresetState::kBootPreferencesVersionKey] = PresetState::kBootPreferencesVersion;
				settings[UnifiedPresetCatalog::kUserSettingsKey] = std::move(presetState);
			}
		}
		settings.merge_patch(patch);
		auto presetState = UnifiedPresetCatalog::GetSingleton().SerializeActiveState();
		presetState[PresetState::kBaseSettingsCustomizationsKey] = BaseSettingsCustomization::SaveEdits(
			presetState.value(PresetState::kBaseSettingsCustomizationsKey, json::object()), patch,
			SettingsOverrideManager::GetSingleton()->GetAppliedSettings());
		settings = BaseSettingsCustomization::UserLayer(std::move(settings), presetState.at(PresetState::kBaseSettingsCustomizationsKey));
		settings[UnifiedPresetCatalog::kUserSettingsKey] = std::move(presetState);
		std::filesystem::create_directories(Util::PathHelpers::GetCommunityShaderPath());

		const auto serializedSettings = settings.dump(1);
		if (!PresetPreferences::Save(settings.at(UnifiedPresetCatalog::kUserSettingsKey), [&] { return WriteConfigAtomically(configPath, serializedSettings); }))
			return false;
		CaptureSavedSettingsFields(settings);
		UnifiedPresetCatalog::GetSingleton().LoadActiveState(settings);
		if (trackInSnapshot && !savedSettingsSnapshot.is_null()) {
			savedSettingsSnapshot["personal"].merge_patch(patch);
			Util::Settings::RestoreSettings(savedSettingsSnapshot["pending"], json::object(), patch);
		}
		return true;
	} catch (const std::exception& e) {
		logger::error("Could not save feature preference to {}: {}", configPath, e.what());
		return false;
	}
}

bool State::SetFeatureBootEnabled(const std::string& featureName, bool enabled)
{
	if (!SaveFeaturePreference(json{ { "Disable at Boot", { { featureName, !enabled } } } }))
		return false;
	userBootPreferences[featureName] = !enabled;
	SetFeatureDisabled(featureName, !enabled);
	globals::shaderCache->MarkExpectedFeatureFlip();
	return true;
}

bool State::ResetFeatureBootPreference(const std::string& featureName)
{
	const auto* bootBase = GetFeatureBase("Disable at Boot");
	if (!bootBase || !bootBase->contains(featureName) ||
		!SaveFeaturePreference(json{ { "Disable at Boot", { { featureName, nullptr } } } }))
		return false;
	userBootPreferences.erase(featureName);
	const bool disabled = bootBase->at(featureName).get<bool>();
	SetFeatureDisabled(featureName, disabled);
	auto* overrides = SettingsOverrideManager::GetSingleton();
	auto applied = overrides->GetAppliedSettings();
	applied["Disable at Boot"][featureName] = disabled;
	overrides->CaptureAppliedSettings(applied);
	CheckSettingsChanged(json{ { "Disable at Boot", { { featureName, true } } } });
	globals::shaderCache->MarkExpectedFeatureFlip();
	return true;
}

bool State::SetFeatureFavorite(const std::string& featureName, bool favorite)
{
	const auto* feature = Feature::FindFeatureByShortName(featureName);
	if (!feature || !feature->IsInMenu() || feature == &globals::features::csEditor)
		return false;
	if (!SaveFeaturePreference(json{ { "Favorites", { { featureName, favorite } } } }))
		return false;
	favoriteFeatures[featureName] = favorite;
	return true;
}

bool State::SetFeatureListSort(bool alphabetical)
{
	const auto sort = alphabetical ? Menu::FeatureSortMode::Alphabetical : Menu::FeatureSortMode::Category;
	if (globals::menu->GetSettings().FeatureListSort == sort)
		return true;
	if (!SaveFeaturePreference(json{ { "Menu", { { "FeatureListSort", sort } } } }, false))
		return false;
	globals::menu->GetSettings().FeatureListSort = sort;
	return true;
}

bool State::IsFeatureFavorite(const std::string& featureName) const
{
	const auto favorite = favoriteFeatures.find(featureName);
	return favorite != favoriteFeatures.end() && favorite->second;
}

bool State::SetSettingInformationVisible(bool visible)
{
	if (!SaveFeaturePreference(json{ { "Menu", { { "ShowSettingInformation", visible } } } }, false))
		return false;
	globals::menu->GetSettings().ShowSettingInformation = visible;
	return true;
}

bool State::ValidateCache(CSimpleIniA& a_ini)
{
	bool valid = true;
	for (auto* feature : Feature::GetFeatureList())
		valid = valid && feature->ValidateCache(a_ini);
	return valid;
}

void State::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	for (auto* feature : Feature::GetFeatureList())
		feature->WriteDiskCacheInfo(a_ini);
}

void State::SetLogLevel(spdlog::level::level_enum a_level)
{
	logLevel = a_level;
	spdlog::set_level(logLevel);
	spdlog::flush_on(logLevel);
	logger::info("Log Level set to {} ({})", magic_enum::enum_name(logLevel), magic_enum::enum_integer(logLevel));
}

spdlog::level::level_enum State::GetLogLevel()
{
	return logLevel;
}

void State::SetDefines(std::string a_defines)
{
	std::vector<std::pair<std::string, std::string>> parsedDefines;
	std::string definesString;
	auto defines = pystring::split(a_defines, ";");
	for (const auto& define : defines) {
		auto cleanedDefine = pystring::strip(define);
		auto token = pystring::split(cleanedDefine, "=");
		if (token.empty() || token[0].empty())
			continue;
		if (token.size() > 2) {
			logger::warn("Define string has too many '='; ignoring {}", define);
			continue;
		}
		std::string name = pystring::strip(token[0]);
		std::string definition = token.size() == 2 ? pystring::strip(token[1]) : std::string{};
		definesString += pystring::strip(define) + ";";
		parsedDefines.emplace_back(std::move(name), std::move(definition));
	}
	definesString = definesString.substr(0, definesString.size() - 1);
	logger::debug("Shader Defines set to {}", definesString);
	{
		std::scoped_lock lock(shaderDefinesMutex);
		shaderDefinesString = std::move(definesString);
		shaderDefines = std::make_shared<const std::vector<std::pair<std::string, std::string>>>(std::move(parsedDefines));
		shaderDefinesVersion.fetch_add(1, std::memory_order_release);
	}
}

std::string State::GetDefinesString()
{
	std::scoped_lock lock(shaderDefinesMutex);
	return shaderDefinesString;
}

std::shared_ptr<const std::vector<std::pair<std::string, std::string>>> State::GetDefines()
{
	std::scoped_lock lock(shaderDefinesMutex);
	return shaderDefines;
}

bool State::ShaderEnabled(const RE::BSShader::Type a_type)
{
	if (a_type > RE::BSShader::Type::None && a_type < RE::BSShader::Type::Total) {
		return enabledClasses[magic_enum::enum_integer(a_type) - 1];
	}
	return false;
}

bool State::IsShaderEnabled(const RE::BSShader& a_shader)
{
	return ShaderEnabled(a_shader.shaderType.get());
}

bool State::IsDeveloperMode()
{
	return enableDeveloperMode || GetLogLevel() <= spdlog::level::debug;
}

void State::ModifyRenderTarget(RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties& a_properties)
{
	a_properties.supportUnorderedAccess = true;
	logger::debug("Adding UAV access to {}", magic_enum::enum_name(a_target));
}

bool State::SupportsTypedUAVLoad(DXGI_FORMAT a_format)
{
	auto device = globals::d3d::device;
	if (!device)
		return false;
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{};
	support2.InFormat = a_format;
	if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2))))
		return false;
	return (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
}

void State::CheckTypedUAVLoadSupport()
{
	auto device = globals::d3d::device;
	if (!device) {
		logger::warn("[TypedUAVLoad] Device unavailable; skipping format support probe.");
		return;
	}

	// Formats this codebase does typed UAV loads on (RWTexture<T> read via subscript).
	// Identified by static analysis; keep in sync with new typed reads.
	// All require the optional D3D11 feature D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD —
	// guaranteed only for R32_FLOAT/R32_UINT/R32_SINT, otherwise gated by
	// D3D11_FEATURE_DATA_D3D11_OPTIONS2.TypedUAVLoadAdditionalFormats (FL12+).
	struct FormatEntry
	{
		DXGI_FORMAT format;
		const char* name;
		const char* usage;
	};
	static const FormatEntry kFormats[] = {
		{ DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", "Dynamic Cubemaps (envCapture/Raw/Position) - non-HDR" },
		{ DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", "Linear Lighting scene decode, Dynamic Cubemaps (HDR), Skylighting outProbeArray" },
		{ DXGI_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", "Grass Collision (collisionTexture)" },
		{ DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM", "Terrain Shadows (RWTexShadowHeights)" },
		{ DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT", "VR Stereo Blend (kMOTION_VECTOR reprojection)" },
		{ DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM", "VR Stereo Reprojection G-buffer fill (NormalRoughness, Albedo)" },
		{ DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM", "VR Stereo Reprojection G-buffer fill (Masks2)" },
		{ DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", "HDR Display UI brightness (uiTexture)" },
		{ DXGI_FORMAT_R16_UINT, "R16_UINT", "Skylighting packed probe state (outAccumFramesArray)" },
		{ DXGI_FORMAT_R16_FLOAT, "R16_FLOAT", "Vanilla volumetric lighting density (DensityRW)" },
	};

	bool anyUnsupported = false;
	logger::info("[TypedUAVLoad] Probing per-format UAV typed-load support:");
	for (const auto& entry : kFormats) {
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{};
		support2.InFormat = entry.format;
		HRESULT hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2));
		if (FAILED(hr)) {
			logger::warn("[TypedUAVLoad] {} ({}): CheckFeatureSupport failed (hr=0x{:08x})", entry.name, entry.usage, static_cast<uint32_t>(hr));
			anyUnsupported = true;
			continue;
		}
		const bool supported = (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
		if (supported) {
			logger::info("[TypedUAVLoad] {} — supported ({})", entry.name, entry.usage);
		} else {
			logger::warn("[TypedUAVLoad] {} — UNSUPPORTED ({})", entry.name, entry.usage);
			anyUnsupported = true;
		}
	}

	if (anyUnsupported) {
		logger::warn(
			"[TypedUAVLoad] One or more required formats lack typed-UAV-load support on this GPU. "
			"Affected features will read undefined data and may produce visual artifacts. "
			"Consider disabling: Dynamic Cubemaps, Grass Collision, Terrain Shadows, Skylighting, HDR Display, VR Stereo Optimisations.");
	}
}

void State::SetupResources()
{
	for (auto& c : drawCalls)
		c = 0;
	for (auto& c : smoothDrawCalls)
		c = 0;
	for (auto& ft : frameTimePerType)
		ft = 0.0f;
	for (auto& sft : smoothFrameTimePerType)
		sft = 0.0f;

	frameTimingActive = false;

	auto renderer = globals::game::renderer;

	permutationCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PermutationCB>(), "State::PermutationCB");
	sharedDataCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SharedDataCB>(), "State::SharedDataCB");

	auto [data, size] = GetFeatureBufferData(false);
	(void)data;
	featureDataCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc((uint32_t)size), "State::FeatureDataCB");

	// Grab main texture to get resolution
	// VR cannot use viewport->screenWidth/Height as it's the desktop preview window's resolution and not HMD
	D3D11_TEXTURE2D_DESC texDesc{};
	renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(Util::AsW32(&texDesc));

	screenSize = float2{ (float)texDesc.Width, (float)texDesc.Height };
	pPerf = nullptr;
	globals::d3d::context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), pPerf.put_void());

	featureLevel = globals::d3d::device->GetFeatureLevel();

	tracyCtx = TracyD3D11Context(globals::d3d::device, globals::d3d::context);
#ifdef TRACY_ENABLE
	Feature::SetTracyCtx(tracyCtx);
#endif

	globals::profiler->Initialize(globals::d3d::device, globals::d3d::context);

	// Checked per pass rather than bound once, so annotations forced on at runtime (RenderDoc capture, Nsight Perf)
	// also cover the profiler's passes. Toggles happen between frames, so a pass's begin and end agree.
	globals::profiler->SetPerfEventCallbacks(
		[this](std::string_view name) {
			if (frameAnnotations)
				BeginPerfEvent(name);
		},
		[this](std::string_view) {
			if (frameAnnotations)
				EndPerfEvent();
		});
}

void State::ModifyShaderLookup(const RE::BSShader& a_shader, uint& a_vertexDescriptor, uint& a_pixelDescriptor, bool a_forceDeferred)
{
	auto deferred = globals::deferred;

	if (a_shader.shaderType.get() != RE::BSShader::Type::Utility && a_shader.shaderType.get() != RE::BSShader::Type::ImageSpace) {
		switch (a_shader.shaderType.get()) {
		case RE::BSShader::Type::Lighting:
			{
				a_vertexDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::AdditionalAlphaMask |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::AmbientSpecular |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::DoAlphaTest |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::ShadowDir |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::DefShadow |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::CharacterLight |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::RimLighting |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::SoftLighting |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::BackLighting |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::Specular |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::AnisoLighting |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::BaseObjectIsSnow |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::Snow |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::TruePbr |
										(uint32_t)SIE::ShaderCache::LightingShaderFlags::LandscapeSeams);

				a_pixelDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::AmbientSpecular |
									   (uint32_t)SIE::ShaderCache::LightingShaderFlags::ShadowDir |
									   (uint32_t)SIE::ShaderCache::LightingShaderFlags::DefShadow |
									   (uint32_t)SIE::ShaderCache::LightingShaderFlags::CharacterLight |
									   (uint32_t)SIE::ShaderCache::LightingShaderFlags::BaseObjectIsSnow);

				{
					uint32_t technique = 0x3F & (a_pixelDescriptor >> 24);
					if (technique != (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODLand &&
						technique != (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODLandNoise &&
						technique != (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODObjects &&
						technique != (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODObjectHD)
						a_pixelDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::Specular |
											   (uint32_t)SIE::ShaderCache::LightingShaderFlags::SoftLighting |
											   (uint32_t)SIE::ShaderCache::LightingShaderFlags::RimLighting |
											   (uint32_t)SIE::ShaderCache::LightingShaderFlags::BackLighting);
				}

				if (a_pixelDescriptor & (uint32_t)SIE::ShaderCache::LightingShaderFlags::AdditionalAlphaMask) {
					a_pixelDescriptor |= (uint32_t)SIE::ShaderCache::LightingShaderFlags::DoAlphaTest;
					a_pixelDescriptor &= ~(uint32_t)SIE::ShaderCache::LightingShaderFlags::AdditionalAlphaMask;
				}

				a_pixelDescriptor &= ~((uint32_t)SIE::ShaderCache::LightingShaderFlags::Snow);

				if (deferred->deferredPass || a_forceDeferred)
					a_pixelDescriptor |= (uint32_t)SIE::ShaderCache::LightingShaderFlags::Deferred | deferredLightingShaderFlags;
				else if (permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(ExtraShaderDescriptors::IsReflections))
					a_pixelDescriptor |= deferredLightingShaderFlags;

				{
					uint32_t technique = 0x3F & (a_vertexDescriptor >> 24);
					if (technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Glowmap ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Parallax ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Facegen ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::FacegenRGBTint ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODObjects ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::LODObjectHD ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::MultiIndexSparkle ||
						technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Hair)
						a_vertexDescriptor &= ~(0x3F << 24);
				}

				{
					uint32_t technique = 0x3F & (a_pixelDescriptor >> 24);
					if (technique == (uint32_t)SIE::ShaderCache::LightingShaderTechniques::Glowmap)
						a_pixelDescriptor &= ~(0x3F << 24);
				}
			}
			break;
		case RE::BSShader::Type::Water:
			{
				auto flags = ~((uint32_t)SIE::ShaderCache::WaterShaderFlags::Reflections |
							   (uint32_t)SIE::ShaderCache::WaterShaderFlags::Cubemap |
							   (uint32_t)SIE::ShaderCache::WaterShaderFlags::Interior);
				a_vertexDescriptor &= flags;
				a_pixelDescriptor &= flags;
			}
			break;
		case RE::BSShader::Type::Effect:
			{
				auto flags = ~((uint32_t)SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor |
							   (uint32_t)SIE::ShaderCache::EffectShaderFlags::GrayscaleToAlpha |
							   (uint32_t)SIE::ShaderCache::EffectShaderFlags::IgnoreTexAlpha);
				a_vertexDescriptor &= flags;
				a_pixelDescriptor &= flags & ~(SIE::ShaderCache::kEffectLinearACEScg | SIE::ShaderCache::kEffectAmbientWorldIBL);
				if (effectAmbientIBLUploaded &&
					(permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(ExtraShaderDescriptors::InWorld)))
					a_pixelDescriptor |= SIE::ShaderCache::kEffectAmbientWorldIBL;
				if (deferredLightingShaderFlags & SIE::ShaderCache::kLightingLinearACEScg)
					a_pixelDescriptor |= SIE::ShaderCache::kEffectLinearACEScg;

				if (deferred->deferredPass || a_forceDeferred)
					a_pixelDescriptor |= (uint32_t)SIE::ShaderCache::EffectShaderFlags::Deferred;
			}
			break;
		case RE::BSShader::Type::DistantTree:
			{
				if (deferred->deferredPass || a_forceDeferred)
					a_pixelDescriptor |= (uint32_t)SIE::ShaderCache::DistantTreeShaderFlags::Deferred;
			}
			break;
		case RE::BSShader::Type::Sky:
			{
				if (deferred->deferredPass || a_forceDeferred)
					a_pixelDescriptor |= 256;
			}
			break;
		default:
			break;
		}
	} else if (a_shader.shaderType.get() == RE::BSShader::Type::Utility) {
		a_vertexDescriptor &= ~(uint32_t)SIE::ShaderCache::UtilityShaderFlags::TruePbr;
	}
}

uint32_t State::GetRuntimePixelBits(const RE::BSShader& a_shader)
{
	if (a_shader.shaderType.get() == RE::BSShader::Type::Lighting)
		return static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::BaseObjectIsSnow);

	// Water's pixel stage branches on Wading at runtime; losing the bit skips the wading displacement without any error.
	return a_shader.shaderType.get() == RE::BSShader::Type::Water ? static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::Wading) : 0u;
}

// Widens a narrow annotation title into a reused wide buffer (per-byte char->wchar_t,
// matching the engine's convention for ASCII annotation strings). resize()+copy reuses
// the buffer; constructing a fresh wstring per call heap-allocates, which at per-draw
// volume contends the process heap lock with the shader-compile worker threads.
static const wchar_t* WidenAnnotation(std::wstring& buffer, std::string_view title)
{
	buffer.resize(title.size());
	std::copy(title.begin(), title.end(), buffer.begin());
	return buffer.c_str();
}

void State::RefreshDrawAnnotations()
{
	drawAnnotationsActive = frameAnnotations &&
	                        ((pPerf && pPerf->GetStatus()) ||
								GetModuleHandleW(L"renderdoc.dll") ||
								GetModuleHandleW(L"WinPixGpuCapturer.dll"));
}

void State::BeginDrawEvent(std::string_view title)
{
	// Per-draw annotation (thousands per frame): emit ONLY the GPU-capture marker
	// (RenderDoc/PIX), never a Tracy zone. A dynamic-named Tracy zone allocs a
	// source location per call -- at per-draw volume that swamps Tracy. Coarse
	// per-pass markers still use BeginPerfEvent for Tracy.
	if (pPerf) {
		thread_local std::wstring s_wtitle;
		pPerf->BeginEvent(WidenAnnotation(s_wtitle, title));
	}
}

void State::EndDrawEvent()
{
	if (pPerf) {
		pPerf->EndEvent();
	}
}

bool State::PerfEventsConsumed() const
{
	if ((pPerf && pPerf->GetStatus()) || NvPerf::RangesActive())
		return true;
#ifdef TRACY_ENABLE
	return TracyIsConnected;
#else
	return false;
#endif
}

void State::BeginPerfEvent(std::string_view title)
{
	// Record whether this event was emitted, so EndPerfEvent stays balanced if a consumer attaches or leaves in between.
	const bool emit = PerfEventsConsumed();
	s_perfEventEmitted.push_back(emit);
	if (!emit)
		return;
#ifdef TRACY_ENABLE
	// Use dynamic source location so Tracy displays the title as the zone name
	// rather than the static function name "BeginPerfEvent".
	const auto srcloc = ___tracy_alloc_srcloc_name(
		static_cast<uint32_t>(__LINE__),
		__FILE__, sizeof(__FILE__) - 1,
		__func__, sizeof(__func__) - 1,
		title.data(), title.size(),
		0);
	const TracyCZoneCtx ctx = ___tracy_emit_zone_begin_alloc(srcloc, true);
	s_tracyPerfZones.push_back(ctx);
#endif
	BeginAnnotation(title);
}

void State::EndPerfEvent()
{
	if (s_perfEventEmitted.empty()) {
		logger::warn("EndPerfEvent called without a matching BeginPerfEvent");
		return;
	}
	const bool emitted = s_perfEventEmitted.back();
	s_perfEventEmitted.pop_back();
	if (!emitted)
		return;
#ifdef TRACY_ENABLE
	if (!s_tracyPerfZones.empty()) {
		TracyCZoneEnd(s_tracyPerfZones.back());
		s_tracyPerfZones.pop_back();
	} else {
		logger::warn("EndPerfEvent called without a matching BeginPerfEvent");
	}
#endif
	EndAnnotation();
}

void State::BeginAnnotation(std::string_view title)
{
	if (pPerf) {
		thread_local std::wstring s_wtitle;
		pPerf->BeginEvent(WidenAnnotation(s_wtitle, title));
	}
	if (NvPerf::RangesActive())
		NvPerf::PushRange(title);
}

void State::EndAnnotation()
{
	if (NvPerf::RangesActive())
		NvPerf::PopRange();
	if (pPerf) {
		pPerf->EndEvent();
	}
}

void State::SetPerfMarker(std::string_view title)
{
	if (pPerf && PerfEventsConsumed()) {
		thread_local std::wstring s_wmarker;
		pPerf->SetMarker(WidenAnnotation(s_wmarker, title));
	}
}

void State::SetAdapterDescription(const std::wstring& description)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	adapterDescription = converter.to_bytes(description);
}

void State::UpdateSharedData([[maybe_unused]] bool a_inWorld, [[maybe_unused]] bool a_prepass)
{
	{
		SharedDataCB data{};

		const auto shaderManager = globals::game::smState;
		const RE::NiTransform& dalcTransform = shaderManager->directionalAmbientTransform;

		auto shadowSceneNode = shaderManager->shadowSceneNode[0];
		auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get());

		auto& lightRuntimeData = dirLight->GetLightRuntimeData();
		data.DirLightColor = float4{ lightRuntimeData.diffuse.red, lightRuntimeData.diffuse.green, lightRuntimeData.diffuse.blue, 1.0f };
		data.DirLightColor *= lightRuntimeData.fade;

		if (auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton()) {
			data.DirLightColor *= imageSpaceManager->GetImageSpaceData().baseData.hdr.sunlightScale;
		}

		const auto& direction = dirLight->GetWorldDirection();
		data.DirLightDirection = float4{ -direction.x, -direction.y, -direction.z, 0.0f };
		data.DirLightDirection.Normalize();

		data.CameraData = Util::GetCameraData();
		data.BufferDim = float4{ screenSize.x, screenSize.y, 1.0f / screenSize.x, 1.0f / screenSize.y };
		data.Timer = timer;
		{
			const auto windData = globals::features::wind.GetSharedWindData();
			data.WindFieldTuning = windData.tuning;
			data.WindFieldAmbient = windData.ambient;
			data.WindFieldPreviousAmbient = windData.previousAmbient;
			data.WindFieldCurrent = windData.current;
			data.WindFieldPrevious = windData.previous;
			data.WindFieldTransition = windData.transition;
			data.WindFieldPreviousTransition = windData.previousTransition;
			data.WindFieldTransitionData = windData.transitionData;
			data.WindFieldSpringDebug = windData.springDebug;
			data.WindFieldActiveCounts = windData.activeCounts;
			data.WindFieldTransientImpulses = windData.transientImpulses;
			data.WindFieldPreviousTransientImpulses = windData.previousTransientImpulses;
		}

		auto temporal = Util::GetTemporal();

		data.FrameCount = frameCount * temporal;
		data.FrameCountAlwaysActive = frameCount;

		// Out-of-world refreshes (VR UI pass, upscaling) keep the last in-world tiles: zeroed tiles read as
		// water at camera height and the VR underwater mask then floods everything below the horizon.
		static std::array<float4, 25> waterTiles = [] {
			std::array<float4, 25> tiles;
			tiles.fill(float4(0.0f, 0.0f, 0.0f, -FLT_MAX));
			return tiles;
		}();
		if (a_inWorld) {
			for (int i = -2; i <= 2; i++) {
				for (int k = -2; k <= 2; k++) {
					int waterTile = (i + 2) + ((k + 2) * 5);
					waterTiles[waterTile] = Util::TryGetWaterData((float)i * 4096.0f, (float)k * 4096.0f);
				}
			}
		}
		std::copy(waterTiles.begin(), waterTiles.end(), data.WaterData);

		// Fallback water height for the VR analytical mask when tile 12 returns the sentinel.
		// Uses player->GetWaterHeight() (reads relevantWaterHeight from LOADED_REF_DATA) gated by
		// underwaterCount > 0 so it is only set when the player is actually in a water body.
		// Covers both interior water (where TES::GetWaterHeight returns -NI_INFINITY) and exterior
		// partial submersion.  Stored as eye-0 camera-relative Z to match WaterData[].w.
		data.WaterSystemHeight = -RE::NI_INFINITY;
		if (globals::game::isVR) {
			if (auto player = globals::game::player) {
				if (player->loadedData && player->loadedData->underwaterCount > 0) {
					float worldHeight = player->GetWaterHeight();
					if (worldHeight > -RE::NI_INFINITY) {
						auto eye0Pos = Util::GetEyePosition(0);
						data.WaterSystemHeight = worldHeight - eye0Pos.z;
					}
				}
			}
		}

		data.InInterior = Util::IsInterior();
		data.HasDirectionalShadows = HasDirectionalShadows();

		if (globals::game::sky)
			data.HideSky = globals::game::sky->flags.any(RE::Sky::Flags::kHideSky);
		else
			data.HideSky = false;

		data.InMapMenu = isMapMenuOpen;

		auto& upscaling = globals::features::upscaling;

		if (upscaling.loaded) {
			auto upscaleMethod = upscaling.GetUpscaleMethod();
			if (temporal && upscaleMethod != Upscaling::UpscaleMethod::kTAA) {
				const auto& perfMode = upscaling.perfMode;
				if (perfMode.IsHookActive()) {
					// PerfMode forces the DRS ratio to identity while rendering
					// sub-native, so bias off the latched eye dims instead.
					data.MipBias = std::log2f(
						static_cast<float>(perfMode.GetRenderEyeWidth()) /
						static_cast<float>(perfMode.GetDisplayEyeWidth()));
				} else {
					auto renderSize = Util::ConvertToDynamic(screenSize, true);
					data.MipBias = std::log2f(renderSize.x / screenSize.x);
				}
				if (upscaleMethod == Upscaling::UpscaleMethod::kDLSS)
					data.MipBias -= 1.0f;
			} else {
				data.MipBias = 0;
			}
		} else {
			data.MipBias = 0;
		}

		if (auto sky = globals::game::sky) {
			// Process sun
			if (auto sun = sky->sun; sun && sun->root && sky->root) {
				const auto sunDirection = Util::GetSunDirection();
				data.SunDirection = float4{ sunDirection.x, sunDirection.y, sunDirection.z, 0.0f };

				if (sun->sunBase) {
					if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get()))
						data.SunColor = float4{ prop->kBlendColor.red * prop->kBlendColor.alpha, prop->kBlendColor.green * prop->kBlendColor.alpha, prop->kBlendColor.blue * prop->kBlendColor.alpha, prop->kBlendColor.alpha };
				}
			}

			if (auto masser = sky->masser) {
				auto dir = Util::GetMoonDirection(masser);
				data.MasserDirection = float4{ dir.x, dir.y, dir.z, 0.0f };
				if (masser->root && !masser->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					data.MasserColor = Util::Moon::GetBlendColor(masser, Util::Moon::MasserBaseColor, globals::features::skySync.settings.NewMoonIntensity, globals::features::skySync.settings.CrescentMoonIntensity, globals::features::skySync.settings.FullMoonIntensity);
			}

			if (auto secunda = sky->secunda) {
				auto dir = Util::GetMoonDirection(secunda);
				data.SecundaDirection = float4{ dir.x, dir.y, dir.z, 0.0f };
				if (secunda->root && !secunda->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					data.SecundaColor = Util::Moon::GetBlendColor(secunda, Util::Moon::SecundaBaseColor, globals::features::skySync.settings.NewMoonIntensity, globals::features::skySync.settings.CrescentMoonIntensity, globals::features::skySync.settings.FullMoonIntensity);
			}
		}

		// DALC to SH
		const auto& m = dalcTransform.rotate;
		const auto& t = dalcTransform.translate;
		float3 dalcColors[6];
		dalcColors[0] = float3{ m.entry[0][0] + t.x, m.entry[1][0] + t.y, m.entry[2][0] + t.z };     // +X
		dalcColors[1] = float3{ -m.entry[0][0] + t.x, -m.entry[1][0] + t.y, -m.entry[2][0] + t.z };  // -X
		dalcColors[2] = float3{ m.entry[0][1] + t.x, m.entry[1][1] + t.y, m.entry[2][1] + t.z };     // +Y
		dalcColors[3] = float3{ -m.entry[0][1] + t.x, -m.entry[1][1] + t.y, -m.entry[2][1] + t.z };  // -Y
		dalcColors[4] = float3{ m.entry[0][2] + t.x, m.entry[1][2] + t.y, m.entry[2][2] + t.z };     // +Z
		dalcColors[5] = float3{ -m.entry[0][2] + t.x, -m.entry[1][2] + t.y, -m.entry[2][2] + t.z };  // -Z

		SphericalHarmonics::SH2Color dalcSH = SphericalHarmonics::DALCToSH(dalcColors);
		data.AmbientSHR = float4{ dalcSH.r.c0, dalcSH.r.c1[0], dalcSH.r.c1[1], dalcSH.r.c1[2] };
		data.AmbientSHG = float4{ dalcSH.g.c0, dalcSH.g.c1[0], dalcSH.g.c1[1], dalcSH.g.c1[2] };
		data.AmbientSHB = float4{ dalcSH.b.c0, dalcSH.b.c1[0], dalcSH.b.c1[1], dalcSH.b.c1[2] };

		data.HDRData = globals::features::hdrDisplay.GetSharedDataHDR();
		data.RefractionScale = refractionScale;

		// VR foveated shader detail (consumed by foveated SSR). Default to off; populate from the
		// active foveation region only when SSR foveation is enabled and SSR is actually running.
		data.VRFoveationData0 = float4{ FoveatedCommon::kCenterScaleMax, FoveatedCommon::kCenterFeather, 1.0f,
			FoveatedCommon::GetShaderMode(FoveatedCommon::DetailMode::Off) };
		data.VRFoveationCenterOffsets = float4{ 0.0f, 0.0f, 0.0f, 0.0f };
		if (globals::game::isVR) {
			const auto& vr = globals::features::vr;
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			const bool ssrFoveationEnabled = vr.loaded && vr.settings.EnableSSRFoveation &&
			                                 dynamicCubemaps.loaded && dynamicCubemaps.settings.EnabledSSR;
			if (ssrFoveationEnabled) {
				const auto profile = upscaling.foveatedRender.GetFoveationProfile();
				if (profile.available) {  // available already implies an active (non-full) coverage scale
					const float ssrMode = FoveatedCommon::GetShaderMode(FoveatedCommon::GetDetailMode(
						true, vr.settings.EnableSSRFoveationHardCutoff));
					data.VRFoveationData0 = float4{ profile.coverageScale, FoveatedCommon::kCenterFeather,
						profile.centerHorizontalScale, ssrMode };
					data.VRFoveationCenterOffsets = float4{
						profile.centerOffsets[0].x, profile.centerOffsets[0].y,
						profile.centerOffsets[1].x, profile.centerOffsets[1].y
					};
				}
			}
		}

		sharedDataCB->Update(data);
	}

	UpdateFeatureData(a_inWorld);

	VanillaBinding::BindSceneDepth();
}

void State::UpdateFeatureData(bool a_inWorld)
{
	auto [data, size] = GetFeatureBufferData(a_inWorld);
	featureDataCB->Update(data, size);
}

void State::ClearDisabledFeatures()
{
	disabledFeatures.clear();
}

bool State::SetFeatureDisabled(const std::string& featureName, bool isDisabled)
{
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->GetShortName() == featureName && feature->IsAlwaysEnabled()) {
			disabledFeatures.erase(featureName);
			return false;
		}
	}
	bool wasPreviouslyDisabled = disabledFeatures.count(featureName) > 0 ? disabledFeatures[featureName] : false;  // Properly check if it exists
	disabledFeatures[featureName] = isDisabled;

	// Log the change
	if (wasPreviouslyDisabled != isDisabled) {
		logger::info("Set feature '{}' to: {}", featureName, isDisabled ? "Disabled" : "Enabled");
	} else {
		logger::info("Feature '{}' state remains: {}", featureName, isDisabled ? "Disabled" : "Enabled");
	}

	return disabledFeatures[featureName];  // Return the current state instead of the input parameter
}

bool State::IsFeatureDisabled(const std::string& featureName)
{
	for (auto* feature : Feature::GetFeatureList())
		if (feature->GetShortName() == featureName && feature->IsAlwaysEnabled())
			return false;
	return disabledFeatures.contains(featureName) && disabledFeatures[featureName];
}

std::unordered_map<std::string, bool>& State::GetDisabledFeatures()
{
	return disabledFeatures;
}

// --- Utility Method Implementations ---

float State::GetTotalSmoothedDrawCalls() const
{
	return static_cast<float>(smoothDrawCalls[magic_enum::enum_integer(RE::BSShader::Type::Total)]);
}

void State::LoadTheme()
{
	// Load the active preset from SettingsUser.json (already read during State::Load)
	auto presetName = globals::menu->GetSettings().SelectedThemePreset;
	if (presetName.empty()) {
		logger::info("No active theme preset set; skipping preset load");
		return;
	}

	// Ensure default themes exist and theme manager has discovered themes
	globals::menu->CreateDefaultThemes();
	auto themeManager = ThemeManager::GetSingleton();
	if (themeManager && !themeManager->IsDiscovered()) {
		themeManager->DiscoverThemes();
	}

	logger::info("Loading active theme preset: '{}'", presetName);
	if (!globals::menu->LoadThemePreset(presetName)) {
		logger::warn("Failed to load preset '{}', attempting to fall back to 'Default'", presetName);
		if (globals::menu->LoadThemePreset("Default")) {
			globals::menu->GetSettings().SelectedThemePreset = "Default";
			logger::info("Fallback to 'Default' theme succeeded");
		} else {
			logger::warn("Fallback to 'Default' theme failed");
		}
	}
}

bool State::HasDirectionalShadows() const
{
	return !Util::IsInterior() || globals::features::interiorSun.IsActiveInteriorSun();
}

void State::PrimeUserEdits()
{
	if (!userEditsCache.is_null())
		return;
	const auto snapshot = BuildSettingsSnapshot();
	userEditsCache = snapshot.at("personal");
	userEditsCache.update(snapshot.at("pending"), true);
}

json State::GetUserEdits(Feature* feature)
{
	if (!feature || !feature->loaded || !feature->UsesMainSettings())
		return json::object();
	PrimeUserEdits();
	const auto found = userEditsCache.find(feature->GetName());
	return found == userEditsCache.end() ? json::object() : *found;
}

bool State::HasUserEdits(Feature* feature)
{
	return !GetUserEdits(feature).empty();
}
