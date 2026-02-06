#include "VRRadialDensityMask.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include <spdlog/spdlog.h>

// Logic inspired by fholger/vrperfkit (MIT License)

namespace VRFeatures
{
	VRRadialDensityMask* VRRadialDensityMask::GetSingleton()
	{
		static VRRadialDensityMask instance;
		return &instance;
	}

	void VRRadialDensityMask::Initialize()
	{
		if (initialized)
			return;

		// Compile Shaders
		auto cs = Util::CompileShader(L"Data\\Shaders\\VR\\RadialDensityMask.hlsl", {}, "cs_5_0");
		if (cs)
			maskGenerationCS.attach(reinterpret_cast<ID3D11ComputeShader*>(cs));

		auto ps = Util::CompileShader(L"Data\\Shaders\\VR\\RDM_ApplyPS.hlsl", {}, "ps_5_0");
		if (ps)
			applyPS.attach(reinterpret_cast<ID3D11PixelShader*>(ps));

		auto vs = Util::CompileShader(L"Data\\Shaders\\VR\\RDM_ApplyVS.hlsl", {}, "vs_5_0");
		if (vs)
			applyVS.attach(reinterpret_cast<ID3D11VertexShader*>(vs));

		// Create Depth/Stencil State for Application
		// We write to Depth Buffer to occlude pixels in the masked region.
		D3D11_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;  // Always overwrite depth
		dsDesc.StencilEnable = FALSE;

		globals::d3d::device->CreateDepthStencilState(&dsDesc, applyStencilState.put());

		// Create Constant Buffer
		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = sizeof(CBData);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		globals::d3d::device->CreateBuffer(&cbDesc, nullptr, paramCB.put());

		initialized = (maskGenerationCS && applyPS && applyVS && applyStencilState && paramCB);

		if (initialized) {
			GenerateMask();  // Initial generation
			logger::info("VRRadialDensityMask: Initialized successfully");
		} else {
			logger::error("VRRadialDensityMask: Failed to initialize shaders or states");
		}
	}

	void VRRadialDensityMask::GenerateMask()
	{
		auto screenSize = globals::state->screenSize;
		if (screenSize.x <= 0 || screenSize.y <= 0)
			return;
		GenerateMaskForSize(static_cast<uint32_t>(screenSize.x), static_cast<uint32_t>(screenSize.y));
	}

	void VRRadialDensityMask::GenerateMaskForSize(uint32_t width, uint32_t height)
	{
		if (!initialized || !maskGenerationCS)
			return;

		if (width <= 0 || height <= 0)
			return;

		bool perf = globals::state->frameAnnotations;
		if (perf)
			globals::state->BeginPerfEvent("RDM Generate");

		auto context = globals::d3d::context;

		// Create/Recreate texture if size changed
		if (!maskTexture || !maskUAV || !maskSRV || currentWidth != width || currentHeight != height) {
			maskTexture = nullptr;
			maskSRV = nullptr;
			maskUAV = nullptr;

			currentWidth = width;
			currentHeight = height;

			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8_UINT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			globals::d3d::device->CreateTexture2D(&desc, nullptr, maskTexture.put());
			globals::d3d::device->CreateShaderResourceView(maskTexture.get(), nullptr, maskSRV.put());
			globals::d3d::device->CreateUnorderedAccessView(maskTexture.get(), nullptr, maskUAV.put());
		}

		// Update Constants for Skyrim VR stereoscopic layout
		// Left eye: [0, 0.5] of texture width, Right eye: [0.5, 1.0]
		D3D11_MAPPED_SUBRESOURCE map;
		if (SUCCEEDED(context->Map(paramCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
			CBData* data = (CBData*)map.pData;

			float halfWidth = width * 0.5f;

			// Left eye center
			data->CenterLeft[0] = leftEyeCenterX * width;
			data->CenterLeft[1] = leftEyeCenterY * height;

			// Right eye center
			data->CenterRight[0] = rightEyeCenterX * width;
			data->CenterRight[1] = rightEyeCenterY * height;

			// Calculate max distance from centers to corners for normalized radius (1.0 = covers everything)
			float cX_L = leftEyeCenterX * width;
			float cY_L = leftEyeCenterY * height;
			float dL_1 = sqrt(cX_L * cX_L + cY_L * cY_L);
			float dL_2 = sqrt((halfWidth - cX_L) * (halfWidth - cX_L) + cY_L * cY_L);
			float dL_3 = sqrt(cX_L * cX_L + (height - cY_L) * (height - cY_L));
			float dL_4 = sqrt((halfWidth - cX_L) * (halfWidth - cX_L) + (height - cY_L) * (height - cY_L));
			float maxDistL = std::max({ dL_1, dL_2, dL_3, dL_4 });

			float cX_R = rightEyeCenterX * width;
			float cY_R = rightEyeCenterY * height;
			float dR_1 = sqrt((cX_R - halfWidth) * (cX_R - halfWidth) + cY_R * cY_R);
			float dR_2 = sqrt((width - cX_R) * (width - cX_R) + cY_R * cY_R);
			float dR_3 = sqrt((cX_R - halfWidth) * (cX_R - halfWidth) + (height - cY_R) * (height - cY_R));
			float dR_4 = sqrt((width - cX_R) * (width - cX_R) + (height - cY_R) * (height - cY_R));
			float maxDistR = std::max({ dR_1, dR_2, dR_3, dR_4 });

			// Use the max distance so 1.0 radius covers the furthest corner
			float radiusBase = std::max(maxDistL, maxDistR);

			data->InnerRadiusSq = (settings.innerRadius * radiusBase) * (settings.innerRadius * radiusBase);
			data->MiddleRadiusSq = (settings.middleRadius * radiusBase) * (settings.middleRadius * radiusBase);
			data->OuterRadiusSq = (settings.outerRadius * radiusBase) * (settings.outerRadius * radiusBase);
			data->HalfWidth = halfWidth;

			float scale = (targetAspectRatio > 0.1f) ? targetAspectRatio : 1.33f;
			data->HeightScale = settings.favorHorizontal ? scale : 1.0f;

			data->Pad[0] = 0.0f;
			data->Pad[1] = 0.0f;
			data->Pad[2] = 0.0f;

			context->Unmap(paramCB.get(), 0);
		}

		// Dispatch CS
		context->CSSetShader(maskGenerationCS.get(), nullptr, 0);
		ID3D11UnorderedAccessView* uavs[] = { maskUAV.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		ID3D11Buffer* cbs[] = { paramCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		UINT x = (width + 7) / 8;
		UINT y = (height + 7) / 8;
		context->Dispatch(x, y, 1);

		// Unbind
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 0, nullptr);

		if (perf)
			globals::state->EndPerfEvent();
	}

	void VRRadialDensityMask::Apply(ID3D11DeviceContext* a_context)
	{
		(void)a_context;
		auto context = globals::d3d::context;

		if (!enabled || !initialized || !maskSRV || !applyVS)
			return;

		bool perf = globals::state->frameAnnotations;
		if (perf)
			globals::state->BeginPerfEvent("RDM Apply");

		auto renderer = globals::game::renderer;
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		// Bind Main Depth/Stencil as target (no color)
		context->OMSetRenderTargets(0, nullptr, depth.views[0]);

		// Set Depth State to REPLACE with 0.0 (Near Plane)
		context->OMSetDepthStencilState(applyStencilState.get(), 0);

		// Set Shaders
		context->VSSetShader(applyVS.get(), nullptr, 0);
		context->PSSetShader(applyPS.get(), nullptr, 0);
		ID3D11ShaderResourceView* srvs[] = { maskSRV.get() };
		context->PSSetShaderResources(0, 1, srvs);

		// Set Viewport (Full Screen)
		auto screenSize = globals::state->screenSize;
		D3D11_VIEWPORT viewport = {};
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Draw Fullscreen Triangle (3 vertices generated by VS)
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->Draw(3, 0);

		// Restore? Caller usually sets state.
		// Unbind resources
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->PSSetShaderResources(0, 1, &nullSRV);

		if (perf)
			globals::state->EndPerfEvent();
	}

	void VRRadialDensityMask::ApplyForRenderTarget(ID3D11DeviceContext* a_context, uint32_t rtWidth, uint32_t rtHeight)
	{
		(void)a_context;

		if (!enabled || !initialized)
			return;

		// Regenerate mask if size changed
		if (rtWidth != currentWidth || rtHeight != currentHeight) {
			GenerateMaskForSize(rtWidth, rtHeight);
		}

		Apply(globals::d3d::context);
	}

	void VRRadialDensityMask::Cleanup()
	{
		maskTexture = nullptr;
		maskSRV = nullptr;
		maskUAV = nullptr;
		currentWidth = 0;
		currentHeight = 0;
	}

	void VRRadialDensityMask::SetEnabled(bool a_enabled)
	{
		enabled = a_enabled;
		if (enabled) {
			Initialize();
			GenerateMask();
		}
	}

	void VRRadialDensityMask::SetEyeCenters(float leftX, float leftY, float rightX, float rightY)
	{
		leftEyeCenterX = leftX;
		leftEyeCenterY = leftY;
		rightEyeCenterX = rightX;
		rightEyeCenterY = rightY;
		UpdateMask();
	}

	void VRRadialDensityMask::SetAspectRatio(float aspect)
	{
		if (abs(targetAspectRatio - aspect) > 0.001f) {
			targetAspectRatio = aspect;
			UpdateMask();
		}
	}

	void VRRadialDensityMask::UpdateMask()
	{
		if (enabled && initialized) {
			GenerateMask();
		}
	}

	void VRRadialDensityMask::UpdateMaskForSize(uint32_t renderWidth, uint32_t renderHeight)
	{
		if (enabled && initialized) {
			GenerateMaskForSize(renderWidth, renderHeight);
		}
	}
}
