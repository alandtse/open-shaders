#include "../PerfMode.h"

#include <algorithm>
#include <cmath>

#include "../../../State.h"
#include "../../Upscaling.h"

// Quality mode -> render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses), avoiding a duplicate scale table here.
#include <FidelityFX/host/ffx_fsr3.h>

void PerfMode::SetupResources()
{
	if (!globals::game::isVR)
		return;

	// Fail closed: drop prior resolution-dependent resources before (re)allocating, so an early
	// return below leaves clean nulls, not stale handles that no longer match the new RT layout.
	// Persistent layout-independent resources (downscale/blit shaders, sampler) are left intact.
	postPipelineReady = false;
	testTexture = nullptr;
	testTextureSRV = nullptr;
	testTextureUAV = nullptr;
	testTextureRTV = nullptr;
	refraTempTex = nullptr;
	refraTempSRV = nullptr;
	fakeDS = nullptr;
	fakeDSV = nullptr;

	auto renderer = globals::game::renderer;
	auto& mainRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!mainRT.texture) {
		logger::error("[PerfMode] kMAIN texture not available in SetupResources");
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
		logger::error("[PerfMode] Failed to create test texture: {:#x}", (uint32_t)hr);
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	hr = device->CreateShaderResourceView(testTexture.get(), &srvDesc, testTextureSRV.put());
	if (FAILED(hr)) {
		logger::error("[PerfMode] Failed to create test texture SRV: {:#x}", (uint32_t)hr);
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
			logger::error("[PerfMode] Failed to create testTexture UAV: {:#x}", (uint32_t)hr);
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
			logger::error("[PerfMode] Failed to create testTexture RTV: {:#x}", (uint32_t)hr);
		}
	}

	// refraTempTex: copy of testTexture for ISRefraction input
	if (hookActive) {
		D3D11_TEXTURE2D_DESC refraDesc = desc;
		refraDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		hr = device->CreateTexture2D(&refraDesc, nullptr, refraTempTex.put());
		if (FAILED(hr)) {
			logger::error("[PerfMode] Failed to create refraTempTex: {:#x}", (uint32_t)hr);
		} else {
			D3D11_SHADER_RESOURCE_VIEW_DESC refraSrvDesc{};
			refraSrvDesc.Format = refraDesc.Format;
			refraSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			refraSrvDesc.Texture2D.MipLevels = 1;
			refraSrvDesc.Texture2D.MostDetailedMip = 0;
			hr = device->CreateShaderResourceView(refraTempTex.get(), &refraSrvDesc, refraTempSRV.put());
			if (FAILED(hr)) {
				logger::error("[PerfMode] Failed to create refraTempSRV: {:#x}", (uint32_t)hr);
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
				logger::error("[PerfMode] Failed to create fake DS texture: {:#x}", (uint32_t)hr2);
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
					logger::error("[PerfMode] Failed to create fake DSV: {:#x}", (uint32_t)hr2);
					fakeDS = nullptr;
				}
			}
		} else {
			logger::warn("[PerfMode] kMAIN DS texture not available, skipping fake DS creation");
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
			Util::CompileShader(L"Data/Shaders/Upscaling/PerfMode/BoxDownscalePS.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
		if (!boxDownscalePS)
			logger::error("[PerfMode] Failed to compile BoxDownscalePS");
	}
	if (hookActive && !boxDownscaleVS) {
		boxDownscaleVS.attach(static_cast<ID3D11VertexShader*>(
			Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0")));
		if (!boxDownscaleVS)
			logger::error("[PerfMode] Failed to compile BoxDownscale VS");
	}
	if (hookActive && !menuBlitPS) {
		menuBlitPS.attach(static_cast<ID3D11PixelShader*>(
			Util::CompileShader(L"Data/Shaders/Upscaling/PerfMode/MenuBGBlitPS.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
		if (!menuBlitPS)
			logger::error("[PerfMode] Failed to compile MenuBGBlitPS");
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
			logger::error("[PerfMode] Failed to create linear sampler");
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
			"[PerfMode] Post pipeline failed to initialize fully — Post wrap "
			"disabled, engine RTs remain at RenderRes. Check upstream resource "
			"creation errors above.");
	}
}

void PerfMode::DrawSettings()
{
	// PerfMode has no user-facing settings of its own — enablement is gated
	// at install time by whether the BSShaderRenderTargets::Create hook ran
	// successfully. A future PR may surface diagnostic info here.
}
