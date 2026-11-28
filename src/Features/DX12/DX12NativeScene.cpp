#include "DX12NativeScene.h"

#include "../LightLimitFix/ShadowCasterMath.h"
#include "../Upscaling.h"
#include "DX12Control.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/M/MainMenu.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectLIGH.h"
#include "RE/U/UI.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"
#include "Utils/PointLightFlags.h"
#include "Utils/TracyZones.h"
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

void DX12NativeScene::DrawSettings()
{
	const auto& control = globals::features::dx12Control;
	if (control.IsFullRTMode()) {
		const char* qualities[]{ T("menu.dx12.scene.rt_quality", "Quality"), T("menu.dx12.scene.rt_performance", "Performance") };
		int quality = fullRTQuality == DX12Native::kShadowQualityPerformance ? 1 : 0;
		if (ImGui::Combo(T("menu.dx12.scene.shadow_quality", "Shadow quality"), &quality, qualities, int(std::size(qualities))))
			fullRTQuality = quality == 0 ? DX12Native::kShadowQualityQuality : DX12Native::kShadowQualityPerformance;
		bool outdoor = outdoorFullRT.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoor))
			outdoorFullRT.store(outdoor, std::memory_order_relaxed);
	} else if (control.IsShadowOffloadMode()) {
		bool outdoor = outdoorShadowOffloading.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoor))
			outdoorShadowOffloading.store(outdoor, std::memory_order_relaxed);
	}
}

void DX12NativeScene::DrawDevSettings()
{
	const auto& control = globals::features::dx12Control;
	if (!control.IsShadowOffloadMode() && !control.IsFullRTMode())
		return;
	ImGui::Text(T("menu.dx12.scene.caster_ownership_counts", "Static casters traced: %llu | Kept in raster: %llu"),
		static_cast<unsigned long long>(castersTraced.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(castersKeptRaster.load(std::memory_order_relaxed)));
	bool debugMask = DX12Native::compositeShadowDebugView.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.debug_shadow_mask", "Show ray-traced shadow mask"), &debugMask))
		DX12Native::compositeShadowDebugView.store(debugMask, std::memory_order_relaxed);
	if (control.IsFullRTMode()) {
		bool exact = exactShadowLights.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.exact_shadow_lights", "Experimental exact shadow-light origins"), &exact))
			exactShadowLights.store(exact, std::memory_order_relaxed);
		int unify = int(runtimeUnificationRadius.load(std::memory_order_relaxed));
		int exclude = int(preparedExclusionRadius.load(std::memory_order_relaxed));
		if (ImGui::SliderInt(T("menu.dx12.scene.runtime_unification_radius", "Runtime light unification radius (units)"), &unify, 0, int(DX12Native::kFullShadowRadiusMax), "%d", ImGuiSliderFlags_AlwaysClamp))
			runtimeUnificationRadius.store(DX12Native::SanitizeFullShadowRadius(uint32_t(unify)), std::memory_order_relaxed);
		if (ImGui::SliderInt(T("menu.dx12.scene.prepared_exclusion_radius", "Prepared light exclusion radius (units)"), &exclude, 0, int(DX12Native::kFullShadowRadiusMax), "%d", ImGuiSliderFlags_AlwaysClamp))
			preparedExclusionRadius.store(DX12Native::SanitizeFullShadowRadius(uint32_t(exclude)), std::memory_order_relaxed);
		bool additive = additivePreparedEmission.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.additive_prepared_emission", "Add excluded runtime emission to prepared lights"), &additive))
			additivePreparedEmission.store(additive, std::memory_order_relaxed);
		int interval = int(lightObservationFrames.load(std::memory_order_relaxed));
		if (ImGui::SliderInt(T("menu.dx12.scene.light_observation_interval", "Light observation interval (frames)"), &interval, 1, int(DX12Native::kMaxLightObservationFrames)))
			lightObservationFrames.store(uint32_t(interval), std::memory_order_relaxed);
		std::scoped_lock lock(runtimeLightMutex);
		const auto stats = lightWorker.GetStats();
		ImGui::Text("%s", std::format("Light tasks: {} | Publications: {} | Unchanged: {} | Deferred: {} | Busy: {}",
							  stats.tasks, stats.publications, stats.unchanged, stats.deferred, stats.busy)
							  .c_str());
	}
	auto& enabled = control.IsFullRTMode() ? debugRTLights : debugOffloadLights;
	bool value = enabled.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.debug_native_lights", "Preview native lights"), &value))
		enabled.store(value, std::memory_order_relaxed);
}

bool DX12NativeScene::WantsLightPreview(DX12::SCMNativeMode mode) const
{
	if (!globals::state || !globals::state->IsDeveloperMode())
		return false;
	switch (mode) {
	case DX12::SCMNativeMode::ShadowOffload:
		return debugOffloadLights.load(std::memory_order_relaxed);
	case DX12::SCMNativeMode::FullRT:
		return debugRTLights.load(std::memory_order_relaxed);
	default:
		return false;
	}
}

bool DX12NativeScene::IsOverlayVisible() const
{
	return WantsLightPreview(GetFrameNativeMode());
}

void DX12NativeScene::DrawOverlay()
{
	if (!IsOverlayVisible() || globals::state->isMapMenuOpen)
		return;
	const auto mask = shadows.Get(globals::game::graphicsState->GetFrameCount());
	if (!mask || mask->debugLights.empty())
		return;
	const auto* viewport = ImGui::GetMainViewport();
	auto* draw = ImGui::GetForegroundDrawList();
	constexpr float markerRadius = 4.0f;
	constexpr float labelOffset = 6.0f;
	const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
	const ImU32 background = ImGui::GetColorU32(ImGuiCol_PopupBg);
	for (size_t i = 0; i < mask->debugLights.size(); ++i) {
		const auto& light = mask->debugLights[i];
		float2 uv;
		if (!Util::GetWorldFocusCoord(RE::NiPoint3{ light.position.x, light.position.y, light.position.z }, uv))
			continue;
		const ImVec2 point{ viewport->Pos.x + uv.x * viewport->Size.x, viewport->Pos.y + uv.y * viewport->Size.y };
		const char* source = mask->quality == DX12Native::kShadowQualityOffloading ? "M" :
		                                                                             (i < mask->debugPreparedCount ? "P" : "R");
		const auto label = light.unifiedCount > 1 ?
		                       std::format("DXR U x{} {:08X} #{}", light.unifiedCount, light.referenceID, i) :
		                       std::format("DXR {} {:08X} #{}", source, light.referenceID, i);
		const ImVec2 text{ point.x + labelOffset, point.y - ImGui::GetTextLineHeight() * 0.5f };
		const auto size = ImGui::CalcTextSize(label.c_str());
		draw->AddCircleFilled(point, markerRadius, color);
		draw->AddRectFilled(text, ImVec2(text.x + size.x, text.y + size.y), background);
		draw->AddText(text, color, label.c_str());
	}
}

void DX12NativeScene::LoadSettings(json& input)
{
	outdoorShadowOffloading = input.value("OutdoorShadowOffloading", false);
	debugOffloadLights.store(input.value("DebugNativeLights", false), std::memory_order_relaxed);
	const auto profile = input.value("FullRT", json::object());
	outdoorFullRT = profile.value("OutdoorShadowOffloading", false);
	debugRTLights.store(profile.value("DebugNativeLights", false), std::memory_order_relaxed);
	fullRTQuality = DX12Native::SanitizeRTQuality(profile.value("ShadowQualityMode", DX12Native::kShadowQualityQuality));
	uint32_t unify = profile.value("RuntimeUnificationRadius", DX12Native::kDefaultRuntimeUnificationRadius);
	uint32_t exclude = profile.value("PreparedExclusionRadius", DX12Native::kDefaultPreparedExclusionRadius);
	if (!profile.contains("AdditivePreparedEmission")) {
		if (unify == DX12Native::kLegacyRuntimeUnificationRadius)
			unify = DX12Native::kDefaultRuntimeUnificationRadius;
		if (exclude == DX12Native::kLegacyPreparedExclusionRadius)
			exclude = DX12Native::kDefaultPreparedExclusionRadius;
	}
	runtimeUnificationRadius.store(DX12Native::SanitizeFullShadowRadius(unify), std::memory_order_relaxed);
	preparedExclusionRadius.store(DX12Native::SanitizeFullShadowRadius(exclude), std::memory_order_relaxed);
	additivePreparedEmission.store(profile.value("AdditivePreparedEmission", true), std::memory_order_relaxed);
	lightObservationFrames.store(std::clamp(profile.value("LightObservationFrames", DX12Native::kDefaultLightObservationFrames), 1u, DX12Native::kMaxLightObservationFrames), std::memory_order_relaxed);
	std::scoped_lock lock(runtimeLightMutex);
	ResetFullLightInventory();
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "OutdoorShadowOffloading", outdoorShadowOffloading.load(std::memory_order_relaxed) },
		{ "DebugNativeLights", debugOffloadLights.load(std::memory_order_relaxed) },
		{ "FullRT", { { "OutdoorShadowOffloading", outdoorFullRT.load(std::memory_order_relaxed) }, { "ShadowQualityMode", fullRTQuality },
						{ "DebugNativeLights", debugRTLights.load(std::memory_order_relaxed) },
						{ "RuntimeUnificationRadius", runtimeUnificationRadius.load(std::memory_order_relaxed) },
						{ "PreparedExclusionRadius", preparedExclusionRadius.load(std::memory_order_relaxed) },
						{ "AdditivePreparedEmission", additivePreparedEmission.load(std::memory_order_relaxed) },
						{ "LightObservationFrames", lightObservationFrames.load(std::memory_order_relaxed) } } } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	outdoorShadowOffloading = false;
	outdoorFullRT = false;
	debugOffloadLights.store(false, std::memory_order_relaxed);
	debugRTLights.store(false, std::memory_order_relaxed);
	fullRTQuality = DX12Native::kShadowQualityQuality;
	runtimeUnificationRadius.store(DX12Native::kDefaultRuntimeUnificationRadius, std::memory_order_relaxed);
	preparedExclusionRadius.store(DX12Native::kDefaultPreparedExclusionRadius, std::memory_order_relaxed);
	additivePreparedEmission.store(true, std::memory_order_relaxed);
	lightObservationFrames.store(DX12Native::kDefaultLightObservationFrames, std::memory_order_relaxed);
	std::scoped_lock lock(runtimeLightMutex);
	ResetFullLightInventory();
}

bool DX12NativeScene::AllowsNativeSceneWork() const
{
	const auto& control = globals::features::dx12Control;
	return control.IsShadowOffloadMode() || control.IsFullRTMode();
}

bool DX12NativeScene::AllowsOutdoorContribution() const
{
	return globals::features::dx12Control.IsFullRTMode() ?
	           outdoorFullRT.load(std::memory_order_relaxed) :
	           outdoorShadowOffloading.load(std::memory_order_relaxed);
}

bool DX12NativeScene::CanOffloadShadowsInCurrentCell() const
{
	if (globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
		return false;
	const auto* cell = Util::GetPlayerCell();
	return cell && AllowsScenePreparation(cell->IsExteriorCell());
}

DX12::SCMNativeMode DX12NativeScene::GetFrameNativeMode() const
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		globals::features::dx12Resources.IsLoading() || !globals::game::graphicsState)
		return DX12::SCMNativeMode::Disabled;
	// SCM schedules before this frame's PreShadowMaps stamps the mode, so the previous frame's stamp governs it.
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	const uint32_t stamped = frameNativeModeID.load(std::memory_order_acquire);
	if (stamped == UINT32_MAX || (stamped != frame && stamped + 1 != frame))
		return DX12::SCMNativeMode::Disabled;
	return frameNativeMode.load(std::memory_order_acquire);
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// SCM keeps raster dynamic casters unconditionally. Static casters may be
	// omitted only when this exact runtime light was matched to a native emitter
	// in the current frame; a nearby prepared light is not sufficient proof.
	if (!light || slot < 0 ||
		light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return false;
	const auto scene = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !lights || !ni ||
		scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame)
		return false;

	const auto found = std::ranges::find_if(runtimeLightSamples, [ni](const RuntimeLightSample& sample) {
		return sample.source == ni;
	});
	if (found == runtimeLightSamples.end())
		return false;
	const auto grant = [&] {
		ownershipGranted.fetch_add(1, std::memory_order_relaxed);
		if (!std::ranges::contains(ownedLights, ni))
			ownedLights.push_back(ni);
		return true;
	};
	// A mesh-attached light has no prepared record; its live sample is traced only once granted here.
	if (!found->referenceID)
		return grant();

	// The shader admits only the published prepared lights. Never suppress a
	// raster static caster for a light not actually selected for DX12 tracing.
	const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
	for (size_t i = 0; i < limit; ++i) {
		const auto& candidate = (*lights)[i];
		if (candidate.referenceID != 0 && candidate.referenceID == found->referenceID)
			return grant();
	}
	return false;
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool /*flickers*/, const RE::NiPoint3& restOrigin)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!light || light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return;
	auto* ni = light->light.get();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	if (!ni)
		return;
	const auto& rt = ni->GetLightRuntimeData();
	const auto& position = ni->world.translate;
	if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
		!std::isfinite(rt.fade) || !std::isfinite(rt.diffuse.red) ||
		!std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
		!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
		return;

	// Mesh-attached lights (Light Placer, NIF lights) have no prepared record and trace from live values alone.
	const DX12::PreparedLight* matched = nullptr;
	if (lights && referenceID != 0) {
		const auto it = std::ranges::find_if(*lights, [referenceID](const auto& candidate) {
			return candidate.referenceID == referenceID;
		});
		if (it != lights->end())
			matched = &*it;
	} else if (lights) {
		float bestDistanceSquared = std::numeric_limits<float>::infinity();
		for (const auto& candidate : *lights) {
			const float dx = candidate.position.x - restOrigin.x;
			const float dy = candidate.position.y - restOrigin.y;
			const float dz = candidate.position.z - restOrigin.z;
			const float errorSquared = dx * dx + dy * dy + dz * dz;
			const float tolerance = std::clamp(candidate.radius * 0.01f, 4.0f, 32.0f);
			if (std::isfinite(errorSquared) && errorSquared < bestDistanceSquared &&
				errorSquared <= tolerance * tolerance) {
				bestDistanceSquared = errorSquared;
				matched = &candidate;
			}
		}
	}
	DX12::PreparedLight live = matched ? *matched : DX12::PreparedLight{};
	live.position = float3{ position.x, position.y, position.z };
	live.radius = rt.radius.x;
	live.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
	live.fade = rt.fade;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame) {
		runtimeLightFrame = frame;
		runtimeLightSamples.clear();
		ownedLights.clear();
	}
	// A prepared emitter must not accidentally account for two different SCM
	// lights that happen to occupy the same position.
	const auto it = std::ranges::find_if(runtimeLightSamples, [ni](const auto& sample) {
		return sample.source == ni;
	});
	const uint32_t matchedID = matched ? matched->referenceID : 0;
	if (it != runtimeLightSamples.end()) {
		it->referenceID = matchedID;
		it->value = live;
	} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights &&
			   (!matchedID || !std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
				   return sample.referenceID == matchedID;
			   }))) {
		runtimeLightSamples.push_back({ ni, matchedID, live });
	}
}

bool DX12NativeScene::WantsRenderPassHook() const
{
	return globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

bool DX12NativeScene::WantsPreShadowMapsHook() const { return WantsRenderPassHook(); }
bool DX12NativeScene::WantsOpaqueRasterReadyHook() const { return WantsRenderPassHook(); }
bool DX12NativeScene::WantsDepthPrepassReadyHook() const { return WantsRenderPassHook(); }

void DX12NativeScene::PublishLightShadowSlots(uint32_t frame, std::span<const DX12::PreparedLight> submitted,
	std::span<const RE::NiLight* const> sources)
{
	slotsBySource.clear();
	slotsByReference.clear();
	slotPositions.clear();
	shadedLightCount.store(shadedThisFrame, std::memory_order_relaxed);
	shadedThisFrame = 0;
	const auto traced = shadows.Get(frame);
	lightSlotFrame = UINT32_MAX;
	// Queue-side wait before any light is flagged: forward lighting must not read slices the trace has not written.
	if (!traced || !traced->lightVisibility || !backend ||
		FAILED(backend->d3d11Context->Wait(traced->lightVisibilityFence.get(), traced->lightVisibilityValue)))
		return;
	lightSlotFrame = frame;
	for (uint32_t i = 0; i < traced->visibilityLights.size() && i < submitted.size(); ++i) {
		if (i < sources.size()) {
			slotsBySource.try_emplace(sources[i], i);
			continue;
		}
		const auto& light = submitted[i];
		slotPositions.push_back({ light.position, i });
		if (light.runtimeIdentity)
			slotsBySource.try_emplace(reinterpret_cast<const RE::NiLight*>(light.runtimeIdentity), i);
		if (light.referenceID)
			slotsByReference.try_emplace(light.referenceID, i);
	}
}

std::optional<uint32_t> DX12NativeScene::NativeShadowSlot(const RE::NiLight* a_light)
{
	if (!a_light || !globals::game::graphicsState || lightSlotFrame != globals::game::graphicsState->GetFrameCount())
		return std::nullopt;
	std::optional<uint32_t> slot;
	if (const auto it = slotsBySource.find(a_light); it != slotsBySource.end())
		slot = it->second;
	else if (const auto* owner = a_light->GetUserData(); owner && !slotsByReference.empty())
		if (const auto ref = slotsByReference.find(owner->GetFormID()); ref != slotsByReference.end())
			slot = ref->second;
	if (!slot && !slotPositions.empty()) {
		// Full RT merges runtime emitters into the traced light they sit beside; shade them with its slice.
		const float radius = float(runtimeUnificationRadius.load(std::memory_order_relaxed));
		const auto& p = a_light->world.translate;
		float best = radius * radius;
		for (const auto& [position, index] : slotPositions) {
			const float dx = position.x - p.x, dy = position.y - p.y, dz = position.z - p.z;
			if (const float d2 = dx * dx + dy * dy + dz * dz; d2 <= best) {
				best = d2;
				slot = index;
			}
		}
	}
	if (slot) {
		lightsShadedFrame = lightSlotFrame;
		++shadedThisFrame;
	}
	return slot;
}

bool DX12NativeScene::ShadesLightsDirectly(uint32_t a_frame) const
{
	return lightsShadedFrame == a_frame;
}

void DX12NativeScene::Prepass()
{
	if (!loaded || globals::game::isVR || !backend || !globals::game::graphicsState)
		return;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	ID3D11ShaderResourceView* view = nullptr;
	if (const auto traced = shadows.Get(frame); traced && traced->lightVisibility && lightSlotFrame == frame)
		view = traced->lightVisibility->srv;
	globals::d3d::context->PSSetShaderResources(kLightShadowRegister, 1, &view);
}

void DX12NativeScene::OnDepthPrepassReady()
{
	if (GetFrameNativeMode() == DX12::SCMNativeMode::Disabled)
		return;
	CS_GPU_PASS("DX12NativeScene::DepthPrepassReady");
	// The same combined mask is produced. The later hook remains the fallback.
	OnOpaqueRasterReady();
}

void DX12NativeScene::PrepareLoadingScene(std::shared_ptr<const DX12::Grid> grid)
{
	ApplyPendingLoadingReset();
	if (!grid || !globals::features::dx12Resources.IsLoading() || !globals::features::dx12Control.IsActive() || !AllowsNativeSceneWork() ||
		!globals::d3d::device || !globals::d3d::context ||
		!std::ranges::any_of(grid->resident, [](const auto& tile) { return tile && !tile->shapes.empty(); }))
		return;
	nativeSessionStarted = true;
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.RequestStaged(std::move(grid));
}

void DX12NativeScene::PreShadowMaps()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive()) {
		OnRuntimeDisabled();
		return;
	}
	ApplyPendingLoadingReset();
	if (globals::features::dx12Resources.IsLoading() || !globals::state || !globals::game::graphicsState ||
		!globals::state->startupMenuInitializationComplete.load(std::memory_order_acquire) ||
		(globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))) {
		frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
		return;
	}
	if (!CanOffloadShadowsInCurrentCell()) {
		if (nativeSessionStarted)
			OnRuntimeDisabled();
		return;
	}
	nativeSessionStarted = true;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	if (frameNativeModeID.load(std::memory_order_acquire) != frame) {
		const auto& control = globals::features::dx12Control;
		const auto mode = DX12::SelectSCMNativeMode(control.IsActive(), globals::game::isVR,
			control.IsShadowOffloadMode(), control.IsFullRTMode(), CanOffloadShadowsInCurrentCell());
		const auto previous = frameNativeMode.exchange(mode, std::memory_order_acq_rel);
		const bool exact = mode == DX12::SCMNativeMode::FullRT && globals::state->IsDeveloperMode() && exactShadowLights.load(std::memory_order_relaxed);
		const bool previousExact = frameExactShadowLights.exchange(exact, std::memory_order_acq_rel);
		if (previous != mode || previousExact != exact) {
			std::scoped_lock lock(runtimeLightMutex);
			ResetFullLightInventory();
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			ownedLights.clear();
		}
		frameNativeModeID.store(frame, std::memory_order_release);
	}
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || grid->requested.empty() || !grid->requested.front().scope) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		std::scoped_lock lock(runtimeLightMutex);
		ResetFullLightInventory();
		return;
	}
	shadowLightScope.store(grid->requested.front().scope, std::memory_order_release);
	// SCM reads a single per-frame publication without re-enumerating tiles
	// for every shadow light; the same ordered sources feed RayQuery.
	if (GetFrameNativeMode() != DX12::SCMNativeMode::FullRT)
		shadowLights.store(std::make_shared<const std::vector<DX12::PreparedLight>>(
							   globals::features::dx12Resources.GetPreparedLights()),
			std::memory_order_release);
	// Only a revision-coalesced mailbox handoff on this thread. All BLAS/TLAS
	// allocations and command recording run as bounded shared-pool tasks.
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
}

namespace
{
	// Engine user data and parent pointers are not reference counted.
	void ReadFullLightOwner(const RE::NiLight* ni, uint32_t& owner, uint32_t& placed) noexcept
	{
		__try {
			if (auto* ref = Util::FindSceneOwner(ni)) {
				owner = ref->GetFormID();
				auto* base = ref->GetObjectReference();
				if (base && base->As<RE::TESObjectLIGH>())
					placed = owner;
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
	void ReadFullLightRestPosition(RE::NiLight* ni, uint32_t placedReference, float3& position) noexcept
	{
		__try {
			const auto formID = PointLightFlags::GetRuntimeLightFormID(ni);
			const auto* light = formID ? RE::TESForm::LookupByID<RE::TESObjectLIGH>(formID) : nullptr;
			if (!light && placedReference) {
				auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(placedReference);
				auto* base = reference ? reference->GetObjectReference() : nullptr;
				light = base ? base->As<RE::TESObjectLIGH>() : nullptr;
			}
			const auto& local = ni->local.translate;
			if (light && !light->GetNoFlicker() && ni->parent &&
				ShadowCasterManager::WithinFlickerReach(local.x, local.y, local.z, light->data.flickerMovementAmplitude)) {
				const auto& rest = ni->parent->world.translate;
				if (std::isfinite(rest.x) && std::isfinite(rest.y) && std::isfinite(rest.z))
					position = float3{ rest.x, rest.y, rest.z };
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
}

void DX12NativeScene::ResetFullLightInventory()
{
	lightWorker.Reset();
	fullRuntimeLights.clear();
	fullRequestedGrid.reset();
	fullRuntimeSnapshot.reset();
	fullRequestedRuntime.reset();
	fullRuntimeScope = 0;
	fullRuntimeCell = 0;
	fullLightVisit = 0;
	fullObserved = false;
}

bool DX12NativeScene::ObserveFullRuntimeLights()
{
	ZoneScopedN("DX12::Lights::Observe");
	const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	if (!node) {
		if (!fullRuntimeSnapshot)
			fullRuntimeSnapshot = std::make_shared<const std::vector<DX12Native::NativeRuntimeEmitter>>();
		return false;
	}
	bool dirty = !fullRuntimeSnapshot;
	const uint64_t visit = ++fullLightVisit;
	const bool preserveExact = frameExactShadowLights.load(std::memory_order_relaxed);
	const auto collect = [&](RE::BSLight* light, bool shadowLight) {
		if (!light || !light->pointLight || !light->light)
			return;
		auto* ni = light->light.get();
		auto [it, inserted] = fullRuntimeLights.try_emplace(ni);
		auto& cached = it->second;
		if (cached.seen == visit)
			return;
		cached.seen = visit;
		if (inserted) {
			cached.source = light->light;
			const auto flags = PointLightFlags::GetVanillaPointLightFlags(light, ni);
			cached.supported = !(flags & PointLightFlags::ToMask(PointLightFlags::Flags::Spot));
			cached.value.runtimeIdentity = reinterpret_cast<std::uintptr_t>(ni);
			ReadFullLightOwner(ni, cached.value.referenceID, cached.placedReference);
		}
		if (!cached.supported)
			return;
		const bool visible = !ni->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		if (visible && !cached.sampled) {
			const auto& rt = ni->GetLightRuntimeData();
			const auto& position = ni->world.translate;
			cached.sampled = std::isfinite(rt.radius.x) && rt.radius.x > 0.0f &&
			                 std::isfinite(rt.fade) && rt.fade > 0.0f &&
			                 std::isfinite(rt.diffuse.red) && std::isfinite(rt.diffuse.green) && std::isfinite(rt.diffuse.blue) &&
			                 std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
			if (cached.sampled) {
				auto& value = cached.value;
				value.position = float3{ position.x, position.y, position.z };
				value.radius = rt.radius.x;
				value.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
				value.fade = rt.fade;
				ReadFullLightRestPosition(ni, cached.placedReference, value.position);
			}
		}
		const bool active = visible && cached.sampled;
		dirty |= cached.active != active || cached.shadowLight != shadowLight;
		cached.active = active;
		cached.shadowLight = shadowLight;
	};
	if (node) {
		const auto& scene = node->GetRuntimeData();
		if (preserveExact)
			for (const auto& light : scene.activeShadowLights)
				collect(light.get(), true);
		for (const auto& light : scene.activeLights)
			collect(light.get(), false);
		if (!preserveExact)
			for (const auto& light : scene.activeShadowLights)
				collect(light.get(), false);
	}
	std::erase_if(fullRuntimeLights, [&](const auto& entry) {
		if (entry.second.seen == visit)
			return false;
		dirty |= entry.second.active;
		return true;
	});
	if (dirty) {
		auto snapshot = std::make_shared<std::vector<DX12Native::NativeRuntimeEmitter>>();
		snapshot->reserve(fullRuntimeLights.size());
		for (const auto& [identity, cached] : fullRuntimeLights)
			if (cached.active)
				snapshot->push_back({ cached.value, cached.placedReference, cached.shadowLight });
		fullRuntimeSnapshot = std::move(snapshot);
	}
	return true;
}

std::shared_ptr<const DX12Native::NativeLightPublication> DX12NativeScene::CollectFullRTLights(uint32_t frame)
{
	std::scoped_lock lock(runtimeLightMutex);
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto* cell = Util::GetPlayerCell();
	const uint32_t cellID = cell ? cell->GetFormID() : 0;
	const uint32_t scope = shadowLightScope.load(std::memory_order_acquire);
	if (!grid || !scope || !cellID || grid->requested.front().scope != scope)
		return {};
	if (fullRuntimeScope != scope || fullRuntimeCell != cellID) {
		ResetFullLightInventory();
		fullRuntimeScope = scope;
		fullRuntimeCell = cellID;
	}
	if (!fullObserved || DX12Native::NativeLightObservationDue(frame, fullLastObservation,
							 lightObservationFrames.load(std::memory_order_relaxed))) {
		fullObserved = ObserveFullRuntimeLights();
		fullLastObservation = frame;
	}
	if (!fullRuntimeSnapshot)
		return lightWorker.GetReady();
	const DX12Native::NativeLightSettings settings{
		runtimeUnificationRadius.load(std::memory_order_relaxed),
		preparedExclusionRadius.load(std::memory_order_relaxed),
		additivePreparedEmission.load(std::memory_order_relaxed),
		frameExactShadowLights.load(std::memory_order_relaxed)
	};
	const bool gridChanged = !fullRequestedGrid ||
	                         (fullRequestedGrid != grid && (fullRequestedGrid->resident != grid->resident ||
															   fullRequestedGrid->predictiveResident != grid->predictiveResident));
	const bool dirty = gridChanged || fullRequestedRuntime != fullRuntimeSnapshot ||
	                   fullRequestedSettings != settings || (lightWorker.NeedsRetry() && DX12Native::NativeLightObservationDue(frame, fullLastRequestFrame, lightObservationFrames.load(std::memory_order_relaxed)));
	// Promote the completed replacement before admitting another GPU upload.
	auto ready = lightWorker.GetReady();
	if (dirty && backend && lightWorker.Request(*backend, grid, fullRuntimeSnapshot, settings, cellID)) {
		fullRequestedGrid = grid;
		fullRequestedRuntime = fullRuntimeSnapshot;
		fullRequestedSettings = settings;
		fullLastRequestFrame = frame;
	} else if (!gridChanged) {
		fullRequestedGrid = grid;
	}
	return ready;
}

// Only opaque identities and frame-local origin values are submitted to native tracing.
void DX12NativeScene::ObserveScheduledShadowOrigin(const RE::NiLight* light, const RE::NiPoint3& origin)
{
	if (!frameExactShadowLights.load(std::memory_order_relaxed) || GetFrameNativeMode() != DX12::SCMNativeMode::FullRT ||
		!light || !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (scheduledOriginsFrame != frame) {
		scheduledOrigins.clear();
		scheduledOriginsFrame = frame;
	}
	if (scheduledOrigins.size() < DX12::kMaxPreparedLights)
		scheduledOrigins.try_emplace(reinterpret_cast<uintptr_t>(light), float3{ origin.x, origin.y, origin.z });
}

void DX12NativeScene::OnOpaqueRasterReady()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	if (!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	// Only capture the depth and camera used by the independent shadow layer.
	const auto mode = GetFrameNativeMode();
	const bool fullRT = mode == DX12::SCMNativeMode::FullRT;
	if (mode != DX12::SCMNativeMode::ShadowOffload && !fullRT)
		return;

	CS_GPU_PASS("DX12NativeScene::OpaqueRasterReady");
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	globals::features::upscaling.nativeComposite.RetireCompletedInputs();
	shadows.BeginFrame(frame);
	if (shadows.Get(frame))
		return;
	shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::None);

	std::shared_ptr<const DX12Native::NativeLightPublication> fullInventory;
	if (fullRT) {
		if (!backend) {
			std::string reason;
			backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
			if (!backend)
				return;
		}
		fullInventory = CollectFullRTLights(frame);
	}
	const auto built = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	if (!built || !built->acceleration || (fullRT ? !fullInventory : !lights) ||
		built->scope != shadowLightScope.load(std::memory_order_acquire))
		return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::NoScene);

	// Admit only matched runtime emitters that the SCM policy can account for.
	// Iterate the immutable prepared selection to preserve its deterministic order.
	std::vector<DX12::PreparedLight> liveLights;
	std::vector<const RE::NiLight*> liveSources;
	if (!fullRT) {
		std::scoped_lock lock(runtimeLightMutex);
		if (runtimeLightFrame != frame || runtimeLightSamples.empty())
			return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::NoLights);
		std::unordered_map<uint32_t, const RuntimeLightSample*> liveByReference;
		liveByReference.reserve(runtimeLightSamples.size());
		for (const auto& sample : runtimeLightSamples)
			if (sample.referenceID)
				liveByReference.try_emplace(sample.referenceID, &sample);
		const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
		liveLights.reserve(std::min(limit, liveByReference.size()));
		for (size_t i = 0; i < limit; ++i) {
			const auto it = liveByReference.find((*lights)[i].referenceID);
			if (it != liveByReference.end()) {
				liveLights.push_back(it->second->value);
				liveSources.push_back(it->second->source);
			}
		}
		for (const auto& sample : runtimeLightSamples)
			if (!sample.referenceID && liveLights.size() < DX12Native::kMaxShadowLights &&
				std::ranges::contains(ownedLights, sample.source)) {
				liveLights.push_back(sample.value);
				liveSources.push_back(sample.source);
			}
	}
	std::span<const DX12::PreparedLight> submittedLights = fullRT ?
	                                                           std::span<const DX12::PreparedLight>(fullInventory->lights) :
	                                                           std::span<const DX12::PreparedLight>(liveLights);
	uint32_t moving = 0;
	if (fullRT && frameExactShadowLights.load(std::memory_order_relaxed)) {
		std::scoped_lock lock(runtimeLightMutex);
		if (scheduledOriginsFrame == frame) {
			constexpr float kMinOriginShift = 0.25f;
			for (size_t i = 0; i < submittedLights.size(); ++i) {
				const auto it = scheduledOrigins.find(submittedLights[i].runtimeIdentity);
				if (it == scheduledOrigins.end())
					continue;
				const auto& p = submittedLights[i].position;
				const auto& q = it->second;
				const float dx = q.x - p.x, dy = q.y - p.y, dz = q.z - p.z;
				if (dx * dx + dy * dy + dz * dz < kMinOriginShift * kMinOriginShift)
					continue;
				if (!moving)
					lightsAtShadowOrigins.assign(submittedLights.begin(), submittedLights.end());
				lightsAtShadowOrigins[i].position = q;
				++moving;
			}
		}
		if (moving)
			submittedLights = lightsAtShadowOrigins;
	}
	const auto previousMoving = movingOriginCount.exchange(moving, std::memory_order_relaxed);
	// Until per-light history exists, reject the combined history on moving-origin frames.
	if (moving || previousMoving)
		globals::features::upscaling.nativeComposite.ResetShadowHistory();
	if (submittedLights.empty())
		return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::NoLights);

	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend)
			return;
	}

	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t viewportWidth = std::isfinite(viewport.x) && viewport.x > 0.0f ? uint32_t(viewport.x) : 0u;
	const uint32_t viewportHeight = std::isfinite(viewport.y) && viewport.y > 0.0f ? uint32_t(viewport.y) : 0u;
	if (!viewportWidth || !viewportHeight)
		return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::ViewportMismatch);
	raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, frame);
	const DX12Native::ShadowTraceControls controls{
		.viewportWidth = viewportWidth,
		.viewportHeight = viewportHeight,
		.quality = fullRT ? fullRTQuality : DX12Native::kShadowQualityOffloading,
		.captureDebugLights = WantsLightPreview(mode),
		.debugPreparedCount = fullRT ? fullInventory->preparedCount : 0u,
		.nativeLights = fullInventory,
		.uploadLights = moving != 0,
		.captureLightVisibility = true,
		.movers = &movers
	};
	shadows.Dispatch(*backend, raster.Get(frame), built, submittedLights, frame, controls);
	PublishLightShadowSlots(frame, submittedLights, liveSources);
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	// Observe visible static geometry already drawn by the existing opaque
	// render path. This does not change render-pass state or DX11 draw behavior.
	if (!loaded || !globals::features::dx12Control.IsActive() || globals::game::isVR ||
		!pass || !pass->geometry ||
		GetFrameNativeMode() == DX12::SCMNativeMode::Disabled ||
		!globals::features::upscaling.loaded ||
		!globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return nullptr;
	ObserveSCMRigidCaster(*pass->geometry, true);
	return nullptr;
}

void DX12NativeScene::ObserveSCMRigidCaster(RE::BSGeometry& geometry, bool requireStability)
{
	// SCM and already-submitted opaque render passes share this bounded
	// CPU-only capture path. No scene traversal, readback or GPU wait.
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		GetFrameNativeMode() == DX12::SCMNativeMode::Disabled)
		return;
	auto* tri = geometry.AsTriShape();
	if (!tri || geometry.AsDynamicTriShape())
		return;
	const auto& runtime = geometry.GetGeometryRuntimeData();
	if (runtime.skinInstance || runtime.alphaProperty || !runtime.rendererData ||
		!runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
		return;
	// Ignore effects, sky, water and other non-lighting draws; geometry only.
	const auto* property = runtime.shaderProperty.get();
	if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
		return;
	// A drawn BSTriShape often has no user data of its own; Skyrim keeps the
	// placed reference on an ancestor NiNode. Walk only the existing parent chain.
	bool ancestryRead = false;
	const RE::TESForm* reference = Util::FindSceneOwner(&geometry, false, &ancestryRead);
	if (!ancestryRead || (reference && (reference->IsDynamicForm() || reference->IsActor())))
		return;
	const auto* file = reference ? reference->GetFile(0) : nullptr;
	auto desc = runtime.vertexDesc;
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	const auto counts = tri->GetTrishapeRuntimeData();
	const uint32_t stride = desc.GetSize();
	if (stride < 16 || stride > 256 || !counts.vertexCount || !counts.triangleCount ||
		counts.vertexCount > 65535 || counts.triangleCount > 65535)
		return;

	DX12Native::RuntimeGeometryCandidate candidate;
	candidate.referenceKey = reference && file && !file->GetFilename().empty() ?
	                             std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename()) :
	                             std::format("render:{:016X}", reinterpret_cast<std::uintptr_t>(&geometry));
	candidate.shapeName = geometry.name.empty() ? "<unnamed>" : geometry.name.c_str();
	candidate.requiresStability = requireStability;
	candidate.objectIdentity = reinterpret_cast<std::uintptr_t>(&geometry);
	candidate.stride = stride;
	candidate.vertexCount = counts.vertexCount;
	candidate.triangleCount = counts.triangleCount;
	candidate.vertices = runtime.rendererData->rawVertexData;
	candidate.indices = runtime.rendererData->rawIndexData;
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			candidate.transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	candidate.transform[3] = geometry.world.translate.x;
	candidate.transform[7] = geometry.world.translate.y;
	candidate.transform[11] = geometry.world.translate.z;
	candidate.centerX = geometry.worldBound.center.x;
	candidate.centerY = geometry.worldBound.center.y;
	const uint32_t frame = globals::game::graphicsState ?
	                           globals::game::graphicsState->GetFrameCount() :
	                           0;
	geometryCapture.Observe(candidate, frame, globals::features::dx12Resources.GetGrid());
}

void DX12NativeScene::OnLoadingMenuEvent(bool opening)
{
	if (opening)
		loadingResetPending.store(true, std::memory_order_release);
}

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening)
		ApplyPendingLoadingReset();
}

void DX12NativeScene::ApplyPendingLoadingReset()
{
	if (!loadingResetPending.exchange(false, std::memory_order_acq_rel))
		return;
	frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
	frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	if (nativeSessionStarted) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			ownedLights.clear();
			ResetFullLightInventory();
		}
		globals::features::upscaling.nativeComposite.ResetShadowHistory();
		raster.Reset();
		shadows.Reset();
		movers.Reset();
		acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	loadingResetPending.store(false, std::memory_order_release);
	frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
	frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	frameExactShadowLights.store(false, std::memory_order_relaxed);
	movingOriginCount.store(0, std::memory_order_relaxed);
	if (!nativeSessionStarted)
		return;
	nativeSessionStarted = false;
	shadowLightScope.store(0, std::memory_order_release);
	shadowLights.store(nullptr, std::memory_order_release);
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		ownedLights.clear();
		ResetFullLightInventory();
	}
	globals::features::upscaling.nativeComposite.ResetShadowHistory();
	raster.Reset();
	shadows.Reset();
	movers.Reset();
	acceleration.Stop();
	geometryCapture.Reset();
}

bool DX12NativeScene::ObserveMover(RE::BSGeometry& a_geometry)
{
	if (!globals::game::graphicsState || !acceleration.GetScene())
		return false;
	return movers.Observe(a_geometry, globals::game::graphicsState->GetFrameCount());
}

bool DX12NativeScene::CasterInTlas(RE::BSGeometry& geometry) const
{
	const auto scene = acceleration.GetScene();
	if (!scene || !scene->inputs || scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;
	const auto* tri = geometry.AsTriShape();
	const auto& rt = geometry.GetGeometryRuntimeData();
	if (!tri || !rt.rendererData || rt.skinInstance || rt.alphaProperty || geometry.AsDynamicTriShape())
		return false;
	const auto counts = tri->GetTrishapeRuntimeData();
	auto desc = rt.vertexDesc;
	const DX12::CasterSource live{ reinterpret_cast<uintptr_t>(&geometry),
		reinterpret_cast<uintptr_t>(rt.rendererData->rawVertexData), reinterpret_cast<uintptr_t>(rt.rendererData->rawIndexData),
		counts.vertexCount, uint32_t(counts.triangleCount) * 3u, desc.GetSize(), 0 };
	std::array<float, 12> transform{};
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	transform[3] = geometry.world.translate.x;
	transform[7] = geometry.world.translate.y;
	transform[11] = geometry.world.translate.z;
	const auto [first, last] = scene->inputs->casterSources.equal_range(live.geometry);
	for (auto it = first; it != last; ++it) {
		const auto& shape = scene->inputs->geometry[it->second];
		if (DX12::SameCasterSource(shape->casterSource, live, shape->transform, transform))
			return true;
	}
	return false;
}

bool DX12NativeScene::ClaimCasterForTrace(RE::BSGeometry& geometry) const
{
	const bool covered = CasterInTlas(geometry);
	(covered ? castersTraced : castersKeptRaster).fetch_add(1, std::memory_order_relaxed);
	return covered;
}

json DX12NativeScene::GetStatus() const
{
	json status = { { "active", globals::features::dx12Control.IsActive() },
		{ "shadowOffloadMode", globals::features::dx12Control.IsShadowOffloadMode() },
		{ "frameNativeMode", magic_enum::enum_name(GetFrameNativeMode()) },
		{ "perLightVisibility", false } };
	if (const auto grid = globals::features::dx12Resources.GetGrid()) {
		json active = json::array();
		json predictive = json::object();
		for (const auto& tile : grid->resident) {
			if (!tile) {
				active.push_back(nullptr);
				continue;
			}
			json rejections = json::array();
			for (const auto& [reason, count] : tile->rejectionReasons)
				rejections.push_back({ { "reason", reason }, { "count", count } });
			active.push_back({ { "x", tile->key.x }, { "y", tile->key.y }, { "state", magic_enum::enum_name(tile->state) },
				{ "shapes", tile->shapes.size() }, { "lights", tile->lights.size() }, { "bytes", tile->bytes },
				{ "failure", tile->failureReason }, { "rejections", std::move(rejections) } });
		}
		for (const auto& tile : grid->predictiveResident) {
			const auto state = tile ? std::string(magic_enum::enum_name(tile->state)) : "Missing";
			predictive[state] = predictive.value(state, 0) + 1;
		}
		status["grid"] = { { "scope", grid->requested.front().scope }, { "size", grid->size },
			{ "revision", grid->geometryRevision }, { "runtimeShapes", grid->runtimeShapes.size() },
			{ "activeTiles", std::move(active) }, { "predictiveStates", std::move(predictive) } };
	}
	if (const auto scene = acceleration.GetScene(); scene && scene->acceleration && scene->inputs) {
		status["tlas"] = { { "published", true }, { "scope", scene->scope }, { "revision", scene->geometryRevision },
			{ "instances", scene->inputs->geometry.size() }, { "preparedShapes", scene->inputs->preparedShapes },
			{ "observedShapes", scene->inputs->observedShapes }, { "duplicateShapes", scene->inputs->duplicateShapes },
			{ "triangles", scene->inputs->triangles } };
	} else {
		status["tlas"] = { { "published", false } };
	}
	status["ownership"] = { { "granted", ownershipGranted.load(std::memory_order_relaxed) },
		{ "castersTraced", castersTraced.load(std::memory_order_relaxed) },
		{ "castersKeptRaster", castersKeptRaster.load(std::memory_order_relaxed) } };
	{
		std::scoped_lock lock(runtimeLightMutex);
		status["lights"] = { { "observedShadowLights", runtimeLightSamples.size() }, { "ownedShadowLights", ownedLights.size() } };
	}
	status["trace"] = { { "lastOutcome", magic_enum::enum_name(shadows.LastOutcome()) }, { "lights", shadows.LastLightCount() },
		{ "movingOrigins", movingOriginCount.load(std::memory_order_relaxed) },
		{ "producedVisibilitySlices", shadows.LastVisibilityCount() },
		{ "shadedLights", shadedLightCount.load(std::memory_order_relaxed) },
		{ "movers", movers.LastTraced() } };
	status["composite"] = { { "lastShadowResult", magic_enum::enum_name(DX12Native::lastCompositeShadowResult.load(std::memory_order_relaxed)) } };
	return status;
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_COMMAND("setMaskDebugView", "Developer-only native visibility preview. Params: enabled (bool).",
		[](Feature*, const json& args) {
			if (globals::state && globals::state->IsDeveloperMode())
				DX12Native::compositeShadowDebugView.store(args.value("enabled", false), std::memory_order_relaxed);
		});
	FEATURE_RENDER_QUERY("status", "Native shadow pipeline readiness, grid and published TLAS counts. Params: none.",
		[](const Feature* self, const json&) -> json { return static_cast<const DX12NativeScene*>(self)->GetStatus(); });
}
