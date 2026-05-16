#pragma once

#include "../DlssEnhancerFeature.h"
#include "Utils/Subrect.h"
#include <d3d11.h>

namespace DlssEnhancer
{
	// Unified parameter block consumed by Mode functions.
	//
	// MVP-B: derived from current global state with no DLSSperf awareness
	// (PR-2 ships DLSSperf separately; integration with DlssEnhancer is
	// deferred). When PR-2 + PR-3 are both enabled, a follow-up will reroute
	// `colorDst` / `colorDstUAV` and `eyeWidthOut`/`eyeHeightOut` here.
	struct VRDlssParams
	{
		// Dimensions
		uint32_t renderW;       // SBS render width  (after DRS)
		uint32_t renderH;       // SBS render height (after DRS)
		uint32_t eyeWidthIn;    // per-eye input  (render) width
		uint32_t eyeHeightIn;   // per-eye input  (render) height
		uint32_t eyeWidthOut;   // per-eye output (display) width
		uint32_t eyeHeightOut;  // per-eye output (display) height

		// Textures
		ID3D11Resource* colorSrc;                // input color  (kMAIN)
		ID3D11Resource* colorDst;                // output color (kMAIN in MVP-B)
		ID3D11UnorderedAccessView* colorDstUAV;  // UAV for stretch output target
		ID3D11Resource* depthTexture;
		ID3D11Resource* reactiveMask;
		ID3D11Resource* transparencyMask;
		ID3D11Resource* motionVectors;

		// Mode & subrect (MVP-B mode set: kDefault, kFaster only — kExtreme deferred)
		DlssEnhancerFeature::DlssMode mode;
		Util::Subrect::UVRegion leftUV;
		Util::Subrect::UVRegion rightUV;
		bool isFullEye;

		// Jitter (pixel-space, render resolution)
		float jitterX;
		float jitterY;

		// Build a complete parameter block from current global state.
		static VRDlssParams Resolve(
			ID3D11Resource* upscalingTexture,
			ID3D11Resource* depthTexture,
			ID3D11Resource* reactiveMask,
			ID3D11Resource* transparencyMask,
			ID3D11Resource* motionVectors);
	};
}
