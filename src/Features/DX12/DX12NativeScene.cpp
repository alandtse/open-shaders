#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "Features/LightLimitFix/Common.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/N/NiLight.h"
#include "RE/T/TESObjectREFR.h"

#include <algorithm>
#include <cmath>

bool DX12NativeScene::IsActive() const
{
	return loaded && settings.Enabled && globals::features::dx12Resources.IsActive() &&
	       globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

void DX12NativeScene::DrawSettings()
{
	if (ImGui::Checkbox("Enable native acceleration scene", &settings.Enabled) && !settings.Enabled)
		OnRuntimeDisabled();
	ImGui::Checkbox("Inherit matched runtime light values", &settings.lights.inheritRuntime);
	ImGui::SliderFloat("Light match distance", &settings.lights.matchDistance, 1.0f, 256.0f, "%.0f");
	ImGui::SliderFloat("Light intensity multiplier", &settings.lights.intensity, 0.0f, 8.0f, "%.2f");
	ImGui::SliderFloat("Light reach multiplier", &settings.lights.reach, 0.1f, 4.0f, "%.2f");
	ImGui::SliderFloat("Light emitter multiplier", &settings.lights.emitterSize, 0.1f, 8.0f, "%.2f");
}

void DX12NativeScene::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", true);
	settings.lights.inheritRuntime = input.value("InheritRuntime", true);
	settings.lights.matchDistance = input.value("MatchDistance", 64.0f);
	settings.lights.intensity = input.value("Intensity", 1.0f);
	settings.lights.reach = input.value("Reach", 1.0f);
	settings.lights.emitterSize = input.value("EmitterSize", 1.0f);
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = {
		{ "Enabled", settings.Enabled },
		{ "InheritRuntime", settings.lights.inheritRuntime },
		{ "MatchDistance", settings.lights.matchDistance },
		{ "Intensity", settings.lights.intensity },
		{ "Reach", settings.lights.reach },
		{ "EmitterSize", settings.lights.emitterSize }
	};
}

void DX12NativeScene::RestoreDefaultSettings()
{
	settings = {};
	OnRuntimeDisabled();
}

void DX12NativeScene::EarlyPrepass()
{
	CS_GPU_PASS("DX12NativeScene::Update");
	if (!IsActive()) {
		if (scene.Ready() || lights.load(std::memory_order_acquire))
			OnRuntimeDisabled();
		return;
	}
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (grid && scene.Start(swapChain.d3d12Device.get()))
		scene.Request(grid);
	UpdateLights();
}

void DX12NativeScene::UpdateLights()
{
	const auto prepared = globals::features::dx12Resources.GetPreparedLights();
	std::vector<DX12::RuntimeLight> runtime;
	if (ShadowCasterManager::IsActive()) {
		const auto& active = ShadowCasterManager::GetLights();
		for (int32_t i = 0; active.Lights && i < active.Size; ++i) {
			const auto* shadow = active.Lights[i].Light;
			const auto* source = shadow ? shadow->light.get() : nullptr;
			if (!source || shadow->GetIsDirectionalLight())
				continue;
			const auto* properties = LLFCommon::RuntimeLightDataExt::Get(const_cast<RE::NiLight*>(source));
			const auto* owner = source->GetUserData();
			const auto* reference = owner ? owner->As<RE::TESObjectREFR>() : nullptr;
			const auto& origin = source->world.translate;
			const int32_t slot = ShadowCasterManager::GetShadowSlot(const_cast<RE::BSShadowLight*>(shadow));
			DX12::RuntimeLight light{};
			light.referenceID = reference ? reference->GetFormID() : 0;
			light.baseID = properties ? properties->lighFormId : 0;
			light.shadowSlot = slot >= 0 ? uint32_t(slot) : UINT32_MAX;
			light.position = { origin.x, origin.y, origin.z };
			if (properties) {
				light.color = { properties->diffuse.red, properties->diffuse.green, properties->diffuse.blue };
				light.radius = properties->radius;
				light.fade = properties->fade;
				light.emitterRadius = properties->size;
			}
			runtime.push_back(light);
		}
	}
	const auto camera = globals::game::player ? globals::game::player->GetPosition() : RE::NiPoint3{};
	const float3 cameraPosition{ camera.x, camera.y, camera.z };
	auto resolved = DX12::MatchPreparedLights(prepared, runtime, settings.lights, cameraPosition);
	lights.store(std::make_shared<const DX12::ResolvedLights>(std::move(resolved)), std::memory_order_release);
}

void DX12NativeScene::OnSceneTransitionReset(bool opening)
{
	if (opening)
		OnRuntimeDisabled();
}

void DX12NativeScene::OnRuntimeDisabled()
{
	scene.Clear();
	lights.store(nullptr, std::memory_order_release);
}
