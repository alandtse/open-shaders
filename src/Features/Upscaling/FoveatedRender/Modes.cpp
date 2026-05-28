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

		switch (p.mode) {
		case FoveatedRender::DlssMode::kFaster:
			return ExecuteFasterMode(streamline, p);
		case FoveatedRender::DlssMode::kExtreme:
			return ExecuteExtremeMode(streamline, p);
		default:
			return ExecuteDefaultMode(streamline, p);
		}
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

		// Snapshot + Stretch DRS → kMAIN (fill full-eye background)
		SnapshotSBS(p.colorSrc, p.renderW, p.renderH);

		// Periphery temporal smooth: blend with reprojected history before stretch
		// so the periphery gets temporal AA that DLSS doesn't cover for the cropped area.
		ID3D11ShaderResourceView* stretchSrc = nullptr;
		if (globals::features::upscaling.foveatedRender.GetPeripheryAAMode() == FoveatedRender::PeripheryAAMode::kTemporalSmooth) {
			EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
			stretchSrc = TemporalSmoothSBS(p.renderW, p.renderH);
		}

		StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, stretchSrc);

		// Crop subrect per-eye from snapshot (not kMAIN which was overwritten by stretch)
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
		auto& upscaling = globals::features::upscaling;
		auto* depthSRV = globals::game::renderer->GetDepthStencilData()
		                     .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN]
		                     .depthSRV;
		if (Core::vrRenderSBS && Core::vrRenderSBS->uav && depthSRV) {
			// Color target IS the SBS snapshot (not a per-eye buffer), so
			// colorOffsetX must select the eye's half — same as depthOffsetX.
			// ClearHMDMaskCS's default contract assumes the color target is
			// per-eye (colorOffsetX = 0) and was written for Streamline's
			// per-eye intermediates; here we're routing both eyes through one
			// SBS texture so we override both offsets together.
			for (uint32_t i = 0; i < 2; ++i) {
				const uint32_t eyeOffsetX = i * p.eyeWidthIn;
				upscaling.ClearHMDMask(Core::vrRenderSBS->uav.get(), depthSRV,
					p.eyeWidthIn, p.eyeHeightIn, eyeOffsetX, eyeOffsetX);
			}
		}
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
			// Periphery temporal smooth for the stretched periphery.
			ID3D11ShaderResourceView* stretchSrc = nullptr;
			if (globals::features::upscaling.foveatedRender.GetPeripheryAAMode() == FoveatedRender::PeripheryAAMode::kTemporalSmooth) {
				EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
				stretchSrc = TemporalSmoothSBS(p.renderW, p.renderH);
			}
			StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, stretchSrc);
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

	// ── Extreme mode: combined strip, 1 resource set, 1 evaluate ──
	// Both eyes' subrects merged horizontally into one long texture. Trades a
	// pre-DLSS copy for a single Streamline evaluate (vs Default/Faster's two).

	bool Core::ExecuteExtremeMode(Streamline& streamline, const VRDlssParams& p)
	{
		const Util::Subrect::UVRegion* eyeUVs[2] = { &p.leftUV, &p.rightUV };

		// Strip allocation uses left-eye dims (auto-mirror constraint, same as
		// Default/Faster). Per-eye loop uses real per-eye sizes.
		uint32_t allocSubInW = p.isFullEye ? p.eyeWidthIn : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthIn * p.leftUV.w));
		uint32_t allocSubInH = p.isFullEye ? p.eyeHeightIn : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightIn * p.leftUV.h));
		uint32_t allocSubOutW = p.isFullEye ? p.eyeWidthOut : std::max<uint32_t>(1, (uint32_t)(p.eyeWidthOut * p.leftUV.w));
		uint32_t allocSubOutH = p.isFullEye ? p.eyeHeightOut : std::max<uint32_t>(1, (uint32_t)(p.eyeHeightOut * p.leftUV.h));
		uint32_t stripInW = allocSubInW * 2;
		uint32_t stripOutW = allocSubOutW * 2;

		EnsureExtremeStripTextures(stripInW, allocSubInH, stripOutW, allocSubOutH,
			p.colorSrc, p.motionVectors, p.reactiveMask, p.transparencyMask);

		auto context = globals::d3d::context;

		// Snapshot + Stretch DRS → kMAIN if subrect (fills periphery).
		if (!p.isFullEye) {
			SnapshotSBS(p.colorSrc, p.renderW, p.renderH);

			ID3D11ShaderResourceView* stretchSrc = nullptr;
			if (globals::features::upscaling.foveatedRender.GetPeripheryAAMode() == FoveatedRender::PeripheryAAMode::kTemporalSmooth) {
				EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
				stretchSrc = TemporalSmoothSBS(p.renderW, p.renderH);
			}

			StretchDRSBothEyes(p.colorDstUAV, p.eyeWidthOut, p.eyeHeightOut, p.eyeWidthIn, p.eyeHeightIn, p.renderW, p.renderH, stretchSrc);
		}

		// Copy both eyes' subrects into the strip. Color reads from the
		// pre-stretch snapshot when available so we don't read kMAIN after stretch.
		ID3D11Resource* colorReadSrc = (!p.isFullEye && Core::vrRenderSBS) ? Core::vrRenderSBS->resource.get() : p.colorSrc;
		for (uint32_t i = 0; i < 2; ++i) {
			const auto& uv = *eyeUVs[i];
			// Per-eye sizing — same fix as Default/Faster (CodeRabbit Major @ original PR).
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

		// Single DLSS evaluate on the entire strip.
		sl::Extent extentIn{ 0, 0, stripInW, allocSubInH };
		sl::Extent extentOut{ 0, 0, stripOutW, allocSubOutH };
		streamline.EvaluateDLSS(streamline.viewport, 0,
			Core::vrExtremeStripColorIn->resource.get(), Core::vrExtremeStripColorOut->resource.get(),
			Core::vrExtremeStripDepth->resource.get(), Core::vrExtremeStripMotionVectors->resource.get(),
			p.reactiveMask ? Core::vrExtremeStripReactiveMask->resource.get() : nullptr,
			p.transparencyMask ? Core::vrExtremeStripTransparencyMask->resource.get() : nullptr,
			extentIn, extentOut, stripOutW);

		// Write each eye's output back using srcOffsetX = i*allocSubOutW.
		// SubrectBlendCS feather edge math uses tid.xy local coords (PR-3b shader fix)
		// so the strip offset doesn't break the feather band.
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
