#include "Params.h"

#include "../../State.h"
#include "../../Utils/Game.h"
#include "../DlssEnhancerFeature.h"
#include "../Upscaling.h"

namespace DlssEnhancer
{
	VRDlssParams VRDlssParams::Resolve(
		ID3D11Resource* upscalingTexture,
		ID3D11Resource* depth,
		ID3D11Resource* reactive,
		ID3D11Resource* transparency,
		ID3D11Resource* mvec)
	{
		VRDlssParams p{};

		// Dimensions. MVP-B uses screenSize as both render baseline and
		// display target — DLSSperf-style RenderRes/DisplayRes split is
		// deferred to a PR-2 + PR-3 integration follow-up.
		const auto screenSize = globals::state->screenSize;
		const auto renderSize = Util::ConvertToDynamic(screenSize);

		p.renderW = (uint32_t)renderSize.x;
		p.renderH = (uint32_t)renderSize.y;
		p.eyeWidthIn = (uint32_t)(renderSize.x / 2);
		p.eyeHeightIn = (uint32_t)renderSize.y;
		p.eyeWidthOut = (uint32_t)(screenSize.x / 2);
		p.eyeHeightOut = (uint32_t)screenSize.y;

		// Textures. colorSrc/colorDst both alias kMAIN in MVP-B.
		p.colorSrc = upscalingTexture;
		p.colorDst = upscalingTexture;
		p.colorDstUAV = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV;

		p.depthTexture = depth;
		p.reactiveMask = reactive;
		p.transparencyMask = transparency;
		p.motionVectors = mvec;

		// Mode & subrect. PR-1's stereo Subrect API: GetUV() returns the
		// primary UV (= left-eye in stereo mode); GetRightEyeUV() returns
		// the mirrored right-eye UV.
		auto& enhancer = globals::features::dlssEnhancer;
		p.mode = enhancer.GetDlssMode();
		p.leftUV = enhancer.subrectController.GetUV();
		p.rightUV = enhancer.subrectController.GetRightEyeUV();
		p.isFullEye = (p.leftUV.w >= 0.999f && p.leftUV.h >= 0.999f);

		// Jitter — ConfigureUpscaling already computed correct DLSS jitter.
		auto& upscaling = globals::features::upscaling;
		p.jitterX = upscaling.jitter.x;
		p.jitterY = upscaling.jitter.y;

		return p;
	}
}
