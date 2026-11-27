#include "DX12NativeScene.h"

#include "../LightLimitFix/ShadowCasterMath.h"
#include "../Upscaling.h"
#include "DX12Control.h"
#include "DX12NativeComposite.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/M/MainMenu.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectLIGH.h"
#include "RE/U/UI.h"
#include "ScenePrepEngineBridge.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"
#include "Utils/PointLightFlags.h"
#include "Utils/SettingsControl.h"
#include "Utils/TracyZones.h"
#include "Utils/UI.h"
#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace
{
	constexpr uint32_t kMinFullShadowRadius = 0;
	constexpr uint32_t kMinLightObservationFrames = 1;
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DX12NativeScene::Settings::FullRTProfile,
	OutdoorShadowOffloading,
	ShadowQualityMode,
	DebugNativeLights,
	RuntimeUnificationRadius,
	PreparedExclusionRadius,
	AdditivePreparedEmission,
	LightObservationFrames)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DX12NativeScene::Settings,
	OutdoorShadowOffloading,
	DebugNativeLights,
	FullRT)

void DX12NativeScene::SyncRuntimeSettings() noexcept
{
	auto& profile = settings.FullRT;
	profile.ShadowQualityMode = DX12Native::SanitizeRTQuality(profile.ShadowQualityMode);
	profile.RuntimeUnificationRadius = DX12Native::SanitizeFullShadowRadius(profile.RuntimeUnificationRadius);
	profile.PreparedExclusionRadius = DX12Native::SanitizeFullShadowRadius(profile.PreparedExclusionRadius);
	profile.LightObservationFrames = std::clamp(profile.LightObservationFrames, 1u, DX12Native::kMaxLightObservationFrames);
	outdoorShadowOffloading.store(settings.OutdoorShadowOffloading, std::memory_order_relaxed);
	debugOffloadLights.store(settings.DebugNativeLights, std::memory_order_relaxed);
	outdoorFullRT.store(profile.OutdoorShadowOffloading, std::memory_order_relaxed);
	debugRTLights.store(profile.DebugNativeLights, std::memory_order_relaxed);
	fullRTQuality.store(profile.ShadowQualityMode, std::memory_order_relaxed);
	runtimeUnificationRadius.store(profile.RuntimeUnificationRadius, std::memory_order_relaxed);
	preparedExclusionRadius.store(profile.PreparedExclusionRadius, std::memory_order_relaxed);
	additivePreparedEmission.store(profile.AdditivePreparedEmission, std::memory_order_relaxed);
	lightObservationFrames.store(profile.LightObservationFrames, std::memory_order_relaxed);
}

void DX12NativeScene::DrawSettings()
{
	const auto& control = globals::features::dx12Control;
	bool changed = false;
	if (control.IsFullRTMode()) {
		const char* qualities[]{ T("menu.dx12.scene.rt_quality", "Quality"), T("menu.dx12.scene.rt_performance", "Performance") };
		int quality = settings.FullRT.ShadowQualityMode == DX12Native::kShadowQualityPerformance ? 1 : 0;
		if (ImGui::Combo(T("menu.dx12.scene.shadow_quality", "Shadow quality"), &quality, qualities, int(std::size(qualities)))) {
			settings.FullRT.ShadowQualityMode = quality == 0 ? DX12Native::kShadowQualityQuality : DX12Native::kShadowQualityPerformance;
			changed = true;
		}
		changed |= ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &settings.FullRT.OutdoorShadowOffloading);
	} else if (control.IsShadowOffloadMode()) {
		changed |= ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &settings.OutdoorShadowOffloading);
	}
	if (changed && !SettingsControl::IsReplicating())
		SyncRuntimeSettings();
}

void DX12NativeScene::DrawDevSettings()
{
	const auto& control = globals::features::dx12Control;
	if (!control.IsShadowOffloadMode() && !control.IsFullRTMode())
		return;
	bool changed = false;
	if (control.IsFullRTMode()) {
		auto& profile = settings.FullRT;
		changed |= ImGui::SliderScalar(T("menu.dx12.scene.runtime_unification_radius", "Runtime light unification radius (units)"), ImGuiDataType_U32,
			&profile.RuntimeUnificationRadius, &kMinFullShadowRadius, &DX12Native::kFullShadowRadiusMax, "%u", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::SliderScalar(T("menu.dx12.scene.prepared_exclusion_radius", "Prepared light exclusion radius (units)"), ImGuiDataType_U32,
			&profile.PreparedExclusionRadius, &kMinFullShadowRadius, &DX12Native::kFullShadowRadiusMax, "%u", ImGuiSliderFlags_AlwaysClamp);
		changed |= ImGui::Checkbox(T("menu.dx12.scene.additive_prepared_emission", "Add excluded runtime emission to prepared lights"), &profile.AdditivePreparedEmission);
		changed |= ImGui::SliderScalar(T("menu.dx12.scene.light_observation_interval", "Light observation interval (frames)"), ImGuiDataType_U32,
			&profile.LightObservationFrames, &kMinLightObservationFrames, &DX12Native::kMaxLightObservationFrames);
		std::scoped_lock lock(runtimeLightMutex);
		const auto stats = lightWorker.GetStats();
		ImGui::Text("%s", std::format("Light tasks: {} | Publications: {} | Unchanged: {} | Deferred: {} | Busy: {}",
							  stats.tasks, stats.publications, stats.unchanged, stats.deferred, stats.busy)
							  .c_str());
	}
	auto& preview = control.IsFullRTMode() ? settings.FullRT.DebugNativeLights : settings.DebugNativeLights;
	changed |= ImGui::Checkbox(T("menu.dx12.scene.debug_native_lights", "Preview native lights"), &preview);
	bool maskView = DX12Native::compositeShadowDebugView.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.debug_shadow_mask", "Show ray-traced shadow mask"), &maskView))
		DX12Native::compositeShadowDebugView.store(maskView, std::memory_order_relaxed);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("menu.dx12.scene.debug_shadow_mask_tooltip",
			"Replaces the frame with the composited shadow visibility: white is lit, black is shadowed, magenta has no matching mask sample."));
	if (changed && !SettingsControl::IsReplicating())
		SyncRuntimeSettings();
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
	settings = input;
	const auto profile = input.find("FullRT");
	if (profile == input.end() || !profile->is_object() || !profile->contains("AdditivePreparedEmission")) {
		if (settings.FullRT.RuntimeUnificationRadius == DX12Native::kLegacyRuntimeUnificationRadius)
			settings.FullRT.RuntimeUnificationRadius = DX12Native::kDefaultRuntimeUnificationRadius;
		if (settings.FullRT.PreparedExclusionRadius == DX12Native::kLegacyPreparedExclusionRadius)
			settings.FullRT.PreparedExclusionRadius = DX12Native::kDefaultPreparedExclusionRadius;
	}
	SyncRuntimeSettings();
	std::scoped_lock lock(runtimeLightMutex);
	ResetFullLightInventory();
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = settings;
}

void DX12NativeScene::RestoreDefaultSettings()
{
	settings = {};
	SyncRuntimeSettings();
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
bool DX12NativeScene::WantsDepthPrepassReadyHook() const { return WantsRenderPassHook(); }

void DX12NativeScene::PublishLightShadowSlots(uint32_t frame, std::span<const DX12::PreparedLight> submitted,
	std::span<const RE::NiLight* const> sources)
{
	slotsBySource.clear();
	slotsByReference.clear();
	slotPositions.clear();
	for (size_t i = 0; i < slotMatches.size(); ++i)
		slotMatches[i].store(slotMatchesThisFrame[i], std::memory_order_relaxed);
	slotMatchesThisFrame.fill(0);
	std::swap(slicedLights, slicedLightsThisFrame);
	slicedLightsThisFrame.clear();
	slicedLightsFrame = frame;
	{
		std::scoped_lock lock(shadowLightSlotsMutex);
		std::swap(shadowLightSlots, shadowLightSlotsThisFrame);
	}
	shadowLightSlotsThisFrame.clear();
	slotSpheres.clear();
	const auto traced = shadows.Get(frame);
	lightSlotFrame = UINT32_MAX;
	// Queue-side wait before any light is flagged: forward lighting must not read slices the trace has not written.
	if (!traced || !traced->lightMask || !backend ||
		FAILED(backend->d3d11Context->Wait(shadows.LightMaskFence().fence11.get(), traced->lightMaskValue)))
		return;
	lightSlotFrame = frame;
	for (uint32_t i = 0; i < traced->lightSlots && i < submitted.size(); ++i)
		slotSpheres.push_back({ submitted[i].position, submitted[i].radius });
	for (uint32_t i = 0; i < traced->lightSlots && i < submitted.size(); ++i) {
		if (i < sources.size()) {
			slotsBySource.try_emplace(sources[i], i);
			continue;
		}
		const auto& light = submitted[i];
		slotPositions.push_back({ light.position, i });
		if (light.runtimeIdentity)
			slotsBySource.try_emplace(reinterpret_cast<const RE::NiLight*>(light.runtimeIdentity), i);
		if (light.referenceID)
			slotsByReference.emplace(light.referenceID, i);
	}
}

std::optional<uint32_t> DX12NativeScene::NativeShadowSlot(const RE::NiLight* a_light, bool a_shadowLight)
{
	if (!a_light || !globals::game::graphicsState || lightSlotFrame != globals::game::graphicsState->GetFrameCount())
		return std::nullopt;
	std::optional<uint32_t> slot;
	auto match = SlotMatch::Source;
	const auto& p = a_light->world.translate;
	const auto distanceSquared = [&p](const float3& position) {
		const float dx = position.x - p.x, dy = position.y - p.y, dz = position.z - p.z;
		return dx * dx + dy * dy + dz * dz;
	};
	const float unifyRadius = float(runtimeUnificationRadius.load(std::memory_order_relaxed));
	if (const auto it = slotsBySource.find(a_light); it != slotsBySource.end())
		slot = it->second;
	else if (const auto* owner = a_light->GetUserData(); owner && !slotsByReference.empty()) {
		// A reference's other emitters sit elsewhere; only the one beside this light shadows it correctly.
		constexpr float kMinReferenceSlotReach = 64.0f;  // Placed and runtime positions differ slightly even when unification is off.
		const float reach = std::max(unifyRadius, kMinReferenceSlotReach);
		float best = reach * reach;
		const auto [first, last] = slotsByReference.equal_range(owner->GetFormID());
		for (auto ref = first; ref != last; ++ref)
			if (ref->second < slotSpheres.size())
				if (const float d2 = distanceSquared(slotSpheres[ref->second].first); d2 <= best) {
					best = d2;
					slot = ref->second;
					match = SlotMatch::Reference;
				}
	}
	if (!slot && !slotPositions.empty()) {
		match = SlotMatch::Proximity;
		// Full RT merges runtime emitters into the traced light they sit beside; shade them with its slice.
		float best = unifyRadius * unifyRadius;
		for (const auto& [position, index] : slotPositions)
			if (const float d2 = distanceSquared(position); d2 <= best) {
				best = d2;
				slot = index;
			}
	}
	if (a_shadowLight && std::ranges::none_of(shadowLightSlotsThisFrame, [a_light](const auto& s) { return s.light == a_light; })) {
		ShadowLightSlot entry{ a_light, slot ? match : SlotMatch::UnslottedShadowLight, 0.0f, a_light->GetLightRuntimeData().radius.x, 0.0f };
		if (slot && *slot < slotSpheres.size()) {
			entry.offset = std::sqrt(distanceSquared(slotSpheres[*slot].first));
			entry.tracedRadius = slotSpheres[*slot].second;
		}
		shadowLightSlotsThisFrame.push_back(entry);
	}
	if (slot) {
		lightsShadedFrame = lightSlotFrame;
		++slotMatchesThisFrame[size_t(match)];
		slicedLightsThisFrame.push_back(a_light);
	} else if (a_shadowLight) {
		++slotMatchesThisFrame[size_t(SlotMatch::UnslottedShadowLight)];
	}
	return slot;
}

bool DX12NativeScene::ShadedFromSlice(const RE::NiLight* a_light) const
{
	if (!a_light || !globals::game::graphicsState || slicedLightsFrame == UINT32_MAX)
		return false;
	// SCM schedules before this frame's trace publishes, so the previous frame's publication governs it.
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	return (slicedLightsFrame == frame || slicedLightsFrame + 1 == frame) && std::ranges::contains(slicedLights, a_light);
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
	if (const auto traced = shadows.Get(frame); traced && traced->lightMask && lightSlotFrame == frame)
		view = traced->lightMask->srv;
	globals::d3d::context->PSSetShaderResources(kLightShadowRegister, 1, &view);
}

void DX12NativeScene::PrepareMenuScene(std::shared_ptr<const DX12::Grid> grid)
{
	if (!grid || !grid->wholeCell || !globals::features::dx12Control.IsActive() || !AllowsNativeSceneWork() ||
		!globals::d3d::device || !globals::d3d::context)
		return;
	nativeSessionStarted = true;
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
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
		if (previous != mode) {
			std::scoped_lock lock(runtimeLightMutex);
			ResetFullLightInventory();
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
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
			const RE::NiAVObject* node = ni;
			for (uint32_t depth = 0; node && depth < 16; ++depth, node = node->parent) {
				if (auto* ref = node->GetUserData()) {
					owner = ref->GetFormID();
					auto* base = ref->GetObjectReference();
					if (base && base->As<RE::TESObjectLIGH>())
						placed = owner;
					return;
				}
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
	const auto collect = [&](RE::BSLight* light) {
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
		dirty |= cached.active != active;
		cached.active = active;
	};
	if (node) {
		const auto& scene = node->GetRuntimeData();
		for (const auto& light : scene.activeLights)
			collect(light.get());
		for (const auto& light : scene.activeShadowLights)
			collect(light.get());
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
				snapshot->push_back({ cached.value, cached.placedReference });
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
	const DX12Native::NativeLightSettings lightSettings{
		runtimeUnificationRadius.load(std::memory_order_relaxed),
		preparedExclusionRadius.load(std::memory_order_relaxed),
		additivePreparedEmission.load(std::memory_order_relaxed)
	};
	const bool gridChanged = !fullRequestedGrid ||
	                         (fullRequestedGrid != grid && (fullRequestedGrid->resident != grid->resident ||
															   fullRequestedGrid->predictiveResident != grid->predictiveResident));
	const bool dirty = gridChanged || fullRequestedRuntime != fullRuntimeSnapshot ||
	                   fullRequestedSettings != lightSettings || (lightWorker.NeedsRetry() && DX12Native::NativeLightObservationDue(frame, fullLastRequestFrame, lightObservationFrames.load(std::memory_order_relaxed)));
	// Promote the completed replacement before admitting another GPU upload.
	auto ready = lightWorker.GetReady();
	if (dirty && backend && lightWorker.Request(*backend, grid, fullRuntimeSnapshot, lightSettings, cellID)) {
		fullRequestedGrid = grid;
		fullRequestedRuntime = fullRuntimeSnapshot;
		fullRequestedSettings = lightSettings;
		fullLastRequestFrame = frame;
	} else if (!gridChanged) {
		fullRequestedGrid = grid;
	}
	return ready;
}

void DX12NativeScene::OnDepthPrepassReady()
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

	CS_GPU_PASS("DX12NativeScene::DepthPrepassTrace");
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	globals::features::upscaling.nativeComposite.RetireCompletedInputs();
	shadows.BeginFrame(frame);
	if (shadows.Get(frame))
		return;

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
		return;

	// Admit only matched runtime emitters that the SCM policy can account for.
	// Iterate the immutable prepared selection to preserve its deterministic order.
	std::vector<DX12::PreparedLight> liveLights;
	std::vector<const RE::NiLight*> liveSources;
	if (!fullRT) {
		std::scoped_lock lock(runtimeLightMutex);
		if (runtimeLightFrame != frame || runtimeLightSamples.empty())
			return;
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
	const std::span<const DX12::PreparedLight> submittedLights = fullRT ?
	                                                                 std::span<const DX12::PreparedLight>(fullInventory->lights) :
	                                                                 std::span<const DX12::PreparedLight>(liveLights);
	if (submittedLights.empty())
		return;

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
		return;
	raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, frame);
	const DX12Native::ShadowTraceControls controls{
		.viewportWidth = viewportWidth,
		.viewportHeight = viewportHeight,
		.quality = fullRT ? fullRTQuality.load(std::memory_order_relaxed) : DX12Native::kShadowQualityOffloading,
		.captureDebugLights = WantsLightPreview(mode),
		.debugPreparedCount = fullRT ? fullInventory->preparedCount : 0u,
		.nativeLights = fullInventory,
		.movers = &movers
	};
	tracedLightCount.store(uint32_t(submittedLights.size()), std::memory_order_relaxed);
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
	const RE::TESForm* reference = nullptr;
	RE::NiAVObject* node = &geometry;
	for (uint32_t depth = 0; node && depth < 16; ++depth, node = node->parent) {
		if (const auto* owner = node->GetUserData()) {
			reference = owner;
			break;
		}
	}
	if (reference && (reference->IsDynamicForm() || reference->IsActor()))
		return;
	const auto* file = reference ? reference->GetFile(0) : nullptr;
	auto desc = runtime.vertexDesc;
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	const auto counts = tri->GetTrishapeRuntimeData();
	const uint32_t stride = DX12::VertexStrideBytes(std::bit_cast<uint64_t>(desc));
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
	candidate.transform = DX12::TransformToRows3x4(DX12::FromNiTransform(geometry.world));
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
			ResetFullLightInventory();
			ownedLights.clear();
		}
		globals::features::upscaling.nativeComposite.ResetShadowHistory();
		raster.Reset();
		shadows.Reset();
		movers.Reset();
		if (!globals::features::dx12Resources.HasMenuPreload())
			acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	loadingResetPending.store(false, std::memory_order_release);
	frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
	frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	if (!nativeSessionStarted)
		return;
	nativeSessionStarted = false;
	shadowLightScope.store(0, std::memory_order_release);
	shadowLights.store(nullptr, std::memory_order_release);
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		ResetFullLightInventory();
		ownedLights.clear();
	}
	globals::features::upscaling.nativeComposite.ResetShadowHistory();
	raster.Reset();
	shadows.Reset();
	movers.Reset();
	acceleration.Stop();
	geometryCapture.Reset();
}

namespace
{
	json TileStatus(const DX12::Tile& a_tile)
	{
		json rejections = json::array();
		for (const auto& [reason, count] : a_tile.rejectionReasons)
			rejections.push_back({ { "reason", reason }, { "count", count } });
		return { { "x", a_tile.key.x }, { "y", a_tile.key.y }, { "state", magic_enum::enum_name(a_tile.state) },
			{ "shapes", a_tile.shapes.size() }, { "lights", a_tile.lights.size() }, { "bytes", a_tile.bytes },
			{ "failure", a_tile.failureReason }, { "rejections", std::move(rejections) } };
	}

	json TimingStatus(const DX12::PreparationTiming& a_timing)
	{
		return { { "completed", a_timing.completed }, { "averageMs", a_timing.AverageMs() }, { "worstMs", a_timing.worstMs } };
	}
}

bool DX12NativeScene::ObserveMover(RE::BSGeometry& a_geometry)
{
	if (!globals::game::graphicsState || !acceleration.GetScene())
		return false;
	return movers.Observe(a_geometry, globals::game::graphicsState->GetFrameCount());
}

bool DX12NativeScene::CasterInTlas(const RE::BSGeometry& a_geometry) const
{
	const auto scene = acceleration.GetScene();
	if (!scene || !scene->inputs)
		return false;
	auto casters = tlasCasters.load(std::memory_order_acquire);
	if (!casters || casters->scene != scene) {
		auto rebuilt = std::make_shared<TlasCasters>();
		rebuilt->scene = scene;
		for (const auto& shape : scene->inputs->geometry)
			if (shape && shape->sourceGeometry)
				rebuilt->vertices.emplace(shape->sourceGeometry, shape->sourceVertices);
		tlasCasters.store(rebuilt, std::memory_order_release);
		casters = std::move(rebuilt);
	}
	const auto* renderer = a_geometry.GetGeometryRuntimeData().rendererData;
	const auto found = casters->vertices.find(reinterpret_cast<uintptr_t>(&a_geometry));
	const bool covered = renderer && found != casters->vertices.end() &&
	                     found->second == reinterpret_cast<uintptr_t>(renderer->rawVertexData);
	(covered ? castersTraced : castersKeptRaster).fetch_add(1, std::memory_order_relaxed);
	return covered;
}

json DX12NativeScene::GetStatus() const
{
	json status = { { "active", globals::features::dx12Control.IsActive() },
		{ "shadowOffloadMode", globals::features::dx12Control.IsShadowOffloadMode() },
		{ "frameNativeMode", magic_enum::enum_name(GetFrameNativeMode()) } };

	json activeTiles = json::array();
	json predictive = json::object();
	if (const auto grid = globals::features::dx12Resources.GetGrid()) {
		for (const auto& tile : grid->resident)
			activeTiles.push_back(tile ? TileStatus(*tile) : json(nullptr));
		for (const auto& tile : grid->predictiveResident) {
			const std::string state = tile ? std::string(magic_enum::enum_name(tile->state)) : "Missing";
			predictive[state] = predictive.value(state, 0) + 1;
		}
		status["grid"] = { { "scope", grid->requested.front().scope }, { "size", grid->size },
			{ "revision", grid->geometryRevision }, { "runtimeShapes", grid->runtimeShapes.size() },
			{ "activeTiles", std::move(activeTiles) }, { "predictiveStates", std::move(predictive) } };
	}

	if (const auto scene = acceleration.GetScene(); scene && scene->acceleration) {
		size_t readyTiles = 0;
		if (scene->inputs && scene->inputs->residency)
			for (const auto& tile : scene->inputs->residency->resident)
				readyTiles += tile && tile->state == DX12::Tile::State::Ready;
		status["tlas"] = { { "published", true }, { "scope", scene->scope }, { "revision", scene->geometryRevision },
			{ "instances", scene->bottoms.size() }, { "readyActiveTiles", readyTiles } };
	} else {
		status["tlas"] = { { "published", false } };
	}

	const auto lights = shadowLights.load(std::memory_order_acquire);
	{
		std::scoped_lock lock(runtimeLightMutex);
		const uint32_t frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0;
		const bool current = runtimeLightFrame == frame || runtimeLightFrame + 1 == frame;
		status["lights"] = { { "prepared", lights ? lights->size() : 0 },
			{ "observedShadowLights", current ? runtimeLightSamples.size() : 0 },
			{ "ownedShadowLights", current ? ownedLights.size() : 0 } };
	}
	status["ownership"] = { { "granted", ownershipGranted.load(std::memory_order_relaxed) },
		{ "castersTraced", castersTraced.load(std::memory_order_relaxed) },
		{ "castersKeptRaster", castersKeptRaster.load(std::memory_order_relaxed) } };

	status["trace"] = { { "lastOutcome", magic_enum::enum_name(shadows.LastOutcome()) },
		{ "lights", tracedLightCount.load(std::memory_order_relaxed) },
		{ "shadedLights", slotMatches[size_t(SlotMatch::Source)].load(std::memory_order_relaxed) +
							  slotMatches[size_t(SlotMatch::Reference)].load(std::memory_order_relaxed) +
							  slotMatches[size_t(SlotMatch::Proximity)].load(std::memory_order_relaxed) },
		{ "slotMatches", { { "source", slotMatches[size_t(SlotMatch::Source)].load(std::memory_order_relaxed) },
							 { "reference", slotMatches[size_t(SlotMatch::Reference)].load(std::memory_order_relaxed) },
							 { "proximity", slotMatches[size_t(SlotMatch::Proximity)].load(std::memory_order_relaxed) } } },
		{ "unslottedShadowLights", slotMatches[size_t(SlotMatch::UnslottedShadowLight)].load(std::memory_order_relaxed) },
		{ "shadowLightSlots", [this] {
			 std::scoped_lock lock(shadowLightSlotsMutex);
			 json rows = json::array();
			 for (const auto& s : shadowLightSlots)
				 rows.push_back({ { "match", magic_enum::enum_name(s.match) }, { "offset", s.offset },
					 { "radius", s.lightRadius }, { "tracedRadius", s.tracedRadius } });
			 return rows;
		 }() },
		{ "movers", movers.LastTraced() } };
	status["composite"] = { { "lastShadowResult",
		magic_enum::enum_name(DX12Native::lastCompositeShadowResult.load(std::memory_order_relaxed)) } };

	if (const auto stats = globals::features::dx12Resources.GetPreparationStats())
		status["preparation"] = { { "active", TimingStatus(stats->active) }, { "predictive", TimingStatus(stats->predictive) },
			{ "corners", TimingStatus(stats->corners) } };
	return status;
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_COMMAND("setMaskDebugView",
		"Writes the native shadow mask into the frame instead of shading it: grayscale = traced visibility, magenta = no valid mask, "
		"red = traced but rejected by the receiver-depth check. Diagnostic only, not saved. Params: enabled (bool).",
		[](Feature*, const json& a_args) { DX12Native::compositeShadowDebugView.store(a_args.value("enabled", false), std::memory_order_relaxed); });
	FEATURE_RENDER_QUERY("status",
		"DX12 shadow offload readiness. Returns active/shadowOffloadMode/canSuppressStatic; grid {scope,size,revision,runtimeShapes,"
		"activeTiles[{x,y,state,shapes,lights,bytes,failure,rejections[{reason,count}]}],predictiveStates}; "
		"tlas {published,scope,revision,instances,readyActiveTiles}; lights {prepared,observedShadowLights,ownedShadowLights} for the latest frame; "
		"ownership {granted (light-frames owned), castersTraced (static casters dropped from raster because the TLAS holds them), castersKeptRaster (static casters of owned lights the TLAS lacks)} "
		"cumulative since boot (diff two reads for a rate); trace {lastOutcome} and composite {lastShadowResult} name the stage where RT shadows stop; "
		"preparation timings. Params: none.",
		[](const Feature* a_self, const json&) -> json { return static_cast<const DX12NativeScene*>(a_self)->GetStatus(); });
}
