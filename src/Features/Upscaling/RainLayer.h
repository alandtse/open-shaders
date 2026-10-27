#pragma once

#include "Buffer.h"
#include "Utils/LazyShader.h"

/** @brief Keeps lit rain out of temporal reconstruction and composites it at display resolution. */
class RainLayer
{
public:
	/** @brief Redirects the current rain draw when all composite resources are available. */
	void CaptureDraw(ID3D11UnorderedAccessView* a_output);
	/** @brief Composites this frame's rain into the upscaler's output. */
	void Composite(ID3D11UnorderedAccessView* a_output);
	/** @brief Invalidates the composite shader and its cached failure. */
	void ClearShaderCache();

private:
	struct alignas(16) CompositeData
	{
		uint32_t sceneLinear;
		uint32_t padding[3]{};
	};

	bool PrepareResources(ID3D11UnorderedAccessView* a_output);

	std::unique_ptr<Texture2D> texture;
	std::unique_ptr<ConstantBuffer> compositeCB;
	winrt::com_ptr<ID3D11Device> typedUavLoadDevice;
	DXGI_FORMAT typedUavLoadFormat = DXGI_FORMAT_UNKNOWN;
	bool typedUavLoadSupported = false;
	winrt::com_ptr<ID3D11BlendState> additiveBlend;
	winrt::com_ptr<ID3D11BlendState> alphaBlend;
	Util::LazyShader<ID3D11ComputeShader> compositeCS;
	CompositeData data{};
	uint32_t frame = UINT32_MAX;
	bool gammaEncoded = false;
	bool pending = false;
	bool failed = false;
};
