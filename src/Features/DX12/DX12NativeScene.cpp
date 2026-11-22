#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"

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

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening)
		raster.Reset();
}

void DX12NativeScene::OnRuntimeDisabled()
{
	raster.Reset();
}
