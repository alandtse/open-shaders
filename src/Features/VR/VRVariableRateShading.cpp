#include "VRVariableRateShading.h"
#include "Globals.h"
#include "State.h"
#include <spdlog/spdlog.h>

namespace VRFeatures
{
	VRVariableRateShading* VRVariableRateShading::GetSingleton()
	{
		static VRVariableRateShading instance;
		return &instance;
	}

	bool VRVariableRateShading::Initialize()
	{
		if (initialized)
			return nvapiAvailable;

		// Initialize NVAPI
		NvAPI_Status status = NvAPI_Initialize();
		if (status != NVAPI_OK) {
			logger::warn("VRVariableRateShading: NvAPI_Initialize failed with status {}", static_cast<int>(status));
			nvapiAvailable = false;
			initialized = true;  // Tried and failed
			return false;
		}

		// Check VRS capability
		NV_D3D1x_GRAPHICS_CAPS caps = {};
		status = NvAPI_D3D1x_GetGraphicsCapabilities(globals::d3d::device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
		if (status != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
			logger::info("VRVariableRateShading: Variable Rate Shading not supported on this GPU (status: {})", static_cast<int>(status));
			nvapiAvailable = false;
			initialized = true;
			return false;
		}

		nvapiAvailable = true;
		logger::info("VRVariableRateShading: NVAPI initialized, VRS supported");

		initialized = true;
		return nvapiAvailable;
	}

	void VRVariableRateShading::CreateShadingRateResource(uint32_t width, uint32_t height)
	{
		if (!nvapiAvailable || !enabled)
			return;

		if (width <= 0 || height <= 0)
			return;

		// If size hasn't changed, don't recreate
		if (currentWidth == width && currentHeight == height && shadingRateView) {
			return;
		}

		currentWidth = width;
		currentHeight = height;

		// NVAPI VRS tile size is 16x16
		uint32_t tileWidth = (width + 15) / 16;
		uint32_t tileHeight = (height + 15) / 16;

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = tileWidth;
		texDesc.Height = tileHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R8_UINT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = 0;

		HRESULT hr = globals::d3d::device->CreateTexture2D(&texDesc, nullptr, srrTexture.ReleaseAndGetAddressOf());
		if (FAILED(hr)) {
			logger::error("VRVariableRateShading: Failed to create SRR texture");
			return;
		}

		NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC desc = {};
		desc.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
		desc.Format = DXGI_FORMAT_R8_UINT;
		desc.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = 0;

		NvAPI_Status status = NvAPI_D3D11_CreateShadingRateResourceView(globals::d3d::device, srrTexture.Get(), &desc, shadingRateView.ReleaseAndGetAddressOf());
		if (status != NVAPI_OK) {
			logger::error("VRVariableRateShading: Failed to create ShadingRateResourceView (Status: {})", static_cast<int>(status));
			shadingRateView = nullptr;
		} else {
			UpdateShadingRatePattern();
		}
	}

	void VRVariableRateShading::UpdateShadingRatePattern()
	{
		if (!shadingRateView || !srrTexture)
			return;

		bool perf = globals::state->frameAnnotations;
		if (perf)
			globals::state->BeginPerfEvent("Update VRS Pattern");

		D3D11_TEXTURE2D_DESC desc;
		srrTexture->GetDesc(&desc);

		uint32_t rowPitch = desc.Width * sizeof(uint8_t);
		std::vector<uint8_t> buffer(rowPitch * desc.Height);

		float innerSq = settings.innerRadius * settings.innerRadius;
		float middleSq = settings.middleRadius * settings.middleRadius;
		float outerSq = settings.outerRadius * settings.outerRadius;

		// Skyrim VR stereoscopic layout: Left eye [0, 0.5], Right eye [0.5, 1.0]
		// The VRS texture is tiled (16x16 tiles), so desc.Width/Height are in tiles
		float halfWidth = desc.Width * 0.5f;
		float eyeWidth = halfWidth;  // Each eye takes half the tile texture

		// Centers in tile coordinates
		float centerX_L = eyeWidth * 0.5f;              // Center of left eye
		float centerX_R = halfWidth + eyeWidth * 0.5f;  // Center of right eye
		float centerY = desc.Height * 0.5f;

		// Normalize radius based on the smaller eye dimension
		float radiusBase = std::min(eyeWidth, static_cast<float>(desc.Height)) * 0.5f;

		for (uint32_t y = 0; y < desc.Height; ++y) {
			for (uint32_t x = 0; x < desc.Width; ++x) {
				// Determine which eye based on x position
				float centerX = (x < static_cast<uint32_t>(halfWidth)) ? centerX_L : centerX_R;

				float dx = (x - centerX) / radiusBase;
				float dy = (y - centerY) / radiusBase;

				float distSq = dx * dx + dy * dy;
				uint8_t rate = 0;  // Index 0 (1x1) - full quality

				if (distSq > outerSq) {
					rate = 3;  // Index 3 -> 4x4 (lowest quality)
				} else if (distSq > middleSq) {
					rate = 2;  // Index 2 -> 2x2
				} else if (distSq > innerSq) {
					rate = 1;  // Index 1 -> 1x2
				}

				buffer[y * desc.Width + x] = rate;
			}
		}

		globals::d3d::context->UpdateSubresource(srrTexture.Get(), 0, nullptr, buffer.data(), rowPitch, 0);

		if (perf)
			globals::state->EndPerfEvent();
	}

	void VRVariableRateShading::Apply(ID3D11DeviceContext* a_context)
	{
		if (!enabled || !nvapiAvailable)
			return;

		// Use screen size for initial apply
		auto screenSize = globals::state->screenSize;
		ApplyForRenderTarget(a_context, static_cast<uint32_t>(screenSize.x), static_cast<uint32_t>(screenSize.y));
	}

	void VRVariableRateShading::ApplyForRenderTarget(ID3D11DeviceContext* a_context, uint32_t rtWidth, uint32_t rtHeight)
	{
		if (!enabled || !nvapiAvailable)
			return;

		// Recreate pattern if render target size changed (dynamic resolution)
		if (rtWidth != currentWidth || rtHeight != currentHeight) {
			CreateShadingRateResource(rtWidth, rtHeight);
		}

		if (!shadingRateView)
			return;

		NvAPI_D3D11_RSSetShadingRateResourceView(a_context, shadingRateView.Get());

		// Set Shading Rate Palette
		NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewportDesc = {};
		viewportDesc.enableVariablePixelShadingRate = true;

		for (int i = 0; i < 16; ++i)
			viewportDesc.shadingRateTable[i] = NV_PIXEL_X1_PER_RASTER_PIXEL;

		viewportDesc.shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;       // 1x1 (Center)
		viewportDesc.shadingRateTable[1] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;  // 1x2 (Inner Ring)
		viewportDesc.shadingRateTable[2] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;  // 2x2 (Middle Ring)
		viewportDesc.shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;  // 4x4 (Outer Ring)

		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC viewportsDesc = {};
		viewportsDesc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		viewportsDesc.numViewports = 1;
		viewportsDesc.pViewports = &viewportDesc;

		NvAPI_D3D11_RSSetViewportsPixelShadingRates(a_context, &viewportsDesc);
	}

	void VRVariableRateShading::Disable(ID3D11DeviceContext* a_context)
	{
		if (!nvapiAvailable)
			return;

		// Disable VRS by setting all shading rates to 1x1
		NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewportDesc = {};
		viewportDesc.enableVariablePixelShadingRate = false;
		memset(viewportDesc.shadingRateTable, 0, sizeof(viewportDesc.shadingRateTable));

		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC viewportsDesc = {};
		viewportsDesc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		viewportsDesc.numViewports = 1;
		viewportsDesc.pViewports = &viewportDesc;

		NvAPI_D3D11_RSSetViewportsPixelShadingRates(a_context, &viewportsDesc);
	}

	void VRVariableRateShading::SetEnabled(bool a_enabled)
	{
		if (enabled != a_enabled) {
			enabled = a_enabled;
			if (enabled) {
				Initialize();
				auto screenSize = globals::state->screenSize;
				if (screenSize.x > 0 && screenSize.y > 0) {
					targetWidth = static_cast<uint32_t>(screenSize.x);
					targetHeight = static_cast<uint32_t>(screenSize.y);
					CreateShadingRateResource(targetWidth, targetHeight);
				}
			}
		}
	}

	void VRVariableRateShading::UpdatePattern()
	{
		if (enabled && shadingRateView)
			UpdateShadingRatePattern();
	}

	void VRVariableRateShading::UpdatePatternForSize(uint32_t renderWidth, uint32_t renderHeight)
	{
		if (!enabled || !nvapiAvailable)
			return;

		if (renderWidth != currentWidth || renderHeight != currentHeight) {
			CreateShadingRateResource(renderWidth, renderHeight);
		} else if (shadingRateView) {
			UpdateShadingRatePattern();
		}
	}

	void VRVariableRateShading::Cleanup()
	{
		shadingRateView.Reset();
		srrTexture.Reset();
		currentWidth = 0;
		currentHeight = 0;
	}
}
