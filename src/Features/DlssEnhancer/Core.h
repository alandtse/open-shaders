#pragma once

// ============================================================================
// DlssEnhancer::Core — GPU resource pool & mode-dispatch entry point
// ============================================================================
//
// Owns all per-mode intermediate textures (Default / Faster), compute-shader
// objects (subrect stretch), and the public entry points consumed by
// Upscaling.cpp.
//
// ============================================================================

#include "Buffer.h"
#include "Params.h"
#include <d3d11_4.h>
#include <winrt/base.h>

class Streamline;

namespace DlssEnhancer
{
	class Core
	{
	public:
		// Stage1: dispatches across Default / Faster modes.
		static bool ExecuteVRDlssCore(Streamline& streamline,
			ID3D11Resource* upscalingTexture,
			ID3D11Resource* depthTexture,
			ID3D11Resource* reactiveMask,
			ID3D11Resource* transparencyMask,
			ID3D11Resource* motionVectors);

		// Shared VR per-eye preprocessing/finalization for non-DLSS callers (e.g. FSR).
		static bool PrepareVRPerEyeInputs(
			ID3D11Resource* colorSrc,
			ID3D11Resource* depthSrc,
			ID3D11Resource* mvecSrc,
			ID3D11Resource* reactiveSrc,
			ID3D11Resource* transparencySrc,
			uint32_t eyeWidthIn,
			uint32_t eyeHeightIn,
			uint32_t eyeWidthOut,
			uint32_t eyeHeightOut);

		static bool FinalizeVRPerEyeOutputs(
			ID3D11Resource* colorDst,
			uint32_t eyeWidthOut,
			uint32_t eyeHeightOut);

		// Release all GPU resources owned by Core.
		static void ClearResources();
		static void ClearShaderCache();

		// ── Own VR resources (independent from Upscaling) ──

		// Per-eye intermediate buffers (Default full-eye mode)
		static inline eastl::unique_ptr<Texture2D> vrIntermediateColorIn[2];
		static inline eastl::unique_ptr<Texture2D> vrIntermediateColorOut[2];
		static inline eastl::unique_ptr<Texture2D> vrIntermediateDepth[2];
		static inline eastl::unique_ptr<Texture2D> vrIntermediateMotionVectors[2];
		static inline eastl::unique_ptr<Texture2D> vrIntermediateReactiveMask[2];
		static inline eastl::unique_ptr<Texture2D> vrIntermediateTransparencyMask[2];

		// Subrect-sized textures (Default/Faster subrect mode)
		static inline eastl::unique_ptr<Texture2D> vrSubrectColorIn[2];
		static inline eastl::unique_ptr<Texture2D> vrSubrectColorOut[2];
		static inline eastl::unique_ptr<Texture2D> vrSubrectDepth[2];
		static inline eastl::unique_ptr<Texture2D> vrSubrectMotionVectors[2];
		static inline eastl::unique_ptr<Texture2D> vrSubrectReactiveMask[2];
		static inline eastl::unique_ptr<Texture2D> vrSubrectTransparencyMask[2];
		static inline uint32_t vrSubrectInW = 0, vrSubrectInH = 0, vrSubrectOutW = 0, vrSubrectOutH = 0;

		// Faster mode per-eye output textures (subOutW × subOutH)
		static inline eastl::unique_ptr<Texture2D> vrFasterColorOut[2];
		static inline uint32_t vrFasterOutW = 0, vrFasterOutH = 0;

		// DRS region copy (render-resolution SBS)
		static inline eastl::unique_ptr<Texture2D> vrRenderSBS;
		static inline uint32_t vrRenderSBSW = 0, vrRenderSBSH = 0;

		// DRS stretch compute shader resources
		static inline winrt::com_ptr<ID3D11ComputeShader> vrSubrectStretchCS;
		static inline winrt::com_ptr<ID3D11Buffer> vrSubrectStretchCB;
		static inline winrt::com_ptr<ID3D11SamplerState> vrSubrectStretchSampler;

		// Subrect UV hash for resource recreation detection
		static inline uint64_t activeSubrectUVHash = 0;

	private:
		static bool ExecuteDefaultMode(Streamline& streamline, const VRDlssParams& p);
		static bool ExecuteFasterMode(Streamline& streamline, const VRDlssParams& p);
	};
}
