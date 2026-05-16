// ============================================================================
// Modes.cpp — Default / Faster / Extreme DLSS execution strategies
// ============================================================================
//
// Each mode composes Ops primitives (snapshot, stretch, crop, blend…) in a
// different order.  Router resolves VRDlssParams and dispatches.
//
// ============================================================================

#include "Core.h"
#include "Ops.h"
#include "Params.h"

#include "../../Globals.h"
#include "../../Utils/Subrect.h"
#include "../Upscaling/Streamline.h"

namespace DlssEnhancer
{
	using namespace Ops;

	// ── Router: resolves params via Params module, dispatches to the selected mode ──

	bool Core::ExecuteVRDlssCore(Streamline& streamline,
		ID3D11Resource* upscalingTexture, ID3D11Resource* depthTexture,
		ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11Resource* motionVectors)
	{
		auto p = VRDlssParams::Resolve(upscalingTexture, depthTexture, reactiveMask, transparencyMask, motionVectors);

		// Detect UV/mode change → destroy DLSS resources so SL recreates them at the new size
		uint64_t uvHash = ComputeSubrectUVHash(p.leftUV, (uint32_t)p.mode);
		if (uvHash != Core::activeSubrectUVHash) {
			logger::info("[DLSSENHANCER] Subrect UV or mode changed, recreating DLSS resources");
			streamline.DestroyDLSSResources();
			Core::activeSubrectUVHash = uvHash;
		}

		switch (p.mode) {
		case DlssEnhancerFeature::DlssMode::kFaster:
			return ExecuteFasterMode(streamline, p);
		case DlssEnhancerFeature::DlssMode::kExtreme:
			return ExecuteExtremeMode(streamline, p);
		default:
			return ExecuteDefaultMode(streamline, p);
		}
	}

	// ── Default mode: per-eye isolation, 2 resource sets, 2 evaluates ──

	bool Core::ExecuteDefaultMode(Streamline& streamline, const VRDlssParams& p)
	{
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
		// set sized to LEFT-eye subrect dimensions. This is correct as long as
		// Util::Subrect's auto-mirror keeps leftUV.w == rightUV.w and
		// leftUV.h == rightUV.h. Per-eye sizing inside the loop below computes
		// real per-eye extents (CodeRabbit Major @ original PR Modes.cpp:80).
		uint32_t allocSubInW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * p.leftUV.w));
		uint32_t allocSubInH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * p.leftUV.h));
		uint32_t allocSubOutW = std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * p.leftUV.w));
		uint32_t allocSubOutH = std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * p.leftUV.h));

		EnsureVRSubrectTextures(allocSubInW, allocSubInH, allocSubOutW, allocSubOutH,
			p.colorSrc, p.motionVectors, p.reactiveMask, p.transparencyMask);

		// Snapshot + Stretch DRS → kMAIN (fill full-eye background)
		SnapshotSBS(p.colorSrc, p.renderW, p.renderH);

		// Periphery temporal smooth: blend with reprojected history before stretch
		// so the periphery (the non-foveal region) gets temporal AA that DLSS
		// doesn't provide for the cropped-out area.
		ID3D11ShaderResourceView* stretchSrc = nullptr;
		if (globals::features::dlssEnhancer.GetPeripheryAAMode() == DlssEnhancerFeature::PeripheryAAMode::kTemporalSmooth) {
			EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
			stretchSrc = TemporalSmoothSBS(p.renderW, p.renderH);
		}

		StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, stretchSrc);

		// Crop subrect per-eye from snapshot (not kMAIN which was overwritten by stretch)
		auto context = globals::d3d::context;
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing: fixes CodeRabbit Major @ original PR Modes.cpp:80
			// (right-eye must use rightUV.w/h, not leftUV's).
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
				extentIn, extentOut, subOutW);
		}

		// Write DLSS output back at subrect position (with optional blend)
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing: fixes CodeRabbit Major @ original PR Modes.cpp:80
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
		const Util::Subrect::UVRegion* eyeUVs[2] = { &p.leftUV, &p.rightUV };

		// NOTE: EnsureFasterOutputTextures allocates one per-eye texture set
		// sized to LEFT-eye subrect dimensions. Correct as long as Util::Subrect
		// auto-mirror keeps leftUV.w == rightUV.w / leftUV.h == rightUV.h.
		// Per-eye DLSS extents are recomputed inside the loop below to fix
		// CodeRabbit Major @ original PR Modes.cpp:146 (right-eye must use rightUV).
		uint32_t allocSubOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * p.leftUV.w));
		uint32_t allocSubOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * p.leftUV.h));

		// Step 1: Ensure per-eye output textures
		EnsureFasterOutputTextures(allocSubOutW, allocSubOutH, p.colorSrc);

		// Step 2: DLSS reads directly from kMAIN via extent offsets → per-eye output
		// sl::Extent field order is {top, left, width, height} — Y before X
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing: fixes CodeRabbit Major @ original PR Modes.cpp:146
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
				p.colorSrc, Core::vrFasterColorOut[i]->resource.get(),
				p.depthTexture, p.motionVectors,
				p.reactiveMask, p.transparencyMask,
				extentIn, extentOut, subOutW);
		}

		// Step 3: Snapshot + Stretch DRS → kMAIN (subrect only)
		if (!p.isFullEye) {
			SnapshotSBS(p.colorSrc, p.renderW, p.renderH);

			// Periphery temporal smooth for the stretched periphery (DLSS only
			// covers the foveal subrect; the rest gets temporal AA here).
			ID3D11ShaderResourceView* stretchSrc = nullptr;
			if (globals::features::dlssEnhancer.GetPeripheryAAMode() == DlssEnhancerFeature::PeripheryAAMode::kTemporalSmooth) {
				EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
				stretchSrc = TemporalSmoothSBS(p.renderW, p.renderH);
			}

			StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, stretchSrc);
		}

		// Step 4: Copy DLSS output back (with optional blend)
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing: fixes CodeRabbit Major @ original PR Modes.cpp:146
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

	// ── Extreme mode: combined strip, 1 resource set, 1 evaluate ──
	// Both eyes' subrects merged horizontally into one long texture. Trades a
	// pre-DLSS copy for a single Streamline evaluate (vs Default/Faster's two)
	// — wins on GPUs where DLSS dispatch overhead dominates the per-eye copy.

	bool Core::ExecuteExtremeMode(Streamline& streamline, const VRDlssParams& p)
	{
		const Util::Subrect::UVRegion* eyeUVs[2] = { &p.leftUV, &p.rightUV };

		// Strip allocation uses left-eye dims (auto-mirror constraint, same as
		// Default/Faster). Per-eye loop below uses real per-eye sizes for the
		// CopySubresourceRegion box dimensions.
		uint32_t allocSubInW = p.isFullEye ? p.eyeWidthIn : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * p.leftUV.w));
		uint32_t allocSubInH = p.isFullEye ? p.eyeHeightIn : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * p.leftUV.h));
		uint32_t allocSubOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * p.leftUV.w));
		uint32_t allocSubOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * p.leftUV.h));
		uint32_t stripInW = allocSubInW * 2;
		uint32_t stripOutW = allocSubOutW * 2;

		EnsureExtremeStripTextures(stripInW, allocSubInH, stripOutW, allocSubOutH,
			p.colorSrc, p.motionVectors, p.reactiveMask, p.transparencyMask);

		auto context = globals::d3d::context;

		// Snapshot + Stretch DRS → kMAIN if subrect (fills the periphery
		// outside the strip with stretched render-res content).
		if (!p.isFullEye) {
			SnapshotSBS(p.colorSrc, p.renderW, p.renderH);

			ID3D11ShaderResourceView* stretchSrc = nullptr;
			if (globals::features::dlssEnhancer.GetPeripheryAAMode() == DlssEnhancerFeature::PeripheryAAMode::kTemporalSmooth) {
				EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
				stretchSrc = TemporalSmoothSBS(p.renderW, p.renderH);
			}

			StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, stretchSrc);
		}

		// Copy both eyes' subrects into the strip. Color reads from the
		// pre-stretch snapshot when available (subrect path) so we don't read
		// kMAIN after StretchDRSBothEyes overwrote it.
		ID3D11Resource* colorReadSrc = (!p.isFullEye && Core::vrRenderSBS) ? Core::vrRenderSBS->resource.get() : p.colorSrc;
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing (same fix pattern as Default/Faster — CodeRabbit
			// Major @ original PR Modes.cpp:80). Each eye reads its own UV
			// extent rather than blindly using leftUV's.
			uint32_t subInW = p.isFullEye ? p.eyeWidthIn : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * uv.w));
			uint32_t subInH = p.isFullEye ? p.eyeHeightIn : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * uv.h));

			uint32_t cropX = p.isFullEye ? 0 : (uint32_t)(uv.x * p.eyeWidthIn);
			uint32_t cropY = p.isFullEye ? 0 : (uint32_t)(uv.y * p.eyeHeightIn);
			uint32_t sbsX = (i == 1 ? p.eyeWidthIn : 0) + cropX;
			D3D11_BOX box = { sbsX, cropY, 0, sbsX + subInW, cropY + subInH, 1 };
			uint32_t stripDstX = i * allocSubInW;

			context->CopySubresourceRegion(Core::vrExtremeStripColorIn->resource.get(), 0, stripDstX, 0, 0, colorReadSrc, 0, &box);
			context->CopySubresourceRegion(Core::vrExtremeStripDepth->resource.get(), 0, stripDstX, 0, 0, p.depthTexture, 0, &box);
			context->CopySubresourceRegion(Core::vrExtremeStripMotionVectors->resource.get(), 0, stripDstX, 0, 0, p.motionVectors, 0, &box);
			if (p.reactiveMask)
				context->CopySubresourceRegion(Core::vrExtremeStripReactiveMask->resource.get(), 0, stripDstX, 0, 0, p.reactiveMask, 0, &box);
			if (p.transparencyMask)
				context->CopySubresourceRegion(Core::vrExtremeStripTransparencyMask->resource.get(), 0, stripDstX, 0, 0, p.transparencyMask, 0, &box);
		}

		// Single DLSS evaluate on the entire strip. Note: dev's
		// Streamline::EvaluateDLSS takes outputWidth (no outputHeight); the
		// PR's version had both. Drop the trailing arg.
		sl::Extent extentIn{ 0, 0, stripInW, allocSubInH };
		sl::Extent extentOut{ 0, 0, stripOutW, allocSubOutH };
		streamline.EvaluateDLSS(streamline.viewport, 0,
			Core::vrExtremeStripColorIn->resource.get(), Core::vrExtremeStripColorOut->resource.get(),
			Core::vrExtremeStripDepth->resource.get(), Core::vrExtremeStripMotionVectors->resource.get(),
			p.reactiveMask ? Core::vrExtremeStripReactiveMask->resource.get() : nullptr,
			p.transparencyMask ? Core::vrExtremeStripTransparencyMask->resource.get() : nullptr,
			extentIn, extentOut, stripOutW);

		// Write each eye's output back. The blend uses srcOffsetX = i*subOutW
		// (per-eye, NOT leftUV-derived) so each eye reads its half of the
		// strip's output texture. SubrectBlendCS feather edge math now uses
		// tid.xy local coords (PR-3b shader fix) so the offset doesn't break
		// the band.
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			uint32_t subOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * uv.w));
			uint32_t subOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * uv.h));

			uint32_t dstCropX = p.isFullEye ? 0 : (uint32_t)(uv.x * p.eyeWidthOut);
			uint32_t dstCropY = p.isFullEye ? 0 : (uint32_t)(uv.y * p.eyeHeightOut);
			uint32_t dstX = (i == 1 ? p.eyeWidthOut : 0) + dstCropX;
			uint32_t stripSrcX = i * allocSubOutW;
			BlendSubrectToOutput(Core::vrExtremeStripColorOut->resource.get(), p.colorDst, p.colorDstUAV,
				dstX, dstCropY, subOutW, subOutH, stripSrcX);
		}

		return true;
	}
}
