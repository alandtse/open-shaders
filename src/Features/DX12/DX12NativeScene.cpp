#include "DX12NativeScene.h"

#include "../Upscaling.h"
#include "DX12Control.h"
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
		ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoorFullRT);
	} else if (control.IsShadowOffloadMode()) {
		ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoorShadowOffloading);
	}
}

void DX12NativeScene::LoadSettings(json& input)
{
	outdoorShadowOffloading = input.value("OutdoorShadowOffloading", false);
	const auto profile = input.value("FullRT", json::object());
	outdoorFullRT = profile.value("OutdoorShadowOffloading", false);
	fullRTQuality = DX12Native::SanitizeRTQuality(profile.value("ShadowQualityMode", DX12Native::kShadowQualityQuality));
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "OutdoorShadowOffloading", outdoorShadowOffloading },
		{ "FullRT", { { "OutdoorShadowOffloading", outdoorFullRT }, { "ShadowQualityMode", fullRTQuality } } } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	outdoorShadowOffloading = false;
	outdoorFullRT = false;
	fullRTQuality = DX12Native::kShadowQualityQuality;
}

bool DX12NativeScene::CanOffloadShadowsInCurrentCell() const
{
	if (globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
		return false;
	const auto* cell = Util::GetPlayerCell();
	return cell && (!cell->IsExteriorCell() || (globals::features::dx12Control.IsFullRTMode() ? outdoorFullRT : outdoorShadowOffloading));
}

DX12::SCMNativeMode DX12NativeScene::GetFrameNativeMode() const
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!globals::game::graphicsState ||
		frameNativeModeID.load(std::memory_order_acquire) != globals::game::graphicsState->GetFrameCount())
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

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool /*flickers*/, const RE::NiPoint3& restOrigin)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
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
	// A prepared emitter must not accidentally account for two different SCM
	// lights that happen to occupy the same position.
	const auto it = std::ranges::find_if(runtimeLightSamples, [ni](const auto& sample) {
		return sample.source == ni;
	});
	if (it != runtimeLightSamples.end()) {
		it->referenceID = matched->referenceID;
		it->value = live;
	} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights &&
			   !std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
				   return sample.referenceID == matched->referenceID;
			   })) {
		runtimeLightSamples.push_back({ ni, matched->referenceID, live });
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
	if (!globals::state || !globals::game::graphicsState ||
		!globals::state->startupMenuInitializationComplete.load(std::memory_order_acquire) ||
		(globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))) {
		frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
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
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
		}
		frameNativeModeID.store(frame, std::memory_order_release);
	}
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || grid->requested.empty() || !grid->requested.front().scope) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		return;
	}
	shadowLightScope.store(grid->requested.front().scope, std::memory_order_release);
	// SCM reads a single per-frame publication without re-enumerating tiles
	// for every shadow light; the same ordered sources feed RayQuery.
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
	uint32_t ReadPlacedLightID(const RE::NiLight* light) noexcept
	{
		__try {
			const RE::NiAVObject* node = light;
			for (uint32_t depth = 0; node && depth < 16; ++depth, node = node->parent) {
				if (auto* ref = node->GetUserData()) {
					auto* base = ref->GetObjectReference();
					return base && base->As<RE::TESObjectLIGH>() ? ref->GetFormID() : 0;
				}
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
		return 0;
	}
}

std::vector<DX12::PreparedLight> DX12NativeScene::CollectFullRTLights(const std::vector<DX12::PreparedLight>& prepared) const
{
	// This inventory is independent of SCM scheduling and ownership matching.
	std::vector<DX12::PreparedLight> result = prepared;
	std::unordered_set<uint32_t> preparedIDs;
	for (const auto& light : prepared)
		if (light.referenceID)
			preparedIDs.insert(light.referenceID);
	std::unordered_set<const RE::NiLight*> seen;
	const auto collect = [&](RE::BSLight* light) {
		if (!light || !light->pointLight || !light->light || result.size() >= DX12Native::kMaxShadowLights)
			return;
		auto* ni = light->light.get();
		if (!seen.insert(ni).second || ni->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return;
		const auto flags = PointLightFlags::GetVanillaPointLightFlags(light, ni);
		if (flags & PointLightFlags::ToMask(PointLightFlags::Flags::Spot))
			return;
		const uint32_t placed = ReadPlacedLightID(ni);
		if (placed && preparedIDs.contains(placed))
			return;
		const auto& rt = ni->GetLightRuntimeData();
		const auto& position = ni->world.translate;
		if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f || !std::isfinite(rt.fade) || rt.fade <= 0.0f ||
			!std::isfinite(rt.diffuse.red) || !std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
			!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
			return;
		DX12::PreparedLight value{};
		value.referenceID = placed;
		value.position = float3{ position.x, position.y, position.z };
		value.radius = rt.radius.x;
		value.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
		value.fade = rt.fade;
		result.push_back(value);
	};
	const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	if (node) {
		const auto& scene = node->GetRuntimeData();
		for (const auto& light : scene.activeLights)
			collect(light.get());
		for (const auto& light : scene.activeShadowLights)
			collect(light.get());
	}
	return result;
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

	const auto built = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	if (!built || !built->acceleration || !lights ||
		built->scope != shadowLightScope.load(std::memory_order_acquire))
		return;

	// Admit only matched runtime emitters that the SCM policy can account for.
	// Iterate the immutable prepared selection to preserve its deterministic order.
	std::vector<DX12::PreparedLight> liveLights;
	if (fullRT) {
		liveLights = CollectFullRTLights(*lights);
	} else {
		std::scoped_lock lock(runtimeLightMutex);
		if (runtimeLightFrame != frame || runtimeLightSamples.empty())
			return;
		std::unordered_map<uint32_t, const DX12::PreparedLight*> liveByReference;
		liveByReference.reserve(runtimeLightSamples.size());
		for (const auto& sample : runtimeLightSamples)
			if (sample.referenceID)
				liveByReference.try_emplace(sample.referenceID, &sample.value);
		const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
		liveLights.reserve(std::min(limit, liveByReference.size()));
		for (size_t i = 0; i < limit; ++i) {
			const auto it = liveByReference.find((*lights)[i].referenceID);
			if (it != liveByReference.end())
				liveLights.push_back(*it->second);
		}
	}
	if (liveLights.empty())
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
		.quality = fullRT ? fullRTQuality : DX12Native::kShadowQualityOffloading
	};
	shadows.Dispatch(*backend, raster.Get(frame), built, liveLights, frame, controls);
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	// Observe visible static geometry already drawn by the existing opaque
	// render path. This does not change render-pass state or DX11 draw behavior.
	if (!loaded || !globals::features::dx12Control.IsActive() || globals::game::isVR ||
		!pass || !pass->geometry ||
		(!globals::features::dx12Control.IsShadowOffloadMode() && !globals::features::dx12Control.IsFullRTMode()) ||
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
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
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

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening) {
		frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
		frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	}
	if (a_opening && nativeSessionStarted) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
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
	}
	raster.Reset();
	shadows.Reset();
	acceleration.Stop();
	geometryCapture.Reset();
}
