#include "DX12NativeScene.h"

#include "../Upscaling.h"
#include "DX12Control.h"
#include "DX12NativeComposite.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TESObjectCELL.h"
#include "ScenePrepEngineBridge.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"
#include "Utils/SettingsControl.h"
#include "Utils/UI.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <ranges>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DX12NativeScene::Settings,
	ShadowMask,
	OutdoorShadowOffloading,
	ShadowTraceDensityMode,
	OffloadSCMStatic,
	OffloadFlickeringStatic,
	FreezeNativeFlickering,
	ShadowNearLightExclusion,
	ShadowNearLightRadius,
	ShadowReachMultiplier,
	ShadowFadeRange,
	ShadowFalloffCurve)

void DX12NativeScene::DrawSettings()
{
	ImGui::Checkbox(T("menu.dx12.scene.independent_shadowmask", "Independent shadowmask"), &settings.ShadowMask);
	ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &settings.OutdoorShadowOffloading);
	Util::InvertedCheckbox(T("menu.dx12.scene.keep_raster_static", "Keep raster static shadows for native-accounted lights"), &settings.OffloadSCMStatic);
	Util::InvertedCheckbox(T("menu.dx12.scene.keep_raster_flickering", "Keep raster static shadows for flickering lights"), &settings.OffloadFlickeringStatic);
	if (ImGui::Checkbox(T("menu.dx12.scene.freeze_flickering_native", "Freeze flickering native shadows (DX12 only)"), &settings.FreezeNativeFlickering) &&
		!SettingsControl::IsReplicating()) {
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	const char* fractions[] = {
		T("menu.dx12.scene.density.full", "Full"), T("menu.dx12.scene.density.two_thirds", "Two-thirds"), T("menu.dx12.scene.density.half", "Half"), T("menu.dx12.scene.density.one_third", "One-third"), T("menu.dx12.scene.density.quarter", "Quarter"),
		T("menu.dx12.scene.density.one_sixth", "One-sixth"), T("menu.dx12.scene.density.one_ninth", "One-ninth"), T("menu.dx12.scene.density.four_ninths", "Four-ninths")
	};
	Util::Combo(T("menu.dx12.scene.trace_density", "Trace density"), &settings.ShadowTraceDensityMode, fractions);
	ImGui::Checkbox(T("menu.dx12.scene.exclude_near_light", "Exclude shadows near light geometry"), &settings.ShadowNearLightExclusion);
	ImGui::BeginDisabled(!settings.ShadowNearLightExclusion);
	if (ImGui::SliderInt(T("menu.dx12.scene.exclusion_radius_units", "Light-centered exclusion radius (units)"), &settings.ShadowNearLightRadius, 0, 512) &&
		!SettingsControl::IsReplicating())
		settings.ShadowNearLightRadius = std::clamp(((settings.ShadowNearLightRadius + 2) / 4) * 4, 0, 512);
	ImGui::EndDisabled();
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_reach_multiplier", "Shadow reach multiplier (1x = vanilla)"), &settings.ShadowReachMultiplier,
		0.1f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
	if (ImGui::SliderFloat(T("menu.dx12.scene.shadow_fade_fraction", "Shadow fade range fraction"), &settings.ShadowFadeRange, 0.0f, 1.0f, "%.2f") &&
		!SettingsControl::IsReplicating())
		settings.ShadowFadeRange = std::round(settings.ShadowFadeRange * 50.0f) / 50.0f;
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_falloff_curve", "Shadow falloff curve"), &settings.ShadowFalloffCurve, 0.0f, 2.5f, "%.2f");
}

void DX12NativeScene::LoadSettings(json& input)
{
	settings = input;
	settings.ShadowTraceDensityMode = std::min(settings.ShadowTraceDensityMode, 7u);
	settings.ShadowNearLightRadius = std::clamp(settings.ShadowNearLightRadius, 0, 512) / 4 * 4;
	settings.ShadowReachMultiplier = std::clamp(settings.ShadowReachMultiplier, 0.1f, 64.0f);
	settings.ShadowFadeRange = std::round(std::clamp(settings.ShadowFadeRange, 0.0f, 1.0f) * 50.0f) / 50.0f;
	settings.ShadowFalloffCurve = std::clamp(settings.ShadowFalloffCurve, 0.0f, 2.5f);
	std::scoped_lock lock(runtimeLightMutex);
	frozenNativeLights.clear();
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = settings;
}

void DX12NativeScene::RestoreDefaultSettings()
{
	settings = {};
	std::scoped_lock lock(runtimeLightMutex);
	frozenNativeLights.clear();
}

bool DX12NativeScene::CanOffloadShadowsInCurrentCell() const
{
	const auto* cell = Util::GetPlayerCell();
	return cell && (!cell->IsExteriorCell() || settings.OutdoorShadowOffloading);
}

bool DX12NativeScene::CanSuppressSCMStatic() const
{
	return loaded && !globals::game::isVR && globals::features::dx12Control.IsActive() &&
	       globals::features::dx12Control.IsShadowOffloadMode() && settings.ShadowMask &&
	       settings.OffloadSCMStatic && CanOffloadShadowsInCurrentCell();
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// SCM keeps raster dynamic casters unconditionally; the cull drops a static caster only if the TLAS holds it.
	if (!light || slot < 0 || !CanSuppressSCMStatic() ||
		light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return false;
	const auto scene = acceleration.GetScene();
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !ni ||
		scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame)
		return false;

	const auto found = std::ranges::find_if(runtimeLightSamples, [ni](const RuntimeLightSample& sample) {
		return sample.source == ni;
	});
	if (found == runtimeLightSamples.end() ||
		(found->flickers && !settings.OffloadFlickeringStatic))
		return false;
	ownershipGranted.fetch_add(1, std::memory_order_relaxed);
	if (!std::ranges::contains(ownedLights, ni))
		ownedLights.push_back(ni);
	return true;
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool flickers, const RE::NiPoint3& restOrigin)
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
	if (settings.FreezeNativeFlickering && flickers && matched && matched->referenceID != 0) {
		// Keep DX11's flicker intact. Only the native shadow has a stable light.
		live.position = float3{ restOrigin.x, restOrigin.y, restOrigin.z };
		live = frozenNativeLights.try_emplace(matched->referenceID, live).first->second;
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
		it->flickers = flickers;
	} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights &&
			   (!matchedID || !std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
				   return sample.referenceID == matchedID;
			   }))) {
		runtimeLightSamples.push_back({ ni, matchedID, live, flickers });
	}
}

bool DX12NativeScene::WantsRenderPassHook() const
{
	return globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

bool DX12NativeScene::WantsPreShadowMapsHook() const { return WantsRenderPassHook(); }
bool DX12NativeScene::WantsOpaqueRasterReadyHook() const { return WantsRenderPassHook(); }

void DX12NativeScene::PreShadowMaps()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive()) {
		OnRuntimeDisabled();
		return;
	}
	nativeSessionStarted = true;
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || !grid->requested.front().scope) {
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

void DX12NativeScene::OnOpaqueRasterReady()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	if (!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	// Only capture the depth and camera used by the independent shadow layer.
	if (!settings.ShadowMask || !globals::features::dx12Control.IsShadowOffloadMode() ||
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
		const auto built = acceleration.GetScene();
		// The native trace handles unavailable inputs without affecting raster
		// rendering. RayQuery dispatch remains in DX12NativeShadowTrace.
		const DX12Native::ShadowTraceControls controls{
			.viewportWidth = viewportWidth,
			.viewportHeight = viewportHeight,
			.densityMode = settings.ShadowTraceDensityMode,
			.nearLightExclusion = settings.ShadowNearLightExclusion,
			.nearLightRadius = float(settings.ShadowNearLightRadius),
			.reachMultiplier = settings.ShadowReachMultiplier,
			.fadeRange = settings.ShadowFadeRange,
			.falloffCurve = settings.ShadowFalloffCurve
		};
		// Trace only lights whose raster static casters were dropped this frame, so no light is shadowed twice.
		std::vector<DX12::PreparedLight> liveLights;
		{
			std::scoped_lock lock(runtimeLightMutex);
			if (runtimeLightFrame == frame) {
				for (const auto& runtime : runtimeLightSamples)
					if (std::ranges::contains(ownedLights, runtime.source))
						liveLights.push_back(runtime.value);
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
		!pass || !pass->geometry || !settings.ShadowMask ||
		!globals::features::dx12Control.IsShadowOffloadMode() ||
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

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening && nativeSessionStarted) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			ownedLights.clear();
			frozenNativeLights.clear();
		}
		raster.Reset();
		shadows.Reset();
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
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		ownedLights.clear();
		frozenNativeLights.clear();
	}
	raster.Reset();
	shadows.Reset();
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
		{ "canSuppressStatic", CanSuppressSCMStatic() } };

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

	status["trace"] = { { "lastOutcome", magic_enum::enum_name(shadows.LastOutcome()) } };
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
