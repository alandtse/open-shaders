#include "DX12NativeScene.h"
#include "Globals.h"
#include "TruePBR.h"
#include <BS_thread_pool.hpp>
#include "Features/NativeComposite/NativeComposite.h"
#include "GpuPass.h"
#include "State.h"
#include "ShaderCache.h"
#include "Utils/DevBenchUx.h"
#include "Utils/D3D.h"
#include <cmath>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12NativeScene::Settings, Enabled, ShadowMask, BudgetMB, FrameBudgetMB, ShadowAllLights, ShadowStochastic, ShadowDistanceFade, ShadowLightIndex, ShadowNormalInfluence, ShadowEmitterRadius, ShadowReachMultiplier, ShadowFalloffFraction, ShadowFalloffExponent, ShadowTraceDensityMode)

void DX12NativeScene::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	if (ImGui::Button("Analyze live PBR candidates")) coverageRequested = true;
	{
		std::scoped_lock coverageLock(coverageMutex);
		if (coverageFailed) ImGui::TextDisabled("Candidate analysis unavailable or failed.");
		if (coverageTask) ImGui::TextDisabled("Analyzing live PBR candidates...");
		if (coverageResult) {
			const auto scene = GetPublishedScene();
			ImGui::TextDisabled("Unverified snapshot candidates%s: %zu unique, %zu ambiguous, %zu unobserved, %zu without source identity",
				!scene || scene->revision != coverageRevision ? " (previous scene)" : "", coverageResult->uniqueCandidates,
				coverageResult->ambiguous, coverageResult->noObservation, coverageResult->missingIdentity);
			if (coverageResult->recipeGenerationAvailable)
				ImGui::TextDisabled("Candidate recipes at revision %llu: %zu available, %zu pending or unavailable",
					static_cast<unsigned long long>(coverageResult->recipeRevision), coverageResult->candidateRecipes, coverageResult->candidateRecipesMissing);
		}
	}
	if (ImGui::Checkbox("Load prepared scene", &settings.Enabled)) resetRequested = true;
	if (ImGui::Checkbox("Produce RT shadow mask", &settings.ShadowMask)) shadowResetRequested = true;
	if (settings.ShadowMask && !shadowStatus.empty()) ImGui::TextUnformatted(shadowStatus.c_str());
	if (settings.ShadowMask) ImGui::TextDisabled("Opaque-frame shadow recoveries: %llu / %llu",
		static_cast<unsigned long long>(shadowOpaqueRetryRecovered),
		static_cast<unsigned long long>(shadowOpaqueRetryAttempts));
	if (ImGui::CollapsingHeader("Early DX12 shadow-mask controls")) {
		bool changed = false;
		ImGui::TextDisabled("This panel owns raw shadow visibility; its final-image contribution is independent in Native Composite.");
		int traceDensity = int(settings.ShadowTraceDensityMode);
		if (ImGui::Combo("Independent shadow trace density", &traceDensity, "Full\0Two-thirds\0Half\0One-third\0Quarter\0One-sixth\0One-ninth\0Four-ninths\0")) {
			settings.ShadowTraceDensityMode = uint32_t(traceDensity);
			changed = true;
		}
		ImGui::TextDisabled("All modes trace at render size; skipped pixels have validity -1.");
		changed |= ImGui::Checkbox("Shadow all prepared point lights", &settings.ShadowAllLights);
		if (!settings.ShadowAllLights) {
			const auto scene = GetPublishedScene();
			int index = int(settings.ShadowLightIndex);
			const int maxIndex = scene && !scene->cell->nativeLights.empty() ? int(scene->cell->nativeLights.size()-1) : 0;
			if (ImGui::SliderInt("Shadow point light index", &index, 0, maxIndex)) { settings.ShadowLightIndex = uint32_t(index); changed = true; }
		}
		changed |= ImGui::Checkbox("Stochastic shadow emitter sampling", &settings.ShadowStochastic);
		ImGui::BeginDisabled(!settings.ShadowStochastic);
		changed |= ImGui::SliderFloat("Shadow emitter radius multiplier", &settings.ShadowEmitterRadius, 0.0f, 8.0f);
		ImGui::EndDisabled();
		changed |= ImGui::SliderFloat("Shadow light reach multiplier", &settings.ShadowReachMultiplier, 0.1f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		changed |= ImGui::SliderFloat("Shadow fade range fraction", &settings.ShadowFalloffFraction, 0.02f, 1.0f);
		changed |= ImGui::SliderFloat("Shadow falloff curve", &settings.ShadowFalloffExponent, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		changed |= ImGui::Checkbox("Fade shadows near light reach", &settings.ShadowDistanceFade);
		int normal = int(settings.ShadowNormalInfluence);
		if (ImGui::Combo("Shadow normal influence", &normal, "Legacy flat\0Geometry normals\0Full material normals\0"))
			{ settings.ShadowNormalInfluence = uint32_t(normal); changed = true; }
		if (ImGui::Button("Reset shadow reach / falloff")) {
			const Settings defaults{};
			settings.ShadowReachMultiplier = defaults.ShadowReachMultiplier;
			settings.ShadowFalloffFraction = defaults.ShadowFalloffFraction;
			settings.ShadowFalloffExponent = defaults.ShadowFalloffExponent;
			settings.ShadowDistanceFade = defaults.ShadowDistanceFade;
			changed = true;
		}
		if (changed) ++shadowControlsRevision;
	}
	ImGui::TextUnformatted(failure.empty() ? "Prepared static scenes" : failure.c_str());
	const uint32_t requestedFrame = requestedFrameBudget.load();
	uint32_t frameLimit = requestedFrame != UINT32_MAX ? requestedFrame : settings.FrameBudgetMB;
	bool automatic = frameLimit == 0;
	if (ImGui::Checkbox("Automatic native frame VRAM budget (DXGI)", &automatic)) {
		frameLimit = automatic ? 0u : 512u;
		requestedFrameBudget = frameLimit;
	}
	if (!automatic) {
		int manual = int(frameLimit);
		if (ImGui::SliderInt("Native frame-pool limit (MiB)", &manual, 64, 4096))
			requestedFrameBudget = uint32_t(manual);
	}
	ImGui::TextDisabled("Pool allocations respect DXGI headroom; independent of the prepared-scene budget.");
}

void DX12NativeScene::LoadSettings(json& a_json)
{
	const Settings selected = a_json;
	// A compositor requesting the independent layer takes precedence over the
	// scene configuration's possibly older disabled producer state.
	if (requestedEnabled.load() != 1) requestedEnabled = selected.Enabled ? 1 : 0;
	if (requestedShadow.load() != 1) requestedShadow = selected.ShadowMask ? 1 : 0;
	requestedBudget = std::clamp(selected.BudgetMB, 64u, 4096u);
	requestedFrameBudget = selected.FrameBudgetMB ? std::clamp(selected.FrameBudgetMB, 64u, 4096u) : 0u;
	std::scoped_lock lock(settingsMutex);
	const auto clamp = [](float value, float fallback, float low, float high) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; };
	++shadowControlsRevision;
	settings.ShadowAllLights = selected.ShadowAllLights;
	settings.ShadowStochastic = selected.ShadowStochastic;
	settings.ShadowDistanceFade = selected.ShadowDistanceFade;
	settings.ShadowLightIndex = selected.ShadowLightIndex;
	settings.ShadowNormalInfluence = std::min(selected.ShadowNormalInfluence, 2u);
	settings.ShadowTraceDensityMode = std::min(selected.ShadowTraceDensityMode, 7u);
	settings.ShadowEmitterRadius = clamp(selected.ShadowEmitterRadius, Settings{}.ShadowEmitterRadius, 0.0f, 8.0f);
	settings.ShadowReachMultiplier = clamp(selected.ShadowReachMultiplier, Settings{}.ShadowReachMultiplier, 0.1f, 64.0f);
	settings.ShadowFalloffFraction = clamp(selected.ShadowFalloffFraction, Settings{}.ShadowFalloffFraction, 0.02f, 1.0f);
	settings.ShadowFalloffExponent = clamp(selected.ShadowFalloffExponent, Settings{}.ShadowFalloffExponent, 0.25f, 4.0f);
	resetRequested = true;
}
void DX12NativeScene::SaveSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	auto selected = settings;
	if (const auto enabled = requestedEnabled.load(); enabled >= 0) selected.Enabled = enabled != 0;
	if (const auto shadowEnabled = requestedShadow.load(); shadowEnabled >= 0) selected.ShadowMask = shadowEnabled != 0;
	if (const auto budget = requestedBudget.load(); budget) selected.BudgetMB = budget;
	if (const auto frameBudget = requestedFrameBudget.load(); frameBudget != UINT32_MAX) selected.FrameBudgetMB = frameBudget;
	a_json = selected;
}
void DX12NativeScene::RestoreDefaultSettings()
{
	requestedEnabled = Settings{}.Enabled ? 1 : 0;
	requestedShadow = Settings{}.ShadowMask ? 1 : 0;
	requestedBudget = Settings{}.BudgetMB;
	requestedFrameBudget = Settings{}.FrameBudgetMB;
	{ std::scoped_lock lock(settingsMutex); settings = Settings{}; ++shadowControlsRevision; }
	resetRequested = true;
}
void DX12NativeScene::OnRuntimeDisabled() { resetRequested = true; }
void DX12NativeScene::OnSceneTransitionReset(bool a_open)
{
	transitionOpen = a_open;
	if (a_open) globals::features::truePBR.nativeRegistry.ClearInstanceHints();
	ResetScene();
}
void DX12NativeScene::GameLoaded() { resetRequested = true; }

void DX12NativeScene::ResetScene()
{
	++revision;
	cellID = 0;
	submitted = false;
	raster.Reset();
	shadow.Reset();
	if (auto old = materialBindings.Get()) retiredMaterials = old;
	materialBindings.Reset();
	prepared.Reset();
	acceleration.Reset();
}

struct DX12NativeScene::CoverageTask
{
	std::atomic<bool> done{ false };
	uint64_t sceneRevision = 0;
	NativePBR::InstanceCoverage coverage;
	bool failed = false;
};

void DX12NativeScene::AdvanceCoverage()
{
	std::scoped_lock lock(coverageMutex);
	if (coverageTask && coverageTask->done.load(std::memory_order_acquire)) {
		coverageFailed = coverageTask->failed;
		if (!coverageFailed) { coverageResult = coverageTask->coverage; coverageRevision = coverageTask->sceneRevision; }
		coverageTask.reset();
	}
	if (coverageTask || !coverageRequested.exchange(false)) return;
	const auto scene = GetPublishedScene();
	if (!scene) { coverageFailed = true; return; }
	try {
		auto task = std::make_shared<CoverageTask>();
		task->sceneRevision = scene->revision;
		auto& pbr = globals::features::truePBR;
		auto recipes = pbr.GetNativeRecipes();
		auto hints = pbr.nativeRegistry.ExportInstanceHints();
		if (recipes && recipes->revision != pbr.nativeRegistry.Revision()) recipes.reset();
		static BS::thread_pool<> pool{ 1 };
		pool.detach_task([task, acceleration = scene->acceleration, hints = std::move(hints), recipes = std::move(recipes)] {
			try {
				std::vector<NativePBR::PreparedInstanceKey> keys;
				keys.reserve(acceleration->instances.size());
				for (const auto& instance : acceleration->instances)
					keys.push_back({ instance.preparedIdentity.referenceKey, instance.preparedIdentity.shapeName });
				task->coverage = NativePBR::AnalyzeInstanceCoverage(keys, hints, recipes.get());
			} catch (...) { task->failed = true; }
			task->done.store(true, std::memory_order_release);
		});
		coverageFailed = false;
		coverageTask = std::move(task);
	} catch (const std::exception& error) { coverageFailed = true; logger::warn("Native PBR candidate analysis: {}", error.what()); }
}

void DX12NativeScene::EarlyPrepass()
{
	CS_GPU_PASS("DX12NativeScene::PreparedSceneProgress");
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	AdvanceCoverage();
	DX12Native::RetireMaterialUploads();
	{
		std::scoped_lock lock(settingsMutex);
		if (const auto enabled = requestedEnabled.exchange(-1); enabled >= 0) settings.Enabled = enabled != 0;
		if (const auto shadowEnabled = requestedShadow.exchange(-1); shadowEnabled >= 0) settings.ShadowMask = shadowEnabled != 0;
		if (const auto budget = requestedBudget.exchange(0); budget) settings.BudgetMB = budget;
		if (const auto frameBudget = requestedFrameBudget.exchange(UINT32_MAX); frameBudget != UINT32_MAX) settings.FrameBudgetMB = frameBudget;
	}
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	const auto currentID = cell ? cell->GetFormID() : 0;
	if (reloadRequested.exchange(false)) {
		ResetScene();
		raster.ReloadShader();
		shadow.ReloadShaders();
	}
	if (resetRequested.exchange(false)) ResetScene();
	if (shadowResetRequested.exchange(false)) shadow.Reset();
	if (!settings.Enabled || transitionOpen) {
		if (cellID) ResetScene();
		return;
	}
	if (!backend) {
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, failure);
		if (!backend) return;
	}
	if (!cell || !cell->IsInteriorCell()) {
		if (cellID) ResetScene();
		return;
	}
	if (cellID != currentID) {
		ResetScene();
		cellID = currentID;
		failure.clear();
	}
	if (!prepared.Requested()) {
		const auto* source = cell->GetFile(0);
		if (!source) { failure = "Cell source unavailable"; return; }
		const auto local = cell->GetLocalFormID();
		const auto key = std::format("{:06X}:{}", local, source->GetFilename());
		const auto directory = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells") / source->GetFilename() / std::format("{:06X}", local);
		const auto budget = uint64_t(settings.BudgetMB) * 1024 * 1024;
		prepared.Request(*backend, directory, key, revision, budget, budget);
	}
	if (!submitted) {
		if (auto scene = prepared.GetCell()) {
			acceleration.Start(*backend, scene->snapshot, scene->geometries, uint64_t(settings.BudgetMB) * 1024 * 1024);
			submitted = true;
		}
	}
	acceleration.Advance();
	if (auto scene = acceleration.GetScene(); scene && shadow.RetireScenesExcept(scene) && retiredMaterials.expired())
		materialBindings.Request(*backend, scene, uint64_t(settings.BudgetMB) * 1024 * 1024);
	materialBindings.Advance();
	DispatchShadow("early-prepass");
}

void DX12NativeScene::DispatchShadow(const char* a_stage)
{
	Settings shadowSettings;
	{ std::scoped_lock lock(settingsMutex); shadowSettings = settings; }
	if (!shadowSettings.Enabled || !shadowSettings.ShadowMask || transitionOpen || globals::game::isVR || !backend || !globals::game::graphicsState ||
		!globals::shaderCache || !globals::shaderCache->IsEnabled() ||
		!globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) {
		shadowStatus = "Shadow mask unavailable for this render scope";
		return;
	}
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (shadow.Get(frame)) return;
	shadowStage = a_stage;
	if (!globals::game::frameBufferCachedValid || globals::game::frameBufferCachedFrame != frame) {
		shadowStatus = "Waiting for current-frame camera";
		return;
	}
	const auto& camera = globals::game::frameBufferCached.nonVR;
	const auto* worldCamera = RE::Main::WorldRootCamera();
	if (!worldCamera || !std::isfinite(camera.CameraProj._44) || std::abs(camera.CameraProj._44) > 1e-5f) {
		shadowStatus = "Main perspective camera unavailable";
		return;
	}
	const auto& origin = worldCamera->world.translate;
	const float dx = camera.CameraPosAdjust.x + camera.CameraViewInverse._14 - origin.x;
	const float dy = camera.CameraPosAdjust.y + camera.CameraViewInverse._24 - origin.y;
	const float dz = camera.CameraPosAdjust.z + camera.CameraViewInverse._34 - origin.z;
	const float errorSquared = dx * dx + dy * dy + dz * dz;
	if (!std::isfinite(errorSquared) || errorSquared > 0.25f) {
		shadowStatus = "Waiting for current main camera";
		return;
	}
	auto scene = GetPublishedScene();
	if (!scene) { shadowStatus = "Prepared scene or acceleration structure unavailable"; return; }
	const auto dynamicSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::array<uint32_t, 2> size{ uint32_t(dynamicSize.x), uint32_t(dynamicSize.y) };
	const auto& lights = scene->cell->nativeLights;
	if (lights.empty() || (shadowSettings.ShadowAllLights && lights.size() > 32) ||
		(!shadowSettings.ShadowAllLights && shadowSettings.ShadowLightIndex >= lights.size())) {
		shadowStatus = "Selected shadow lights unavailable or exceed 32";
		return;
	}
	const auto selectedLights = shadowSettings.ShadowAllLights ?
		std::span<const DX12Native::PreparedPointLight>(lights) :
		std::span<const DX12Native::PreparedPointLight>(lights).subspan(shadowSettings.ShadowLightIndex, 1);
	const DX12Native::ShadowTraceControls controls{
		shadowSettings.ShadowReachMultiplier, shadowSettings.ShadowFalloffFraction,
		shadowSettings.ShadowFalloffExponent, shadowSettings.ShadowEmitterRadius,
		shadowSettings.ShadowStochastic, shadowSettings.ShadowDistanceFade, shadowSettings.ShadowNormalInfluence,
		shadowSettings.ShadowTraceDensityMode
	};
	shadow.Dispatch(*backend, scene->acceleration, scene->materials, selectedLights, camera, size, frame,
		uint64_t(shadowSettings.FrameBudgetMB) * 1024 * 1024, controls);
	shadowStatus = shadow.Status();
}

std::optional<DX12NativeScene::SceneFrame> DX12NativeScene::GetSceneFrame(uint32_t a_frame) const
{
	if (!loaded || !settings.Enabled || transitionOpen || !globals::game::graphicsState || globals::game::graphicsState->GetFrameCount() != a_frame)
		return std::nullopt;
	SceneFrame frame{ a_frame, raster.Get(a_frame), GetPublishedScene(), GetShadowMask(a_frame) };
	if (!frame.raster && !frame.prepared && !frame.shadow) return std::nullopt;
	return frame;
}

std::shared_ptr<const DX12Native::ShadowFrame> DX12NativeScene::GetShadowMask(uint32_t a_frame) const
{
	if (!loaded || !settings.Enabled || !settings.ShadowMask || transitionOpen) return nullptr;
	auto output = shadow.Get(a_frame);
	auto scene = GetPublishedScene();
	return output && scene && output->scene == scene->acceleration ? output : nullptr;
}

void DX12NativeScene::OnOpaqueRasterReady(const OpaqueRasterInputs& a_inputs)
{
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	if (!settings.Enabled || transitionOpen || !backend || !globals::game::graphicsState || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) return;
	const std::array inputs{ a_inputs.litColor, a_inputs.litSpecular, a_inputs.albedo, a_inputs.normalRoughness,
		a_inputs.masks, a_inputs.masks2, a_inputs.reflectance, a_inputs.motion };
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const globals::FrameBuffer* camera = nullptr;
	if (!globals::game::isVR && globals::game::frameBufferCachedValid && globals::game::frameBufferCachedFrame == frame) {
		const auto& cached = globals::game::frameBufferCached.nonVR;
		const auto* worldCamera = RE::Main::WorldRootCamera();
		if (worldCamera && std::isfinite(cached.CameraProj._44) && std::abs(cached.CameraProj._44) <= 1e-5f) {
			const auto& origin = worldCamera->world.translate;
			const float dx = cached.CameraPosAdjust.x + cached.CameraViewInverse._14 - origin.x;
			const float dy = cached.CameraPosAdjust.y + cached.CameraViewInverse._24 - origin.y;
			const float dz = cached.CameraPosAdjust.z + cached.CameraViewInverse._34 - origin.z;
			const float errorSquared = dx * dx + dy * dy + dz * dz;
			if (std::isfinite(errorSquared) && errorSquared <= 0.25f) camera = &cached;
		}
	}
	raster.Capture(*backend, inputs, Util::GetCurrentSceneDepthSRV(false), camera, frame, uint64_t(settings.FrameBudgetMB) * 1024 * 1024);
	// Earlier shadow hooks may precede the current-frame main camera.
	// Retry only when the same-frame producer has not already published a mask;
	// DispatchShadow retains the camera, cell, material, scope and fence guards.
	if (settings.ShadowMask && !shadow.Get(frame)) {
		++shadowOpaqueRetryAttempts;
		DispatchShadow("opaque-raster-ready");
		if (shadow.Get(frame)) ++shadowOpaqueRetryRecovered;
	}
}

std::optional<DX12NativeScene::PublishedScene> DX12NativeScene::GetPublishedScene() const
{
	if (!loaded || !settings.Enabled || transitionOpen) return std::nullopt;
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	if (!cell || cell->GetFormID() != cellID) return std::nullopt;
	auto data = prepared.GetCell();
	auto scene = acceleration.GetScene();
	if (!data || !scene || scene->scope.scene != revision) return std::nullopt;
	auto materials = materialBindings.Get();
	if (materials && materials->scene != scene) materials.reset();
	return PublishedScene{ std::move(data), std::move(scene), std::move(materials), cellID, revision };
}

void DX12NativeScene::PublishDiagnostics()
{
	const auto bindings = materialBindings.Get();
	const auto retainedPages = DX12Native::RetainedMaterialPageBytes();
	json snapshot{ { "enabled", settings.Enabled }, { "cellID", cellID }, { "revision", revision }, { "backendFailure", failure },
		{ "raster", raster.GetDiagnostics() }, { "shadowStage", shadowStage }, { "shadowStatus", shadowStatus }, { "shadow", shadow.GetDiagnostics() },
		{ "shadowOpaqueRetryAttempts", shadowOpaqueRetryAttempts }, { "shadowOpaqueRetryRecovered", shadowOpaqueRetryRecovered },
		{ "shadowTraceControls", { { "revision", shadowControlsRevision },
			{ "allLights", settings.ShadowAllLights }, { "lightIndex", settings.ShadowLightIndex },
			{ "stochastic", settings.ShadowStochastic }, { "emitterRadius", settings.ShadowEmitterRadius },
			{ "reachMultiplier", settings.ShadowReachMultiplier }, { "falloffFraction", settings.ShadowFalloffFraction },
			{ "falloffExponent", settings.ShadowFalloffExponent }, { "distanceFade", settings.ShadowDistanceFade },
			{ "normalInfluence", settings.ShadowNormalInfluence }, { "traceDensityMode", settings.ShadowTraceDensityMode } } },
		{ "prepared", prepared.GetDiagnostics() }, { "materialFailure", materialBindings.Failure() }, { "materialUploadPending", materialBindings.UploadPending() },
		{ "acceleration", acceleration.GetDiagnostics() }, { "published", GetPublishedScene().has_value() },
		{ "materialBudget", { { "dxgiAvailable", bindings ? bindings->materialBudget.dxgiAvailable : false },
			{ "effectiveBytes", bindings ? bindings->materialBudget.effectiveBytes : 0 },
			{ "localBudgetBytes", bindings ? bindings->materialBudget.localBudget : 0 },
			{ "localUsageBytes", bindings ? bindings->materialBudget.localUsage : 0 },
			{ "textureBytes", bindings ? bindings->materials->bytes : 0 }, { "tableBytes", bindings ? bindings->materials->tableBytes : 0 },
			{ "retainedPageBytes", retainedPages ? json(*retainedPages) : json(nullptr) },
			{ "peakStagingBytes", bindings ? bindings->materials->peakStagingBytes : 0 },
			{ "uploadedBytes", bindings ? bindings->materials->uploadedBytes : 0 }, { "uploadedPages", bindings ? bindings->materials->uploadedPages : 0 },
			{ "reusedPages", bindings ? bindings->materials->reusedPages : 0 },
			{ "reusedTextures", bindings ? bindings->materials->reusedTextures : 0 },
			{ "asyncBatchPages", bindings ? bindings->materials->asyncBatchPages : 0 },
			{ "asyncBatchStagingBytes", bindings ? bindings->materials->asyncBatchStagingBytes : 0 },
			{ "rejectedTextureSources", bindings ? bindings->materials->rejectedTextureSources : 0 },
			{ "firstTextureSourceFailure", bindings ? bindings->materials->firstTextureSourceFailure : std::string{} },
			{ "uploadWaitMicroseconds", bindings ? bindings->materials->uploadWaitMicroseconds : 0 } } } };
	{
		std::scoped_lock lock(coverageMutex);
		snapshot["pbrCandidates"] = { { "pending", bool(coverageTask) }, { "failed", coverageFailed },
			{ "sceneRevision", coverageRevision }, { "currentScene", coverageResult && coverageRevision == revision },
			{ "unique", coverageResult ? coverageResult->uniqueCandidates : 0 }, { "ambiguous", coverageResult ? coverageResult->ambiguous : 0 },
			{ "recipeGenerationAvailable", coverageResult && coverageResult->recipeGenerationAvailable },
			{ "recipeRevision", coverageResult ? coverageResult->recipeRevision : 0 },
			{ "candidateRecipes", coverageResult ? coverageResult->candidateRecipes : 0 },
			{ "candidateRecipesMissing", coverageResult ? coverageResult->candidateRecipesMissing : 0 },
			{ "unobserved", coverageResult ? coverageResult->noObservation : 0 }, { "missingIdentity", coverageResult ? coverageResult->missingIdentity : 0 } };
	}
	std::scoped_lock lock(diagnosticsMutex);
	diagnostics = std::move(snapshot);
}

json DX12NativeScene::GetDiagnostics()
{
	std::scoped_lock lock(diagnosticsMutex);
	return diagnostics;
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_COMMAND("analyzePBRCandidates", "Queue an analysis of unverified reference and shape candidates and available recipes; read pbrCandidates in diagnostics.", [](Feature* self, const json&) {
		static_cast<DX12NativeScene*>(self)->coverageRequested = true;
	});
	FEATURE_COMMAND("setShadowMask", "Queue the separate early prepared-scene RT shadow mask setting. Params: enabled (boolean).", [](Feature* self, const json& args) {
		auto& scene = *static_cast<DX12NativeScene*>(self);
		const bool enabled = args.at("enabled").get<bool>();
		scene.requestedShadow = enabled ? 1 : 0;
		if (enabled) scene.requestedEnabled = 1;
		scene.shadowResetRequested = true;
	});
	FEATURE_COMMAND("setShadowLayer", "Legacy compatibility: layer enabled/contribution/strength/blend/radiusPixels/receiverTolerance/preview/afterLighting forward to Native Composite; allLights/lightIndex/stochastic/emitterRadius/reachMultiplier/falloffFraction/falloffExponent/distanceFade/normalInfluence/traceDensityMode (0 Full, 1 Two-thirds, 2 Half, 3 One-third, 4 Quarter, 5 One-sixth, 6 One-ninth, 7 Four-ninths) configure raw DX12 shadow-mask tracing.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<DX12NativeScene*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		auto& s = feature.settings;
		const auto tune = [&](const char* key, float& target, float low, float high) {
			if (!args.contains(key)) return;
			const float v = args.at(key).get<float>();
			if (std::isfinite(v)) target = std::clamp(v, low, high);
		};
				tune("emitterRadius", s.ShadowEmitterRadius, 0.0f, 8.0f);
		tune("reachMultiplier", s.ShadowReachMultiplier, 0.1f, 64.0f);
		tune("falloffFraction", s.ShadowFalloffFraction, 0.02f, 1.0f);
		tune("falloffExponent", s.ShadowFalloffExponent, 0.25f, 4.0f);
		if (args.contains("allLights")) s.ShadowAllLights = args.at("allLights").get<bool>();
		if (args.contains("lightIndex")) s.ShadowLightIndex = args.at("lightIndex").get<uint32_t>();
		if (args.contains("stochastic")) s.ShadowStochastic = args.at("stochastic").get<bool>();
		if (args.contains("distanceFade")) s.ShadowDistanceFade = args.at("distanceFade").get<bool>();
		if (args.contains("normalInfluence")) s.ShadowNormalInfluence = std::min(args.at("normalInfluence").get<uint32_t>(), 2u);
		if (args.contains("traceDensityMode")) s.ShadowTraceDensityMode = std::min(args.at("traceDensityMode").get<uint32_t>(), 7u);
		++feature.shadowControlsRevision;
		globals::features::nativeComposite.ConfigureShadowLayer(args);
	});
	FEATURE_COMMAND("setPreparedScene", "Queue the prepared static-scene loading settings. Params: enabled (boolean), budgetMB (optional integer, 64..4096).", [](Feature* self, const json& args) {
		auto& scene = *static_cast<DX12NativeScene*>(self);
		scene.requestedEnabled = args.at("enabled").get<bool>() ? 1 : 0;
		if (args.contains("budgetMB")) scene.requestedBudget = std::clamp(args.at("budgetMB").get<uint32_t>(), 64u, 4096u);
		scene.resetRequested = true;
	});
}
