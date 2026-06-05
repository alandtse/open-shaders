#include "../PerfMode.h"

#include <algorithm>
#include <cmath>

#include "../../../State.h"
#include "../../Upscaling.h"

// Quality mode -> render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses), avoiding a duplicate scale table here.
#include <FidelityFX/host/ffx_fsr3.h>

PerfMode::FullscreenPassScope::FullscreenPassScope(ID3D11DeviceContext* a_context) :
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

PerfMode::FullscreenPassScope::~FullscreenPassScope()
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
