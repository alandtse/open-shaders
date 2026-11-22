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
	ImGui::TextDisabled("Raw shadowmask always uses the full render resolution.");
	ImGui::TextDisabled("Capture is independent of native acceleration, Native Composite, and FSR.");
	if (!startupFailure.empty())
		ImGui::TextWrapped("Native raster transfer: %s", startupFailure.c_str());
	else if (!raster.Status().empty())
		ImGui::TextWrapped("Native raster capture: %s", raster.Status().c_str());
	if (!shadows.Status().empty())
		ImGui::TextWrapped("RayQuery: %s", shadows.Status().c_str());
	if (!acceleration.Failure().empty())
		ImGui::TextWrapped("Native acceleration: %s", acceleration.Failure().c_str());
}

void DX12NativeScene::LoadSettings(json& input)
{
	shadowMaskEnabled = input.value("ShadowMask", true);
	offloadStaticShadows = input.value("OffloadSCMStatic", true);
	offloadFlickeringStatic = input.value("OffloadFlickeringStatic", true);
	densityMode = std::min(input.value("ShadowTraceDensityMode", 0u), 7u);
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "ShadowMask", shadowMaskEnabled }, { "ShadowTraceDensityMode", densityMode },
		{ "OffloadSCMStatic", offloadStaticShadows },
		{ "OffloadFlickeringStatic", offloadFlickeringStatic } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	shadowMaskEnabled = true;
	offloadStaticShadows = true;
	offloadFlickeringStatic = true;
	densityMode = 0;
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// Selection is per source light. It is independent of camera facing,
	// full-tile geometry coverage, Native Lighting and FSR.
	if (!light || slot < 0 || !loaded || globals::game::isVR ||
		!globals::features::dx12Control.IsActive() ||
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
	if (shadowMaskEnabled) {
		const auto captured = raster.Get(frame);
		const auto built = acceleration.GetScene();
		const auto lights = shadowLights.load(std::memory_order_acquire);
		if (captured && built && lights)
			shadows.Dispatch(*backend, std::move(captured), std::move(built),
				*lights, frame, densityMode);
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
	const auto desc = runtime.vertexDesc;
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
