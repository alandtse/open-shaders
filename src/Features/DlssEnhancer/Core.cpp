#include "Core.h"
#include "Ops.h"

#include "../../State.h"
#include "../../Util.h"
#include "../DlssEnhancerFeature.h"
#include "../Upscaling.h"

#include <cstring>

namespace DlssEnhancer::Ops
{
	eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags, bool createSRV, bool createUAV, const char* name)
	{
		D3D11_TEXTURE2D_DESC srcDesc;
		static_cast<ID3D11Texture2D*>(src)->GetDesc(&srcDesc);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = srcDesc.Format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = copyBindFlags ? srcDesc.BindFlags : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

		auto tex = eastl::make_unique<Texture2D>(desc);

		if (name) {
			Util::SetResourceName(tex->resource.get(), name);
		}

		if (createSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = srcDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			tex->CreateSRV(srvDesc);
		}

		if (createUAV) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = srcDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			tex->CreateUAV(uavDesc);
		}

		return tex;
	}

	void EnsureVRIntermediateTextures(
		uint32_t inWidth,
		uint32_t inHeight,
		uint32_t outWidth,
		uint32_t outHeight,
		ID3D11Resource* colorSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc)
	{
		bool needsRecreate = !Core::vrIntermediateColorIn[0] || !Core::vrIntermediateColorOut[0];
		if (!needsRecreate) {
			needsRecreate = (Core::vrIntermediateColorIn[0]->desc.Width != inWidth ||
							 Core::vrIntermediateColorIn[0]->desc.Height != inHeight ||
							 Core::vrIntermediateColorOut[0]->desc.Width != outWidth ||
							 Core::vrIntermediateColorOut[0]->desc.Height != outHeight);
		}
		// Recreate if reactive/transparency source appeared but intermediate is missing
		if (!needsRecreate) {
			needsRecreate = (reactiveSrc && !Core::vrIntermediateReactiveMask[0]) ||
			                (transparencySrc && !Core::vrIntermediateTransparencyMask[0]);
		}

		if (!needsRecreate) {
			return;
		}

		for (int i = 0; i < 2; i++) {
			std::string suffix = (i == 0) ? "Left" : "Right";

			Core::vrIntermediateColorIn[i] = CreateTextureFromSource(colorSrc, inWidth, inHeight, false, true, true, ("Enh_ColorIn_" + suffix).c_str());
			Core::vrIntermediateColorOut[i] = CreateTextureFromSource(colorSrc, outWidth, outHeight, false, true, false, ("Enh_ColorOut_" + suffix).c_str());

			D3D11_TEXTURE2D_DESC depthDesc = {};
			depthDesc.Width = inWidth;
			depthDesc.Height = inHeight;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			Core::vrIntermediateDepth[i] = eastl::make_unique<Texture2D>(depthDesc);
			Util::SetResourceName(Core::vrIntermediateDepth[i]->resource.get(), ("Enh_Depth_" + suffix).c_str());

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			Core::vrIntermediateDepth[i]->CreateSRV(srvDesc);

			Core::vrIntermediateMotionVectors[i] = CreateTextureFromSource(mvecSrc, inWidth, inHeight, false, true, false, ("Enh_MVec_" + suffix).c_str());
			if (reactiveSrc)
				Core::vrIntermediateReactiveMask[i] = CreateTextureFromSource(reactiveSrc, inWidth, inHeight, false, true, false, ("Enh_Reactive_" + suffix).c_str());
			else
				Core::vrIntermediateReactiveMask[i].reset();
			if (transparencySrc)
				Core::vrIntermediateTransparencyMask[i] = CreateTextureFromSource(transparencySrc, inWidth, inHeight, false, true, false, ("Enh_Transparency_" + suffix).c_str());
			else
				Core::vrIntermediateTransparencyMask[i].reset();
		}
	}

	void EnsureVRSubrectTextures(
		uint32_t subInW,
		uint32_t subInH,
		uint32_t subOutW,
		uint32_t subOutH,
		ID3D11Resource* colorSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc)
	{
		bool needsRecreate = !Core::vrSubrectColorIn[0] ||
		                     Core::vrSubrectInW != subInW || Core::vrSubrectInH != subInH ||
		                     Core::vrSubrectOutW != subOutW || Core::vrSubrectOutH != subOutH;
		// Recreate if reactive/transparency source appeared but intermediate is missing
		if (!needsRecreate) {
			needsRecreate = (reactiveSrc && !Core::vrSubrectReactiveMask[0]) ||
			                (transparencySrc && !Core::vrSubrectTransparencyMask[0]);
		}

		if (needsRecreate) {
			for (int i = 0; i < 2; i++) {
				std::string suffix = (i == 0) ? "Left" : "Right";
				Core::vrSubrectColorIn[i] = CreateTextureFromSource(colorSrc, subInW, subInH, false, true, true, ("Enh_Subrect_ColorIn_" + suffix).c_str());
				Core::vrSubrectColorOut[i] = CreateTextureFromSource(colorSrc, subOutW, subOutH, false, true, false, ("Enh_Subrect_ColorOut_" + suffix).c_str());

				D3D11_TEXTURE2D_DESC depthDesc = {};
				depthDesc.Width = subInW;
				depthDesc.Height = subInH;
				depthDesc.MipLevels = 1;
				depthDesc.ArraySize = 1;
				depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
				depthDesc.SampleDesc.Count = 1;
				depthDesc.Usage = D3D11_USAGE_DEFAULT;
				depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				Core::vrSubrectDepth[i] = eastl::make_unique<Texture2D>(depthDesc);
				Util::SetResourceName(Core::vrSubrectDepth[i]->resource.get(), ("Enh_Subrect_Depth_" + suffix).c_str());

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				Core::vrSubrectDepth[i]->CreateSRV(srvDesc);

				Core::vrSubrectMotionVectors[i] = CreateTextureFromSource(mvecSrc, subInW, subInH, false, true, false, ("Enh_Subrect_MVec_" + suffix).c_str());
				if (reactiveSrc)
					Core::vrSubrectReactiveMask[i] = CreateTextureFromSource(reactiveSrc, subInW, subInH, false, true, false, ("Enh_Subrect_Reactive_" + suffix).c_str());
				else
					Core::vrSubrectReactiveMask[i].reset();
				if (transparencySrc)
					Core::vrSubrectTransparencyMask[i] = CreateTextureFromSource(transparencySrc, subInW, subInH, false, true, false, ("Enh_Subrect_Transparency_" + suffix).c_str());
				else
					Core::vrSubrectTransparencyMask[i].reset();
			}

			Core::vrSubrectInW = subInW;
			Core::vrSubrectInH = subInH;
			Core::vrSubrectOutW = subOutW;
			Core::vrSubrectOutH = subOutH;
		}
	}

	bool PreparePerEyeInputs(
		ID3D11Resource* colorSrc,
		ID3D11Resource* depthSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc,
		uint32_t eyeWidthIn,
		uint32_t eyeHeightIn,
		uint32_t eyeWidthOut,
		uint32_t eyeHeightOut)
	{
		EnsureVRIntermediateTextures(
			eyeWidthIn,
			eyeHeightIn,
			eyeWidthOut,
			eyeHeightOut,
			colorSrc,
			mvecSrc,
			reactiveSrc,
			transparencySrc);

		for (uint32_t i = 0; i < 2; ++i) {
			if (!Core::vrIntermediateColorIn[i] || !Core::vrIntermediateColorOut[i] ||
				!Core::vrIntermediateDepth[i] || !Core::vrIntermediateMotionVectors[i] ||
				(reactiveSrc && !Core::vrIntermediateReactiveMask[i]) ||
				(transparencySrc && !Core::vrIntermediateTransparencyMask[i])) {
				logger::error("[DLSSENHANCER] Missing per-eye intermediate resources for eye {}", i);
				return false;
			}
		}

		auto context = globals::d3d::context;
		auto* depthSRV = globals::game::renderer->GetDepthStencilData()
		                     .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN]
		                     .depthSRV;
		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t offsetXIn = (i == 1) ? eyeWidthIn : 0;
			D3D11_BOX srcBox = { offsetXIn, 0, 0, offsetXIn + eyeWidthIn, eyeHeightIn, 1 };

			context->CopySubresourceRegion(Core::vrIntermediateColorIn[i]->resource.get(), 0, 0, 0, 0, colorSrc, 0, &srcBox);
			context->CopySubresourceRegion(Core::vrIntermediateDepth[i]->resource.get(), 0, 0, 0, 0, depthSrc, 0, &srcBox);
			context->CopySubresourceRegion(Core::vrIntermediateMotionVectors[i]->resource.get(), 0, 0, 0, 0, mvecSrc, 0, &srcBox);
			if (transparencySrc)
				context->CopySubresourceRegion(Core::vrIntermediateTransparencyMask[i]->resource.get(), 0, 0, 0, 0, transparencySrc, 0, &srcBox);
			if (reactiveSrc)
				context->CopySubresourceRegion(Core::vrIntermediateReactiveMask[i]->resource.get(), 0, 0, 0, 0, reactiveSrc, 0, &srcBox);

			// Reapply HMD hidden-area mask clear into the per-eye intermediate so DLSS
			// history doesn't accumulate garbage from the masked-out region.
			// Depth source is full SBS (read at per-eye offset); color destination is per-eye
			// sized (write at offset 0).
			globals::features::upscaling.ClearHMDMask(
				Core::vrIntermediateColorIn[i]->uav.get(),
				depthSRV,
				eyeWidthIn,
				eyeHeightIn,
				i * eyeWidthIn,
				0);
		}

		return true;
	}

	bool FinalizePerEyeOutputs(ID3D11Resource* colorDst, uint32_t eyeWidthOut, uint32_t eyeHeightOut)
	{
		if (!colorDst) {
			logger::error("[DLSSENHANCER] FinalizePerEyeOutputs received null destination color resource");
			return false;
		}

		for (uint32_t i = 0; i < 2; ++i) {
			if (!Core::vrIntermediateColorOut[i]) {
				logger::error("[DLSSENHANCER] Missing per-eye output resource for eye {}", i);
				return false;
			}
		}

		auto context = globals::d3d::context;
		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t offsetXOut = (i == 1) ? eyeWidthOut : 0;
			D3D11_BOX outBox = { 0, 0, 0, eyeWidthOut, eyeHeightOut, 1 };
			context->CopySubresourceRegion(colorDst, 0, offsetXOut, 0, 0, Core::vrIntermediateColorOut[i]->resource.get(), 0, &outBox);
		}

		return true;
	}

	void StretchDRSToFullEye(
		ID3D11ShaderResourceView* renderSBSSRV,
		ID3D11UnorderedAccessView* kMainUAV,
		uint32_t dstOffsetX,
		uint32_t dstWidth,
		uint32_t dstHeight,
		uint32_t srcOffsetX,
		uint32_t srcWidth,
		uint32_t srcHeight,
		uint32_t srcEyeWidth,
		uint32_t srcEyeHeight)
	{
		auto context = globals::d3d::context;

		if (!Core::vrSubrectStretchCS) {
			Core::vrSubrectStretchCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Features/SubrectStretchCS.hlsl", {}, "cs_5_0"));

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.ByteWidth = 48;
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, Core::vrSubrectStretchCB.put()))) {
				logger::error("[DLSSENHANCER] Failed to create SubrectStretch constant buffer");
				return;
			}

			D3D11_SAMPLER_DESC sampDesc = {};
			sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			if (FAILED(globals::d3d::device->CreateSamplerState(&sampDesc, Core::vrSubrectStretchSampler.put()))) {
				logger::error("[DLSSENHANCER] Failed to create SubrectStretch sampler");
				return;
			}
		}

		if (!Core::vrSubrectStretchCS || !Core::vrSubrectStretchCB || !Core::vrSubrectStretchSampler) {
			return;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(Core::vrSubrectStretchCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			auto& enhSettings = globals::features::dlssEnhancer.settings;
			struct
			{
				uint32_t data[8];
				uint32_t stretchMode;
				float blurRadius;
				uint32_t pad[2];
			} cb = {};
			cb.data[0] = dstOffsetX;
			cb.data[1] = dstWidth;
			cb.data[2] = dstHeight;
			cb.data[3] = srcOffsetX;
			cb.data[4] = srcWidth;
			cb.data[5] = srcHeight;
			cb.data[6] = srcEyeWidth;
			cb.data[7] = srcEyeHeight;
			cb.stretchMode = enhSettings.stretchMode;
			// MVP-B uses a fixed 1.0 blur radius for the GaussianBlur stretch
			// path. The PR's user-tunable peripheryBlurRadius slider is
			// deferred to PR-3b along with the rest of the periphery quality
			// surface.
			cb.blurRadius = 1.0f;
			std::memcpy(mapped.pData, &cb, sizeof(cb));
			context->Unmap(Core::vrSubrectStretchCB.get(), 0);
		}

		context->CSSetShader(Core::vrSubrectStretchCS.get(), nullptr, 0);
		ID3D11Buffer* cbs[1] = { Core::vrSubrectStretchCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[1] = { renderSBSSRV };
		context->CSSetShaderResources(0, 1, srvs);
		ID3D11SamplerState* samplers[1] = { Core::vrSubrectStretchSampler.get() };
		context->CSSetSamplers(0, 1, samplers);
		ID3D11UnorderedAccessView* uavs[1] = { kMainUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->Dispatch((dstWidth + 7) / 8, (dstHeight + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		ID3D11SamplerState* nullSampler[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	void EnsureVRRenderSBS(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc)
	{
		if (!Core::vrRenderSBS || Core::vrRenderSBSW != renderW || Core::vrRenderSBSH != renderH) {
			Core::vrRenderSBS = CreateTextureFromSource(colorSrc, renderW, renderH, false, true, false, "Enh_RenderSBS");
			Core::vrRenderSBSW = renderW;
			Core::vrRenderSBSH = renderH;
		}
	}

	void EnsureFasterOutputTextures(uint32_t subOutW, uint32_t subOutH, ID3D11Resource* colorSrc)
	{
		bool needsRecreate = !Core::vrFasterColorOut[0] ||
		                     Core::vrFasterOutW != subOutW || Core::vrFasterOutH != subOutH;
		if (!needsRecreate)
			return;
		for (int i = 0; i < 2; i++) {
			std::string suffix = (i == 0) ? "Left" : "Right";
			Core::vrFasterColorOut[i] = CreateTextureFromSource(colorSrc, subOutW, subOutH, false, true, false, ("Enh_Faster_ColorOut_" + suffix).c_str());
		}
		Core::vrFasterOutW = subOutW;
		Core::vrFasterOutH = subOutH;
	}

	uint64_t ComputeSubrectUVHash(const Util::Subrect::UVRegion& uv, uint32_t mode)
	{
		uint64_t h = 0;
		auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 12) + (h >> 4); };
		mix(std::hash<float>{}(uv.x));
		mix(std::hash<float>{}(uv.y));
		mix(std::hash<float>{}(uv.w));
		mix(std::hash<float>{}(uv.h));
		mix(std::hash<uint32_t>{}(mode));
		return h;
	}

	void SnapshotSBS(ID3D11Resource* src, uint32_t renderW, uint32_t renderH)
	{
		EnsureVRRenderSBS(renderW, renderH, src);
		auto context = globals::d3d::context;
		D3D11_BOX drsBox = { 0, 0, 0, renderW, renderH, 1 };
		context->CopySubresourceRegion(Core::vrRenderSBS->resource.get(), 0, 0, 0, 0, src, 0, &drsBox);
	}

	void StretchDRSBothEyes(ID3D11UnorderedAccessView* dstUAV, uint32_t eyeWidthOut, uint32_t eyeHeightOut,
		uint32_t eyeWidthIn, uint32_t eyeHeightIn, uint32_t renderW, uint32_t renderH,
		ID3D11ShaderResourceView* srcOverride)
	{
		auto* src = srcOverride ? srcOverride : Core::vrRenderSBS->srv.get();
		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t dstX = (i == 1) ? eyeWidthOut : 0;
			uint32_t srcX = (i == 1) ? eyeWidthIn : 0;
			StretchDRSToFullEye(
				src, dstUAV,
				dstX, eyeWidthOut, eyeHeightOut,
				srcX, renderW, renderH,
				eyeWidthIn, eyeHeightIn);
		}
	}

	void BlendSubrectToOutput(ID3D11Resource* dlssSrc, ID3D11Resource* dst,
		uint32_t dstOffsetX, uint32_t dstOffsetY, uint32_t subWidth, uint32_t subHeight, uint32_t srcOffsetX)
	{
		auto context = globals::d3d::context;
		D3D11_BOX srcBox = { srcOffsetX, 0, 0, srcOffsetX + subWidth, subHeight, 1 };
		context->CopySubresourceRegion(dst, 0, dstOffsetX, dstOffsetY, 0, dlssSrc, 0, &srcBox);
	}

}  // namespace DlssEnhancer::Ops

namespace DlssEnhancer
{
	bool Core::PrepareVRPerEyeInputs(
		ID3D11Resource* colorSrc,
		ID3D11Resource* depthSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc,
		uint32_t eyeWidthIn,
		uint32_t eyeHeightIn,
		uint32_t eyeWidthOut,
		uint32_t eyeHeightOut)
	{
		return Ops::PreparePerEyeInputs(
			colorSrc,
			depthSrc,
			mvecSrc,
			reactiveSrc,
			transparencySrc,
			eyeWidthIn,
			eyeHeightIn,
			eyeWidthOut,
			eyeHeightOut);
	}

	bool Core::FinalizeVRPerEyeOutputs(
		ID3D11Resource* colorDst,
		uint32_t eyeWidthOut,
		uint32_t eyeHeightOut)
	{
		return Ops::FinalizePerEyeOutputs(colorDst, eyeWidthOut, eyeHeightOut);
	}

	void Core::ClearResources()
	{
		for (int i = 0; i < 2; ++i) {
			vrIntermediateColorIn[i].reset();
			vrIntermediateColorOut[i].reset();
			vrIntermediateDepth[i].reset();
			vrIntermediateMotionVectors[i].reset();
			vrIntermediateReactiveMask[i].reset();
			vrIntermediateTransparencyMask[i].reset();

			vrSubrectColorIn[i].reset();
			vrSubrectColorOut[i].reset();
			vrSubrectDepth[i].reset();
			vrSubrectMotionVectors[i].reset();
			vrSubrectReactiveMask[i].reset();
			vrSubrectTransparencyMask[i].reset();
		}
		vrSubrectInW = vrSubrectInH = vrSubrectOutW = vrSubrectOutH = 0;

		vrRenderSBS.reset();
		vrRenderSBSW = vrRenderSBSH = 0;

		vrFasterColorOut[0].reset();
		vrFasterColorOut[1].reset();
		vrFasterOutW = vrFasterOutH = 0;

		activeSubrectUVHash = 0;
	}

	void Core::ClearShaderCache()
	{
		vrSubrectStretchCS = nullptr;
		vrSubrectStretchCB = nullptr;
		vrSubrectStretchSampler = nullptr;
	}
}
