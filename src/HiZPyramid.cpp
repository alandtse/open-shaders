#include "HiZPyramid.h"

#include "Globals.h"
#include "GpuPass.h"
#include "Profiler.h"
#include "State.h"

void HiZPyramid::SetupResources()
{
	paramsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<BaseParams>(), "HiZPyramid::ParamsCB");
}

void HiZPyramid::ClearShaderCache()
{
	baseCS.Reset();
	spdCS.Reset();
}

bool HiZPyramid::CreateTextures(ID3D11Device* device, uint32_t dstW, uint32_t dstH, uint32_t count)
{
	for (auto& texture : textures)
		texture.reset();
	for (auto& uavs : mipUAVs)
		uavs.clear();
	for (auto& srv : mip0SRVs)
		srv = nullptr;
	paddedWidth = 0;
	paddedHeight = 0;
	mipCount = 1;

	uint32_t mips = 1;
	for (uint32_t d = std::max(dstW, dstH); d > 1; d >>= 1)
		++mips;
	mips = std::min(mips, kExactMips + 1u);

	D3D11_TEXTURE2D_DESC td{};
	td.Width = dstW;
	td.Height = dstH;
	td.MipLevels = mips;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

	// Full-chain SRV for the consumer plus single-level views for the reduction passes.
	D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
	sd.Format = DXGI_FORMAT_R32_FLOAT;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MostDetailedMip = 0;
	sd.Texture2D.MipLevels = mips;

	const char* reductionName = reduction == Reduction::Max ? "Max" : "Min";

	for (uint32_t eye = 0; eye < count; ++eye) {
		const auto name = std::format("HiZPyramid::{} Eye{}", reductionName, eye);
		try {
			textures[eye] = std::make_unique<Texture2D>(td, name.c_str());
			textures[eye]->CreateSRV(sd);

			// A mip-0-only SRV lets SPD read mip 0 without overlapping the output UAVs.
			sd.Texture2D.MipLevels = 1;
			DX::ThrowIfFailed(device->CreateShaderResourceView(textures[eye]->resource.get(), &sd, mip0SRVs[eye].put()));
			Util::SetResourceName(mip0SRVs[eye].get(), "%s Mip0 SRV", name.c_str());
			sd.Texture2D.MipLevels = mips;
		} catch (...) {
			logger::error("[HIZ PYRAMID] texture create failed");
			for (auto& texture : textures)
				texture.reset();
			return false;
		}

		for (uint32_t m = 0; m < mips; ++m) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
			ud.Format = DXGI_FORMAT_R32_FLOAT;
			ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			ud.Texture2D.MipSlice = m;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			if (FAILED(device->CreateUnorderedAccessView(textures[eye]->resource.get(), &ud, uav.put()))) {
				logger::error("[HIZ PYRAMID] mip UAV create failed");
				return false;
			}
			Util::SetResourceName(uav.get(), "%s Mip%u UAV", name.c_str(), m);
			mipUAVs[eye].push_back(uav);
		}
	}

	paddedWidth = dstW;
	paddedHeight = dstH;
	allocatedEyes = count;
	mipCount = mips;
	return true;
}

bool HiZPyramid::Build(ID3D11Device* device, ID3D11DeviceContext* ctx, const BuildDesc& desc)
{
	valid = false;

	if (!paramsCB || !globals::game::renderer || !desc.sourceDepth)
		return false;

	// globals::game::graphicsState->screenWidth/Height is the desktop preview window's resolution on
	// VR, not the HMD's -- use the same nominal size UpdateGrass derives from the live render target.
	float2 screenSize = globals::state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	const uint32_t srcW = std::max(1u, (uint32_t)std::lround(renderSize.x));
	const uint32_t srcH = std::max(1u, (uint32_t)std::lround(renderSize.y));

	// Eye 0 owns the packed source's left half and eye 1 its right half. Both windows take the floored
	// half width, so an odd packed width drops eye 1's last column rather than overlapping the halves.
	eyeCount = desc.eyeSeparated ? 2u : 1u;
	reduction = desc.reduction;
	const uint32_t windowWidth = desc.eyeSeparated ? std::max(1u, srcW >> 1) : srcW;

	const uint32_t validW = (windowWidth + kDownsampleFactor - 1) / kDownsampleFactor;
	const uint32_t validH = (srcH + kDownsampleFactor - 1) / kDownsampleFactor;
	if (!validW || !validH)
		return false;

	// Converts a consumer's nominal-pixel projPx into texels, derived from the extent resolved above so
	// the two cannot disagree. One scalar covers both axes, so the axis that shrank least wins: too
	// small a radius picks a level whose fixed 3x3 footprint misses part of the tested object.
	const float effX = (float)srcW / std::max(1.0f, screenSize.x);
	const float effY = (float)srcH / std::max(1.0f, screenSize.y);
	texelPixels = kDownsampleFactor / std::clamp(std::max(effX, effY), 0.01f, 1.0f);

	// Sized from the nominal extent so a shifting dynamic-resolution ratio never reallocates, then padded to SPD's tile granularity so every allocated level halves exactly. An odd level would drop its last row and leave that row's depth out of every coarser level.
	const auto padToTile = [](uint32_t v) { return (v + tileSize - 1) & ~(tileSize - 1); };
	const uint32_t nominalW = desc.eyeSeparated ? std::max(1u, (uint32_t)screenSize.x / 2) : (uint32_t)screenSize.x;
	const uint32_t padW = padToTile((nominalW + kDownsampleFactor - 1) / kDownsampleFactor);
	const uint32_t padH = padToTile(((uint32_t)screenSize.y + kDownsampleFactor - 1) / kDownsampleFactor);

	if (!padW || !padH || ((padW != paddedWidth || padH != paddedHeight || eyeCount != allocatedEyes) && !CreateTextures(device, padW, padH, eyeCount)))
		return false;

	width = validW;
	height = validH;

	std::vector<std::pair<const char*, const char*>> defines;
	if (desc.reduction == Reduction::Min)
		defines.push_back({ "HIZ_REDUCTION_MIN", "" });
	if (desc.sourceUnormTyped)
		defines.push_back({ "HIZ_SOURCE_UNORM", "" });

	if (!baseCS.Get(L"Data\\Shaders\\Common\\HiZBaseCS.hlsl", defines, "cs_5_0", "main", "HiZPyramid::BaseCS")) {
		logger::error("[HIZ PYRAMID] base reduction shader load failed -- the pyramid is unavailable this frame");
		return false;
	}

	if (!spdCS.Get(L"Data\\Shaders\\Common\\SPD.hlsl", defines, "cs_5_0", "main", "HiZPyramid::SpdCS"))
		logger::error("[HIZ PYRAMID] mip chain shader load failed -- only the base level is reduced");

	ID3D11ShaderResourceView* srcSRV = desc.sourceDepth;

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;

	{
		CS_GPU_PASS("HiZPyramid::Base");
		// Unbind the depth target for the dispatch, so the source is used solely as an SRV, since a
		// resource cannot be bound as both a DSV and an SRV at the same time.
		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* dsv = nullptr;
		if (desc.sourceBoundAsRenderTarget) {
			ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
			ctx->OMSetRenderTargets(0, nullptr, nullptr);
		}

		ID3D11ComputeShader* base = baseCS.get();
		ID3D11Buffer* cb = paramsCB->CB();
		ctx->CSSetShader(base, nullptr, 0);
		ctx->CSSetConstantBuffers(0, 1, &cb);
		ctx->CSSetShaderResources(0, 1, &srcSRV);

		for (uint32_t eye = 0; eye < eyeCount; ++eye) {
			// Keeps each chain's reads inside its own half of the packed source.
			paramsCB->Update(BaseParams{ srcW, srcH, padW, padH, eye * windowWidth, windowWidth });

			ID3D11UnorderedAccessView* baseUAV = mipUAVs[eye][0].get();
			ctx->CSSetUnorderedAccessViews(0, 1, &baseUAV, nullptr);
			ctx->Dispatch((padW + 7) / 8, (padH + 7) / 8, 1);
			ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		}

		ctx->CSSetShaderResources(0, 1, &nullSRV);

		if (desc.sourceBoundAsRenderTarget) {
			ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
			for (auto* rtv : rtvs) {
				if (rtv)
					rtv->Release();
			}
			if (dsv)
				dsv->Release();
		}
	}

	// Each level is the exact reduction of the one above, so a consumer can test an object of any
	// on-screen size against a fixed number of texels. One dispatch for a whole chain: every group
	// reduces its own tile from LDS, and each chain reads and writes only its own texture.
	if (spdCS && GetMipCount() > 1) {
		CS_GPU_PASS("HiZPyramid::Mips");

		const uint32_t outputMips = GetMipCount() - 1;
		const uint32_t groupsX = padW / tileSize;
		const uint32_t groupsY = padH / tileSize;

		const uint32_t totalGroups = groupsX * groupsY;
		paramsCB->Update(SPDParams{ padW, padH, outputMips, totalGroups });

		ID3D11ComputeShader* spd = spdCS.get();
		ctx->CSSetShader(spd, nullptr, 0);

		for (uint32_t eye = 0; eye < eyeCount; ++eye) {
			ID3D11UnorderedAccessView* spdUAVs[6]{};
			for (uint32_t i = 0; i < outputMips; ++i)
				spdUAVs[i] = mipUAVs[eye][i + 1].get();
			ID3D11ShaderResourceView* spdSourceSRV = mip0SRVs[eye].get();

			ctx->CSSetShaderResources(0, 1, &spdSourceSRV);
			ctx->CSSetUnorderedAccessViews(0, 6, spdUAVs, nullptr);
			ctx->Dispatch(groupsX, groupsY, 1);

			ID3D11UnorderedAccessView* spdNulls[6]{};
			ctx->CSSetUnorderedAccessViews(0, 6, spdNulls, nullptr);
		}

		ctx->CSSetShaderResources(0, 1, &nullSRV);
	}

	stats = Stats{ windowWidth, srcH, padW, padH };
	valid = true;
	return true;
}
