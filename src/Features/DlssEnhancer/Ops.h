#pragma once

#include "Core.h"
#include "Utils/Subrect.h"

#include <EASTL/unique_ptr.h>
#include <d3d11_4.h>

class Texture2D;

// Primitive operations for the DlssEnhancer VR DLSS pipeline.
//
// Each function is a self-contained building block. Mode pipelines in
// Modes.cpp compose these in different orders to form the Default, Faster,
// and Extreme strategies.
namespace DlssEnhancer::Ops
{
	// Texture creation helper.
	eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	// Lazy/idempotent resource ensure helpers.
	void EnsureVRIntermediateTextures(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	void EnsureVRSubrectTextures(uint32_t subInW, uint32_t subInH, uint32_t subOutW, uint32_t subOutH,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	void EnsureFasterOutputTextures(uint32_t subOutW, uint32_t subOutH, ID3D11Resource* colorSrc);

	void EnsureExtremeStripTextures(uint32_t stripInW, uint32_t stripInH, uint32_t stripOutW, uint32_t stripOutH,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	void EnsureVRRenderSBS(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc);

	// Copy full-eye slices from SBS textures into per-eye intermediates.
	bool PreparePerEyeInputs(ID3D11Resource* colorSrc, ID3D11Resource* depthSrc, ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc,
		uint32_t eyeWidthIn, uint32_t eyeHeightIn, uint32_t eyeWidthOut, uint32_t eyeHeightOut);

	// Copy per-eye output intermediates back into the SBS output texture.
	bool FinalizePerEyeOutputs(ID3D11Resource* colorDst, uint32_t eyeWidthOut, uint32_t eyeHeightOut);

	// Snapshot kMAIN DRS data into vrRenderSBS.
	void SnapshotSBS(ID3D11Resource* src, uint32_t renderW, uint32_t renderH);

	// Compute-shader stretch of a single eye region from renderSBS → kMAIN.
	void StretchDRSToFullEye(ID3D11ShaderResourceView* renderSBSSRV, ID3D11UnorderedAccessView* kMainUAV,
		uint32_t dstOffsetX, uint32_t dstWidth, uint32_t dstHeight,
		uint32_t srcOffsetX, uint32_t srcWidth, uint32_t srcHeight,
		uint32_t srcEyeWidth, uint32_t srcEyeHeight);

	// StretchDRS for both eyes (snapshot must already exist in vrRenderSBS).
	void StretchDRSBothEyes(ID3D11UnorderedAccessView* dstUAV, uint32_t eyeWidthOut, uint32_t eyeHeightOut,
		uint32_t eyeWidthIn, uint32_t eyeHeightIn, uint32_t renderW, uint32_t renderH,
		ID3D11ShaderResourceView* srcOverride = nullptr);

	// Periphery temporal smooth: ensure ping-pong history textures and mvec
	// SRV cache for the render-res SBS smoothing pass.
	void EnsureTemporalResources(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc);

	// Apply temporal smoothing on vrRenderSBS. Returns SRV of smoothed result
	// (for use as srcOverride in StretchDRSBothEyes). SnapshotSBS must be
	// called first.
	ID3D11ShaderResourceView* TemporalSmoothSBS(uint32_t renderW, uint32_t renderH);

	// Blend a DLSS subrect output onto the destination at (offsetX, offsetY).
	// Falls back to CopySubresourceRegion when blend mode is kHardCopy;
	// otherwise dispatches the feather/dither subrect-blend CS into dstUAV.
	void BlendSubrectToOutput(ID3D11Resource* dlssSrc, ID3D11Resource* dst, ID3D11UnorderedAccessView* dstUAV,
		uint32_t dstOffsetX, uint32_t dstOffsetY, uint32_t subWidth, uint32_t subHeight, uint32_t srcOffsetX = 0);

	// Hash of UV + mode for change detection (forces SL DLSS resource recreation).
	uint64_t ComputeSubrectUVHash(const Util::Subrect::UVRegion& uv, uint32_t mode);
}
