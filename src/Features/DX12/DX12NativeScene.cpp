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

void DX12NativeScene::DrawSettings()
{
	ImGui::Checkbox("Independent shadowmask", &shadowMaskEnabled);
	ImGui::Checkbox("Replace SCM static casters with native shadows", &offloadStaticShadows);
	ImGui::Checkbox("Include flickering lights in static offload", &offloadFlickeringStatic);
	static constexpr const char* fractions[] = {
		"Full", "Two-thirds", "Half", "One-third", "Quarter",
		"One-sixth", "One-ninth", "Four-ninths"
	};
	int mode = static_cast<int>(densityMode);
	if (ImGui::Combo("Trace density", &mode, fractions, 8))
		densityMode = static_cast<uint32_t>(mode);
	ImGui::Checkbox("Stochastic shadow emitter sampling", &stochasticEmitterSampling);
	ImGui::BeginDisabled(!stochasticEmitterSampling);
	int rays = int(emitterRayCount);
	if (ImGui::SliderInt("Rays per sampled pixel and light", &rays, 1, 16))
		emitterRayCount = uint32_t(rays);
	ImGui::EndDisabled();
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
	ImGui::TextDisabled("Full density = every pixel. Emitter rays are independent of trace density.");
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
	ImGui::Text("Emitter sampling: %s; rays/light: %u", stochasticEmitterSampling ? "stochastic" : "deterministic center", stochasticEmitterSampling ? emitterRayCount : 1u);
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
	stochasticEmitterSampling = input.value("ShadowStochastic", false);
	emitterRayCount = std::clamp(input.value("ShadowEmitterRays", 4u), 1u, 16u);
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
		{ "ShadowStochastic", stochasticEmitterSampling },
		{ "ShadowEmitterRays", emitterRayCount },
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
	stochasticEmitterSampling = false;
	emitterRayCount = 4;
	nearLightShadowExclusion = false;
	nearLightExclusionUnits = 0;
	shadowReachMultiplier = 1.0f;
	shadowFadeRange = 0.50f;
	shadowFalloffCurve = 1.0f;
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// Selection is per source light. It is independent of camera facing,
	// full-tile geometry coverage, Native Lighting and FSR.
	if (!light || slot < 0 || !loaded || globals::game::isVR ||
		!globals::features::dx12Control.IsActive() ||
		!globals::features::dx12Control.IsShadowOffloadMode() ||
		!shadowMaskEnabled || !offloadStaticShadows ||
		light->GetIsFrustumOrDirectionalLight())
		return false;
	const auto scene = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !lights || !ni)
		return false;

	const auto& position = ni->world.translate;
	// The shadow shader traces the first 64 prepared lights in exactly this
	// order. Do not suppress the raster caster of a light it does not trace.
	for (size_t i = 0; i < std::min(lights->size(), size_t{ 64 }); ++i) {
		const auto& candidate = (*lights)[i];
		const float dx = position.x - candidate.position.x;
		const float dy = position.y - candidate.position.y;
		const float dz = position.z - candidate.position.z;
		const float tolerance = std::clamp(candidate.radius * 0.01f, 4.0f, 32.0f);
		if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
			dx * dx + dy * dy + dz * dz <= tolerance * tolerance)
			return true;
	}
	return false;
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
	raster.Capture(*backend, sources, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, frame);
	const uint32_t previewMode = globals::features::dx12Control.GetShadowPreview();
	const bool geometryPreview = previewMode >= 3u && previewMode <= 5u;
	if ((shadowMaskEnabled && globals::features::dx12Control.IsShadowOffloadMode()) || geometryPreview) {
		const auto captured = raster.Get(frame);
		const auto built = acceleration.GetScene();
		const auto lights = shadowLights.load(std::memory_order_acquire);
		// Report missing prerequisites even when no raw frame can be submitted.
		// The only RayQuery dispatch remains inside the DX12 shadow trace.
		const DX12Native::ShadowTraceControls controls{
			.densityMode = densityMode,
			.debugGeometryMode = geometryPreview ? previewMode - 2u : 0u,
			.emitterSamples = emitterRayCount,
			.stochastic = stochasticEmitterSampling,
			.nearLightExclusion = nearLightShadowExclusion,
			.nearLightRadius = float(nearLightExclusionUnits),
			.reachMultiplier = shadowReachMultiplier,
			.fadeRange = shadowFadeRange,
			.falloffCurve = shadowFalloffCurve
		};
		if (lights)
			shadows.Dispatch(*backend, captured, built, *lights, frame, controls);
		else
			shadows.Dispatch(*backend, captured, built, {}, frame, controls);
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
		raster.Reset();
		shadows.Reset();
		acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	shadowLights.store(nullptr, std::memory_order_release);
	raster.Reset();
	shadows.Reset();
	acceleration.Reset();
	geometryCapture.Reset();
}
