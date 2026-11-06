#include "DX12NativeScene.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "ShaderCache.h"
#include "Utils/DevBenchUx.h"
#include "Utils/D3D.h"
#include <cmath>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12NativeScene::Settings, Enabled, ShadowMask, BudgetMB)

void DX12NativeScene::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	if (ImGui::Checkbox("Load prepared scene", &settings.Enabled)) resetRequested = true;
	if (ImGui::Checkbox("Produce RT shadow mask", &settings.ShadowMask)) shadowResetRequested = true;
	if (settings.ShadowMask && !shadowStatus.empty()) ImGui::TextUnformatted(shadowStatus.c_str());
	ImGui::TextUnformatted(failure.empty() ? "Prepared static scenes" : failure.c_str());
}

void DX12NativeScene::LoadSettings(json& a_json)
{
	const Settings selected = a_json;
	requestedEnabled = selected.Enabled ? 1 : 0;
	requestedShadow = selected.ShadowMask ? 1 : 0;
	requestedBudget = std::clamp(selected.BudgetMB, 64u, 4096u);
	resetRequested = true;
}
void DX12NativeScene::SaveSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	auto selected = settings;
	if (const auto enabled = requestedEnabled.load(); enabled >= 0) selected.Enabled = enabled != 0;
	if (const auto shadowEnabled = requestedShadow.load(); shadowEnabled >= 0) selected.ShadowMask = shadowEnabled != 0;
	if (const auto budget = requestedBudget.load(); budget) selected.BudgetMB = budget;
	a_json = selected;
}
void DX12NativeScene::RestoreDefaultSettings()
{
	requestedEnabled = 0;
	requestedShadow = 0;
	requestedBudget = Settings{}.BudgetMB;
	resetRequested = true;
}
void DX12NativeScene::OnRuntimeDisabled() { resetRequested = true; }
void DX12NativeScene::OnSceneTransitionReset(bool a_open) { transitionOpen = a_open; ResetScene(); }
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

void DX12NativeScene::EarlyPrepass()
{
	CS_GPU_PASS("DX12NativeScene::PreparedSceneProgress");
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	{
		std::scoped_lock lock(settingsMutex);
		if (const auto enabled = requestedEnabled.exchange(-1); enabled >= 0) settings.Enabled = enabled != 0;
		if (const auto shadowEnabled = requestedShadow.exchange(-1); shadowEnabled >= 0) settings.ShadowMask = shadowEnabled != 0;
		if (const auto budget = requestedBudget.exchange(0); budget) settings.BudgetMB = budget;
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
	if (!settings.Enabled || !settings.ShadowMask || transitionOpen || globals::game::isVR || !backend || !globals::game::graphicsState ||
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
	shadow.Dispatch(*backend, scene->acceleration, scene->materials, scene->cell->nativeLights, camera, size, frame, uint64_t(settings.BudgetMB) * 1024 * 1024);
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
	raster.Capture(*backend, inputs, Util::GetCurrentSceneDepthSRV(false), camera, frame, uint64_t(settings.BudgetMB) * 1024 * 1024);
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
	json snapshot{ { "enabled", settings.Enabled }, { "cellID", cellID }, { "revision", revision }, { "backendFailure", failure },
		{ "raster", raster.GetDiagnostics() }, { "shadowStage", shadowStage }, { "shadowStatus", shadowStatus }, { "shadow", shadow.GetDiagnostics() },
		{ "prepared", prepared.GetDiagnostics() }, { "materialFailure", materialBindings.Failure() }, { "acceleration", acceleration.GetDiagnostics() }, { "published", GetPublishedScene().has_value() } };
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
	FEATURE_COMMAND("setShadowMask", "Queue the separate early prepared-scene RT shadow mask setting. Params: enabled (boolean).", [](Feature* self, const json& args) {
		auto& scene = *static_cast<DX12NativeScene*>(self);
		const bool enabled = args.at("enabled").get<bool>();
		scene.requestedShadow = enabled ? 1 : 0;
		if (enabled) scene.requestedEnabled = 1;
		scene.shadowResetRequested = true;
	});
	FEATURE_COMMAND("setPreparedScene", "Queue the prepared static-scene loading settings. Params: enabled (boolean), budgetMB (optional integer, 64..4096).", [](Feature* self, const json& args) {
		auto& scene = *static_cast<DX12NativeScene*>(self);
		scene.requestedEnabled = args.at("enabled").get<bool>() ? 1 : 0;
		if (args.contains("budgetMB")) scene.requestedBudget = std::clamp(args.at("budgetMB").get<uint32_t>(), 64u, 4096u);
		scene.resetRequested = true;
	});
}
