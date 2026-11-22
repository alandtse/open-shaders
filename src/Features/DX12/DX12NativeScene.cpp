#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>

void DX12NativeScene::DrawSettings()
{
	ImGui::Checkbox("Independent shadowmask", &shadowMaskEnabled);
	bool keepRasterStatic = !offloadStaticShadows;
	if (ImGui::Checkbox("Keep raster static shadows for native-accounted lights", &keepRasterStatic))
		offloadStaticShadows = !keepRasterStatic;
	bool retainFlickeringRaster = !offloadFlickeringStatic;
	if (ImGui::Checkbox("Keep raster static shadows for flickering lights", &retainFlickeringRaster))
		offloadFlickeringStatic = !retainFlickeringRaster;
	if (ImGui::Checkbox("Freeze flickering native shadows (DX12 only)", &freezeNativeFlickering)) {
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	ImGui::TextDisabled("Static casters are omitted only for lights confirmed in Native Scene.");
	ImGui::TextDisabled("Dynamic raster casters always remain enabled.");
	static constexpr const char* fractions[] = {
		"Full", "Two-thirds", "Half", "One-third", "Quarter",
		"One-sixth", "One-ninth", "Four-ninths"
	};
	int mode = static_cast<int>(densityMode);
	if (ImGui::Combo("Trace density", &mode, fractions, 8))
		densityMode = static_cast<uint32_t>(mode);
	ImGui::Checkbox("Exclude shadows near light geometry", &nearLightShadowExclusion);
	ImGui::BeginDisabled(!nearLightShadowExclusion);
	int nearUnits = int(nearLightExclusionUnits);
	if (ImGui::SliderInt("Light-centered exclusion radius (units)", &nearUnits, 0, 512))
		nearLightExclusionUnits = uint32_t(std::clamp(((nearUnits + 2) / 4) * 4, 0, 512));
	ImGui::EndDisabled();
	ImGui::SliderFloat("Shadow reach multiplier (1x = vanilla)", &shadowReachMultiplier,
		0.1f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
	float fade = shadowFadeRange;
	if (ImGui::SliderFloat("Shadow fade range fraction", &fade, 0.0f, 1.0f, "%.2f"))
		shadowFadeRange = std::round(fade * 50.0f) / 50.0f;
	ImGui::SliderFloat("Shadow falloff curve", &shadowFalloffCurve, 0.0f, 2.5f, "%.2f");
	ImGui::TextDisabled("Full density = every pixel. Shadow visibility traces one ray per light.");
	ImGui::TextDisabled("Raw shadowmask always uses the full render resolution.");
	ImGui::TextDisabled("Capture is independent of native acceleration, Native Composite, and FSR.");
}

void DX12NativeScene::DrawDiagnostics() const
{
	ImGui::SeparatorText("Native Scene / DXR");
	ImGui::Text("Shadowmask dispatch: %s", shadowMaskEnabled &&
												   globals::features::dx12Control.IsShadowOffloadMode() ?
											   "enabled" :
											   "paused");
	ImGui::Text("Trace density mode: %u (0=full, 1=2/3, 2=1/2, 3=1/3, 4=1/4, 5=1/6, 6=1/9, 7=4/9)", densityMode);
	ImGui::Text("Light visibility: deterministic center ray; stochastic sampling removed.");
	ImGui::Text("Flickering static raster: %s; freeze native flicker: %s",
		offloadFlickeringStatic ? "offload allowed" : "kept", freezeNativeFlickering ? "on" : "off");
	{
		std::scoped_lock lock(runtimeLightMutex);
		ImGui::Text("Matched live SCM lights: %zu (engine frame %u); frozen: %zu",
			runtimeLightSamples.size(), runtimeLightFrame, frozenNativeLights.size());
	}
	ImGui::Text("Near-light exclusion: %s; radius %u units", nearLightShadowExclusion ? "enabled" : "disabled", nearLightExclusionUnits);
	ImGui::Text("Shadow reach %.2fx vanilla; fade fraction %.2f; curve %.2f", shadowReachMultiplier, shadowFadeRange, shadowFalloffCurve);
	const auto lights = shadowLights.load(std::memory_order_acquire);
	ImGui::Text("Prepared lights: %zu (first %zu traceable this frame)",
		lights ? lights->size() : 0u, lights ? std::min(lights->size(), size_t{ 64 }) : 0u);
	const auto scene = acceleration.GetScene();
	ImGui::Text("TLAS: %s", scene && scene->acceleration ? "published" : "not available");
	const uint32_t frame = globals::game::graphicsState ?
	                           globals::game::graphicsState->GetFrameCount() :
	                           0u;
	if (scene && scene->inputs) {
		const auto& geometry = scene->inputs->geometry;
		const auto runtime = std::ranges::count_if(geometry, [](const auto& input) { return input.supplementary; });
		ImGui::Text("Published TLAS shapes: companion %zu, runtime %zu", geometry.size() - size_t(runtime), size_t(runtime));
	}
	const auto& mainWorld = globals::game::mainWorldCameraSnapshot;
	ImGui::Text("Main-world camera: %s (snapshot frame %u, current frame %u)",
		mainWorld.valid && mainWorld.frame == frame ? "verified" : "not current / unavailable",
		mainWorld.frame, frame);
	const auto raw = shadows.Get(frame);
	ImGui::Text("Raw mask for engine frame %u: %s", frame, raw ? "submitted" : "not published");
	if (raw) {
		ImGui::Text("Mask: %u x %u; GPU density %u; fence %llu",
			raw->width, raw->height, raw->densityMode, static_cast<unsigned long long>(raw->readyValue));
	}
	if (raw && raw->readyFence) {
		const auto done = raw->readyFence->GetCompletedValue();
		ImGui::Text("Shadow GPU fence: %s", done != UINT64_MAX && done >= raw->readyValue ?
												"completed" :
												"pending / unavailable");
	}
	const auto rasterFrame = raster.Get(frame);
	ImGui::Text("DX11 opaque raster capture: %s", rasterFrame ? "published" : "not published");
	const auto counters = raster.GetCounters();
	ImGui::Text("Raster capture submissions: %llu; busy slot deferrals: %llu",
		static_cast<unsigned long long>(counters.submissions),
		static_cast<unsigned long long>(counters.busyDeferrals));
	if (!startupFailure.empty())
		ImGui::TextWrapped("Native raster transfer: %s", startupFailure.c_str());
	if (!raster.Status().empty())
		ImGui::TextWrapped("Native raster capture: %s", raster.Status().c_str());
	if (!shadows.Status().empty())
		ImGui::TextWrapped("RayQuery: %s", shadows.Status().c_str());
	const auto accelerationFailure = acceleration.Failure();
	if (!accelerationFailure.empty())
		ImGui::TextWrapped("Native acceleration: %s", accelerationFailure.c_str());
}

void DX12NativeScene::LoadSettings(json& input)
{
	shadowMaskEnabled = input.value("ShadowMask", true);
	offloadStaticShadows = input.value("OffloadSCMStatic", true);
	offloadFlickeringStatic = input.value("OffloadFlickeringStatic", true);
	densityMode = std::min(input.value("ShadowTraceDensityMode", 0u), 7u);
	freezeNativeFlickering = input.value("FreezeNativeFlickering", false);
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	nearLightShadowExclusion = input.value("ShadowNearLightExclusion", false);
	nearLightExclusionUnits = std::clamp(input.value("ShadowNearLightRadius", 0u), 0u, 512u) / 4u * 4u;
	shadowReachMultiplier = std::clamp(input.value("ShadowReachMultiplier", 1.0f), 0.1f, 64.0f);
	shadowFadeRange = std::round(std::clamp(input.value("ShadowFadeRange", 0.50f), 0.0f, 1.0f) * 50.0f) / 50.0f;
	shadowFalloffCurve = std::clamp(input.value("ShadowFalloffCurve", 1.0f), 0.0f, 2.5f);
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "ShadowMask", shadowMaskEnabled }, { "ShadowTraceDensityMode", densityMode },
		{ "OffloadSCMStatic", offloadStaticShadows },
		{ "OffloadFlickeringStatic", offloadFlickeringStatic },
		{ "FreezeNativeFlickering", freezeNativeFlickering },
		{ "ShadowNearLightExclusion", nearLightShadowExclusion },
		{ "ShadowNearLightRadius", nearLightExclusionUnits },
		{ "ShadowReachMultiplier", shadowReachMultiplier },
		{ "ShadowFadeRange", shadowFadeRange },
		{ "ShadowFalloffCurve", shadowFalloffCurve } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	shadowMaskEnabled = true;
	offloadStaticShadows = true;
	offloadFlickeringStatic = true;
	densityMode = 0;
	freezeNativeFlickering = false;
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	nearLightShadowExclusion = false;
	nearLightExclusionUnits = 0;
	shadowReachMultiplier = 1.0f;
	shadowFadeRange = 0.50f;
	shadowFalloffCurve = 1.0f;
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// SCM keeps raster dynamic casters unconditionally. Static casters may be
	// omitted only when this exact runtime light was matched to a native emitter
	// in the current frame; a nearby prepared light is not sufficient proof.
	if (!light || slot < 0 || !loaded || globals::game::isVR ||
		!globals::features::dx12Control.IsActive() ||
		!globals::features::dx12Control.IsShadowOffloadMode() ||
		!shadowMaskEnabled || !offloadStaticShadows ||
		light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return false;
	const auto scene = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !lights || !ni)
		return false;

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame)
		return false;

	const auto found = std::ranges::find_if(runtimeLightSamples, [ni](const RuntimeLightSample& sample) {
		return sample.source == ni;
	});
	if (found == runtimeLightSamples.end() ||
		(found->flickers && !offloadFlickeringStatic))
		return false;

	// The shader admits only the first 64 prepared lights. Never suppress a
	// raster static caster for a light not actually selected for DX12 tracing.
	const size_t limit = std::min(lights->size(), size_t{ 64 });
	for (size_t i = 0; i < limit; ++i) {
		const auto& candidate = (*lights)[i];
		if (candidate.referenceID != 0 && candidate.referenceID == found->referenceID)
			return true;
	}
	return false;
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool flickers, const RE::NiPoint3& restOrigin)
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
	live.position = { position.x, position.y, position.z };
	live.radius = rt.radius.x;
	live.color = { rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
	live.fade = rt.fade;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame) {
		runtimeLightFrame = frame;
		runtimeLightSamples.clear();
	}
	if (freezeNativeFlickering && flickers && matched->referenceID != 0) {
		// Keep DX11's flicker intact. Only the native shadow has a stable light.
		live.position = { restOrigin.x, restOrigin.y, restOrigin.z };
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
	} else if (runtimeLightSamples.size() < 64 &&
		!std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
			return sample.referenceID == matched->referenceID;
		})) {
		runtimeLightSamples.push_back({ ni, matched->referenceID, live, flickers });
	}
}

void DX12NativeScene::PreShadowMaps()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive()) {
		shadowLights.store(nullptr, std::memory_order_release);
		return;
	}
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || !grid->requested.front().scope) {
		shadowLights.store(nullptr, std::memory_order_release);
		return;
	}
	// SCM reads a single per-frame publication without re-enumerating tiles
	// for every shadow light; the same ordered sources feed RayQuery.
	shadowLights.store(std::make_shared<const std::vector<DX12::PreparedLight>>(
						   globals::features::dx12Resources.GetPreparedLights()),
		std::memory_order_release);
	// Only a revision-coalesced mailbox handoff on this thread. All BLAS/TLAS
	// allocations, command recording and GPU builds run on their own worker.
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
}

void DX12NativeScene::OnOpaqueRasterReady(const OpaqueRasterInputs& a_inputs)
{
	CS_GPU_PASS("DX12NativeScene::OpaqueRasterReady");
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	if (!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;

	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend) {
			startupFailure = std::move(reason);
			return;
		}
		startupFailure.clear();
	}

	const std::array sources{
		a_inputs.litColor, a_inputs.litSpecular, a_inputs.albedo, a_inputs.normalRoughness,
		a_inputs.masks, a_inputs.masks2, a_inputs.reflectance, a_inputs.motion
	};
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	const uint32_t previewMode = globals::features::dx12Control.GetShadowPreview();
	const bool geometryPreview = previewMode >= 3u && previewMode <= 5u;
	// Never fabricate the geometry preview from an old or unrelated shadow camera.
	const auto& mainWorld = globals::game::mainWorldCameraSnapshot;
	const bool mainWorldReady = mainWorld.valid && mainWorld.frame == frame;
	const globals::FrameBuffer* camera = geometryPreview ?
	                                         (mainWorldReady ? &mainWorld.camera : nullptr) :
	                                         &globals::game::frameBufferCached.nonVR;
	raster.Capture(*backend, sources, Util::GetCurrentSceneDepthSRV(false), camera, frame);
	// The active view may occupy only a subrect of the full-size raster allocation.
	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t viewportWidth = std::isfinite(viewport.x) && viewport.x > 0.0f ? uint32_t(viewport.x) : 0u;
	const uint32_t viewportHeight = std::isfinite(viewport.y) && viewport.y > 0.0f ? uint32_t(viewport.y) : 0u;
	if ((shadowMaskEnabled && globals::features::dx12Control.IsShadowOffloadMode()) || geometryPreview) {
		const auto captured = raster.Get(frame);
		const auto built = acceleration.GetScene();
		const auto lights = shadowLights.load(std::memory_order_acquire);
		// Report missing prerequisites even when no raw frame can be submitted.
		// The only RayQuery dispatch remains inside the DX12 shadow trace.
		const DX12Native::ShadowTraceControls controls{
			.viewportWidth = viewportWidth,
			.viewportHeight = viewportHeight,
			.densityMode = densityMode,
			.debugGeometryMode = geometryPreview ? previewMode - 2u : 0u,
			.nearLightExclusion = nearLightShadowExclusion,
			.nearLightRadius = float(nearLightExclusionUnits),
			.reachMultiplier = shadowReachMultiplier,
			.fadeRange = shadowFadeRange,
			.falloffCurve = shadowFalloffCurve
		};
		std::vector<DX12::PreparedLight> liveLights = lights ? *lights : std::vector<DX12::PreparedLight>{};
		if (!geometryPreview && !liveLights.empty()) {
			std::scoped_lock lock(runtimeLightMutex);
			if (runtimeLightFrame == frame) {
				for (const auto& runtime : runtimeLightSamples) {
					auto it = std::ranges::find_if(liveLights, [&](const auto& candidate) {
						return candidate.referenceID == runtime.referenceID;
					});
					if (it != liveLights.end())
						*it = runtime.value;
				}
			}
		}
		shadows.Dispatch(*backend, captured, built, liveLights, frame, controls);
	}
}

void DX12NativeScene::ObserveSCMRigidCaster(RE::BSGeometry& geometry)
{
	// SCM already visits this caster. Only CPU-backed immutable rigid triangles
	// are copied; no extra cull traversal, raster draw, D3D readback or GPU wait.
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	auto* tri = geometry.AsTriShape();
	if (!tri || geometry.AsDynamicTriShape() || geometry.name.empty())
		return;
	const auto& runtime = geometry.GetGeometryRuntimeData();
	if (runtime.skinInstance || runtime.alphaProperty || !runtime.rendererData ||
		!runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
		return;
	auto* reference = geometry.GetUserData();
	if (!reference || reference->IsDynamicForm())
		return;
	const auto* file = reference->GetFile(0);
	if (!file || file->GetFilename().empty())
		return;
	auto desc = runtime.vertexDesc;
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	const auto counts = tri->GetTrishapeRuntimeData();
	const uint32_t stride = desc.GetSize();
	if (stride < 16 || stride > 256 || !counts.vertexCount || !counts.triangleCount ||
		counts.vertexCount > 65535 || counts.triangleCount > 65535)
		return;

	DX12Native::RuntimeGeometryCandidate candidate;
	candidate.referenceKey = std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename());
	candidate.shapeName = geometry.name.c_str();
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
		shadowLights.store(nullptr, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
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
	shadowLights.store(nullptr, std::memory_order_release);
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		frozenNativeLights.clear();
	}
	raster.Reset();
	shadows.Reset();
	acceleration.Reset();
	geometryCapture.Reset();
}
