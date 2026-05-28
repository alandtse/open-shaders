// ============================================================================
// Modes.cpp — Default / Faster / Extreme DLSS execution strategies
// ============================================================================
//
// Each mode composes Ops primitives (snapshot, stretch, crop, blend…) in a
// different order.  Router resolves VRDlssParams and dispatches.
//
// ============================================================================

#include "Bridge.h"
#include "Core.h"
#include "Ops.h"
#include "Params.h"

#include "../../../Globals.h"
#include "../../../Utils/Subrect.h"
#include "../../Upscaling.h"
#include "../Streamline.h"

namespace FoveatedRenderImpl
{
	using namespace Ops;

	// ── Router: resolves params via Params module, dispatches to the selected mode ──

	bool Core::ExecuteVRDlssCore(Streamline& streamline,
		ID3D11Resource* upscalingTexture, ID3D11Resource* depthTexture,
		ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11Resource* motionVectors)
	{
		auto p = VRDlssParams::Resolve(upscalingTexture, depthTexture, reactiveMask, transparencyMask, motionVectors);

		// Detect UV/mode change → destroy DLSS resources so SL recreates them at
		// the new size. Both eye UVs feed the hash; asymmetric presets (e.g.
		// Nasal Convergence) can change rightUV while leftUV stays put.
		uint64_t uvHash = ComputeSubrectUVHash(p.leftUV, p.rightUV, (uint32_t)p.mode);
		if (uvHash != Core::activeSubrectUVHash) {
			logger::info("[FOVEATED] Subrect UV or mode changed, recreating DLSS resources");
			streamline.DestroyDLSSResources();
			Core::activeSubrectUVHash = uvHash;
		}

		Bridge::foveatedEvaluating = true;
		bool result = (p.mode == FoveatedRender::DlssMode::kFaster) ?
		                  ExecuteFasterMode(streamline, p) :
		                  ExecuteDefaultMode(streamline, p);
		Bridge::foveatedEvaluating = false;
		return result;
	}

	// ── Default mode: per-eye isolation, 2 resource sets, 2 evaluates ──

	bool Core::ExecuteDefaultMode(Streamline& streamline, const VRDlssParams& p)
	{
		// Subrect path needs colorDstUAV (StretchDRSBothEyes writes through it).
		// Full-eye path doesn't touch it. Return false on the subrect path so
		// the router falls back to standard DLSS rather than hitting the null
		// guard inside StretchDRSToFullEye every frame. (CodeRabbit on PR #44.)
		if (!p.isFullEye && !p.colorDstUAV) {
			logger::error("[FOVEATED] ExecuteDefaultMode subrect path missing colorDstUAV — falling back");
			return false;
		}
		if (p.isFullEye) {
			// Full-eye path: same as standard VR DLSS
			if (!PreparePerEyeInputs(
					p.colorSrc, p.depthTexture, p.motionVectors, p.reactiveMask, p.transparencyMask,
					p.eyeWidthIn, p.eyeHeightIn, p.eyeWidthOut, p.eyeHeightOut))
				return false;

			for (uint32_t i = 0; i < 2; ++i) {
				sl::ViewportHandle vp = (i == 1) ? streamline.viewportRight : streamline.viewport;
				sl::Extent extentIn{ 0, 0, p.eyeWidthIn, p.eyeHeightIn };
				sl::Extent extentOut{ 0, 0, p.eyeWidthOut, p.eyeHeightOut };
				streamline.EvaluateDLSS(vp, i,
					Core::vrIntermediateColorIn[i]->resource.get(), Core::vrIntermediateColorOut[i]->resource.get(),
					Core::vrIntermediateDepth[i]->resource.get(), Core::vrIntermediateMotionVectors[i]->resource.get(),
					p.reactiveMask ? Core::vrIntermediateReactiveMask[i]->resource.get() : nullptr,
					p.transparencyMask ? Core::vrIntermediateTransparencyMask[i]->resource.get() : nullptr,
					extentIn, extentOut, p.eyeWidthOut);
			}

			return FinalizePerEyeOutputs(p.colorDst, p.eyeWidthOut, p.eyeHeightOut);
		}

		// ── Subrect path: crop per-eye, DLSS at subrect size, stretch back ──

		const Util::Subrect::UVRegion* eyeUVs[2] = { &p.leftUV, &p.rightUV };

		// NOTE: EnsureVRSubrectTextures allocates a single shared per-eye texture
		// set sized to LEFT-eye subrect dimensions. Correct only while
		// Util::Subrect's auto-mirror keeps leftUV.w/h == rightUV.w/h — the
		// per-eye loop below uses the eye's own uv for the real extents.
		uint32_t allocSubInW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * p.leftUV.w));
		uint32_t allocSubInH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * p.leftUV.h));
		uint32_t allocSubOutW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * p.leftUV.w));
		uint32_t allocSubOutH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * p.leftUV.h));

		EnsureVRSubrectTextures(allocSubInW, allocSubInH, allocSubOutW, allocSubOutH,
			p.colorSrc, p.motionVectors, p.reactiveMask, p.transparencyMask);

		// Snapshot + clear HMD hidden-area ring before cropping into subrect inputs.
		SnapshotSBS(p.colorSrc, p.renderW, p.renderH);
		ClearHMDMaskOnSnapshot(p);
		StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, MaybeTemporalSmooth(p));

		// Crop subrect per-eye from mask-cleared snapshot (not kMAIN which was overwritten by stretch)
		auto context = globals::d3d::context;
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing — right eye uses rightUV.w/h, not leftUV.
			uint32_t subInW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * uv.w));
			uint32_t subInH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * uv.h));
			uint32_t subOutW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * uv.w));
			uint32_t subOutH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * uv.h));

			uint32_t cropX = (uint32_t)(uv.x * p.eyeWidthIn);
			uint32_t cropY = (uint32_t)(uv.y * p.eyeHeightIn);
			uint32_t sbsX = (i == 1 ? p.eyeWidthIn : 0) + cropX;
			D3D11_BOX sbsCrop = { sbsX, cropY, 0, sbsX + subInW, cropY + subInH, 1 };

			context->CopySubresourceRegion(Core::vrSubrectColorIn[i]->resource.get(), 0, 0, 0, 0, Core::vrRenderSBS->resource.get(), 0, &sbsCrop);
			context->CopySubresourceRegion(Core::vrSubrectDepth[i]->resource.get(), 0, 0, 0, 0, p.depthTexture, 0, &sbsCrop);
			context->CopySubresourceRegion(Core::vrSubrectMotionVectors[i]->resource.get(), 0, 0, 0, 0, p.motionVectors, 0, &sbsCrop);
			if (p.reactiveMask)
				context->CopySubresourceRegion(Core::vrSubrectReactiveMask[i]->resource.get(), 0, 0, 0, 0, p.reactiveMask, 0, &sbsCrop);
			if (p.transparencyMask)
				context->CopySubresourceRegion(Core::vrSubrectTransparencyMask[i]->resource.get(), 0, 0, 0, 0, p.transparencyMask, 0, &sbsCrop);

			sl::ViewportHandle vp = (i == 1) ? streamline.viewportRight : streamline.viewport;
			sl::Extent extentIn{ 0, 0, subInW, subInH };
			sl::Extent extentOut{ 0, 0, subOutW, subOutH };
			streamline.EvaluateDLSS(vp, i,
				Core::vrSubrectColorIn[i]->resource.get(), Core::vrSubrectColorOut[i]->resource.get(),
				Core::vrSubrectDepth[i]->resource.get(), Core::vrSubrectMotionVectors[i]->resource.get(),
				p.reactiveMask ? Core::vrSubrectReactiveMask[i]->resource.get() : nullptr,
				p.transparencyMask ? Core::vrSubrectTransparencyMask[i]->resource.get() : nullptr,
				extentIn, extentOut, subOutW, subOutH);
		}

		// Write DLSS output back at subrect position (with optional blend)
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing.
			uint32_t subOutW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * uv.w));
			uint32_t subOutH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * uv.h));

			uint32_t dstCropX = (uint32_t)(uv.x * p.eyeWidthOut);
			uint32_t dstCropY = (uint32_t)(uv.y * p.eyeHeightOut);
			uint32_t dstX = (i == 1 ? p.eyeWidthOut : 0) + dstCropX;
			BlendSubrectToOutput(Core::vrSubrectColorOut[i]->resource.get(), p.colorDst, p.colorDstUAV,
				dstX, dstCropY, subOutW, subOutH);
		}

		return true;
	}

	// ── Faster mode: DLSS reads directly from SBS via extents, per-eye output, 2 evaluates ──
	// Input:  kMAIN/depth/mvec SBS textures using extent offsets (zero input copies).
	// Output: per-eye independent textures with extent {0,0}.
	// Flow:   DLSS read → snapshot+stretch background → copy outputs back to kMAIN.

	bool Core::ExecuteFasterMode(Streamline& streamline, const VRDlssParams& p)
	{
		// Subrect path needs colorDstUAV (StretchDRSBothEyes writes through it
		// in Step 3). Full-eye Faster skips Step 3 — don't reject it here just
		// because the UAV isn't bound.
		if (!p.isFullEye && !p.colorDstUAV) {
			logger::error("[FOVEATED] ExecuteFasterMode subrect path missing colorDstUAV — falling back");
			return false;
		}
		const Util::Subrect::UVRegion* eyeUVs[2] = { &p.leftUV, &p.rightUV };

		// NOTE: EnsureFasterOutputTextures allocates one per-eye texture set
		// sized to LEFT-eye subrect dimensions. Correct only while Util::Subrect
		// auto-mirror keeps leftUV.w/h == rightUV.w/h. Per-eye DLSS extents
		// below use the eye's own uv.
		uint32_t allocSubOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * p.leftUV.w));
		uint32_t allocSubOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * p.leftUV.h));

		// Step 1: Ensure per-eye output textures
		EnsureFasterOutputTextures(allocSubOutW, allocSubOutH, p.colorSrc);

		// Step 2a: Snapshot kMAIN into vrRenderSBS so we can clear the HMD
		// hidden-area ring without writing to kMAIN itself. Without this clear
		// DLSS's temporal accumulation drags Skyrim's default sky clear from
		// the masked-out edge into the visible region on fast head motion —
		// the standard Streamline path (Streamline.cpp) and Default mode both
		// pre-clear via per-eye intermediates.
		SnapshotSBS(p.colorSrc, p.renderW, p.renderH);
		ClearHMDMaskOnSnapshot(p);
		ID3D11Resource* dlssColorSrc = (Core::vrRenderSBS ? Core::vrRenderSBS->resource.get() : p.colorSrc);

		// Step 2b: DLSS reads from the mask-cleared SBS snapshot via extent offsets
		// → per-eye output. sl::Extent field order is {top, left, width, height}.
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing.
			uint32_t subInW = p.isFullEye ? p.eyeWidthIn : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * uv.w));
			uint32_t subInH = p.isFullEye ? p.eyeHeightIn : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * uv.h));
			uint32_t subOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * uv.w));
			uint32_t subOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * uv.h));

			uint32_t cropX = p.isFullEye ? 0 : (uint32_t)(uv.x * p.eyeWidthIn);
			uint32_t cropY = p.isFullEye ? 0 : (uint32_t)(uv.y * p.eyeHeightIn);
			uint32_t inOffsetX = (i == 1 ? p.eyeWidthIn : 0) + cropX;
			uint32_t inOffsetY = cropY;

			sl::ViewportHandle vp = (i == 1) ? streamline.viewportRight : streamline.viewport;
			sl::Extent extentIn{ inOffsetY, inOffsetX, subInW, subInH };
			sl::Extent extentOut{ 0, 0, subOutW, subOutH };

			streamline.EvaluateDLSS(vp, i,
				dlssColorSrc, Core::vrFasterColorOut[i]->resource.get(),
				p.depthTexture, p.motionVectors,
				p.reactiveMask, p.transparencyMask,
				extentIn, extentOut, subOutW, subOutH);
		}

		// Step 3: Stretch DRS → kMAIN (subrect only) — snapshot reused from Step 2a.
		if (!p.isFullEye) {
			StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, MaybeTemporalSmooth(p));
		}

		// Step 4: Copy DLSS output back (with optional blend)
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing.
			uint32_t subOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * uv.w));
			uint32_t subOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * uv.h));

			uint32_t dstCropX = p.isFullEye ? 0 : (uint32_t)(uv.x * p.eyeWidthOut);
			uint32_t dstCropY = p.isFullEye ? 0 : (uint32_t)(uv.y * p.eyeHeightOut);
			uint32_t dstX = (i == 1 ? p.eyeWidthOut : 0) + dstCropX;
			BlendSubrectToOutput(Core::vrFasterColorOut[i]->resource.get(), p.colorDst, p.colorDstUAV,
				dstX, dstCropY, subOutW, subOutH);
		}

		return true;
	}

}
