#include "TextureManager.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

TextureManager& TextureManager::GetSingleton()
{
	static TextureManager instance;
	return instance;
}

void TextureManager::Initialize()
{
	CreateCommonTextures();
	CreateDownsampleResources();
}

TextureManager::Texture* TextureManager::GetCommonTexture(const std::string& name)
{
	auto it = commonTextureCache.find(name);
	if (it != commonTextureCache.end()) {
		return &it->second;
	}
	return nullptr;
}

void TextureManager::SwapTextures(const std::string& name1, const std::string& name2)
{
	auto it1 = commonTextureCache.find(name1);
	auto it2 = commonTextureCache.find(name2);
	if (it1 != commonTextureCache.end() && it2 != commonTextureCache.end()) {
		std::swap(it1->second, it2->second);
	}
}

void TextureManager::CreateCommonTextures()
{
	// graphicsState->screenWidth/Height is the mirror window's resolution, not the HMD render
	// size on VR -- use globals::state->screenSize instead, or these textures are undersized.
	UINT screenWidth = static_cast<UINT>(globals::state->screenSize.x);
	UINT screenHeight = static_cast<UINT>(globals::state->screenSize.y);
	CreateResizableTextures(screenWidth, screenHeight);

	commonTextureCache.insert({ "TextureBloom", CreateTexture(1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::TextureBloom") });

	commonTextureCache.insert({ "TextureBloomTemp", CreateTexture(1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::TextureBloomLensTemp") });

	commonTextureCache.insert({ "TextureAdaptation", CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "TextureManager::TextureAdaptation") });
	commonTextureCache.insert({ "TextureAdaptationSwap", CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "TextureManager::TextureAdaptationSwap") });

	// Create fixed-size render targets for bloom/lens
	std::vector<std::pair<std::string, UINT>> fixedSizes = {
		{ "RenderTarget1024", 1024 },
		{ "RenderTarget512", 512 },
		{ "RenderTarget256", 256 },
		{ "RenderTarget128", 128 },
		{ "RenderTarget64", 64 },
		{ "RenderTarget32", 32 },
		{ "RenderTarget16", 16 }
	};

	for (auto& [name, size] : fixedSizes) {
		commonTextureCache[name] = CreateTexture(size, size, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::" + name);
	}
}

// Recreates only the canvas-sized textures, leaving fixed-size ones (bloom mip chain,
// 1x1 adaptation) untouched.
void TextureManager::CreateResizableTextures(uint32_t width, uint32_t height)
{
	commonTextureCache["TextureHDRTemp"] = CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::TextureHDRTemp");
	commonTextureCache["TextureHDRTemp2"] = CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::TextureHDRTemp2");

	commonTextureCache["RenderTargetRGBA32"] = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "TextureManager::RenderTargetRGBA32");
	commonTextureCache["RenderTargetRGBA64"] = CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_UNORM, "TextureManager::RenderTargetRGBA64");
	commonTextureCache["RenderTargetRGBA64F"] = CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::RenderTargetRGBA64F");
	commonTextureCache["RenderTargetR16F"] = CreateTexture(width, height, DXGI_FORMAT_R16_FLOAT, "TextureManager::RenderTargetR16F");
	commonTextureCache["RenderTargetR32F"] = CreateTexture(width, height, DXGI_FORMAT_R32_FLOAT, "TextureManager::RenderTargetR32F");
	commonTextureCache["RenderTargetRGB32F"] = CreateTexture(width, height, DXGI_FORMAT_R11G11B10_FLOAT, "TextureManager::RenderTargetRGB32F");

	commonTextureCache["TextureSDRTemp"] = CreateTexture(width, height, DXGI_FORMAT_R10G10B10A2_UNORM, "TextureManager::TextureSDRTemp");
	commonTextureCache["TextureSDRTemp2"] = CreateTexture(width, height, DXGI_FORMAT_R10G10B10A2_UNORM, "TextureManager::TextureSDRTemp2");

	commonTextureCache["TextureLens"] = CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "TextureManager::TextureLens");

	currentWidth = width;
	currentHeight = height;
}

void TextureManager::EnsureSize(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0 || (width == currentWidth && height == currentHeight))
		return;
	CreateResizableTextures(width, height);
}

TextureManager::Texture TextureManager::CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& debugName)
{
	TextureManager::Texture result;

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = width;
	texDesc.Height = height;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = format;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = 0;

	DX::ThrowIfFailed(globals::d3d::device->CreateTexture2D(&texDesc, nullptr, result.texture.put()));

	if (!debugName.empty()) {
		result.texture->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(debugName.length()), debugName.c_str());
	}

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Texture2D.MipSlice = 0;

	DX::ThrowIfFailed(globals::d3d::device->CreateRenderTargetView(result.texture.get(), &rtvDesc, result.rtv.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;

	DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(result.texture.get(), &srvDesc, result.srv.put()));

	return result;
}

void TextureManager::CreateDownsampleResources()
{
	auto device = globals::d3d::device;

	// Create linear sampler
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));

	// Create downsample vertex shader
	downsampleVS.attach(static_cast<ID3D11VertexShader*>(
		Util::CompileShader(L"Data\\Shaders\\Effects11\\QuadVS.hlsl", {}, "vs_5_0")));
	if (!downsampleVS) {
		logger::error("[TextureManager] Downsample vertex shader compilation failed");
		return;
	}

	// Create downsample pixel shader
	downsamplePS.attach(static_cast<ID3D11PixelShader*>(
		Util::CompileShader(L"Data\\Shaders\\Effects11\\DownsamplePS.hlsl", {}, "ps_5_0")));
	if (!downsamplePS) {
		logger::error("[TextureManager] Downsample pixel shader compilation failed");
		return;
	}

	// Create shared downsample texture
	sharedDownsampleTexture = CreateDownsampleTexture(DXGI_FORMAT_R11G11B10_FLOAT);
}

TextureManager::DownsampleTexture TextureManager::CreateDownsampleTexture(DXGI_FORMAT format)
{
	auto device = globals::d3d::device;

	DownsampleTexture fixedTexture;

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = 1024;
	texDesc.Height = 1024;
	texDesc.MipLevels = 3;  // 1024, 512, 256
	texDesc.ArraySize = 1;
	texDesc.Format = format;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, fixedTexture.texture.put()));

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Texture2D.MipSlice = 0;

	DX::ThrowIfFailed(device->CreateRenderTargetView(fixedTexture.texture.get(), &rtvDesc, fixedTexture.rtv.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 3;
	srvDesc.Texture2D.MostDetailedMip = 0;
	DX::ThrowIfFailed(device->CreateShaderResourceView(fixedTexture.texture.get(), &srvDesc, fixedTexture.srvChain.put()));

	srvDesc.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(device->CreateShaderResourceView(fixedTexture.texture.get(), &srvDesc, fixedTexture.srv.put()));

	srvDesc.Texture2D.MostDetailedMip = 2;
	DX::ThrowIfFailed(device->CreateShaderResourceView(fixedTexture.texture.get(), &srvDesc, fixedTexture.srvBlurry.put()));

	// Set debug names
	Util::SetResourceName(fixedTexture.texture.get(), "TextureManager::DownsampleTexture (1024x1024, 3 mips)");
	Util::SetResourceName(fixedTexture.rtv.get(), "TextureManager::DownsampleTexture RTV");
	Util::SetResourceName(fixedTexture.srvChain.get(), "TextureManager::DownsampleTexture SRV Chain");
	Util::SetResourceName(fixedTexture.srv.get(), "TextureManager::DownsampleTexture SRV 1024x1024");
	Util::SetResourceName(fixedTexture.srvBlurry.get(), "TextureManager::DownsampleTexture SRV 256x256");

	logger::info("[TextureManager] Created downsample texture: 1024x1024 with 3 mips (1024, 512, 256)");

	return fixedTexture;
}

void TextureManager::DownsampleToFixed(ID3D11ShaderResourceView* source, DownsampleTexture& texture)
{
	if (!source || !texture.rtv || !downsampleVS || !downsamplePS || !linearSampler || !texture.srvChain) {
		return;
	}

	auto context = globals::d3d::context;

	D3D11_VIEWPORT viewport = {};
	viewport.Width = 1024.0f;
	viewport.Height = 1024.0f;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	context->VSSetShader(downsampleVS.get(), nullptr, 0);

	ID3D11SamplerState* samplerArray[] = { linearSampler.get() };
	context->PSSetSamplers(0, 1, samplerArray);

	ID3D11RenderTargetView* rtvArray[] = { texture.rtv.get() };
	context->OMSetRenderTargets(1, rtvArray, nullptr);
	context->PSSetShaderResources(0, 1, &source);
	context->PSSetShader(downsamplePS.get(), nullptr, 0);
	globals::profiler->BeginPass("Effects11::Downsample");
	context->Draw(4, 0);
	globals::profiler->EndPass();

	context->GenerateMips(texture.srvChain.get());
}

void TextureManager::UpdateDownsampledTexture(ID3D11ShaderResourceView* source)
{
	DownsampleToFixed(source, sharedDownsampleTexture);
}

ID3D11ShaderResourceView* TextureManager::GetDownsampleTexture() const
{
	return sharedDownsampleTexture.srv.get();
}

ID3D11ShaderResourceView* TextureManager::GetDownsampleTextureBlurry() const
{
	return sharedDownsampleTexture.srvBlurry.get();
}