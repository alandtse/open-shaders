#include "Preprocess.h"

#include "../../../Deferred.h"
#include "../../../GpuPass.h"
#include "../../../State.h"
#include "../../../Util.h"
#include "../../Upscaling.h"

namespace
{
	ID3D11ComputeShader* GetEnhancerEncodeTexturesCS(Upscaling& upscaling, Upscaling::UpscaleMethod upscaleMethod)
	{
		uint methodIndex = (uint)upscaleMethod;
		// This cache slot is shared with Upscaling::GetEncodeTexturesCS -- the
		// define must match its own per-method selection or a hardcoded define
		// compiles the wrong shader variant into the shared slot.
		std::vector<std::pair<const char*, const char*>> defines;
		switch (upscaleMethod) {
		case Upscaling::UpscaleMethod::kDLSS:
			defines.push_back({ "DLSS", "" });
			break;
		case Upscaling::UpscaleMethod::kFSR:
			defines.push_back({ "FSR", "" });
			break;
		default:
			break;
		}

		return upscaling.encodeTexturesCS[methodIndex].Get(
			L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0");
	}
}

namespace FoveatedRenderImpl
{
	bool Preprocess::EncodeUpscalingTextures(Upscaling& upscaling)
	{
		auto upscaleMethod = upscaling.GetUpscaleMethod();
		if (upscaleMethod != Upscaling::UpscaleMethod::kDLSS && upscaleMethod != Upscaling::UpscaleMethod::kFSR) {
			logger::error("[FOVEATED] Preprocess path only supports DLSS/FSR; method={}", (int)upscaleMethod);
			return false;
		}

		auto context = globals::d3d::context;
		auto renderer = globals::game::renderer;

		if (!upscaling.upscalingDataCB || !upscaling.reactiveMaskTexture || !upscaling.transparencyCompositionMaskTexture) {
			logger::error("[FOVEATED] Missing preprocess resources");
			return false;
		}

		// motionVectorCopyTexture is dereferenced unconditionally in the UAV array
		// below — the foveated route always needs a per-frame snapshot to crop
		// per-eye from (DLSS and FSR both). The above resource check did not
		// cover it. Fail closed rather than null-deref.
		if (!upscaling.motionVectorCopyTexture) {
			logger::error("[FOVEATED] Missing motionVectorCopyTexture for preprocess");
			return false;
		}

		auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		auto& temporalAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
		auto& normals = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[2]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		// CSSetShaderResources with a null view in the array doesn't crash, but
		// the encode shader reads all four — a null among them silently corrupts
		// the reactive/transparency masks DLSS will sample next.
		if (!temporalAAMask.SRV || !normals.SRV || !motionVector.SRV || !depth.depthSRV) {
			logger::error("[FOVEATED] Missing preprocess SRV inputs");
			return false;
		}

		// Resolve the shader before binding any resources -- a failed fetch must
		// leave the compute stage untouched so the DLSS/FSR fallback below doesn't
		// inherit stale SRV/UAV/CB bindings from this aborted pass.
		ID3D11ComputeShader* cs = GetEnhancerEncodeTexturesCS(upscaling, upscaleMethod);
		if (!cs) {
			logger::error("[FOVEATED] Failed to get encode compute shader");
			return false;
		}

		auto dispatchCount = Util::GetScreenDispatchCount(true);

		CS_GPU_PASS("FoveatedRender::EncodeUpscalingTextures");

		auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
		Upscaling::UpscalingDataCB upscalingData{};
		upscalingData.trueSamplingDim = renderSize;
		upscaling.upscalingDataCB->Update(upscalingData);

		auto upscalingBuffer = upscaling.upscalingDataCB->CB();
		context->CSSetConstantBuffers(0, 1, &upscalingBuffer);

		ID3D11ShaderResourceView* views[4] = { temporalAAMask.SRV, normals.SRV, motionVector.SRV, depth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[3] = {
			upscaling.reactiveMaskTexture->uav.get(),
			upscaling.transparencyCompositionMaskTexture->uav.get(),
			upscaling.motionVectorCopyTexture->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(cs, nullptr, 0);
		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

		ID3D11ShaderResourceView* nullViews[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
		ID3D11UnorderedAccessView* nullUavs[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetShader(nullptr, nullptr, 0);

		return true;
	}
}
