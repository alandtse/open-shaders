#include "Postprocess.h"

#include "../../Globals.h"
#include "../../State.h"
#include "../DlssEnhancerFeature.h"
#include "../Upscaling.h"

#include <cmath>

namespace DlssEnhancer
{
	bool Postprocess::ApplyDlssSharpening(Upscaling& upscaling)
	{
		auto& enhancer = globals::features::dlssEnhancer;
		if (enhancer.GetSharpenMode() == DlssEnhancerFeature::SharpenMode::kNone) {
			return true;
		}

		// MVP-B reads sharpness directly from Upscaling::Settings. The PR's
		// GetActiveSharpnessDLSS() helper consulted DlssEnhancer's own
		// settings.sharpnessDLSS override; deferred to PR-3b along with the
		// rest of the per-route override surface.
		const float sharpnessSetting = upscaling.settings.sharpnessDLSS;
		if (sharpnessSetting <= 0.0f) {
			return true;
		}

		if (!upscaling.sharpenerTexture || !upscaling.sharpenerTexture->uav || !upscaling.sharpenerTexture->resource) {
			logger::error("[DLSSENHANCER] Missing sharpener resources");
			return false;
		}

		auto context = globals::d3d::context;
		auto renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		if (!main.SRV) {
			logger::error("[DLSSENHANCER] Missing main SRV for sharpening");
			return false;
		}

		// Same exponential mapping dev's ApplySharpening uses: lower setting
		// = stronger sharpen. Range matches Upscaling's dev path so MVP-B
		// produces identical RCAS output to today.
		float currentSharpness = (-2.0f * sharpnessSetting) + 2.0f;
		currentSharpness = exp2(-currentSharpness);

		// MVP-B sharpen path: in-place RCAS on kMAIN through sharpenerTexture.
		// PR-2's DLSSperf-aware path (which routes RCAS through testTexture +
		// refraTempTex to dodge an SRV/UAV hazard at DisplayRes) is deferred
		// to a follow-up that bridges PR-2 and PR-3.
		ID3D11Resource* mainResource = nullptr;
		main.SRV->GetResource(&mainResource);
		if (!mainResource) {
			logger::error("[DLSSENHANCER] Failed to acquire main resource for sharpening");
			return false;
		}

		context->OMSetRenderTargets(0, nullptr, nullptr);
		upscaling.rcas.ApplySharpen(main.SRV, upscaling.sharpenerTexture->uav.get(), currentSharpness);
		context->CopyResource(mainResource, upscaling.sharpenerTexture->resource.get());
		mainResource->Release();

		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		return true;
	}
}
