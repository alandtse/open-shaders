#pragma once

// ============================================================================
// DLSSperf — render-target size hook + post-processing interception
// ============================================================================
//
// Opt-in VR upscaling feature. Hooks BSOpenVR::GetRenderTargetSize so all
// engine render targets are allocated at a small RenderRes while DLSS writes
// its output to a private DisplayRes testTexture. Ships standalone — the
// "DlssEnhancer" prerequisite from earlier drafts no longer applies.
//
//  Benefits:
//   - VRAM and bandwidth savings proportional to the quality-mode scale ratio.
//   - UpscaleRT is no longer needed.
//   - Game menus are no longer occluded by the upscaler output.
//
//  Current limitations:
//   - Post-processing still runs on renderRes kMAIN via a 3x3-box downscale
//     of testTexture (see BoxDownscalePS.hlsl). Performance is good and
//     visual loss is minimal. Once the post chain is rewritten to consume
//     testTexture natively the downscale can be removed.
//   - Main menu / pause backgrounds render through a path that doesn't pass
//     through Main_PostProcessing, so HandlePostProcessing's two-layer swap
//     doesn't wrap them. The 3D background shows at renderRes stretched
//     over displayRes; the UI itself draws correctly at displayRes.
//
// ============================================================================

#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <winrt/base.h>

struct DLSSperf
{
	void SetupResources();
	void DrawSettings();

	// Phase 1: standalone test texture that receives Upscaling output instead of kMAIN.
	// Returns nullptr when not ready.
	ID3D11Texture2D* GetTestTexture() const { return testTexture.get(); }
	ID3D11ShaderResourceView* GetTestTextureSRV() const { return testTextureSRV.get(); }
	ID3D11UnorderedAccessView* GetTestTextureUAV() const { return testTextureUAV.get(); }
	ID3D11Texture2D* GetRefraTempTex() const { return refraTempTex.get(); }
	ID3D11ShaderResourceView* GetRefraTempSRV() const { return refraTempSRV.get(); }

	// Phase 2: resolution hook status
	bool IsHookActive() const { return hookActive; }
	bool IsPostInterceptActive() const { return postInterceptActive; }
	bool IsPostChainDone() const { return postChainDone; }
	void ClearPostChainDone() { postChainDone = false; }
	uint32_t GetDisplayEyeWidth() const { return displayEyeWidth; }
	uint32_t GetDisplayEyeHeight() const { return displayEyeHeight; }
	uint32_t GetRenderEyeWidth() const { return renderEyeWidth; }
	uint32_t GetRenderEyeHeight() const { return renderEyeHeight; }

	// Boot snapshots — engine RTs are sized once against these, so runtime
	// upscaler reads must route through here instead of live `Upscaling::
	// settings` (mid-session UI changes would otherwise break the HMD).
	bool HasBootSnapshot() const { return hookActive; }
	uint32_t GetBootUpscaleMethod() const { return bootUpscaleMethod; }
	uint32_t GetBootQualityMode() const { return bootQualityMode; }

	// Phase 3: real HMD display resolution in SBS format (e.g. 3072×1632)
	// Used by Upscaling pipeline to override polluted screenSize (which equals RenderRes after hook)
	float2 GetDisplayScreenSize() const
	{
		return { static_cast<float>(displayEyeWidth * 2), static_cast<float>(displayEyeHeight) };
	}

	// Phase 2: called from BSShaderRenderTargets_Create::thunk (before func())
	// where BSOpenVR is guaranteed to be available
	void InstallRenderTargetSizeHook();

	// Hybrid Post: tonemap interception via IS shader hooks
	// Call BeginPostIntercept() before func(), EndPostIntercept() after.
	void BeginPostIntercept();
	void EndPostIntercept();

	// Downscale testTexture (3k AA'd DLSS output) → kMAIN (1k)
	// so the HDR pyramid builds from anti-aliased content instead of raw 1k render.
	// Only kMAIN: no-refra reads kMAIN directly; with-refra engine copies kMAIN→kMAIN_COPY.
	void DownscaleToKMain();

	// Post hybrid entry point: called from Upscaling's Main_PostProcessing::thunk.
	// Wraps the engine Post chain with DLSSperf's two-layer struct swap.
	// Keyed on postPipelineReady (set at the end of SetupResources) so a
	// partial-init state can't slip past the gate into a null deref. The
	// runtime upscaler-method gate is enforced separately by callers (the
	// engine's BSOpenVR hook is install-time, so a mid-session DLSS→FSR swap
	// would leave hookActive=true but testTexture stale — see Upscaling.cpp).
	bool ShouldHandlePost() const { return postPipelineReady; }
	void HandlePostProcessing(const std::function<void()>& enginePost);

	// Fake 3k DepthStencil for Post pass DS swap
	ID3D11DepthStencilView* GetFakeDSV() const { return fakeDSV.get(); }

	// Bridge the DLSS-reconstructed menu BG (testTexture, displayRes) into
	// the bound enlarged RT so OpenVR submit sees both BG + UI compositor
	// output. One-shot per frame; gated via blittedFrameId.
	void MaybeBlitMenuBG(uint32_t boundRTIdx);

	// Generic DS swap for draws binding an enlarged RT against kMAIN/kMAIN
	// _COPY DS — without this the rasterizer clips to the smaller DS and
	// fills only the renderRes corner of the enlarged RT. Skipped when
	// postInterceptActive (HandlePostProcessing already redirects DS).
	bool MaybeSwapDSForEnlargedRT();
	void RestoreSwappedDS();

	// Install the 3 named per-site CreateRenderTarget thunks (kMENUBG,
	// kIMAGESPACE_TEMP_COPY, kTOTAL — VR-only) at startup. Called from
	// Hooks::Install in the BSShaderRenderTargets::Create install sequence.
	// Thunks are no-ops outside the BeginEnlarge/EndEnlarge window so flat
	// Skyrim is unaffected.
	void InstallCreateRTThunks();

	// Install the Draw vfunc detour (D3D11DeviceContext vtable index 13)
	// that fixes the scene-fade overlay viewport. Called from Globals::
	// InstallD3DHooks. VR-only; thunk early-outs unless VertexCount==30
	// and the hook is live, so cost is one comparison per Draw call when
	// DLSSperf isn't active.
	void InstallFadeOverlayHook(ID3D11DeviceContext* context);

	// Enlarge window — set true around the engine's BSShaderRenderTargets::
	// Create call from Hooks.cpp's wrapper. The 3 installed thunks read
	// enlargeActive/Width/Height directly.
	void BeginCreateRTEnlarge();
	void EndCreateRTEnlarge();
	bool IsCreateRTEnlargeActive() const { return enlargeActive; }
	uint32_t GetEnlargeWidth() const { return enlargeWidth; }
	uint32_t GetEnlargeHeight() const { return enlargeHeight; }

private:
	// Phase 1
	winrt::com_ptr<ID3D11Texture2D> testTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> testTextureSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> testTextureUAV;

	// Phase 2: resolution hook state
	bool hookActive = false;

	// Set at the end of SetupResources after every critical Post resource
	// (textures, views, fake DS, downscale shaders, sampler) successfully
	// initialized. ShouldHandlePost() returns this — a partial-init state
	// (e.g., refraTempTex OOM after testTexture succeeds) flips this to false
	// and the engine Post chain runs unwrapped on the small kMAIN, which is
	// visually degraded but stable.
	bool postPipelineReady = false;

	// Post intercept phase flag: when true, VP post-correction is skipped
	// so enlarged kTEMP/kTOTAL get correct 3k VP from engine.
	bool postInterceptActive = false;

	// Post-chain-done flag: set true after EndPostIntercept, cleared at
	// PlayerView end by PlayerViewRender_Hook. When true, UpdateViewPort
	// hook expands VP to displayRes so draws after the Post chain
	// (UI composition, scene fade, submit prep) use the correct
	// display-res VP.
	bool postChainDone = false;

	uint32_t displayEyeWidth = 0;
	uint32_t displayEyeHeight = 0;
	uint32_t renderEyeWidth = 0;
	uint32_t renderEyeHeight = 0;

	// Boot snapshot — see HasBootSnapshot() accessor above.
	uint32_t bootUpscaleMethod = 0;
	uint32_t bootQualityMode = 0;

	// Phase 2: vtable hook for BSOpenVR::GetRenderTargetSize (vfunc 0x12)
	struct GetRenderTargetSize_Hook
	{
		static void thunk(RE::BSOpenVR* a_this, uint32_t* a_width, uint32_t* a_height);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// IS shader hook: ISHDRTonemapBlendCinematic (Render vfunc 0x1 on vtable[3])
	// Chains after FrameAnnotations (if active). Swaps kMAIN SRV + kMAIN DS before
	// tonemap, restores after.
	struct TonemapRender_Hook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool tonemapHookInstalled = false;

	// IS shader hook: ISRefraction (Render vfunc 0x1 on vtable[3])
	// Replay DrawIndexed: func() runs 1k refraction normally, then replays 3k draw with sticky D3D state.
	struct RefractionRender_Hook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool refractionHookInstalled = false;

	// IS shader hook: ISCopy (Render vfunc 0x1 on vtable[3]).
	// The VR main menu / pause compositor uses a single ISCopy draw from kMAIN
	// (RenderRes when DLSSperf is active) into kPROJECTEDMENU (fixed 2048²) or
	// kMENUBG (DisplayRes via enlargement). With a 1:1 viewport the small
	// source gets stamped into the top-left of the larger dest — that's the
	// "main menu looks downscaled" bug. Strategy: let func() draw normally,
	// then if dest > VP, replay the draw with the viewport stretched to the
	// dest's full dims so the sampler-clamped source is rescaled across the
	// whole panel. ISCopy's PS/CB/IA stay sticky on the context after func(),
	// so the replay only needs a VP change + a DrawIndexed.
	struct ISCopyRender_Hook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool isCopyHookInstalled = false;

	// UI pass hook: FinishAccumulatingDispatch (vfunc 0x2A on BSShaderAccumulator)
	// When renderMode==24 (UI pass), swaps KMAIN DS → fakeDS so 3k kMENUBG gets 3k depth.
	struct UIPassDispatch_Hook
	{
		static void thunk(RE::BSGraphics::BSShaderAccumulator* shaderAccumulator, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool uiPassHookInstalled = false;

	// PlayerView end hook: Main_RenderPlayerView (REL 35560/36559)
	// Clears postChainDone after the entire VR pipeline (World→Post→UI→Submit)
	// so Present-前 UI chain and next frame use normal VP compression.
	struct PlayerViewRender_Hook
	{
		static void thunk(void* a1, bool a2, bool a3);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool playerViewHookInstalled = false;

	// Chains via stl::detour_thunk on the same address Hooks.cpp + Terrain-
	// Blending already detour. Wraps MaybeSwapDSForEnlargedRT around the
	// engine's RT/DS flush; runs after the prior thunk so DLSSperf's swap
	// is the innermost wrap.
	struct BSGraphics_SetDirtyStates_Hook
	{
		static void thunk(bool isCompute);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool setDirtyStatesHookInstalled = false;

	// D3D11 Draw vfunc detour. Engine's scene-fade overlay is a Draw(30)
	// that fires after the Post chain and before Submit. Under DLSSperf
	// the draw's VP/vertices are computed at renderRes while the RT
	// (kTOTAL) is displayRes — produces a partial-screen "black stamp"
	// without this swap.
	struct ID3D11DeviceContext_Draw_Hook
	{
		static void thunk(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Post-corrects the engine viewport whenever it differs from our
	// enlarged RTs. Chains via stl::detour_thunk.
	struct BSGraphics_Renderer_UpdateViewPort_Hook
	{
		static void thunk(RE::BSGraphics::Renderer* a_this, uint32_t a_width, uint32_t a_height, bool a_forceMatchRT);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	bool updateViewPortHookInstalled = false;

	// Refraction: 3k temp texture (copy of testTexture) for ISRefraction input
	winrt::com_ptr<ID3D11Texture2D> refraTempTex;
	winrt::com_ptr<ID3D11ShaderResourceView> refraTempSRV;
	// Refraction: RTV for testTexture (ISRefraction 3k output target)
	winrt::com_ptr<ID3D11RenderTargetView> testTextureRTV;

	// Two-layer swap: saved pointers for restore.
	// Outer layer (BeginPostIntercept/EndPostIntercept): kMAIN_COPY DS views
	//   only — the engine writes the post chain's DS through kMAIN_COPY's
	//   depth slot, so we redirect it at the start/end of Post.
	// Inner layer (TonemapRender_Hook): kMAIN + kMAIN_COPY SRVs and kMAIN DS
	//   views — the tonemap shader reads from kMAIN SRV (and the refraction
	//   path reads from kMAIN_COPY SRV); both need to point at testTextureSRV
	//   so the tonemap consumes the AA'd 3k DLSS output instead of the small
	//   kMAIN. savedKMainCopySRV is captured/restored by the inner layer, not
	//   the outer one
	ID3D11DepthStencilView* savedKMainCopyViews[8] = {};
	ID3D11DepthStencilView* savedKMainCopyReadOnlyViews[8] = {};
	ID3D11ShaderResourceView* savedKMainCopySRV = nullptr;
	ID3D11DepthStencilView* savedKMainViews[8] = {};
	ID3D11DepthStencilView* savedKMainReadOnlyViews[8] = {};
	ID3D11ShaderResourceView* savedKMainSRV = nullptr;

	// Fake 3k DepthStencil (DisplayRes, same format as engine kMAIN DS)
	winrt::com_ptr<ID3D11Texture2D> fakeDS;
	winrt::com_ptr<ID3D11DepthStencilView> fakeDSV;

	// autoSwapDSIdx == UINT32_MAX → no active swap; otherwise it's the
	// kMAIN/kMAIN_COPY slot whose views[] were rewritten and must be
	// restored on the matching RestoreSwappedDS().
	ID3D11DepthStencilView* autoSwapSavedViews[8] = {};
	ID3D11DepthStencilView* autoSwapSavedReadOnlyViews[8] = {};
	uint32_t autoSwapDSIdx = UINT32_MAX;

	// Downscale pass: Box 3×3 downscale testTexture (3k) → kMAIN (1k).
	// (Named "boxDownscale" — earlier revisions called this "bilinearCopy"
	// when the implementation was a true bilinear sample. It became a 9-tap
	// box during development; the rename happened pre-release.)
	winrt::com_ptr<ID3D11PixelShader> boxDownscalePS;
	winrt::com_ptr<ID3D11VertexShader> boxDownscaleVS;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;

	// Menu BG blit — fullscreen sample of testTexture into kTOTAL/kMENUBG
	// with format conversion (R16G16B16A16_FLOAT → R8G8B8A8_UNORM via the
	// RTV view). Reuses boxDownscaleVS + linearSampler. blittedFrameId is
	// the per-frame one-shot guard, compared against state->frameCount
	// (PlayerView doesn't fire in main menu, so a flag-clear hook wouldn't
	// reliably reset across all states).
	winrt::com_ptr<ID3D11PixelShader> menuBlitPS;
	uint32_t blittedFrameId = UINT32_MAX;

	// CreateRenderTarget enlarge window — see BeginCreateRTEnlarge.
	bool enlargeActive = false;
	uint32_t enlargeWidth = 0;
	uint32_t enlargeHeight = 0;

	// Per-site CreateRenderTarget thunks. Each fires from a single
	// installed call site within BSShaderRenderTargets::Create and overrides
	// the RT's allocation dimensions when the enlarge window is active.
	// Static `func` ptrs are per-struct so each chains the original
	// CreateRenderTarget call independently.
	struct CreateRT_MenuBG_Hook
	{
		static void thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct CreateRT_ImagespaceTempCopy_Hook
	{
		static void thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct CreateRT_Total_Hook
	{
		static void thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
