#include "Preprocess.h"

#include "../../../GpuPass.h"
#include "../../../State.h"
#include "../../../Util.h"
#include "../../Upscaling.h"

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

		Upscaling::EncodeInputViews views{};
		const char* missingInput = nullptr;
		if (!upscaling.GetEncodeInputs(views, missingInput)) {
			logger::error("[FOVEATED] Missing preprocess SRV input ({})", missingInput);
			return false;
		}

		// Resolve the shader before binding any resources -- a failed fetch must
		// leave the compute stage untouched so the DLSS/FSR fallback below doesn't
		// inherit stale SRV/UAV/CB bindings from this aborted pass.
		ID3D11ComputeShader* cs = upscaling.GetEncodeTexturesCS(upscaleMethod, Upscaling::EncodeOutput::kMasksOnly);
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

		context->CSSetShaderResources(0, (uint)views.size(), views.data());

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
