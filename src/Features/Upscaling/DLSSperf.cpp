#include "DLSSperf.h"

#include <algorithm>
#include <cmath>

#include "../../State.h"
#include "../Upscaling.h"

// Quality mode → render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses at ConfigureUpscaling), avoiding a duplicate
// scale table here. Decoupled from the original PR's DlssEnhancer::Bridge so
// DLSSperf can ship without the larger enhancer framework.
#include <FidelityFX/host/ffx_fsr3.h>

DLSSperf::FullscreenPassScope::FullscreenPassScope(ID3D11DeviceContext* a_context) :
	ctx(a_context)
{
	ctx->OMGetRenderTargets(1, &savedRTV, &savedDSV);
	ctx->RSGetViewports(&numVP, savedVP);
	ctx->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);
	ctx->OMGetDepthStencilState(&savedDSState, &savedStencilRef);
	ctx->VSGetShader(&savedVS, nullptr, nullptr);
	ctx->PSGetShader(&savedPS, nullptr, nullptr);
	ctx->GSGetShader(&savedGS, nullptr, nullptr);
	ctx->HSGetShader(&savedHS, nullptr, nullptr);
	ctx->DSGetShader(&savedDS, nullptr, nullptr);
	ctx->RSGetState(&savedRS);
	ctx->PSGetSamplers(0, 1, &savedSampler0);
	ctx->PSGetShaderResources(0, 1, &savedSRV0);
	ctx->IAGetInputLayout(&savedIL);
	ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, savedVB, savedVBStride, savedVBOffset);
	ctx->IAGetIndexBuffer(&savedIB, &savedIBFormat, &savedIBOffset);
	ctx->IAGetPrimitiveTopology(&savedTopology);
}

DLSSperf::FullscreenPassScope::~FullscreenPassScope()
{
	// Null the SRV slot before restoring to break any potential SRV-vs-RTV
	// hazard from the pass we just ran (matches the explicit null-pass the
	// previous inline code did).
	ID3D11ShaderResourceView* nullSRV[] = { nullptr };
	ctx->PSSetShaderResources(0, 1, nullSRV);
	ctx->PSSetShaderResources(0, 1, &savedSRV0);

	ctx->OMSetRenderTargets(1, &savedRTV, savedDSV);
	if (numVP > 0)
		ctx->RSSetViewports(numVP, savedVP);
	ctx->OMSetBlendState(savedBlend, savedBlendFactor, savedSampleMask);
	ctx->OMSetDepthStencilState(savedDSState, savedStencilRef);
	ctx->VSSetShader(savedVS, nullptr, 0);
	ctx->PSSetShader(savedPS, nullptr, 0);
	ctx->GSSetShader(savedGS, nullptr, 0);
	ctx->HSSetShader(savedHS, nullptr, 0);
	ctx->DSSetShader(savedDS, nullptr, 0);
	ctx->RSSetState(savedRS);
	ctx->PSSetSamplers(0, 1, &savedSampler0);
	ctx->IASetInputLayout(savedIL);
	ctx->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, savedVB, savedVBStride, savedVBOffset);
	ctx->IASetIndexBuffer(savedIB, savedIBFormat, savedIBOffset);
	ctx->IASetPrimitiveTopology(savedTopology);

	if (savedRTV)
		savedRTV->Release();
	if (savedDSV)
		savedDSV->Release();
	if (savedBlend)
		savedBlend->Release();
	if (savedDSState)
		savedDSState->Release();
	if (savedVS)
		savedVS->Release();
	if (savedPS)
		savedPS->Release();
	if (savedGS)
		savedGS->Release();
	if (savedHS)
		savedHS->Release();
	if (savedDS)
		savedDS->Release();
	if (savedRS)
		savedRS->Release();
	if (savedSampler0)
		savedSampler0->Release();
	if (savedSRV0)
		savedSRV0->Release();
	if (savedIL)
		savedIL->Release();
	for (auto*& vb : savedVB) {
		if (vb)
			vb->Release();
	}
	if (savedIB)
		savedIB->Release();
}

void DLSSperf::InstallRenderTargetSizeHook()
{
	if (!globals::game::isVR)
		return;

	if (hookActive)
		return;

	// Eager capture — get real HMD resolution BEFORE installing hook
	auto* openvr = RE::BSOpenVR::GetSingleton();
	if (!openvr || !openvr->vrSystem) {
		logger::error("[DLSSperf] BSOpenVR or vrSystem not available — hook NOT installed");
		return;
	}

	uint32_t w = 0, h = 0;
	openvr->vrSystem->GetRecommendedRenderTargetSize(&w, &h);
	if (w == 0 || h == 0) {
		logger::error("[DLSSperf] GetRecommendedRenderTargetSize returned {}x{} — hook NOT installed", w, h);
		return;
	}

	displayEyeWidth = w;
	displayEyeHeight = h;

	// BSShaderRenderTargets::Create runs after SKSE feature settings load, so
	// upscaling.settings.qualityMode here reflects the user-saved value. We
	// snapshot the corresponding scale at install time and never re-read it —
	// the engine's RT allocations happen once, so a later UI quality change
	// can't shrink/grow RTs anyway. (Requires a game restart, same as DLSS.)
	//
	// Validate before division: a bad/corrupt JSON could put qualityMode
	// outside FFX's range, returning 0/inf/NaN; that would propagate to bogus
	// renderEye dimensions and silently mis-size every engine RT. Fail closed
	// — leave hookActive=false so the rest of DLSSperf is dormant and DLSS
	// runs on dev's standard path.
	const uint32_t qualityModeRaw = globals::features::upscaling.settings.qualityMode;
	const uint32_t qualityMode = std::clamp<uint32_t>(qualityModeRaw, 0, 4);  // FfxFsr3QualityMode range
	const float scale = ffxFsr3GetUpscaleRatioFromQualityMode(static_cast<FfxFsr3QualityMode>(qualityMode));
	if (!std::isfinite(scale) || scale <= 0.0f) {
		logger::error("[DLSSperf] FFX returned invalid upscale ratio {} for qualityMode {} (raw {}); hook NOT installed", scale, qualityMode, qualityModeRaw);
		return;
	}
	renderEyeWidth = std::max<uint32_t>(1, (uint32_t)(w / scale));
	renderEyeHeight = std::max<uint32_t>(1, (uint32_t)(h / scale));

	// Restart-required settings snapshot is latched by the render-target
	// creation hook, but keep this robust to call-order changes.
	globals::features::upscaling.bootSnapshot.LatchIfNeeded(globals::features::upscaling.settings);

	stl::write_vfunc<0x12, GetRenderTargetSize_Hook>(RE::VTABLE_BSOpenVR[0]);

	// Per-frame detours that used to live in Hooks.cpp. Both addresses are
	// already detoured by core/other features; stl::detour_thunk chains
	// (each new install wraps the prior thunk via its static func ptr).
	if (!setDirtyStatesHookInstalled) {
		stl::detour_thunk<BSGraphics_SetDirtyStates_Hook>(REL::RelocationID(75580, 77386));
		setDirtyStatesHookInstalled = true;
	}
	if (!updateViewPortHookInstalled) {
		stl::detour_thunk<BSGraphics_Renderer_UpdateViewPort_Hook>(REL::RelocationID(75455, 77240));
		updateViewPortHookInstalled = true;
	}

	hookActive = true;
}

void DLSSperf::GetRenderTargetSize_Hook::thunk(RE::BSOpenVR* a_this, uint32_t* a_width, uint32_t* a_height)
{
	// Call original to get real HMD resolution
	func(a_this, a_width, a_height);

	auto& dlssPerf = globals::features::upscaling.dlssPerf;

	*a_width = dlssPerf.renderEyeWidth;
	*a_height = dlssPerf.renderEyeHeight;
}

void DLSSperf::SetupResources()
{
	if (!globals::game::isVR)
		return;

	auto renderer = globals::game::renderer;
	auto& mainRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!mainRT.texture) {
		logger::error("[DLSSperf] kMAIN texture not available in SetupResources");
		return;
	}

	D3D11_TEXTURE2D_DESC mainDesc{};
	static_cast<ID3D11Texture2D*>(mainRT.texture)->GetDesc(&mainDesc);

	D3D11_TEXTURE2D_DESC desc{};
	if (hookActive) {
		desc.Width = displayEyeWidth * 2;
		desc.Height = displayEyeHeight;
	} else {
		desc.Width = mainDesc.Width;
		desc.Height = mainDesc.Height;
	}
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = mainDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

	auto device = globals::d3d::device;
	HRESULT hr = device->CreateTexture2D(&desc, nullptr, testTexture.put());
	if (FAILED(hr)) {
		logger::error("[DLSSperf] Failed to create test texture: {:#x}", (uint32_t)hr);
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	hr = device->CreateShaderResourceView(testTexture.get(), &srvDesc, testTextureSRV.put());
	if (FAILED(hr)) {
		logger::error("[DLSSperf] Failed to create test texture SRV: {:#x}", (uint32_t)hr);
		testTexture = nullptr;
		testTextureUAV = nullptr;
		return;
	}

	// UAV for testTexture
	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		hr = device->CreateUnorderedAccessView(testTexture.get(), &uavDesc, testTextureUAV.put());
		if (FAILED(hr)) {
			logger::error("[DLSSperf] Failed to create testTexture UAV: {:#x}", (uint32_t)hr);
		}
	}

	// RTV for testTexture (ISRefraction output)
	if (hookActive) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = desc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		hr = device->CreateRenderTargetView(testTexture.get(), &rtvDesc, testTextureRTV.put());
		if (FAILED(hr)) {
			logger::error("[DLSSperf] Failed to create testTexture RTV: {:#x}", (uint32_t)hr);
		}
	}

	// refraTempTex: copy of testTexture for ISRefraction input
	if (hookActive) {
		D3D11_TEXTURE2D_DESC refraDesc = desc;
		refraDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		hr = device->CreateTexture2D(&refraDesc, nullptr, refraTempTex.put());
		if (FAILED(hr)) {
			logger::error("[DLSSperf] Failed to create refraTempTex: {:#x}", (uint32_t)hr);
		} else {
			D3D11_SHADER_RESOURCE_VIEW_DESC refraSrvDesc{};
			refraSrvDesc.Format = refraDesc.Format;
			refraSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			refraSrvDesc.Texture2D.MipLevels = 1;
			refraSrvDesc.Texture2D.MostDetailedMip = 0;
			hr = device->CreateShaderResourceView(refraTempTex.get(), &refraSrvDesc, refraTempSRV.put());
			if (FAILED(hr)) {
				logger::error("[DLSSperf] Failed to create refraTempSRV: {:#x}", (uint32_t)hr);
				refraTempTex = nullptr;
			}
		}
	}

	// Fake DepthStencil at DisplayRes, matching engine kMAIN DS format.
	if (hookActive) {
		auto& dsData = renderer->GetDepthStencilData();
		auto* mainDSTex = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
		if (mainDSTex) {
			D3D11_TEXTURE2D_DESC dsDesc{};
			mainDSTex->GetDesc(&dsDesc);

			D3D11_TEXTURE2D_DESC fakeDesc = dsDesc;
			fakeDesc.Width = displayEyeWidth * 2;
			fakeDesc.Height = displayEyeHeight;

			HRESULT hr2 = device->CreateTexture2D(&fakeDesc, nullptr, fakeDS.put());
			if (FAILED(hr2)) {
				logger::error("[DLSSperf] Failed to create fake DS texture: {:#x}", (uint32_t)hr2);
			} else {
				// Create DSV — format depends on typeless base format
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				dsvDesc.Texture2D.MipSlice = 0;

				// Map typeless→typed DSV format
				if (fakeDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
					dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
				else if (fakeDesc.Format == DXGI_FORMAT_R24G8_TYPELESS)
					dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				else if (fakeDesc.Format == DXGI_FORMAT_R32_TYPELESS)
					dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
				else if (fakeDesc.Format == DXGI_FORMAT_R16_TYPELESS)
					dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
				else
					dsvDesc.Format = fakeDesc.Format;  // fallback: hope it's already a DS format

				hr2 = device->CreateDepthStencilView(fakeDS.get(), &dsvDesc, fakeDSV.put());
				if (FAILED(hr2)) {
					logger::error("[DLSSperf] Failed to create fake DSV: {:#x}", (uint32_t)hr2);
					fakeDS = nullptr;
				}
			}
		} else {
			logger::warn("[DLSSperf] kMAIN DS texture not available, skipping fake DS creation");
		}
	}

	if (hookActive && fakeDSV) {
		auto* ctx = globals::d3d::context;
		ctx->ClearDepthStencilView(fakeDSV.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	}

	// IS shader hooks (must be installed AFTER FrameAnnotations)
	if (hookActive && !tonemapHookInstalled) {
		stl::write_vfunc<0x1, TonemapRender_Hook>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[3]);
		tonemapHookInstalled = true;
	}

	if (hookActive && !refractionHookInstalled && testTextureRTV && refraTempSRV) {
		stl::write_vfunc<0x1, RefractionRender_Hook>(RE::VTABLE_BSImagespaceShaderRefraction[3]);
		refractionHookInstalled = true;
	}

	// Menu-background fix: ISCopy is the entire menu post-chain (verified via
	// RenderDoc on a baseline frame — single ISCopy draw, source → kPROJECTED-
	// MENU 2048² / kMENUBG). Hook the same vtable slot FrameAnnotations uses
	// for its passthrough annotation, then replay with a stretched VP when
	// dest > source. No resource dependencies — pure VP/Draw replay.
	if (hookActive && !isCopyHookInstalled) {
		stl::write_vfunc<0x1, ISCopyRender_Hook>(RE::VTABLE_BSImagespaceShaderCopy[3]);
		isCopyHookInstalled = true;
	}

	if (hookActive && !uiPassHookInstalled && fakeDSV) {
		stl::write_vfunc<0x2A, UIPassDispatch_Hook>(RE::VTABLE_BSShaderAccumulator[0]);
		uiPassHookInstalled = true;
	}

	// PlayerView end hook: chains after FrameAnnotations' Main_RenderPlayerView.
	// Clears postChainDone so Present-前 UI and next frame use normal VP.
	if (hookActive && !playerViewHookInstalled) {
		stl::detour_thunk<PlayerViewRender_Hook>(REL::RelocationID(35560, 36559));
		playerViewHookInstalled = true;
	}

	// Downscale + blit shaders
	if (hookActive && !boxDownscalePS) {
		boxDownscalePS.attach(static_cast<ID3D11PixelShader*>(
			Util::CompileShader(L"Data/Shaders/Upscaling/DLSSperf/BoxDownscalePS.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
		if (!boxDownscalePS)
			logger::error("[DLSSperf] Failed to compile BoxDownscalePS");
	}
	if (hookActive && !boxDownscaleVS) {
		boxDownscaleVS.attach(static_cast<ID3D11VertexShader*>(
			Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0")));
		if (!boxDownscaleVS)
			logger::error("[DLSSperf] Failed to compile BoxDownscale VS");
	}
	if (hookActive && !menuBlitPS) {
		menuBlitPS.attach(static_cast<ID3D11PixelShader*>(
			Util::CompileShader(L"Data/Shaders/Upscaling/DLSSperf/MenuBGBlitPS.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
		if (!menuBlitPS)
			logger::error("[DLSSperf] Failed to compile MenuBGBlitPS");
	}
	if (hookActive && !linearSampler) {
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxAnisotropy = 1;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sd, linearSampler.put())))
			logger::error("[DLSSperf] Failed to create linear sampler");
	}

	// Fail-closed pipeline-ready gate
	// hookActive only means "BSOpenVR size hook is live + the engine has been
	// sized at RenderRes." If any of the downstream resources we depend on at
	// Post time failed to create, we'd previously still claim ShouldHandlePost
	// and walk into a null deref inside HandlePostProcessing/Tonemap/Refraction.
	// Compute the readiness flag once, here — every consumer keys off it.
	//
	// The minimum viable set for Post wrapping:
	//   testTexture + testTextureSRV  — read by tonemap inner-swap (always)
	//   fakeDS + fakeDSV              — bound as the 3k DS during Post
	//   boxDownscaleVS/PS + linearSampler — DownscaleToKMain needs these
	// Refraction (refraTempTex/SRV + testTextureRTV) is optional: its hook
	// gates on those resources at install time, so absence just means the
	// refraction draw runs on the engine's 1k path — degraded but stable.
	postPipelineReady =
		hookActive &&
		testTexture && testTextureSRV &&
		fakeDS && fakeDSV &&
		boxDownscaleVS && boxDownscalePS && linearSampler;

	if (hookActive && !postPipelineReady) {
		logger::error(
			"[DLSSperf] Post pipeline failed to initialize fully — Post wrap "
			"disabled, engine RTs remain at RenderRes. Check upstream resource "
			"creation errors above.");
	}
}

// ============================================================================
// TonemapRender_Hook: IS shader hook for ISHDRTonemapBlendCinematic
// ============================================================================
// Installed via stl::write_vfunc<0x1> on vtable[3], chains after FrameAnnotations.
// Inner layer of two-layer swap: swaps kMAIN SRV → testTextureSRV and
// kMAIN DS → fakeDS before tonemap Render(), restores after.

void DLSSperf::TonemapRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& dlssPerf = globals::features::upscaling.dlssPerf;

	if (!dlssPerf.hookActive || !dlssPerf.testTextureSRV || !dlssPerf.fakeDSV) {
		func(imageSpaceShader, shape, param);
		return;
	}

	// Menu/loading-screen path: the engine's bridge into kTOTAL assumes
	// RT.size == kMAIN.size (true for DLAA, broken under DLSS presets where
	// kMAIN is renderRes), so the BG ends up missing and OpenVR reprojects
	// stale content as movement smears. Skip the gameplay SRV/DS hijack and
	// let tonemap run untouched, then call MaybeBlitMenuBG to drive a
	// one-shot Upscaling::Upscale() + MenuBGBlitPS blit of the resulting
	// DLSS-reconstructed testTexture into kTOTAL. Per-frame guarded so it
	// runs at most once per frame regardless of how many menu redraws fire.
	if (globals::state && globals::state->IsMainOrLoadingMenuOpen()) {
		func(imageSpaceShader, shape, param);
		dlssPerf.MaybeBlitMenuBG(RE::RENDER_TARGETS::kTOTAL);
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "DLSSperf::TonemapRender");

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& rtData = renderer->GetRuntimeData();
	auto& dsData = renderer->GetDepthStencilData();

	// --- Swap kMAIN SRV → testTextureSRV (so tonemap reads 3k upscaled color) ---
	auto& kmainRT = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN];
	dlssPerf.savedKMainSRV = kmainRT.SRV;
	kmainRT.SRV = dlssPerf.testTextureSRV.get();

	// --- Also swap kMAIN_COPY SRV (refraction path reads this instead of kMAIN) ---
	auto& kmainCopyRT = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];
	dlssPerf.savedKMainCopySRV = kmainCopyRT.SRV;
	kmainCopyRT.SRV = dlssPerf.testTextureSRV.get();

	// --- Swap kMAIN DS views → fakeDS (so 3k RT doesn't mismatch 1k DS) ---
	auto& kmainDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	for (int i = 0; i < 8; i++) {
		dlssPerf.savedKMainViews[i] = kmainDS.views[i];
		if (kmainDS.views[i])
			kmainDS.views[i] = dlssPerf.fakeDSV.get();
	}
	for (int i = 0; i < 8; i++) {
		dlssPerf.savedKMainReadOnlyViews[i] = kmainDS.readOnlyViews[i];
		if (kmainDS.readOnlyViews[i])
			kmainDS.readOnlyViews[i] = dlssPerf.fakeDSV.get();
	}

	// --- Call original (or FrameAnnotations chain) ---
	func(imageSpaceShader, shape, param);

	// --- Restore kMAIN SRV ---
	kmainRT.SRV = dlssPerf.savedKMainSRV;
	dlssPerf.savedKMainSRV = nullptr;

	// --- Restore kMAIN_COPY SRV ---
	kmainCopyRT.SRV = dlssPerf.savedKMainCopySRV;
	dlssPerf.savedKMainCopySRV = nullptr;

	// --- Restore kMAIN DS views ---
	for (int i = 0; i < 8; i++)
		kmainDS.views[i] = dlssPerf.savedKMainViews[i];
	for (int i = 0; i < 8; i++)
		kmainDS.readOnlyViews[i] = dlssPerf.savedKMainReadOnlyViews[i];
}

// ============================================================================
// RefractionRender_Hook: IS shader hook for ISRefraction
// ============================================================================
// Strategy: let func() run normally (1k refraction, kMAIN→kMAIN_COPY).
// After func() returns, D3D11 state is sticky (PS/CB/sampler/IA all still bound).
// We replay the draw with our own RT (testTexture 3k), VP (3k), and SRV (refraTempTex 3k).

void DLSSperf::RefractionRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& dlssPerf = globals::features::upscaling.dlssPerf;

	if (!dlssPerf.hookActive || !dlssPerf.testTextureRTV || !dlssPerf.refraTempSRV) {
		func(imageSpaceShader, shape, param);
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "DLSSperf::RefractionRender");

	// --- Pass 1: engine's normal 1k refraction (untouched) ---
	func(imageSpaceShader, shape, param);

	// --- Pass 2: our 3k refraction replay ---
	// func() left PS/CB/sampler/IA/VB/IB all bound on the D3D context.
	// We only change RT, VP, and t0 SRV, then DrawIndexed with the same geometry.

	auto* context = globals::d3d::context;

	// Save current RT so we can restore after our draw
	ID3D11RenderTargetView* savedRTV = nullptr;
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

	// Save the full viewport stack rather than a single VP — RSSetViewports/
	// RSGetViewports work on arrays sized up to D3D11_VIEWPORT_AND_SCISSORRECT_
	// OBJECT_COUNT_PER_PIPELINE (16). Truncating to one would silently drop
	// extra bound viewports if a later engine pass relied on multi-VP.
	UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	context->RSGetViewports(&numVP, savedVP);

	// Save current t0 SRV (kMAIN.SRV used by ISRefraction as scene input)
	ID3D11ShaderResourceView* savedSRV0 = nullptr;
	context->PSGetShaderResources(0, 1, &savedSRV0);

	// Set 3k output: testTexture RTV, no DS needed for fullscreen IS shader
	ID3D11RenderTargetView* rtv3k = dlssPerf.testTextureRTV.get();
	context->OMSetRenderTargets(1, &rtv3k, nullptr);

	// Set 3k VP
	D3D11_VIEWPORT vp3k = {};
	vp3k.TopLeftX = 0.0f;
	vp3k.TopLeftY = 0.0f;
	vp3k.Width = static_cast<float>(dlssPerf.displayEyeWidth * 2);
	vp3k.Height = static_cast<float>(dlssPerf.displayEyeHeight);
	vp3k.MinDepth = 0.0f;
	vp3k.MaxDepth = 1.0f;
	context->RSSetViewports(1, &vp3k);

	// Set 3k input: refraTempTex as t0 (scene color for refraction sampling)
	ID3D11ShaderResourceView* srv3k = dlssPerf.refraTempSRV.get();
	context->PSSetShaderResources(0, 1, &srv3k);

	// Draw with the same geometry (BSTriShape fullscreen quad, IA still bound)
	context->DrawIndexed(6, 0, 0);

	// --- Restore D3D state so engine continues normally ---
	context->OMSetRenderTargets(1, &savedRTV, savedDSV);
	// RSGetViewports may have returned 0 if the prior pass left no viewport
	// bound; skip the restore in that case rather than pushing a zero-init VP.
	if (numVP > 0) {
		context->RSSetViewports(numVP, savedVP);
	}
	context->PSSetShaderResources(0, 1, &savedSRV0);

	// Release COM refs from Get calls
	if (savedRTV)
		savedRTV->Release();
	if (savedDSV)
		savedDSV->Release();
	if (savedSRV0)
		savedSRV0->Release();
}

// ============================================================================
// ISCopyRender_Hook: stretch ISCopy when source < dest (menu compositor fix)
// ============================================================================
// The VR menu compositor uses a single ISCopy draw to blit the rendered scene
// (kMAIN — RenderRes under DLSSperf) into a fixed-size projection surface
// (kPROJECTEDMENU 2048², or kMENUBG which DLSSperf enlarges to DisplayRes).
// Engine ISCopy uses a 1:1 viewport sized to the source, so the small source
// is stamped into the top-left of the larger dest. Symptom: "main menu image
// looks downscaled."
//
// Fix: after func() runs, if dest > current VP we replay the draw with the
// VP expanded to the dest's full dims. ISCopy's PS/CB/IA/sampler are sticky
// on the D3D context after func() returns (same pattern RefractionRender_Hook
// relies on), so the replay needs only a viewport change + DrawIndexed and
// the engine's clamp-sampler stretches the source naturally.
//
// In-game ISCopy (where source.w == dest.w under DLSSperf — kMAIN renderRes
// → kMAIN_COPY renderRes) takes the early-out branch and the engine's draw
// is the final pixel.

void DLSSperf::ISCopyRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& dlssPerf = globals::features::upscaling.dlssPerf;

	// Inactive / non-VR: passthrough.
	if (!dlssPerf.hookActive) {
		func(imageSpaceShader, shape, param);
		return;
	}

	// Let the engine draw first. After func() returns the IS shader's PS, CB,
	// sampler, IA layout, vertex/index buffers, and topology are all still
	// bound on the context (sticky D3D11 state). We only need to override the
	// viewport for the replay draw.
	func(imageSpaceShader, shape, param);

	auto* context = globals::d3d::context;

	// Inspect the current RTV's dest dimensions.
	ID3D11RenderTargetView* curRTV = nullptr;
	ID3D11DepthStencilView* curDSV = nullptr;
	context->OMGetRenderTargets(1, &curRTV, &curDSV);
	if (!curRTV) {
		if (curDSV)
			curDSV->Release();
		return;
	}

	ID3D11Resource* rtRes = nullptr;
	curRTV->GetResource(&rtRes);
	if (!rtRes) {
		curRTV->Release();
		if (curDSV)
			curDSV->Release();
		return;
	}

	D3D11_TEXTURE2D_DESC rtDesc{};
	static_cast<ID3D11Texture2D*>(rtRes)->GetDesc(&rtDesc);
	rtRes->Release();

	// Current VP (the one func() used) — full array so we can restore exactly.
	UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	context->RSGetViewports(&numVP, savedVP);

	// Only intervene when either axis of the engine's VP is smaller than the
	// dest. The +1 guard avoids float-equality issues (VPs are floats, RT
	// dims are uint32). Width-only would miss the case where the engine
	// binds a taller-than-VP RT (e.g., a square 2048² panel against a
	// renderRes-height VP).
	bool needsStretch = numVP > 0 &&
	                    (rtDesc.Width > static_cast<UINT>(savedVP[0].Width + 1.0f) ||
							rtDesc.Height > static_cast<UINT>(savedVP[0].Height + 1.0f));

	if (needsStretch) {
		ZoneScoped;
		TracyD3D11Zone(globals::state->tracyCtx, "DLSSperf::ISCopyStretch");

		// Replay viewport: full dest extent, preserve depth range from the
		// original so anything sampling depth (unlikely for ISCopy but safe)
		// keeps the same Z behavior.
		D3D11_VIEWPORT stretchVP = savedVP[0];
		stretchVP.TopLeftX = 0.0f;
		stretchVP.TopLeftY = 0.0f;
		stretchVP.Width = static_cast<float>(rtDesc.Width);
		stretchVP.Height = static_cast<float>(rtDesc.Height);
		context->RSSetViewports(1, &stretchVP);

		// ISCopy is a fullscreen quad drawn as a triangle list (6 indices).
		// Same index count RefractionRender_Hook uses for the same reason —
		// both replay the IS shader's standard fullscreen geometry.
		context->DrawIndexed(6, 0, 0);

		// Restore engine's VP so any state inspector downstream sees what it
		// expects. numVP guaranteed > 0 inside this branch.
		context->RSSetViewports(numVP, savedVP);
	}

	curRTV->Release();
	if (curDSV)
		curDSV->Release();
}

// ============================================================================
// UIPassDispatch_Hook: swap KMAIN DS → fakeDS for UI pass (renderMode==24)
// ============================================================================
// UI pass draws VR HUD to kMENUBG (now 3k). Engine binds KMAIN(DS) as DS,
// which is still 1k → size mismatch. Swap to fakeDS (3k) before, restore after.

void DLSSperf::UIPassDispatch_Hook::thunk(RE::BSGraphics::BSShaderAccumulator* shaderAccumulator, uint32_t renderFlags)
{
	auto& dlssPerf = globals::features::upscaling.dlssPerf;

	// Only intercept renderMode==24 (UI pass) when hook is active
	auto& rtData = shaderAccumulator->GetRuntimeData();
	if (!dlssPerf.hookActive || !dlssPerf.fakeDSV || rtData.renderMode != 24) {
		func(shaderAccumulator, renderFlags);
		return;
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& dsData = renderer->GetDepthStencilData();
	auto& kmainDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	// Save original KMAIN DS views and swap to fakeDS
	ID3D11DepthStencilView* savedViews[8] = {};
	ID3D11DepthStencilView* savedReadOnlyViews[8] = {};
	for (int i = 0; i < 8; i++) {
		savedViews[i] = kmainDS.views[i];
		if (kmainDS.views[i])
			kmainDS.views[i] = dlssPerf.fakeDSV.get();
	}
	for (int i = 0; i < 8; i++) {
		savedReadOnlyViews[i] = kmainDS.readOnlyViews[i];
		if (kmainDS.readOnlyViews[i])
			kmainDS.readOnlyViews[i] = dlssPerf.fakeDSV.get();
	}

	// Force engine to re-bind DS from struct
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	// Force 3k VP: engine may not call UpdateViewPort during UI pass,
	// so we directly set shadowState viewport to DisplayRes and mark dirty.
	auto* ss = globals::game::shadowState;
	D3D11_VIEWPORT savedVP = {};
	if (ss) {
		auto& vp = ss->GetVRRuntimeData().viewPort;
		savedVP = vp;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		vp.Width = static_cast<float>(dlssPerf.displayEyeWidth * 2);
		vp.Height = static_cast<float>(dlssPerf.displayEyeHeight);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	}

	// Skip VP compression in UpdateViewPort hook during UI pass
	dlssPerf.postInterceptActive = true;

	func(shaderAccumulator, renderFlags);

	dlssPerf.postInterceptActive = false;

	// Restore viewport
	if (ss) {
		ss->GetVRRuntimeData().viewPort = savedVP;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	}

	// Restore original KMAIN DS views
	for (int i = 0; i < 8; i++)
		kmainDS.views[i] = savedViews[i];
	for (int i = 0; i < 8; i++)
		kmainDS.readOnlyViews[i] = savedReadOnlyViews[i];

	// Re-dirty so subsequent passes get correct DS
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

// ============================================================================
// PlayerViewRender_Hook: clear postChainDone at PlayerView end
// ============================================================================
// PlayerView covers the entire VR pipeline (World→Post→UI→Submit).
// After func() returns, clear postChainDone so the Present-前 UI chain
// and the next frame use normal VP compression.

void DLSSperf::PlayerViewRender_Hook::thunk(void* a1, bool a2, bool a3)
{
	func(a1, a2, a3);

	globals::features::upscaling.dlssPerf.ClearPostChainDone();
}

// ============================================================================
// BSGraphics_SetDirtyStates_Hook
// ============================================================================
// Wraps DS swap around the engine's RT/DS flush so enlarged-RT draws don't
// rasterizer-clip to the smaller kMAIN DS bounds.
void DLSSperf::BSGraphics_SetDirtyStates_Hook::thunk(bool isCompute)
{
	bool swapped = false;
	if (!isCompute)
		swapped = globals::features::upscaling.dlssPerf.MaybeSwapDSForEnlargedRT();
	func(isCompute);
	if (swapped)
		globals::features::upscaling.dlssPerf.RestoreSwappedDS();
}

// ============================================================================
// BSGraphics_Renderer_UpdateViewPort_Hook
// ============================================================================
// Post-corrects the engine viewport when the bound RT and the requested VP
// don't agree about render-vs-display extent. Was originally in Hooks.cpp.
void DLSSperf::BSGraphics_Renderer_UpdateViewPort_Hook::thunk(RE::BSGraphics::Renderer* a_this, uint32_t a_width, uint32_t a_height, bool a_forceMatchRT)
{
	func(a_this, a_width, a_height, a_forceMatchRT);

	auto& dlssPerf = globals::features::upscaling.dlssPerf;
	if (!dlssPerf.IsHookActive())
		return;

	// During Post intercept enlarged kTEMP/kTOTAL already get the right VP
	// from func() because of their inflated RT dims — don't second-guess it.
	if (dlssPerf.IsPostInterceptActive())
		return;

	auto* ss = globals::game::shadowState;
	if (!ss)
		return;
	auto& vp = ss->GetVRRuntimeData().viewPort;
	const uint32_t displayW = dlssPerf.GetDisplayEyeWidth() * 2;
	const uint32_t displayH = dlssPerf.GetDisplayEyeHeight();
	const uint32_t renderW = dlssPerf.GetRenderEyeWidth() * 2;
	const uint32_t renderH = dlssPerf.GetRenderEyeHeight();

	// After the Post chain, UI / submit-prep draws target enlarged kTOTAL
	// at displayRes — expand any renderRes VP the engine sets back up.
	// The fade Draw(30) bypasses this path entirely (direct D3D RSSet-
	// Viewports) and is handled by the Draw vfunc hook in Globals.cpp.
	if (dlssPerf.IsPostChainDone()) {
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

// ============================================================================
// BeginPostIntercept / EndPostIntercept
// ============================================================================
// Outer layer of two-layer swap: swaps kMAIN_COPY DS → fakeDS before the
// entire Post chain (covers the copy step #10 which binds kMAIN_COPY DS).
// Inner layer (tonemap hook) handles kMAIN DS + kMAIN SRV for step #9.

void DLSSperf::BeginPostIntercept()
{
	if (!hookActive || !fakeDSV)
		return;

	ZoneScoped;
	auto state = globals::state;
	state->BeginPerfEvent("DLSSperf::BeginPostIntercept");

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& dsData = renderer->GetDepthStencilData();
	auto& kmainCopyDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];

	postInterceptActive = true;

	// Swap kMAIN_COPY DS views → fakeDS
	for (int i = 0; i < 8; i++) {
		savedKMainCopyViews[i] = kmainCopyDS.views[i];
		if (kmainCopyDS.views[i])
			kmainCopyDS.views[i] = fakeDSV.get();
	}
	for (int i = 0; i < 8; i++) {
		savedKMainCopyReadOnlyViews[i] = kmainCopyDS.readOnlyViews[i];
		if (kmainCopyDS.readOnlyViews[i])
			kmainCopyDS.readOnlyViews[i] = fakeDSV.get();
	}

	state->EndPerfEvent();
}

void DLSSperf::EndPostIntercept()
{
	if (!hookActive || !fakeDSV)
		return;

	ZoneScoped;
	auto state = globals::state;
	state->BeginPerfEvent("DLSSperf::EndPostIntercept");

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& dsData = renderer->GetDepthStencilData();
	auto& kmainCopyDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];

	postInterceptActive = false;
	postChainDone = true;

	// Restore kMAIN_COPY DS views
	for (int i = 0; i < 8; i++)
		kmainCopyDS.views[i] = savedKMainCopyViews[i];
	for (int i = 0; i < 8; i++)
		kmainCopyDS.readOnlyViews[i] = savedKMainCopyReadOnlyViews[i];

	state->EndPerfEvent();
}

// ============================================================================
// DownscaleToKMain: Box 3×3 downscale testTexture (3k) → kMAIN (1k)
// ============================================================================
// Called before the Post chain so the HDR pyramid builds from AA'd DLSS output
// instead of the raw 1k render, eliminating shimmer in bloom/exposure.
// Only kMAIN needs writing:
//   - No refraction: kMAIN is the pyramid input directly.
//   - With refraction: engine composites kMAIN → kMAIN_COPY, which enters pyramid.

void DLSSperf::DownscaleToKMain()
{
	if (!hookActive || !testTextureSRV || !boxDownscalePS || !boxDownscaleVS || !linearSampler)
		return;

	ZoneScoped;
	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto& rtData = renderer->GetRuntimeData();

	auto& kmain = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN];

	// Bail before opening the perf event so we don't leak a dangling
	// Begin without End on the null-RTV early-return path.
	if (!kmain.RTV)
		return;

	state->BeginPerfEvent("DLSSperf::DownscaleToKMain");
	TracyD3D11Zone(state->tracyCtx, "DLSSperf::DownscaleToKMain");

	{
		FullscreenPassScope stateScope(context);

		// IA: fullscreen triangle (no vertex/index buffers)
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Shaders — clear GS/HS/DS to prevent pipeline interference
		context->VSSetShader(boxDownscaleVS.get(), nullptr, 0);
		context->PSSetShader(boxDownscalePS.get(), nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { testTextureSRV.get() };
		context->PSSetShaderResources(0, 1, srvs);
		ID3D11SamplerState* samplers[] = { linearSampler.get() };
		context->PSSetSamplers(0, 1, samplers);

		// Opaque overwrite: no blending, no depth test, default rasterizer
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);

		// Viewport at RenderRes SBS (1k)
		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<float>(renderEyeWidth * 2);
		vp.Height = static_cast<float>(renderEyeHeight);
		vp.MaxDepth = 1.0f;
		context->RSSetViewports(1, &vp);

		ID3D11RenderTargetView* rtv = kmain.RTV;
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->Draw(3, 0);
	}

	state->EndPerfEvent();
}

// Bridge the DLSS-reconstructed menu BG into kTOTAL/kMENUBG. Driven from
// TonemapRender_Hook post-func in menu/loading state: the engine's tonemap
// shader's UV math assumes RT.size == kMAIN.size (true for DLAA, broken
// under DLSS), so we run our own DLSS evaluate against the engine's
// menu-state inputs (jitter via Main_UpdateJitter, depth via menu BG pre-
// pass, motion vectors as ISTemporalAA reads them) and blit testTexture →
// dest. One-shot per frame via blittedFrameId (Present doesn't fire here
// and PlayerView doesn't fire in main-menu, so the frame-id guard is the
// only reliable per-frame boundary).
void DLSSperf::MaybeBlitMenuBG(uint32_t boundRTIdx)
{
	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	if (!hookActive || blittedFrameId == currentFrame || !menuBlitPS || !boxDownscaleVS || !linearSampler)
		return;
	if (!testTexture || !testTextureSRV)
		return;
	if (!globals::state || !globals::state->IsMainOrLoadingMenuOpen())
		return;
	if (boundRTIdx != RE::RENDER_TARGETS::kTOTAL &&
		boundRTIdx != RE::RENDER_TARGETS::kMENUBG)
		return;

	auto renderer = globals::game::renderer;
	auto& rtData = renderer->GetRuntimeData();
	auto& dest = rtData.renderTargets[boundRTIdx];
	if (!dest.RTV || !dest.texture)
		return;

	ZoneScoped;
	auto state = globals::state;
	auto* context = globals::d3d::context;
	state->BeginPerfEvent("DLSSperf::MenuBGBlit");
	TracyD3D11Zone(state->tracyCtx, "DLSSperf::MenuBGBlit");

	globals::features::upscaling.Upscale();

	{
		FullscreenPassScope stateScope(context);

		// IA: fullscreen triangle, no VB/IB
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		context->VSSetShader(boxDownscaleVS.get(), nullptr, 0);
		context->PSSetShader(menuBlitPS.get(), nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { testTextureSRV.get() };
		context->PSSetShaderResources(0, 1, srvs);
		ID3D11SamplerState* samplers[] = { linearSampler.get() };
		context->PSSetSamplers(0, 1, samplers);

		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);

		D3D11_TEXTURE2D_DESC destDesc{};
		static_cast<ID3D11Texture2D*>(dest.texture)->GetDesc(&destDesc);
		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<float>(destDesc.Width);
		vp.Height = static_cast<float>(destDesc.Height);
		vp.MaxDepth = 1.0f;
		context->RSSetViewports(1, &vp);

		ID3D11RenderTargetView* rtv = dest.RTV;
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->Draw(3, 0);
	}

	blittedFrameId = currentFrame;
	state->EndPerfEvent();
}

void DLSSperf::HandlePostProcessing(const std::function<void()>& enginePost)
{
	ZoneScoped;
	auto state = globals::state;
	state->BeginPerfEvent("DLSSperf::HandlePostProcessing");

	// Copy testTexture → refraTempTex before Post, so ISRefraction can read 3k scene
	if (refraTempTex) {
		globals::d3d::context->CopyResource(refraTempTex.get(), testTexture.get());
	}

	// Downscale testTexture (3k AA'd) → kMAIN (1k) so the HDR pyramid and
	// bloom compute from anti-aliased content instead of raw 1k render.
	DownscaleToKMain();

	// Underwater mask analytical repair. Engine RTs (depth, mask) are at
	// renderRes under DLSSperf, so the full-resolution path of UpscaleDepth
	// would apply — but routing through UpscaleDepth here leaves pipeline
	// state dirty and the trailing enginePost() loses kMAIN. Drive the
	// mask-only draw directly inside our own FullscreenPassScope so the
	// inbound engine state is restored on exit.
	{
		FullscreenPassScope scope(globals::d3d::context);
		globals::features::upscaling.RunUnderwaterMaskRepair();
	}

	// Outer layer: swap kMAIN_COPY DS + SRV for refraction path coverage
	BeginPostIntercept();

	// Run full engine Post chain; IS shader hook handles tonemap step (#9) swap/restore
	enginePost();

	// Restore kMAIN_COPY DS
	EndPostIntercept();

	state->EndPerfEvent();
}

bool DLSSperf::MaybeSwapDSForEnlargedRT()
{
	if (!hookActive || postInterceptActive)
		return false;
	if (autoSwapDSIdx != UINT32_MAX)
		return false;  // re-entry guard

	auto* ss = globals::game::shadowState;
	if (!ss)
		return false;
	auto& srd = ss->GetVRRuntimeData();

	// Only the three RTs DLSSperf_MaybeEnlargeRT inflates to displayRes.
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

void DLSSperf::RestoreSwappedDS()
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
// Under DLSSperf the draw's VP is computed at renderRes while the RT (kTOTAL)
// is displayRes — partial-screen "black stamp" without this swap. Gate on
// VertexCount==30 + isVR keeps the cost a single comparison on flat / non-
// fade draws.
void DLSSperf::ID3D11DeviceContext_Draw_Hook::thunk(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
{
	if (VertexCount == 30 && globals::game::isVR) {
		auto& dlssPerf = globals::features::upscaling.dlssPerf;
		if (dlssPerf.IsHookActive() && dlssPerf.IsPostChainDone()) {
			UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
			This->RSGetViewports(&numVP, savedVP);

			D3D11_VIEWPORT vp{};
			vp.Width = static_cast<float>(dlssPerf.GetDisplayEyeWidth() * 2);
			vp.Height = static_cast<float>(dlssPerf.GetDisplayEyeHeight());
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

void DLSSperf::InstallFadeOverlayHook(ID3D11DeviceContext* context)
{
	if (!globals::game::isVR || !context)
		return;
	stl::detour_vfunc<13, ID3D11DeviceContext_Draw_Hook>(context);
}

void DLSSperf::InstallCreateRTThunks()
{
	if (!REL::Module::IsVR())
		return;
	auto vrBase = REL::RelocationID(100458, 107175).address();
	stl::write_thunk_call<CreateRT_MenuBG_Hook>(vrBase + 0x6cc);
	stl::write_thunk_call<CreateRT_ImagespaceTempCopy_Hook>(vrBase + 0x7a3);
	stl::write_thunk_call<CreateRT_Total_Hook>(vrBase + 0x1547);
}

void DLSSperf::BeginCreateRTEnlarge()
{
	if (!hookActive)
		return;
	enlargeWidth = displayEyeWidth * 2;
	enlargeHeight = displayEyeHeight;
	enlargeActive = true;
}

void DLSSperf::EndCreateRTEnlarge()
{
	enlargeActive = false;
}

namespace
{
	void EnlargeProps(RE::BSGraphics::RenderTargetProperties* a_props)
	{
		auto& dp = globals::features::upscaling.dlssPerf;
		if (!dp.IsCreateRTEnlargeActive())
			return;
		a_props->width = dp.GetEnlargeWidth();
		a_props->height = dp.GetEnlargeHeight();
	}
}

void DLSSperf::CreateRT_MenuBG_Hook::thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
{
	EnlargeProps(a_properties);
	func(a_this, a_target, a_properties);
}

void DLSSperf::CreateRT_ImagespaceTempCopy_Hook::thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
{
	EnlargeProps(a_properties);
	func(a_this, a_target, a_properties);
}

void DLSSperf::CreateRT_Total_Hook::thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
{
	EnlargeProps(a_properties);
	func(a_this, a_target, a_properties);
}

void DLSSperf::DrawSettings()
{
	// DLSSperf has no user-facing settings of its own — enablement is gated
	// at install time by whether the BSShaderRenderTargets::Create hook ran
	// successfully. A future PR may surface diagnostic info here.
}
