#include "DX12NativeScene.h"

#include "../Upscaling.h"
#include "DX12Control.h"
#include "DX12CasterCoverage.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/M/MainMenu.h"
#include "RE/U/UI.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/PointLightFlags.h"
#include "RE/T/TESObjectLIGH.h"
#include "../LightLimitFix/ShadowCasterMath.h"
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>

void DX12NativeScene::DrawSettings()
{
	ImGui::Checkbox(T("menu.dx12.scene.independent_shadowmask", "Independent shadowmask"), &shadowMaskEnabled);
	const bool fullShadow = globals::features::dx12Control.IsFullShadowMode();
	if (fullShadow) {
		int unify = int(runtimeUnificationRadius.load(std::memory_order_relaxed));
		int exclude = int(preparedExclusionRadius.load(std::memory_order_relaxed));
		if (ImGui::SliderInt(T("menu.dx12.scene.runtime_unification_radius", "Runtime light unification radius (units)"), &unify, 0, int(DX12Native::kFullShadowRadiusMax), "%d", ImGuiSliderFlags_AlwaysClamp))
			runtimeUnificationRadius.store(DX12Native::SanitizeFullShadowRadius(uint32_t(unify)), std::memory_order_relaxed);
		if (ImGui::SliderInt(T("menu.dx12.scene.prepared_exclusion_radius", "Prepared light exclusion radius (units)"), &exclude, 0, int(DX12Native::kFullShadowRadiusMax), "%d", ImGuiSliderFlags_AlwaysClamp))
			preparedExclusionRadius.store(DX12Native::SanitizeFullShadowRadius(uint32_t(exclude)), std::memory_order_relaxed);
		bool additive = additivePreparedEmission.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.additive_prepared_emission", "Add excluded runtime emission to prepared lights"), &additive))
			additivePreparedEmission.store(additive, std::memory_order_relaxed);
		ImGui::TextDisabled("%s", T("menu.dx12.scene.stationary_full_lights", "Stationary native emitters (enforced)"));
	}
	ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoorShadowOffloading);
	bool debug = debugNativeLights.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.debug_native_lights", "Debug: label submitted native lights"), &debug))
		debugNativeLights.store(debug, std::memory_order_relaxed);
	ImGui::BeginDisabled(fullShadow);
	bool keepRasterStatic = !offloadStaticShadows;
	if (ImGui::Checkbox(T("menu.dx12.scene.keep_raster_static", "Keep raster static shadows for native-accounted lights"), &keepRasterStatic))
		offloadStaticShadows = !keepRasterStatic;
	bool retainFlickeringRaster = !offloadFlickeringStatic;
	if (ImGui::Checkbox(T("menu.dx12.scene.keep_raster_flickering", "Keep raster static shadows for flickering lights"), &retainFlickeringRaster))
		offloadFlickeringStatic = !retainFlickeringRaster;
	if (ImGui::Checkbox(T("menu.dx12.scene.freeze_flickering_native", "Freeze flickering native shadows (DX12 only)"), &freezeNativeFlickering)) {
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	ImGui::EndDisabled();
	const char* qualities[] = {
		T("menu.dx12.scene.quality.full", "Full"),
		T("menu.dx12.scene.quality.quality", "Quality"),
		T("menu.dx12.scene.quality.medium", "Medium"),
		T("menu.dx12.scene.quality.low", "Low"),
		T("menu.dx12.scene.quality.potato", "Potato")
	};
	static_assert(std::size(qualities) == DX12Native::kShadowQualityCount);
	int mode = static_cast<int>(qualityMode);
	if (ImGui::Combo(T("menu.dx12.scene.shadow_quality", "Shadow quality"), &mode, qualities, int(std::size(qualities))))
		qualityMode = static_cast<uint32_t>(mode);
	ImGui::Checkbox(T("menu.dx12.scene.shadow_stochastic", "Stochastic shadow emitter sampling"), &shadowStochastic);
	ImGui::BeginDisabled(!shadowStochastic);
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_emitter_multiplier", "Shadow emitter radius multiplier"), &shadowEmitterRadiusMultiplier,
		0.0f, DX12Native::kMaximumShadowEmitterRadiusMultiplier, "%.2fx");
	ImGui::EndDisabled();
	ImGui::Checkbox(T("menu.dx12.scene.exclude_near_light", "Exclude shadows near light geometry"), &nearLightShadowExclusion);
	ImGui::BeginDisabled(!nearLightShadowExclusion);
	int nearUnits = int(nearLightExclusionUnits);
	if (ImGui::SliderInt(T("menu.dx12.scene.exclusion_radius_units", "Light-centered exclusion radius (units)"), &nearUnits, 0, 512))
		nearLightExclusionUnits = uint32_t(std::clamp(((nearUnits + 2) / 4) * 4, 0, 512));
	ImGui::EndDisabled();
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_reach_multiplier", "Shadow reach multiplier (1x = vanilla)"), &shadowReachMultiplier,
		0.1f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
	float fade = shadowFadeRange;
	if (ImGui::SliderFloat(T("menu.dx12.scene.shadow_fade_fraction", "Shadow fade range fraction"), &fade, 0.0f, 1.0f, "%.2f"))
		shadowFadeRange = std::round(fade * 50.0f) / 50.0f;
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_falloff_curve", "Shadow falloff curve"), &shadowFalloffCurve, 0.0f, 2.5f, "%.2f");
}

bool DX12NativeScene::IsOverlayVisible() const
{
	return loaded && globals::features::dx12Control.IsActive() && !globals::game::isVR &&
	       debugNativeLights.load(std::memory_order_relaxed);
}

void DX12NativeScene::DrawOverlay()
{
	if (!IsOverlayVisible() || !globals::game::graphicsState || !CanOffloadShadowsInCurrentCell())
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
	for (const auto& light : mask->debugLights) {
		float2 uv;
		if (!Util::GetWorldFocusCoord(RE::NiPoint3{ light.position.x, light.position.y, light.position.z }, uv))
			continue;
		const ImVec2 point{ viewport->Pos.x + uv.x * viewport->Size.x, viewport->Pos.y + uv.y * viewport->Size.y };
		const auto label = light.unifiedCount > 1 ?
			std::format("DXR U x{} {:08X}", light.unifiedCount, light.referenceID) :
			(light.runtimeIdentity ? std::format("DXR R {:08X}/{:X}", light.referenceID, light.runtimeIdentity) :
				std::format("DXR P {:08X}", light.referenceID));
		const ImVec2 text{ point.x + labelOffset, point.y - ImGui::GetTextLineHeight() * 0.5f };
		const auto size = ImGui::CalcTextSize(label.c_str());
		draw->AddCircleFilled(point, markerRadius, color);
		draw->AddRectFilled(text, ImVec2(text.x + size.x, text.y + size.y), background);
		draw->AddText(text, color, label.c_str());
	}
}

void DX12NativeScene::LoadSettings(json& input)
{
	shadowMaskEnabled = input.value("ShadowMask", true);
	globals::features::dx12Control.MigrateLegacyFullShadowMode(input.value("TraceAllPreparedLights", false));
	additivePreparedEmission.store(input.value("AdditivePreparedEmission", false), std::memory_order_relaxed);
	uint32_t unify = input.value("RuntimeUnificationRadius", DX12Native::kDefaultRuntimeUnificationRadius);
	uint32_t exclude = input.value("PreparedExclusionRadius", DX12Native::kDefaultPreparedExclusionRadius);
	if (!input.contains("AdditivePreparedEmission")) {
		if (unify == DX12Native::kLegacyRuntimeUnificationRadius)
			unify = DX12Native::kDefaultRuntimeUnificationRadius;
		if (exclude == DX12Native::kLegacyPreparedExclusionRadius)
			exclude = DX12Native::kDefaultPreparedExclusionRadius;
	}
	runtimeUnificationRadius.store(DX12Native::SanitizeFullShadowRadius(unify), std::memory_order_relaxed);
	preparedExclusionRadius.store(DX12Native::SanitizeFullShadowRadius(exclude), std::memory_order_relaxed);
	debugNativeLights.store(input.value("DebugNativeLights", false), std::memory_order_relaxed);
	outdoorShadowOffloading = input.value("OutdoorShadowOffloading", false);
	offloadStaticShadows = input.value("OffloadSCMStatic", true);
	offloadFlickeringStatic = input.value("OffloadFlickeringStatic", true);
	qualityMode = input.value("ShadowQualityMode", DX12Native::kDefaultShadowQualityMode);
	if (qualityMode >= DX12Native::kShadowQualityCount)
		qualityMode = DX12Native::kDefaultShadowQualityMode;
	freezeNativeFlickering = input.value("FreezeNativeFlickering", kDefaultFreezeNativeFlickering);
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	nearLightShadowExclusion = input.value("ShadowNearLightExclusion", true);
	nearLightExclusionUnits = std::clamp(input.value("ShadowNearLightRadius", 16u), 0u, 512u) / 4u * 4u;
	shadowReachMultiplier = std::clamp(input.value("ShadowReachMultiplier", DX12Native::kDefaultShadowReachMultiplier), 0.1f, 64.0f);
	shadowFadeRange = std::round(std::clamp(input.value("ShadowFadeRange", DX12Native::kDefaultShadowFadeRange), 0.0f, 1.0f) * 50.0f) / 50.0f;
	shadowFalloffCurve = std::clamp(input.value("ShadowFalloffCurve", 1.0f), 0.0f, 2.5f);
	shadowStochastic = input.value("ShadowStochastic", true);
	const float emitterMultiplier = input.value("ShadowEmitterRadiusMultiplier", DX12Native::kDefaultShadowEmitterRadiusMultiplier);
	shadowEmitterRadiusMultiplier = std::isfinite(emitterMultiplier) ?
	                                    std::clamp(emitterMultiplier, 0.0f, DX12Native::kMaximumShadowEmitterRadiusMultiplier) :
	                                    DX12Native::kDefaultShadowEmitterRadiusMultiplier;
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "ShadowMask", shadowMaskEnabled }, { "DebugNativeLights", debugNativeLights.load(std::memory_order_relaxed) }, { "OutdoorShadowOffloading", outdoorShadowOffloading }, { "ShadowQualityMode", qualityMode },
		{ "RuntimeUnificationRadius", runtimeUnificationRadius.load(std::memory_order_relaxed) },
		{ "PreparedExclusionRadius", preparedExclusionRadius.load(std::memory_order_relaxed) },
		{ "AdditivePreparedEmission", additivePreparedEmission.load(std::memory_order_relaxed) },
		{ "OffloadSCMStatic", offloadStaticShadows },
		{ "OffloadFlickeringStatic", offloadFlickeringStatic },
		{ "FreezeNativeFlickering", freezeNativeFlickering },
		{ "ShadowNearLightExclusion", nearLightShadowExclusion },
		{ "ShadowNearLightRadius", nearLightExclusionUnits },
		{ "ShadowReachMultiplier", shadowReachMultiplier },
		{ "ShadowFadeRange", shadowFadeRange },
		{ "ShadowFalloffCurve", shadowFalloffCurve },
		{ "ShadowStochastic", shadowStochastic },
		{ "ShadowEmitterRadiusMultiplier", shadowEmitterRadiusMultiplier } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	shadowMaskEnabled = true;
	additivePreparedEmission.store(false, std::memory_order_relaxed);
	runtimeUnificationRadius.store(DX12Native::kDefaultRuntimeUnificationRadius, std::memory_order_relaxed);
	preparedExclusionRadius.store(DX12Native::kDefaultPreparedExclusionRadius, std::memory_order_relaxed);
	debugNativeLights.store(false, std::memory_order_relaxed);
	outdoorShadowOffloading = false;
	offloadStaticShadows = true;
	offloadFlickeringStatic = true;
	qualityMode = DX12Native::kDefaultShadowQualityMode;
	freezeNativeFlickering = kDefaultFreezeNativeFlickering;
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	nearLightShadowExclusion = true;
	nearLightExclusionUnits = 16;
	shadowReachMultiplier = DX12Native::kDefaultShadowReachMultiplier;
	shadowFadeRange = DX12Native::kDefaultShadowFadeRange;
	shadowFalloffCurve = 1.0f;
	shadowStochastic = true;
	shadowEmitterRadiusMultiplier = DX12Native::kDefaultShadowEmitterRadiusMultiplier;
}

bool DX12NativeScene::CanOffloadShadowsInCurrentCell() const
{
	if (globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
		return false;
	const auto* cell = Util::GetPlayerCell();
	return cell && (!cell->IsExteriorCell() || outdoorShadowOffloading);
}

bool DX12NativeScene::CanSuppressSCMStatic() const
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!globals::features::dx12Control.IsShadowOffloadMode() || !shadowMaskEnabled)
		return false;
	if (TraceAllPreparedLights()) {
		const auto* cell = Util::GetPlayerCell();
		return !cell || !cell->IsExteriorCell() || outdoorShadowOffloading;
	}
	return offloadStaticShadows && CanOffloadShadowsInCurrentCell() &&
	       globals::features::dx12Control.GetShadowStrength() > 0.0f &&
	       globals::features::dx12Control.GetShadowBlend() > 0.0f &&
	       previousShadowSubmitted.load(std::memory_order_acquire);
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// The default mode requires an exact runtime light match; all-light mode
	// deliberately trades raster coverage guarantees for lower CPU overhead.
	if (!light || slot < 0 || light->GetIsFrustumOrDirectionalLight())
		return false;
	if (TraceAllPreparedLights())
		return AllLightStaticReady();
	if (!CanSuppressSCMStatic() || !globals::game::graphicsState)
		return false;
	const auto scene = GetFrameShadowScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !lights || lights->empty() || !ni ||
		scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame)
		return false;

	const auto found = std::ranges::find_if(runtimeLightSamples, [ni](const RuntimeLightSample& sample) {
		return sample.source == ni;
	});
	if (found == runtimeLightSamples.end() || found->value.fade <= 0.0f ||
		(found->flickers && !offloadFlickeringStatic))
		return false;

	// The shader admits only the published prepared lights. Never suppress a
	// raster static caster for a light not actually selected for DX12 tracing.
	const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
	for (size_t i = 0; i < limit; ++i) {
		const auto& candidate = (*lights)[i];
		if (candidate.referenceID != 0 && candidate.referenceID == found->referenceID)
			return true;
	}
	return false;
}

std::shared_ptr<const DX12Native::TopLevel> DX12NativeScene::GetFrameShadowScene() const
{
	if (!globals::game::graphicsState ||
		frameShadowSceneID.load(std::memory_order_acquire) != globals::game::graphicsState->GetFrameCount())
		return {};
	return frameShadowScene.load(std::memory_order_acquire);
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool flickers, const RE::NiPoint3& restOrigin)
{
	if (TraceAllPreparedLights() || !loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!light || light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return;
	auto* ni = light->light.get();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	if (!ni || !lights)
		return;
	const auto& rt = ni->GetLightRuntimeData();
	const auto& position = ni->world.translate;
	if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
		!std::isfinite(rt.fade) || !std::isfinite(rt.diffuse.red) ||
		!std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
		!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
		return;

	const DX12::PreparedLight* matched = nullptr;
	if (referenceID != 0) {
		const auto it = std::ranges::find_if(*lights, [referenceID](const auto& candidate) {
			return candidate.referenceID == referenceID;
		});
		if (it == lights->end())
			return;  // Never let a different nearby light inherit this source's animation.
		matched = &*it;
	} else {
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
	if (!matched)
		return;
	DX12::PreparedLight live = *matched;
	live.position = float3{ position.x, position.y, position.z };
	live.radius = rt.radius.x;
	live.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
	live.fade = rt.fade;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame) {
		runtimeLightFrame = frame;
		runtimeLightSamples.clear();
	}
	if (freezeNativeFlickering && flickers && matched->referenceID != 0) {
		// Keep DX11's flicker intact. Only the native shadow has a stable light.
		live.position = float3{ restOrigin.x, restOrigin.y, restOrigin.z };
		live = frozenNativeLights.try_emplace(matched->referenceID, live).first->second;
	}
	// A prepared emitter must not accidentally account for two different SCM
	// lights that happen to occupy the same position.
	const auto it = std::ranges::find_if(runtimeLightSamples, [ni](const auto& sample) {
		return sample.source == ni;
	});
	if (it != runtimeLightSamples.end()) {
		it->referenceID = matched->referenceID;
		it->value = live;
		it->flickers = flickers;
	} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights &&
			   !std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
				   return sample.referenceID == matched->referenceID;
			   })) {
		runtimeLightSamples.push_back({ ni, matched->referenceID, live, flickers });
	}
}

bool DX12NativeScene::WantsRenderPassHook() const
{
	return globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

bool DX12NativeScene::WantsPreShadowMapsHook() const { return WantsRenderPassHook(); }
bool DX12NativeScene::WantsOpaqueRasterReadyHook() const { return WantsRenderPassHook(); }

void DX12NativeScene::PrepareMenuScene(std::shared_ptr<const DX12::Grid> grid)
{
	if (!grid || !grid->wholeCell || !globals::features::dx12Control.IsActive() ||
		!globals::d3d::device || !globals::d3d::context)
		return;
	nativeSessionStarted = true;
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
}

void DX12NativeScene::PreShadowMaps()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive()) {
		OnRuntimeDisabled();
		return;
	}
	if (!globals::state || !globals::state->startupMenuInitializationComplete.load(std::memory_order_acquire) ||
		(globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME)))
		return;
	nativeSessionStarted = true;
	if (globals::game::graphicsState) {
		const uint32_t frame = globals::game::graphicsState->GetFrameCount();
		if (frameShadowSceneID.load(std::memory_order_acquire) != frame) {
			const bool fullShadow = globals::features::dx12Control.IsFullShadowMode();
			if (frameTraceAllLights.exchange(fullShadow, std::memory_order_acq_rel) && !fullShadow) {
				std::scoped_lock lock(runtimeLightMutex);
				ResetFullLightInventory();
			}
			previousShadowSubmitted.store(shadows.Get(frame - 1u) != nullptr, std::memory_order_release);
			frameShadowScene.store(acceleration.GetScene(), std::memory_order_release);
			frameShadowSceneID.store(frame, std::memory_order_release);
		}
	}
	allLightStaticReady.store(TraceAllPreparedLights() && CanSuppressSCMStatic(), std::memory_order_release);
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || !grid->requested.front().scope) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		frameShadowScene.store(nullptr, std::memory_order_release);
		return;
	}
	shadowLightScope.store(grid->requested.front().scope, std::memory_order_release);
	if (auto scene = GetFrameShadowScene(); scene && scene->scope != grid->requested.front().scope)
		frameShadowScene.store(nullptr, std::memory_order_release);
	// SCM reads a single per-frame publication without re-enumerating tiles
	// for every shadow light; the same ordered sources feed RayQuery.
	const auto lights = std::make_shared<const std::vector<DX12::PreparedLight>>(
		globals::features::dx12Resources.GetPreparedLights());
	shadowLights.store(lights, std::memory_order_release);
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
	fullRuntimeLights.clear();
	fullPreparedSnapshot.clear();
	fullConsolidatedLights.reset();
	fullRuntimeScope = 0;
	fullLightVisit = 0;
}

std::shared_ptr<const std::vector<DX12::PreparedLight>> DX12NativeScene::CollectFullShadowLights(std::span<const DX12::PreparedLight> prepared)
{
	std::scoped_lock lock(runtimeLightMutex);
	const uint32_t scope = shadowLightScope.load(std::memory_order_acquire);
	if (fullRuntimeScope != scope) {
		ResetFullLightInventory();
		fullRuntimeScope = scope;
	}
	const uint32_t unify = runtimeUnificationRadius.load(std::memory_order_relaxed);
	const uint32_t exclude = preparedExclusionRadius.load(std::memory_order_relaxed);
	const bool additive = additivePreparedEmission.load(std::memory_order_relaxed);
	const bool preparedChanged = prepared.size() != fullPreparedSnapshot.size() ||
		!std::equal(prepared.begin(), prepared.end(), fullPreparedSnapshot.begin(),
			DX12Native::SamePreparedShadowLight<DX12::PreparedLight>);
	bool dirty = !fullConsolidatedLights || preparedChanged || unify != fullCachedUnificationRadius || exclude != fullCachedExclusionRadius || additive != fullCachedAdditiveEmission;
	if (preparedChanged)
		fullPreparedSnapshot.assign(prepared.begin(), prepared.end());

	const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
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
	if (!dirty)
		return fullConsolidatedLights;

	std::unordered_set<uint32_t> preparedIDs;
	for (const auto& light : prepared)
		if (light.referenceID)
			preparedIDs.insert(light.referenceID);
	std::vector<DX12::PreparedLight> runtime;
	runtime.reserve(fullRuntimeLights.size());
	for (const auto& [identity, cached] : fullRuntimeLights) {
		if (cached.active && (!cached.placedReference || !preparedIDs.contains(cached.placedReference)))
			runtime.push_back(cached.value);
	}
	fullConsolidatedLights = std::make_shared<const std::vector<DX12::PreparedLight>>(
		DX12Native::ConsolidateFullShadowLights<DX12::PreparedLight>(prepared, runtime, unify, exclude, additive));
	fullCachedUnificationRadius = unify;
	fullCachedExclusionRadius = exclude;
	fullCachedAdditiveEmission = additive;
	return fullConsolidatedLights;
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
	if (!shadowMaskEnabled || !globals::features::dx12Control.IsShadowOffloadMode() ||
		!CanOffloadShadowsInCurrentCell())
		return;

	CS_GPU_PASS("DX12NativeScene::OpaqueRasterReady");
	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend) {
			return;
		}
	}

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, frame);
	// The active view may occupy only a subrect of the full-size raster allocation.
	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t viewportWidth = std::isfinite(viewport.x) && viewport.x > 0.0f ? uint32_t(viewport.x) : 0u;
	const uint32_t viewportHeight = std::isfinite(viewport.y) && viewport.y > 0.0f ? uint32_t(viewport.y) : 0u;
	{
		const auto captured = raster.Get(frame);
		const auto built = GetFrameShadowScene();
		const auto lights = shadowLights.load(std::memory_order_acquire);
		// The native trace handles unavailable inputs without affecting raster
		// rendering. RayQuery dispatch remains in DX12NativeShadowTrace.
		const DX12Native::ShadowTraceControls controls{
			.viewportWidth = viewportWidth,
			.viewportHeight = viewportHeight,
			.qualityMode = qualityMode,
			.nearLightExclusion = nearLightShadowExclusion,
			.nearLightRadius = float(nearLightExclusionUnits),
			.reachMultiplier = shadowReachMultiplier,
			.fadeRange = shadowFadeRange,
			.falloffCurve = shadowFalloffCurve,
			.stochasticSampling = shadowStochastic,
			.emitterRadiusMultiplier = shadowEmitterRadiusMultiplier,
			.captureDebugLights = debugNativeLights.load(std::memory_order_relaxed)
		};
		if (TraceAllPreparedLights()) {
			const auto inventory = CollectFullShadowLights(lights ? std::span<const DX12::PreparedLight>(*lights) : std::span<const DX12::PreparedLight>{});
			shadows.Dispatch(*backend, captured, built, *inventory, frame, controls);
			return;
		}
		std::vector<DX12::PreparedLight> liveLights;
		if (lights) {
			std::scoped_lock lock(runtimeLightMutex);
			liveLights.reserve(lights->size());
			for (const auto& prepared : *lights) {
				const auto observed = runtimeLightFrame == frame ?
					std::ranges::find_if(runtimeLightSamples, [&](const auto& sample) {
						return sample.referenceID == prepared.referenceID;
					}) : runtimeLightSamples.end();
				if (observed != runtimeLightSamples.end()) {
					if (observed->value.fade > 0.0f &&
						(offloadFlickeringStatic || !observed->flickers))
						liveLights.push_back(observed->value);
				}
			}
		}
		shadows.Dispatch(*backend, captured, built, liveLights, frame, controls);
	}
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	// Observe visible static geometry already drawn by the existing opaque
	// render path. This does not change render-pass state or DX11 draw behavior.
	if (!loaded || !globals::features::dx12Control.IsActive() || globals::game::isVR ||
		!pass || !pass->geometry || !shadowMaskEnabled ||
		!globals::features::dx12Control.IsShadowOffloadMode() ||
		!globals::features::upscaling.loaded ||
		!globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return nullptr;
	ObserveSCMRigidCaster(*pass->geometry, true);
	return nullptr;
}

namespace
{
	std::optional<DX12Native::RuntimeGeometryCandidate> ReadRigidCaster(RE::BSGeometry& geometry, bool identity)
	{
		auto* tri = geometry.AsTriShape();
		if (!tri || geometry.AsDynamicTriShape())
			return std::nullopt;
		const auto& runtime = geometry.GetGeometryRuntimeData();
		if (runtime.skinInstance || runtime.alphaProperty || !runtime.rendererData ||
			!runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
			return std::nullopt;
		// Ignore effects, sky, water and other non-lighting draws; geometry only.
		const auto* property = runtime.shaderProperty.get();
		if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
			return std::nullopt;
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
			return std::nullopt;
		const auto* file = identity && reference ? reference->GetFile(0) : nullptr;
		auto desc = runtime.vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
			return std::nullopt;
		const auto counts = tri->GetTrishapeRuntimeData();
		const uint32_t stride = desc.GetSize();
		if (stride < 16 || stride > 256 || !counts.vertexCount || !counts.triangleCount ||
			counts.vertexCount > 65535 || counts.triangleCount > 65535)
			return std::nullopt;

		DX12Native::RuntimeGeometryCandidate candidate;
		if (identity) {
			candidate.referenceKey = reference && file && !file->GetFilename().empty() ?
			                             std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename()) :
			                             std::format("render:{:016X}", reinterpret_cast<std::uintptr_t>(&geometry));
			candidate.shapeName = geometry.name.empty() ? "<unnamed>" : geometry.name.c_str();
		}
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
		return candidate;
	}
}

bool DX12NativeScene::IsPreparedStaticCaster(RE::BSGeometry& geometry) const
{
	if (!CanSuppressSCMStatic())
		return false;
	const auto scene = GetFrameShadowScene();
	if (!scene || !scene->acceleration || scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;
	const auto candidate = ReadRigidCaster(geometry, false);
	if (!candidate)
		return false;
	const auto key = DX12Native::CasterCoverageKey(candidate->vertexCount, uint64_t(candidate->triangleCount) * 3u);
	const auto [first, last] = scene->casterCoverage.equal_range(key);
	for (auto it = first; it != last; ++it) {
		if (DX12Native::MatchesCasterGeometry(*it->second, candidate->transform, candidate->vertices,
				candidate->indices, candidate->stride, candidate->vertexCount, candidate->triangleCount))
			return true;
	}
	return false;
}

void DX12NativeScene::ObserveSCMRigidCaster(RE::BSGeometry& geometry, bool requireStability)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	auto candidate = ReadRigidCaster(geometry, true);
	if (!candidate)
		return;
	candidate->requiresStability = requireStability;
	const uint32_t frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
	geometryCapture.Observe(*candidate, frame, globals::features::dx12Resources.GetGrid());
}

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening && nativeSessionStarted) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		frameShadowScene.store(nullptr, std::memory_order_release);
		frameShadowSceneID.store(UINT32_MAX, std::memory_order_release);
		previousShadowSubmitted.store(false, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			frozenNativeLights.clear();
			ResetFullLightInventory();
		}
		raster.Reset();
		shadows.Reset();
		if (!globals::features::dx12Resources.HasMenuPreload())
			acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	if (!nativeSessionStarted)
		return;
	nativeSessionStarted = false;
	shadowLightScope.store(0, std::memory_order_release);
	shadowLights.store(nullptr, std::memory_order_release);
	frameShadowScene.store(nullptr, std::memory_order_release);
	frameShadowSceneID.store(UINT32_MAX, std::memory_order_release);
	previousShadowSubmitted.store(false, std::memory_order_release);
	frameTraceAllLights.store(false, std::memory_order_release);
	allLightStaticReady.store(false, std::memory_order_release);
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		frozenNativeLights.clear();
		ResetFullLightInventory();
	}
	raster.Reset();
	shadows.Reset();
	acceleration.Stop();
	geometryCapture.Reset();
}
