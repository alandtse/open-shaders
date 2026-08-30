#include "Integration.h"

#include "Renderer.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Globals.h"
#include "GpuPass.h"

#include <array>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t lastAppliedFrame = UINT32_MAX;
		bool writebackLogged = false;
		bool flatRouteWasActive = false;
		bool flatFrameGenerationBlockLogged = false;

		bool EnsureColorResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (color[0] && colorWidth == width && colorHeight == height && colorFormat == sourceDesc.Format)
				return true;
			const std::uint32_t resourceCount = globals::game::isVR ? 2u : 1u;
			for (std::uint32_t eye = 0; eye < resourceCount; ++eye) {
				color[eye] = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					eye == 0 ? "NeuralRendering::LdrColorLeft" : "NeuralRendering::LdrColorRight");
				if (!color[eye])
					return false;
			}
			if (!globals::game::isVR)
				color[1].reset();
			colorWidth = width;
			colorHeight = height;
			colorFormat = sourceDesc.Format;
			return true;
		}

		Tuning GetTuning(const FoveatedRender::Settings& settings)
		{
			return {
				settings.neuralRenderingIntensity,
				settings.neuralRenderingLocalTone,
				settings.neuralRenderingLocalStructure,
				settings.neuralRenderingSkinStructure,
				settings.neuralRenderingStyle,
				settings.neuralRenderingAutoMask,
				settings.neuralRenderingUICorrection,
			};
		}

		bool ApplyFlatLdr(Upscaling& upscaling, FoveatedRender& foveated)
		{
			const bool frameGenerationConfigured = upscaling.IsFrameGenerationConfiguredForSession();
			const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				foveated.settings.neuralRenderingEnabled && !frameGenerationConfigured;
			if (!routeActive) {
				if (foveated.settings.neuralRenderingEnabled && frameGenerationConfigured && !flatFrameGenerationBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: disable Frame Generation and restart the game");
					flatFrameGenerationBlockLogged = true;
				}
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			flatRouteWasActive = true;

			const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (lastAppliedFrame == frame)
				return true;
			auto* renderer = globals::game::renderer;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!total.texture || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource)
				return false;

			D3D11_TEXTURE2D_DESC totalDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			total.texture->GetDesc(&totalDesc);
			upscaling.motionVectorCopyTexture->resource->GetDesc(&motionDesc);
			if (!EnsureColorResources(total.texture, totalDesc.Width, totalDesc.Height))
				return false;

			CS_GPU_PASS("NeuralRendering::FlatLdrBeforeUI");
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->CopyResource(color[0]->resource.get(), total.texture);

			const bool succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				color[0]->resource.get(), depth.texture, depth.depthSRV,
				upscaling.motionVectorCopyTexture->resource.get(), motionDesc.Width, motionDesc.Height,
				totalDesc.Width, totalDesc.Height, static_cast<float>(motionDesc.Width),
				static_cast<float>(motionDesc.Height), GetTuning(foveated.settings));
			if (succeeded) {
				context->CopyResource(total.texture, color[0]->resource.get());
				lastAppliedFrame = frame;
				if (!writebackLogged) {
					logger::info("[DLSSNR] Flat LDR output written before UI composite guides={}x{} color={}x{}",
						motionDesc.Width, motionDesc.Height, totalDesc.Width, totalDesc.Height);
					writebackLogged = true;
				}
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv) rtv->Release();
			if (savedDSV) savedDSV->Release();
			return succeeded;
		}
	}

	bool ApplyFoveatedLdr()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!globals::game::isVR)
			return ApplyFlatLdr(upscaling, foveated);
		if (!globals::game::isVR || !FoveatedRenderImpl::Bridge::IsRouteActive() ||
			upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
			foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault ||
			!foveated.settings.neuralRenderingEnabled || upscaling.IsFrameGenerationActive())
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		const std::uint32_t guideFrame = FoveatedRenderImpl::Core::neuralGuidesFrame;
		if (lastAppliedFrame == frame || (guideFrame != frame && !(frame > 0 && guideFrame == frame - 1)))
			return false;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device ||
			!FoveatedRenderImpl::Core::vrSubrectDepth[0] || !FoveatedRenderImpl::Core::vrSubrectDepth[1] ||
			!FoveatedRenderImpl::Core::vrSubrectMotionVectors[0] || !FoveatedRenderImpl::Core::vrSubrectMotionVectors[1])
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		if (!total.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		const auto& leftUV = foveated.subrectController.GetUV();
		const auto& rightUV = foveated.subrectController.GetRightEyeUV();
		if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
			return false;
		const std::uint32_t eyeWidth = totalDesc.Width / 2;
		const std::uint32_t outWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidth * leftUV.w));
		const std::uint32_t outHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(totalDesc.Height * leftUV.h));

		CS_GPU_PASS("NeuralRendering::FoveatedLdrBeforeUI");
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };
		std::array<Renderer::StereoEyeInput, 2> inputs{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = *eyeUVs[eye];
			const std::uint32_t x = (eye ? eyeWidth : 0) + static_cast<std::uint32_t>(eyeWidth * uv.x);
			const std::uint32_t y = static_cast<std::uint32_t>(totalDesc.Height * uv.y);
			float motionScaleX = 1.0f;
			float motionScaleY = 1.0f;
			FoveatedRenderImpl::Bridge::ComputeMvecScale(eye, motionScaleX, motionScaleY);
			inputs[eye] = {
				.depth = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->resource.get(),
				.depthSRV = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->srv.get(),
				.motionVectors = FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->resource.get(),
				.sourceX = x,
				.sourceY = y,
				.motionVectorScaleX = motionScaleX * FoveatedRenderImpl::Core::vrSubrectInW,
				.motionVectorScaleY = motionScaleY * FoveatedRenderImpl::Core::vrSubrectInH,
			};
		}
		const bool succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
			total.texture, inputs, FoveatedRenderImpl::Core::vrSubrectInW, FoveatedRenderImpl::Core::vrSubrectInH,
			outWidth, outHeight, GetTuning(foveated.settings));
		if (succeeded) {
			lastAppliedFrame = frame;
			if (!writebackLogged) {
				logger::info("[DLSSNR] LDR output written before UI composite size={}x{} batchedAsync=true", outWidth, outHeight);
				writebackLogged = true;
			}
		}

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs)
			if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		return succeeded;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		color[0].reset();
		color[1].reset();
		colorWidth = colorHeight = 0;
		colorFormat = DXGI_FORMAT_UNKNOWN;
		lastAppliedFrame = UINT32_MAX;
		writebackLogged = false;
		flatRouteWasActive = false;
		flatFrameGenerationBlockLogged = false;
	}
}
