#include "../PerfMode.h"

#include <algorithm>
#include <cmath>

#include "../../../State.h"
#include "../../Upscaling.h"

// Quality mode -> render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses), avoiding a duplicate scale table here.
#include <FidelityFX/host/ffx_fsr3.h>

// ============================================================================
// PlayerViewRender_Hook: clear postChainDone at PlayerView end
// ============================================================================
// PlayerView covers the entire VR pipeline (World→Post→UI→Submit).
// After func() returns, clear postChainDone so the Present-前 UI chain
// and the next frame use normal VP compression.

void PerfMode::PlayerViewRender_Hook::thunk(void* a1, bool a2, bool a3)
{
	func(a1, a2, a3);

	globals::features::upscaling.perfMode.ClearPostChainDone();
}

// ============================================================================
// BSGraphics_SetDirtyStates_Hook
// ============================================================================
// Wraps DS swap around the engine's RT/DS flush so enlarged-RT draws don't
// rasterizer-clip to the smaller kMAIN DS bounds.
void PerfMode::BSGraphics_SetDirtyStates_Hook::thunk(bool isCompute)
{
	bool swapped = false;
	if (!isCompute)
		swapped = globals::features::upscaling.perfMode.MaybeSwapDSForEnlargedRT();
	func(isCompute);
	if (swapped)
		globals::features::upscaling.perfMode.RestoreSwappedDS();
}

// ============================================================================
// BSGraphics_Renderer_UpdateViewPort_Hook
// ============================================================================
// Post-corrects the engine viewport when the bound RT and the requested VP
// don't agree about render-vs-display extent. Was originally in Hooks.cpp.
void PerfMode::BSGraphics_Renderer_UpdateViewPort_Hook::thunk(RE::BSGraphics::Renderer* a_this, uint32_t a_width, uint32_t a_height, bool a_forceMatchRT)
{
	func(a_this, a_width, a_height, a_forceMatchRT);

	auto& perfMode = globals::features::upscaling.perfMode;
	if (!perfMode.IsHookActive())
		return;

	// During Post intercept enlarged kTEMP/kTOTAL already get the right VP
	// from func() because of their inflated RT dims — don't second-guess it.
	if (perfMode.IsPostInterceptActive())
		return;

	auto* ss = globals::game::shadowState;
	if (!ss)
		return;
	auto& vp = ss->GetVRRuntimeData().viewPort;
	const uint32_t displayW = perfMode.GetDisplayEyeWidth() * 2;
	const uint32_t displayH = perfMode.GetDisplayEyeHeight();
	const uint32_t renderW = perfMode.GetRenderEyeWidth() * 2;
	const uint32_t renderH = perfMode.GetRenderEyeHeight();

	// After the Post chain, UI / submit-prep draws target enlarged kTOTAL
	// at displayRes — expand any renderRes VP the engine sets back up.
	// The fade Draw(30) bypasses this path entirely (direct D3D RSSet-
	// Viewports) and is handled by the Draw vfunc hook in Globals.cpp.
	if (perfMode.IsPostChainDone()) {
		if (static_cast<uint32_t>(vp.Width) == renderW &&
			static_cast<uint32_t>(vp.Height) == renderH) {
			vp.Width = static_cast<float>(displayW);
			vp.Height = static_cast<float>(displayH);
		}
		return;
	}

	// Honor forceMatchRT for displayRes-enlarged RTs — shrinking VP there
	// leaves menu content in a renderRes corner of kTOTAL.
	if (a_forceMatchRT)
		return;

	// Same risk on the non-forceMatchRT path: the menu compositor calls
	// UpdateViewPort(displayW, displayH, false) directly with screen-space
	// dims, and compressing those would clip the BG.
	{
		const uint32_t rtIdx = static_cast<uint32_t>(ss->GetVRRuntimeData().renderTargets[0]);
		if (rtIdx == RE::RENDER_TARGETS::kTOTAL ||
			rtIdx == RE::RENDER_TARGETS::kMENUBG ||
			rtIdx == RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY)
			return;
	}

	// Normal world/depth path: compress displayRes → renderRes so draws
	// stay inside the renderRes-sized kMAIN family.
	if (static_cast<uint32_t>(vp.Width) == displayW &&
		static_cast<uint32_t>(vp.Height) == displayH) {
		vp.Width = static_cast<float>(renderW);
		vp.Height = static_cast<float>(renderH);
	}
}

bool PerfMode::MaybeSwapDSForEnlargedRT()
{
	if (!hookActive || postInterceptActive)
		return false;
	if (autoSwapDSIdx != UINT32_MAX)
		return false;  // re-entry guard

	auto* ss = globals::game::shadowState;
	if (!ss)
		return false;
	auto& srd = ss->GetVRRuntimeData();

	// Only the three RTs PerfMode_MaybeEnlargeRT inflates to displayRes.
	const uint32_t rtIdx = static_cast<uint32_t>(srd.renderTargets[0]);
	if (rtIdx != RE::RENDER_TARGETS::kTOTAL &&
		rtIdx != RE::RENDER_TARGETS::kMENUBG &&
		rtIdx != RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY)
		return false;

	const uint32_t dsIdx = static_cast<uint32_t>(srd.depthStencil);
	if (dsIdx != RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN &&
		dsIdx != RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY)
		return false;

	auto renderer = globals::game::renderer;
	auto& dsData = renderer->GetDepthStencilData();
	auto& bound = dsData.depthStencils[dsIdx];

	// Unbind DS entirely rather than rebind to fakeDSV. fakeDSV is cleared
	// once at init with stencil=0; ISHDRTonemapBlendCinematic (the menu's
	// kMAIN→kTOTAL bridge at event 931 in the baseline capture) reads
	// stencil to mask sky vs. world, so a wrong stencil value discards every
	// pixel and the BG never reaches kTOTAL. Fullscreen IS shaders and the
	// menu UI quad don't actually depth-test, so nullptr DS is safe and
	// sidesteps the stencil-content mismatch. Swap pattern matches UIPass-
	// Dispatch_Hook (all 8 view slots).
	for (int i = 0; i < 8; ++i) {
		autoSwapSavedViews[i] = bound.views[i];
		if (bound.views[i])
			bound.views[i] = nullptr;
	}
	for (int i = 0; i < 8; ++i) {
		autoSwapSavedReadOnlyViews[i] = bound.readOnlyViews[i];
		if (bound.readOnlyViews[i])
			bound.readOnlyViews[i] = nullptr;
	}
	autoSwapDSIdx = dsIdx;
	return true;
}

void PerfMode::RestoreSwappedDS()
{
	if (autoSwapDSIdx == UINT32_MAX)
		return;
	auto renderer = globals::game::renderer;
	auto& dsData = renderer->GetDepthStencilData();
	auto& bound = dsData.depthStencils[autoSwapDSIdx];
	for (int i = 0; i < 8; ++i)
		bound.views[i] = autoSwapSavedViews[i];
	for (int i = 0; i < 8; ++i)
		bound.readOnlyViews[i] = autoSwapSavedReadOnlyViews[i];
	autoSwapDSIdx = UINT32_MAX;
}

// ============================================================================
// CreateRenderTarget enlarge — install + per-site thunks
// ============================================================================
// Three specific call sites inside BSShaderRenderTargets::Create produce the
// displayRes-enlarged RTs (kMENUBG, kIMAGESPACE_TEMP_COPY, kTOTAL). Offsets
// identified in Ghidra (see CreateRT_k* labels inside the renamed
// BSShaderRenderTargets__Create function in SkyrimVR.exe). VR-only.

// ============================================================================
// ID3D11DeviceContext_Draw_Hook (vtable index 13)
// ============================================================================
// Engine fade-overlay Draw(30) fires after the Post chain and before Submit.
// Under PerfMode the draw's VP is computed at renderRes while the RT (kTOTAL)
// is displayRes — partial-screen "black stamp" without this swap. Gate on
// VertexCount==30 + isVR keeps the cost a single comparison on flat / non-
// fade draws.
void PerfMode::ID3D11DeviceContext_Draw_Hook::thunk(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
{
	if (VertexCount == 30 && globals::game::isVR) {
		auto& perfMode = globals::features::upscaling.perfMode;
		if (perfMode.IsHookActive() && perfMode.IsPostChainDone()) {
			UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
			This->RSGetViewports(&numVP, savedVP);

			D3D11_VIEWPORT vp{};
			vp.Width = static_cast<float>(perfMode.GetDisplayEyeWidth() * 2);
			vp.Height = static_cast<float>(perfMode.GetDisplayEyeHeight());
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			This->RSSetViewports(1, &vp);

			func(This, VertexCount, StartVertexLocation);

			if (numVP > 0)
				This->RSSetViewports(numVP, savedVP);
			return;
		}
	}
	func(This, VertexCount, StartVertexLocation);
}

void PerfMode::InstallFadeOverlayHook(ID3D11DeviceContext* context)
{
	if (!globals::game::isVR || !context)
		return;
	stl::detour_vfunc<13, ID3D11DeviceContext_Draw_Hook>(context);
}
