#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"

#include <format>

void DX12NativeScene::DrawSettings()
{
	ImGui::TextUnformatted("Early opaque raster submission to native DX12.");
	ImGui::TextDisabled("Capture is independent of native acceleration, Native Composite, and FSR.");
	if (!startupFailure.empty())
		ImGui::TextWrapped("Native raster transfer: %s", startupFailure.c_str());
	else if (!raster.Status().empty())
		ImGui::TextWrapped("Native raster capture: %s", raster.Status().c_str());
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
		globals::game::graphicsState->GetFrameCount() : 0;
	geometryCapture.Observe(candidate, frame, globals::features::dx12Resources.GetGrid());
}

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening) {
		raster.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	raster.Reset();
	geometryCapture.Reset();
}
