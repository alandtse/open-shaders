#include "RasterPass.h"

#include "Globals.h"
#include "State.h"

#include <cassert>

namespace PostProcessingRaster
{
	RasterPass::RasterPass(ID3D11DeviceContext* a_context) :
		context(a_context),
		savedState(a_context, kPSSRVCount, 0, kPSCBCount)
	{
		assert(context && globals::state && globals::state->sharedDataCB && globals::state->featureDataCB);

		const std::array sharedBuffers{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		context->PSSetConstantBuffers(kSharedCBStart, static_cast<UINT>(sharedBuffers.size()), sharedBuffers.data());

		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetInputLayout(nullptr);
		context->RSSetState(nullptr);
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
		context->OMSetDepthStencilState(nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
	}

	void RasterPass::SetTargets(std::initializer_list<ID3D11RenderTargetView*> a_rtvs, float a_width, float a_height)
	{
		assert(a_rtvs.size() <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && a_width > 0.f && a_height > 0.f);
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		UINT count = 0;
		for (auto* rtv : a_rtvs) {
			if (count >= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
				break;
			rtvs[count++] = rtv;
		}

		context->OMSetRenderTargets(count, rtvs, nullptr);

		D3D11_VIEWPORT viewport = {};
		viewport.Width = a_width;
		viewport.Height = a_height;
		viewport.MinDepth = 0.f;
		viewport.MaxDepth = 1.f;
		context->RSSetViewports(1, &viewport);
	}

	void RasterPass::SetShaders(ID3D11VertexShader* a_vs, ID3D11PixelShader* a_ps)
	{
		context->VSSetShader(a_vs, nullptr, 0);
		context->PSSetShader(a_ps, nullptr, 0);
	}

	void RasterPass::SetBlendState(ID3D11BlendState* a_blend, const float (&a_blendFactor)[4], UINT a_sampleMask)
	{
		context->OMSetBlendState(a_blend, a_blendFactor, a_sampleMask);
	}

	void RasterPass::Draw()
	{
		context->Draw(3, 0);
	}
}
