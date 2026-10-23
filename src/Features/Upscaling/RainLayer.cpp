#include "RainLayer.h"

#include "Features/RainRefraction.h"
#include "GpuPass.h"
#include "ShaderCache.h"
#include "State.h"

namespace
{
	constexpr auto kGammaRenderTarget = static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
}

bool RainLayer::PrepareResources(ID3D11UnorderedAccessView* a_output)
{
	if (failed)
		return false;

	D3D11_TEXTURE2D_DESC desc{};
	if (!Util::GetTexture2DDesc(a_output, desc) || desc.SampleDesc.Count != 1)
		return false;
	if (texture && texture->desc.Width == desc.Width && texture->desc.Height == desc.Height && texture->desc.Format == desc.Format)
		return compositeCS.get() != nullptr;

	try {
		const std::vector<std::pair<const char*, const char*>> defines = globals::game::isVR ?
		                                                                     std::vector<std::pair<const char*, const char*>>{ { "VR", "" } } :
		                                                                     std::vector<std::pair<const char*, const char*>>{};
		if (!compositeCS.Get(L"Data\\Shaders\\Upscaling\\CompositeRainCS.hlsl", defines, "cs_5_0", "main", "Upscaling::CompositeRain"))
			return false;

		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		auto nextTexture = std::make_unique<Texture2D>(desc, "Upscaling::RainLayer");
		nextTexture->CreateRTV({ .Format = desc.Format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D });
		nextTexture->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MipLevels = 1 } });

		if (!compositeCB)
			compositeCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CompositeData>(), "Upscaling::RainCompositeData");
		if (!additiveBlend || !alphaBlend) {
			D3D11_BLEND_DESC blendDesc{};
			auto& blend = blendDesc.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend.DestBlend = D3D11_BLEND_ONE;
			blend.BlendOp = D3D11_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D11_BLEND_ZERO;
			blend.DestBlendAlpha = D3D11_BLEND_ONE;
			blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, additiveBlend.put()));
			Util::SetResourceName(additiveBlend.get(), "Upscaling::RainAdditiveBlend");
			blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend.SrcBlendAlpha = D3D11_BLEND_ONE;
			blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, alphaBlend.put()));
			Util::SetResourceName(alphaBlend.get(), "Upscaling::RainAlphaBlend");
		}
		texture = std::move(nextTexture);
		return true;
	} catch (const std::exception& error) {
		logger::error("[Upscaling] Rain layer unavailable: {}", error.what());
		failed = true;
		return false;
	}
}

void RainLayer::CaptureDraw(ID3D11UnorderedAccessView* a_output)
{
	auto* state = globals::state;
	if (state->currentShader->shaderType.get() != RE::BSShader::Type::Particle ||
		state->currentPixelDescriptor != static_cast<uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::EnvCubeRain))
		return;
	if (!globals::features::rainRefraction.loaded)
		return;

	auto* context = globals::d3d::context;
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	if (!a_output)
		return;
	D3D11_UNORDERED_ACCESS_VIEW_DESC outputDesc{};
	a_output->GetDesc(&outputDesc);
	if (typedUavLoadDevice.get() != globals::d3d::device || typedUavLoadFormat != outputDesc.Format) {
		typedUavLoadDevice.copy_from(globals::d3d::device);
		typedUavLoadFormat = outputDesc.Format;
		typedUavLoadSupported = State::SupportsTypedUAVLoad(typedUavLoadFormat);
	}
	if (!typedUavLoadSupported)
		return;

	winrt::com_ptr<ID3D11RenderTargetView> target;
	context->OMGetRenderTargets(1, target.put(), nullptr);
	if (target.get() != Util::AsReal(main.RTV) || !PrepareResources(a_output))
		return;

	winrt::com_ptr<ID3D11BlendState> blendState;
	float blendFactor[4];
	UINT sampleMask;
	context->OMGetBlendState(blendState.put(), blendFactor, &sampleMask);
	if (!blendState)
		return;
	D3D11_BLEND_DESC blendDesc{};
	blendState->GetDesc(&blendDesc);
	const auto& blend = blendDesc.RenderTarget[0];
	if (!blend.BlendEnable || blend.SrcBlend != D3D11_BLEND_SRC_ALPHA || blend.BlendOp != D3D11_BLEND_OP_ADD ||
		(blend.DestBlend != D3D11_BLEND_ONE && blend.DestBlend != D3D11_BLEND_INV_SRC_ALPHA))
		return;
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	if (!depthSRV)
		return;
	UINT viewportCount = 1;
	D3D11_VIEWPORT viewport{};
	context->RSGetViewports(&viewportCount, &viewport);
	const float2 rasterSize{ static_cast<float>(texture->desc.Width), static_cast<float>(texture->desc.Height) };
	if (!globals::features::rainRefraction.BindRenderSpace(rasterSize, { viewport.Width / rasterSize.x, viewport.Height / rasterSize.y }, true))
		return;

	if (frame != state->frameCount) {
		CS_GPU_PASS("Upscaling::ClearRain");
		const float clear[4]{};
		context->ClearRenderTargetView(texture->rtv.get(), clear);
		gammaEncoded = (state->permutationData.ExtraShaderDescriptor & kGammaRenderTarget) != 0;
		frame = state->frameCount;
	}
	auto* layerRTV = texture->rtv.get();
	context->OMSetRenderTargets(1, &layerRTV, nullptr);
	context->PSSetShaderResources(82, 1, &depthSRV);
	viewport.Width = rasterSize.x;
	viewport.Height = rasterSize.y;
	context->RSSetViewports(1, &viewport);
	context->OMSetBlendState(globals::features::rainRefraction.GetCommonBufferData().Enable ? alphaBlend.get() : additiveBlend.get(), blendFactor, sampleMask);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET, RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND, RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	pending = true;
}

void RainLayer::Composite(ID3D11UnorderedAccessView* a_output)
{
	if (!pending || frame != globals::state->frameCount)
		return;
	pending = false;
	if (!a_output)
		return;

	CS_GPU_PASS("Upscaling::CompositeRain");
	D3D11_TEXTURE2D_DESC outputDesc{};
	if (!Util::GetTexture2DDesc(a_output, outputDesc))
		return;
	data.sceneLinear = gammaEncoded && (globals::state->permutationData.ExtraShaderDescriptor & kGammaRenderTarget) == 0;
	compositeCB->Update(data);
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CSSetShader(compositeCS.get(), nullptr, 0);
	globals::state->BindSharedDataCS(context, true);
	auto* cb = compositeCB->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	auto* srv = texture->srv.get();
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &a_output, nullptr);
	context->Dispatch((outputDesc.Width + 7) / 8, (outputDesc.Height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void RainLayer::ClearShaderCache()
{
	compositeCS.Reset();
	texture.reset();
	typedUavLoadDevice = nullptr;
	typedUavLoadFormat = DXGI_FORMAT_UNKNOWN;
	typedUavLoadSupported = false;
	failed = false;
	pending = false;
	frame = UINT32_MAX;
}
