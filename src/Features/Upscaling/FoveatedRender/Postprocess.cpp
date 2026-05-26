#include "Postprocess.h"

#include "../../../Globals.h"
#include "../../../State.h"
#include "../../Upscaling.h"
#include "../FoveatedRender.h"

#include <cmath>

namespace FoveatedRenderImpl
{
	bool Postprocess::ApplyDlssSharpening(Upscaling& upscaling)
	{
		// sharpnessDLSS <= 0 is the single disable signal — sharpness lives on
		// Upscaling::Settings so the route shares the global slider.
		const float sharpnessSetting = upscaling.settings.sharpnessDLSS;
		if (sharpnessSetting <= 0.0f) {
			return true;
		}

		if (!upscaling.sharpenerTexture || !upscaling.sharpenerTexture->uav || !upscaling.sharpenerTexture->resource) {
			logger::error("[FOVEATED] Missing sharpener resources");
			return false;
		}

		auto context = globals::d3d::context;
		auto renderer = globals::game::renderer;
		if (!context || !renderer) {
			logger::error("[FOVEATED] Missing D3D context or renderer for sharpening");
			return false;
		}
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		if (!main.SRV) {
			logger::error("[FOVEATED] Missing main SRV for sharpening");
			return false;
		}

		// Same exponential mapping Upscaling::ApplySharpening uses: lower
		// setting = stronger sharpen.
		float currentSharpness = (-2.0f * sharpnessSetting) + 2.0f;
		currentSharpness = exp2(-currentSharpness);

		// In-place RCAS on kMAIN through sharpenerTexture.
		ID3D11Resource* mainResource = nullptr;
		main.SRV->GetResource(&mainResource);
		if (!mainResource) {
			logger::error("[FOVEATED] Failed to acquire main resource for sharpening");
			return false;
		}

		context->OMSetRenderTargets(0, nullptr, nullptr);
		upscaling.rcas.ApplySharpen(main.SRV, upscaling.sharpenerTexture->uav.get(), currentSharpness);
		context->CopyResource(mainResource, upscaling.sharpenerTexture->resource.get());
		mainResource->Release();

		if (globals::game::stateUpdateFlags) {
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
		return true;
	}
}
