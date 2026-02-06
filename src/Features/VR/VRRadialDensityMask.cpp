#include "VRRadialDensityMask.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include <spdlog/spdlog.h>

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

		// Create Stencil State for Application
		D3D11_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = FALSE;
		dsDesc.StencilEnable = TRUE;
		dsDesc.StencilReadMask = 0xFF;
		dsDesc.StencilWriteMask = 0xFF;
		dsDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		dsDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		dsDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		dsDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		dsDesc.BackFace = dsDesc.FrontFace;

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
			float eyeWidth = halfWidth;

			// Left eye center: middle of left half
			data->CenterLeft[0] = eyeWidth * 0.5f;
			data->CenterLeft[1] = height * 0.5f;

			// Right eye center: middle of right half
			data->CenterRight[0] = halfWidth + eyeWidth * 0.5f;
			data->CenterRight[1] = height * 0.5f;

			// Radii based on smaller eye dimension
			float radiusBase = std::min(eyeWidth, static_cast<float>(height)) * 0.5f;
			data->InnerRadiusSq = (settings.innerRadius * radiusBase) * (settings.innerRadius * radiusBase);
			data->OuterRadiusSq = (settings.outerRadius * radiusBase) * (settings.outerRadius * radiusBase);
			data->HalfWidth = halfWidth;
			data->Pad = 0.0f;

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

		// Set Stencil State to REPLACE with 1
		context->OMSetDepthStencilState(applyStencilState.get(), 1);

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
