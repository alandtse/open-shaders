#include "Upscaling.h"

#include "Deferred.h"
#include "FoveatedCommon.h"
#include "Hooks.h"
#include "State.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/Streamline.h"
#include "Utils/UI.h"
#include "VR.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <directx/d3dx12.h>
#include <format>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	qualityMode,
	dlssPreset,
	frameLimitMode,
	frameGenerationMode,
	frameGenerationForceEnable,
	streamlineLogLevel,
	sharpnessFSR,
	sharpnessDLSS,
	fsr4RuntimeEnable,
	fsr4AllowNonRx90Amd,
	foveatedVendorDispatch,
	foveatedCenterArea,
	foveatedCenterHorizontalScale,
	foveatedLeftEyeMaskOffsetX,
	foveatedLeftEyeMaskOffsetY,
	foveatedRightEyeMaskOffsetX,
	foveatedRightEyeMaskOffsetY,
	periphery_taa_center_area,
	periphery_taa_center_horizontal_scale,
	periphery_taa_left_eye_mask_offset_x,
	periphery_taa_left_eye_mask_offset_y,
	periphery_taa_right_eye_mask_offset_x,
	periphery_taa_right_eye_mask_offset_y,
	foveatedPeripheryMaskVisualization,
	periphery_taa_enable,
	periphery_taa_outer_scale,
	periphery_taa_center_blend_feather,
	foveatedSetupVersion,
	foveatedStep1Confirmed,
	foveatedStep2Confirmed,
	reflexLowLatencyMode,
	reflexLowLatencyBoost,
	reflexUseMarkersToOptimize,
	reflexUseFPSLimit,
	reflexFPSLimit);

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChainUpscaling;

namespace
{
	constexpr float kPeripheryTAAOuterScaleMin = 0.30f;
	constexpr float kPeripheryTAAOuterScaleMax = 1.0f;
	constexpr float kPeripheryTAACenterBlendFeatherMin = 0.0f;
	constexpr float kPeripheryTAACenterBlendFeatherMax = 0.10f;
	constexpr float kFoveatedMaskOffsetAdjustMin = -0.15f;
	constexpr float kFoveatedMaskOffsetAdjustMax = 0.15f;
	constexpr float kFoveatedMaskOffsetResolvedMin = -0.25f;
	constexpr float kFoveatedMaskOffsetResolvedMax = 0.25f;
	constexpr uint32_t kRequiredFoveatedSetupVersion = 1u;

	float ClampFoveatedCenterArea(float value)
	{
		return FoveatedCommon::ClampCenterArea(value);
	}

	float ClampFoveatedCenterHorizontalScale(float value)
	{
		return FoveatedCommon::ClampCenterHorizontalScale(value);
	}

	float ClampFoveatedMaskOffsetAdjustment(float value)
	{
		if (!std::isfinite(value))
			return 0.0f;
		return std::clamp(value, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax);
	}

	uint ClampToggleUInt(uint value)
	{
		return std::min<uint>(value, 1u);
	}

	uint ClampQualityModeUInt(uint value)
	{
		return std::min<uint>(value, 4u);
	}

	uint ClampStreamlineLogLevelUInt(uint value)
	{
		return std::min<uint>(value, 2u);
	}

	float ClampPeripheryTAACenterBlendFeather(float value)
	{
		if (!std::isfinite(value))
			return FoveatedCommon::kCenterFeather;
		return std::clamp(value, kPeripheryTAACenterBlendFeatherMin, kPeripheryTAACenterBlendFeatherMax);
	}

	float ClampPeripheryTAAOuterScale(float value)
	{
		if (!std::isfinite(value))
			return 1.0f;
		return std::clamp(value, kPeripheryTAAOuterScaleMin, kPeripheryTAAOuterScaleMax);
	}

	float GetPeripheryTAAOuterScaleFloor(float centerScale, float centerHorizontalScale, float centerFeather)
	{
		centerScale = ClampFoveatedCenterArea(centerScale);
		centerHorizontalScale = ClampFoveatedCenterHorizontalScale(centerHorizontalScale);
		centerFeather = std::max(ClampPeripheryTAACenterBlendFeather(centerFeather), 1e-4f);

		const float radiusX = std::max(centerScale * centerHorizontalScale * 0.5f, 1e-4f);
		const float radiusY = std::max(centerScale * 0.5f, 1e-4f);
		const float baseRadius = std::max(std::min(radiusX, radiusY), 1e-4f);
		const float normalizedFeather = centerFeather / baseRadius;
		const float minOuterScale = centerScale * (1.0f + normalizedFeather);
		return std::clamp(minOuterScale, kPeripheryTAAOuterScaleMin, kPeripheryTAAOuterScaleMax);
	}

	float ClampPeripheryTAAOuterScaleForCenter(float value, float centerScale, float centerHorizontalScale, float centerFeather)
	{
		const float minOuterScale = GetPeripheryTAAOuterScaleFloor(centerScale, centerHorizontalScale, centerFeather);
		return std::clamp(ClampPeripheryTAAOuterScale(value), minOuterScale, kPeripheryTAAOuterScaleMax);
	}

	float ClampFiniteUnitRange(float value, float fallback)
	{
		if (!std::isfinite(value))
			return fallback;
		return std::clamp(value, 0.0f, 1.0f);
	}

	int32_t QuantizePeripheryTAATileParam(float value)
	{
		if (!std::isfinite(value))
			value = 0.0f;
		return static_cast<int32_t>(std::lround(value * 10000.0f));
	}

	uint32_t MapOutputToInputMin(uint32_t outputValue, uint32_t outputExtent, uint32_t inputExtent)
	{
		if (outputExtent == 0)
			return 0u;
		const double scale = static_cast<double>(inputExtent) / static_cast<double>(outputExtent);
		return static_cast<uint32_t>(std::floor(static_cast<double>(outputValue) * scale));
	}

	uint32_t MapOutputToInputMax(uint32_t outputValue, uint32_t outputExtent, uint32_t inputExtent)
	{
		if (outputExtent == 0)
			return 0u;
		const double scale = static_cast<double>(inputExtent) / static_cast<double>(outputExtent);
		return static_cast<uint32_t>(std::ceil(static_cast<double>(outputValue) * scale));
	}

	struct MappedRect
	{
		uint32_t minX = 0;
		uint32_t minY = 0;
		uint32_t maxX = 0;
		uint32_t maxY = 0;

		bool IsValid() const
		{
			return maxX > minX && maxY > minY;
		}
	};

	MappedRect MapOutputRectToInputRect(
		uint32_t outputMinX,
		uint32_t outputMinY,
		uint32_t outputMaxX,
		uint32_t outputMaxY,
		uint32_t outputWidth,
		uint32_t outputHeight,
		uint32_t inputWidth,
		uint32_t inputHeight,
		uint32_t padding = 0u)
	{
		MappedRect mapped{};
		if (outputWidth == 0 || outputHeight == 0 || inputWidth == 0 || inputHeight == 0)
			return mapped;

		outputMinX = std::min(outputMinX, outputWidth);
		outputMinY = std::min(outputMinY, outputHeight);
		outputMaxX = std::min(outputMaxX, outputWidth);
		outputMaxY = std::min(outputMaxY, outputHeight);
		if (outputMaxX < outputMinX)
			std::swap(outputMaxX, outputMinX);
		if (outputMaxY < outputMinY)
			std::swap(outputMaxY, outputMinY);
		if (outputMaxX <= outputMinX || outputMaxY <= outputMinY)
			return mapped;

		uint32_t inputMinX = MapOutputToInputMin(outputMinX, outputWidth, inputWidth);
		uint32_t inputMaxX = MapOutputToInputMax(outputMaxX, outputWidth, inputWidth);
		uint32_t inputMinY = MapOutputToInputMin(outputMinY, outputHeight, inputHeight);
		uint32_t inputMaxY = MapOutputToInputMax(outputMaxY, outputHeight, inputHeight);

		if (padding > 0) {
			inputMinX = inputMinX > padding ? inputMinX - padding : 0u;
			inputMinY = inputMinY > padding ? inputMinY - padding : 0u;
			inputMaxX = static_cast<uint32_t>(std::min<uint64_t>(inputWidth, static_cast<uint64_t>(inputMaxX) + padding));
			inputMaxY = static_cast<uint32_t>(std::min<uint64_t>(inputHeight, static_cast<uint64_t>(inputMaxY) + padding));
		}

		inputMinX = std::min(inputMinX, inputWidth);
		inputMinY = std::min(inputMinY, inputHeight);
		inputMaxX = std::min(inputMaxX, inputWidth);
		inputMaxY = std::min(inputMaxY, inputHeight);
		if (inputMaxX <= inputMinX || inputMaxY <= inputMinY)
			return mapped;

		mapped.minX = inputMinX;
		mapped.minY = inputMinY;
		mapped.maxX = inputMaxX;
		mapped.maxY = inputMaxY;
		return mapped;
	}

	struct FoveatedMaskProfileParams
	{
		float centerArea = 0.6f;
		float centerHorizontalScale = 1.0f;
		float leftOffsetX = 0.0f;
		float leftOffsetY = 0.0f;
		float rightOffsetX = 0.0f;
		float rightOffsetY = 0.0f;
	};

	FoveatedMaskProfileParams GetFoveatedMaskProfileParams(const Upscaling::Settings& settings, bool usePeripheryTAAProfile)
	{
		FoveatedMaskProfileParams params{};
		if (usePeripheryTAAProfile) {
			params.centerArea = ClampFoveatedCenterArea(settings.periphery_taa_center_area);
			params.centerHorizontalScale = ClampFoveatedCenterHorizontalScale(settings.periphery_taa_center_horizontal_scale);
			params.leftOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_left_eye_mask_offset_x);
			params.leftOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_left_eye_mask_offset_y);
			params.rightOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_right_eye_mask_offset_x);
			params.rightOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_right_eye_mask_offset_y);
			return params;
		}

		params.centerArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
		params.centerHorizontalScale = ClampFoveatedCenterHorizontalScale(settings.foveatedCenterHorizontalScale);
		params.leftOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetX);
		params.leftOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetY);
		params.rightOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetX);
		params.rightOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetY);
		return params;
	}

	bool IsFoveatedSetupComplete(const Upscaling::Settings& settings)
	{
		return settings.foveatedSetupVersion >= kRequiredFoveatedSetupVersion &&
		       settings.foveatedStep1Confirmed &&
		       settings.foveatedStep2Confirmed;
	}

	float FoveatedMaskDistanceUV(float uvX, float uvY, float centerScale, float centerHorizontalScale, float centerOffsetX, float centerOffsetY)
	{
		centerScale = ClampFoveatedCenterArea(centerScale);
		centerHorizontalScale = ClampFoveatedCenterHorizontalScale(centerHorizontalScale);

		const float radiusX = std::max(centerScale * centerHorizontalScale * 0.5f, 1e-4f);
		const float radiusY = std::max(centerScale * 0.5f, 1e-4f);
		const float centerX = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerY = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);
		const float normalizedX = std::abs((uvX - centerX) / radiusX);
		const float normalizedY = std::abs((uvY - centerY) / radiusY);
		const float pNorm = std::pow(normalizedX, FoveatedCommon::kMaskShapePower) + std::pow(normalizedY, FoveatedCommon::kMaskShapePower);
		return std::pow(std::max(pNorm, 0.0f), 1.0f / FoveatedCommon::kMaskShapePower);
	}

	float FoveatedMaskDistancePixelCenter(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float centerScale, float centerHorizontalScale, float centerOffsetX, float centerOffsetY)
	{
		const float invWidth = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
		const float invHeight = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
		return FoveatedMaskDistanceUV((static_cast<float>(x) + 0.5f) * invWidth, (static_cast<float>(y) + 0.5f) * invHeight, centerScale, centerHorizontalScale, centerOffsetX, centerOffsetY);
	}

	float FoveatedMaskTileMinDistance(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY, uint32_t width, uint32_t height, float centerScale, float centerHorizontalScale, float centerOffsetX, float centerOffsetY)
	{
		const float invWidth = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
		const float invHeight = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
		const float minUvX = (static_cast<float>(minX) + 0.5f) * invWidth;
		const float minUvY = (static_cast<float>(minY) + 0.5f) * invHeight;
		const float maxUvX = (static_cast<float>(maxX - 1u) + 0.5f) * invWidth;
		const float maxUvY = (static_cast<float>(maxY - 1u) + 0.5f) * invHeight;
		const float centerUvX = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerUvY = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);
		return FoveatedMaskDistanceUV(
			std::clamp(centerUvX, minUvX, maxUvX),
			std::clamp(centerUvY, minUvY, maxUvY),
			centerScale,
			centerHorizontalScale,
			centerOffsetX,
			centerOffsetY);
	}

	float FoveatedMaskTileMaxDistance(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY, uint32_t width, uint32_t height, float centerScale, float centerHorizontalScale, float centerOffsetX, float centerOffsetY)
	{
		const uint32_t maxPixelX = maxX - 1u;
		const uint32_t maxPixelY = maxY - 1u;
		return std::max({
			FoveatedMaskDistancePixelCenter(minX, minY, width, height, centerScale, centerHorizontalScale, centerOffsetX, centerOffsetY),
			FoveatedMaskDistancePixelCenter(maxPixelX, minY, width, height, centerScale, centerHorizontalScale, centerOffsetX, centerOffsetY),
			FoveatedMaskDistancePixelCenter(minX, maxPixelY, width, height, centerScale, centerHorizontalScale, centerOffsetX, centerOffsetY),
			FoveatedMaskDistancePixelCenter(maxPixelX, maxPixelY, width, height, centerScale, centerHorizontalScale, centerOffsetX, centerOffsetY) });
	}

	void SanitizeFoveatedSettings(Upscaling::Settings& settings)
	{
		settings.foveatedCenterArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
		settings.foveatedCenterHorizontalScale = ClampFoveatedCenterHorizontalScale(settings.foveatedCenterHorizontalScale);
		settings.foveatedLeftEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetX);
		settings.foveatedLeftEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetY);
		settings.foveatedRightEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetX);
		settings.foveatedRightEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetY);
		settings.periphery_taa_center_area = ClampFoveatedCenterArea(settings.periphery_taa_center_area);
		settings.periphery_taa_center_horizontal_scale = ClampFoveatedCenterHorizontalScale(settings.periphery_taa_center_horizontal_scale);
		settings.periphery_taa_left_eye_mask_offset_x = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_left_eye_mask_offset_x);
		settings.periphery_taa_left_eye_mask_offset_y = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_left_eye_mask_offset_y);
		settings.periphery_taa_right_eye_mask_offset_x = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_right_eye_mask_offset_x);
		settings.periphery_taa_right_eye_mask_offset_y = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_right_eye_mask_offset_y);
	}

	void SanitizeUpscalingSettings(Upscaling::Settings& settings)
	{
		settings.upscaleMethod = std::min<uint>(settings.upscaleMethod, static_cast<uint>(Upscaling::UpscaleMethod::kDLSS));
		settings.upscaleMethodNoDLSS = std::min<uint>(settings.upscaleMethodNoDLSS, static_cast<uint>(Upscaling::UpscaleMethod::kFSR));
		settings.qualityMode = ClampQualityModeUInt(settings.qualityMode);
		settings.dlssPreset = std::min<uint>(settings.dlssPreset, Upscaling::kDLSSPresetMaxIndex);
		settings.frameLimitMode = ClampToggleUInt(settings.frameLimitMode);
		settings.frameGenerationMode = ClampToggleUInt(settings.frameGenerationMode);
		settings.frameGenerationForceEnable = ClampToggleUInt(settings.frameGenerationForceEnable);
		settings.streamlineLogLevel = ClampStreamlineLogLevelUInt(settings.streamlineLogLevel);
		settings.sharpnessFSR = ClampFiniteUnitRange(settings.sharpnessFSR, 0.0f);
		settings.sharpnessDLSS = ClampFiniteUnitRange(settings.sharpnessDLSS, 0.1f);
		settings.periphery_taa_center_blend_feather = ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather);
		SanitizeFoveatedSettings(settings);
		settings.periphery_taa_outer_scale = ClampPeripheryTAAOuterScaleForCenter(
			settings.periphery_taa_outer_scale,
			settings.periphery_taa_center_area,
			settings.periphery_taa_center_horizontal_scale,
			settings.periphery_taa_center_blend_feather);
		if (!settings.foveatedStep1Confirmed) {
			settings.foveatedStep2Confirmed = false;
			settings.foveatedSetupVersion = 0u;
			settings.periphery_taa_enable = false;
		}
		if (!settings.foveatedStep2Confirmed)
			settings.foveatedSetupVersion = 0u;
	}

	void ResetVRSpecificUpscalingSettings(Upscaling::Settings& settings)
	{
		settings.foveatedVendorDispatch = false;
		settings.foveatedCenterArea = 0.6f;
		settings.foveatedCenterHorizontalScale = 1.0f;
		settings.foveatedLeftEyeMaskOffsetX = 0.0f;
		settings.foveatedLeftEyeMaskOffsetY = 0.0f;
		settings.foveatedRightEyeMaskOffsetX = 0.0f;
		settings.foveatedRightEyeMaskOffsetY = 0.0f;
		settings.periphery_taa_center_area = 0.6f;
		settings.periphery_taa_center_horizontal_scale = 1.0f;
		settings.periphery_taa_left_eye_mask_offset_x = 0.0f;
		settings.periphery_taa_left_eye_mask_offset_y = 0.0f;
		settings.periphery_taa_right_eye_mask_offset_x = 0.0f;
		settings.periphery_taa_right_eye_mask_offset_y = 0.0f;
		settings.foveatedPeripheryMaskVisualization = false;
		settings.periphery_taa_enable = false;
		settings.periphery_taa_outer_scale = 0.70f;
		settings.periphery_taa_center_blend_feather = FoveatedCommon::kCenterFeather;
		settings.foveatedSetupVersion = 0u;
		settings.foveatedStep1Confirmed = false;
		settings.foveatedStep2Confirmed = false;
	}

	void StripVRSpecificUpscalingSettings(json& o_json)
	{
		o_json.erase("foveatedVendorDispatch");
		o_json.erase("foveatedCenterArea");
		o_json.erase("foveatedCenterHorizontalScale");
		o_json.erase("foveatedLeftEyeMaskOffsetX");
		o_json.erase("foveatedLeftEyeMaskOffsetY");
		o_json.erase("foveatedRightEyeMaskOffsetX");
		o_json.erase("foveatedRightEyeMaskOffsetY");
		o_json.erase("periphery_taa_center_area");
		o_json.erase("periphery_taa_center_horizontal_scale");
		o_json.erase("periphery_taa_left_eye_mask_offset_x");
		o_json.erase("periphery_taa_left_eye_mask_offset_y");
		o_json.erase("periphery_taa_right_eye_mask_offset_x");
		o_json.erase("periphery_taa_right_eye_mask_offset_y");
		o_json.erase("foveatedPeripheryMaskVisualization");
		o_json.erase("periphery_taa_enable");
		o_json.erase("periphery_taa_outer_scale");
		o_json.erase("periphery_taa_center_blend_feather");
		o_json.erase("foveatedSetupVersion");
		o_json.erase("foveatedStep1Confirmed");
		o_json.erase("foveatedStep2Confirmed");
	}

	bool SupportsFoveatedVendorDispatch(Upscaling::UpscaleMethod a_upscaleMethod)
	{
		// Foveated vendor dispatch is VR-only and currently DLSS-only.
		return globals::game::isVR && a_upscaleMethod == Upscaling::UpscaleMethod::kDLSS;
	}

	bool IsVRRuntimeActive()
	{
		return globals::game::isVR;
	}

	bool TryGetTexture2DDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& outDesc)
	{
		if (!resource)
			return false;

		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;

		texture->GetDesc(&outDesc);
		return true;
	}

	eastl::unique_ptr<Texture2D> CreateNamedTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format, bool createSRV, bool createUAV, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = (createSRV ? D3D11_BIND_SHADER_RESOURCE : 0u) | (createUAV ? D3D11_BIND_UNORDERED_ACCESS : 0u);

		auto texture = eastl::make_unique<Texture2D>(desc);
		if (name) {
			Util::SetResourceName(texture->resource.get(), name);
		}

		if (createSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			texture->CreateSRV(srvDesc);
		}

		if (createUAV) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			texture->CreateUAV(uavDesc);
		}

		return texture;
	}
}

/**
 * @brief Creates a Direct3D 11 device and swap chain, with support for advanced upscaling and frame generation features.
 *
 * This function intercepts the standard D3D11 device and swap chain creation process to enable integration with Streamline and FidelityFX technologies, as well as optional D3D12 proxying for frame generation. It adjusts swap chain flags for tearing support, manages feature checks, and conditionally routes device creation through Streamline or FidelityFX proxies based on runtime settings and hardware capabilities. If frame generation is enabled and supported, a D3D12 proxy is used; otherwise, the standard D3D11 creation path is followed.
 *
 * @return HRESULT indicating the success or failure of device and swap chain creation.
 */
HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainUpscaling(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);
	globals::state->SetAdapterDescription(adapterDesc.Description);

	auto& upscaling = globals::features::upscaling;
	upscaling.LoadUpscalingSDKs();

	if (upscaling.IsBackendInitialized())
		upscaling.CheckBackendFeatures(pAdapter);

	// Use better swap effect to prevent tearing and improve performance
	pSwapChainDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	const bool isVR = REL::Module::IsVR();
	bool shouldProxy = !isVR;
	if (shouldProxy)
		if (!pSwapChainDesc->Windowed)
			shouldProxy = false;

	auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
	upscaling.refreshRate = refreshRate;

	if (shouldProxy) {
		if (upscaling.settings.frameGenerationMode)
			if (refreshRate >= 120)
				shouldProxy = true;
			else if (upscaling.settings.frameGenerationForceEnable)
				shouldProxy = true;
			else
				shouldProxy = false;
		else
			shouldProxy = false;
	}

	upscaling.lowRefreshRate = refreshRate < 120;
	upscaling.isWindowed = pSwapChainDesc->Windowed;

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

	if (shouldProxy) {
		logger::info("[Frame Generation] Frame Generation enabled, using D3D12 proxy");

		if (upscaling.HasFrameGenModule()) {
			DX::ThrowIfFailed(D3D11CreateDevice(
				pAdapter,
				DriverType,
				Software,
				Flags,
				&featureLevel,
				1,
				SDKVersion,
				ppDevice,
				pFeatureLevel,
				ppImmediateContext));

			upscaling.SetProxyD3D11Device(*ppDevice);
			upscaling.SetProxyD3D11DeviceContext(*ppImmediateContext);
			upscaling.CreateProxySwapChain(pAdapter, *pSwapChainDesc);
			upscaling.CreateProxyInterop();

			*ppSwapChain = upscaling.GetProxySwapChain();

			upscaling.d3d12SwapChainActive = true;

			if (upscaling.IsBackendInitialized()) {
				upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
				upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
				upscaling.SetBackendD3DDevice(*ppDevice);
				// Some Streamline features (notably Reflex/PCL) may not report
				// load/support status reliably until the D3D device is bound.
				upscaling.CheckBackendFeatures(pAdapter);
				upscaling.PostBackendDevice();
			}

			return S_OK;
		} else {
			logger::warn("[Frame Generation] FidelityFX DLLs are not loaded, skipping proxy");
			upscaling.fidelityFXMissing = true;
		}
	}

	auto ret = ptrD3D11CreateDeviceAndSwapChainUpscaling(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	if (upscaling.IsBackendInitialized()) {
		upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
		upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
		upscaling.SetBackendD3DDevice(*ppDevice);
		// Re-check after device bind to ensure feature availability is accurate.
		upscaling.CheckBackendFeatures(pAdapter);
		upscaling.PostBackendDevice();
	}

	return ret;
}

void Upscaling::DrawSettings()
{
	struct UpscaleUiChoice
	{
		UpscaleMethod method;
		bool useRuntimeFsr4;
		const char* label;
	};
	std::vector<UpscaleUiChoice> upscaleChoices = {
		{ UpscaleMethod::kNONE, false, "None" },
		{ UpscaleMethod::kTAA, false, "TAA" },
		{ UpscaleMethod::kFSR, false, "AMD FSR 3.1" }
	};

	const bool isAmdAdapter = fidelityFX.IsAmdAdapterDetected();
	const bool isNvidiaAdapter = fidelityFX.IsNvidiaAdapterDetected();
	const bool runtimeFsr4Present = fidelityFX.IsRuntimeUpscalerPresent();
	const bool runtimeFsr4Available = fidelityFX.IsRuntimeUpscalerAvailable();
	if (runtimeFsr4Available)
		upscaleChoices.push_back({ UpscaleMethod::kFSR, true, "AMD FSR 4" });

	const bool featureDLSS = streamline.featureDLSS;
	if (featureDLSS)
		upscaleChoices.push_back({ UpscaleMethod::kDLSS, false, "NVIDIA DLSS" });

	// Determine available modes
	uint32_t* currentUpscaleMode = &settings.upscaleMethod;
	if (!featureDLSS)
		currentUpscaleMode = &settings.upscaleMethodNoDLSS;
	const bool requestedRuntimeFsr4BeforeMethodSelection =
		*currentUpscaleMode == static_cast<uint32_t>(UpscaleMethod::kFSR) &&
		settings.fsr4RuntimeEnable;

	auto matchesCurrentChoice = [&](const UpscaleUiChoice& choice) {
		if (static_cast<uint32_t>(choice.method) != *currentUpscaleMode)
			return false;
		if (choice.method == UpscaleMethod::kFSR)
			return settings.fsr4RuntimeEnable == choice.useRuntimeFsr4;
		return true;
	};

	int methodUiIndex = 0;
	for (int i = 0; i < static_cast<int>(upscaleChoices.size()); ++i) {
		if (matchesCurrentChoice(upscaleChoices[i])) {
			methodUiIndex = i;
			break;
		}
	}
	if (methodUiIndex == 0 && !matchesCurrentChoice(upscaleChoices[0])) {
		for (int i = 0; i < static_cast<int>(upscaleChoices.size()); ++i) {
			if (static_cast<uint32_t>(upscaleChoices[i].method) == *currentUpscaleMode) {
				methodUiIndex = i;
				break;
			}
		}
	}

	const char* currentMethodLabel = upscaleChoices[methodUiIndex].label;
	ImGui::SliderInt("Method", &methodUiIndex, 0, static_cast<int>(upscaleChoices.size() - 1), currentMethodLabel);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted("Selects the upscaling backend.");
		ImGui::TextUnformatted("Range: choose between TAA, DLSS, FSR 3.1, Runtime FSR 4, or None.");
	}
	methodUiIndex = std::clamp(methodUiIndex, 0, static_cast<int>(upscaleChoices.size() - 1));
	const auto& selectedUpscaleChoice = upscaleChoices[methodUiIndex];
	*currentUpscaleMode = static_cast<uint32_t>(selectedUpscaleChoice.method);
	if (selectedUpscaleChoice.method == UpscaleMethod::kFSR)
		settings.fsr4RuntimeEnable = selectedUpscaleChoice.useRuntimeFsr4;

	// Check the current upscale method
	auto upscaleMethod = GetUpscaleMethod();

	auto drawFsr4OverrideControls = [&]() {
		if (runtimeFsr4Present && isAmdAdapter) {
			ImGui::Checkbox("Allow FSR4 on Non-RX90 AMD", &settings.fsr4AllowNonRx90Amd);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Enables Runtime FSR4 on AMD cards that are not auto-detected as RX 90.");
				ImGui::TextUnformatted("Keep this off unless your AMD card supports FSR4 and detection failed.");
			}
		}
		if (!runtimeFsr4Present && requestedRuntimeFsr4BeforeMethodSelection) {
			ImGui::TextDisabled("Runtime FSR4 unavailable: missing FidelityFX upscaler runtime.");
		}
	};

	// Display warning for DLSS resolution limits (non-VR only; VR handles this automatically)
	if (!globals::game::isVR && upscaleMethod == UpscaleMethod::kDLSS) {
		auto screenSize = globals::state->screenSize;
		if (screenSize.x > streamline.MAX_RESOLUTION || screenSize.y > streamline.MAX_RESOLUTION) {
			ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
			ImGui::Text("Warning: Requested resolution %.0f x %.0f exceeds maximum supported resolution %d x %d for DLSS.",
				screenSize.x, screenSize.y, streamline.MAX_RESOLUTION, streamline.MAX_RESOLUTION);
			ImGui::Text("DLSS will not function. Lower your resolution or select a different upscaling method.");
			ImGui::PopStyleColor();
		}
	}

	if (upscaleMethod == UpscaleMethod::kNONE || upscaleMethod == UpscaleMethod::kTAA)
		drawFsr4OverrideControls();

	// Display upscaling settings if applicable
	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		const char* upscalePresetsDLSS[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "DLAA" };
		const char* upscalePresets[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "Native AA" };

		// Compute a safe preset index (4 - qualityMode) clamped to [0,4] to avoid negative/overflow indexing
		int presetIndex = 0;
		if (settings.qualityMode <= 4)
			presetIndex = 4 - static_cast<int>(settings.qualityMode);
		presetIndex = std::clamp(presetIndex, 0, 4);

		// Choose preset name set and the corresponding scales once, then show a
		// single SliderInt to avoid duplicated calls.
		const char* baseLabel = nullptr;

		if (upscaleMethod == UpscaleMethod::kFSR) {
			baseLabel = upscalePresets[presetIndex];
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			baseLabel = upscalePresetsDLSS[presetIndex];
		}

		if (baseLabel) {
			// Format the label with preset name and resolution scale
			std::string labelWithScale = std::format("{} ( {:.2f}x )", baseLabel, (resolutionScale.x + resolutionScale.y) * 0.5f);

			ImGui::SliderInt("Upscale Preset", (int*)&settings.qualityMode, 0, 4, labelWithScale.c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Controls internal render scale / quality level.");
				ImGui::TextUnformatted("Range: low 0 (highest quality, lowest performance gain) to high 4 (highest performance gain, lowest quality).");
			}
		}

		if (upscaleMethod == UpscaleMethod::kFSR) {
			ImGui::SliderFloat("Sharpness", &settings.sharpnessFSR, 0.0f, 1.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Adjusts post-upscale sharpness for FSR.");
				ImGui::TextUnformatted("Range: low 0.0 (softest) to high 1.0 (sharpest).");
			}
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			// Keep persisted preset values stable (0=J,1=K,2=L,3=M,4=F) while
			// presenting an alphabetical selection list in the UI.
			const uint32_t dlssProfileOrder[] = { 4u, 0u, 1u, 2u, 3u };  // F, J, K, L, M
			const char* dlssProfiles[] = { "F", "J", "K", "L", "M" };
			settings.dlssPreset = std::min(settings.dlssPreset, kDLSSPresetMaxIndex);

			int dlssProfileUiIndex = 0;
			for (int i = 0; i < IM_ARRAYSIZE(dlssProfileOrder); ++i) {
				if (dlssProfileOrder[i] == settings.dlssPreset) {
					dlssProfileUiIndex = i;
					break;
				}
			}

			ImGui::SliderInt("DLSS Profile", &dlssProfileUiIndex, 0, static_cast<int>(kDLSSPresetMaxIndex), dlssProfiles[dlssProfileUiIndex]);
			dlssProfileUiIndex = std::clamp(dlssProfileUiIndex, 0, static_cast<int>(kDLSSPresetMaxIndex));
			settings.dlssPreset = dlssProfileOrder[dlssProfileUiIndex];

			if (auto _tt = Util::HoverTooltipWrapper()) {
				switch (settings.dlssPreset) {
				case 0:
					ImGui::Text("DLAA/Quality/Balanced preset. Slightly less ghosting than K, but more flicker. Speed: ~K. Use only if K ghosts.");
					break;
				case 1:
					ImGui::Text("Default for DLAA/Quality/Balanced. Best all-round stability and image quality. Speed: fast. Recommended for most users.");
					break;
				case 2:
					ImGui::Text("Default for Ultra Performance on newer RTX cards. Sharper and more stable, but higher cost than J/K/F.");
					ImGui::Text("For RTX 3000-series cards, F is usually the better Performance/Ultra Performance choice.");
					break;
				case 3:
					ImGui::Text("Default for Performance on newer RTX cards. Similar image-quality improvements to L, closer in speed to J/K.");
					ImGui::Text("For RTX 3000-series cards, F is usually the better Performance/Ultra Performance choice.");
					break;
				case 4:
					ImGui::Text("Intended for Ultra Performance/DLAA. Default preset for Ultra Performance.");
					ImGui::Text("Best Performance/Ultra Performance choice for RTX 3000-series cards.");
					break;
				default:
					ImGui::Text("Default for DLAA/Quality/Balanced. Best all-round stability and image quality. Speed: fast. Recommended for most users.");
					break;
				}
			}

			ImGui::SliderFloat("Sharpness", &settings.sharpnessDLSS, 0.0f, 1.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Adjusts post-upscale sharpness for DLSS.");
				ImGui::TextUnformatted("Range: low 0.0 (softest) to high 1.0 (sharpest).");
			}

			if (isNvidiaAdapter) {
				ImGui::TextWrapped("Note: Use K for DLAA/Quality/Balanced. For Performance and Ultra Performance, use L/M on newer RTX cards and F on RTX 3000-series cards.");
			}
		}

		drawFsr4OverrideControls();

		if (globals::game::isVR) {
			const bool foveatedDispatchSupportedForMethod = SupportsFoveatedVendorDispatch(upscaleMethod);
			if (!foveatedDispatchSupportedForMethod) {
				ImGui::TextDisabled("Foveated Upscaling is currently DLSS-only. FSR uses full-eye dispatch.");
			}
			ImGui::BeginDisabled(!foveatedDispatchSupportedForMethod);
			{
				Util::BlueFrameStyleWrapper foveatedStyle(true);
				ImGui::Checkbox("Foveated Upscaling", &settings.foveatedVendorDispatch);
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Master switch for VR FOV-mask upscaling.");
				ImGui::TextUnformatted("On: enables Step 1 and Step 2 FOV mask controls.");
				ImGui::TextUnformatted("Off: uses full-eye upscaler dispatch (no FOV masks).");
				ImGui::TextUnformatted("Normal foveated runtime is unlocked only after required Step 1 + Step 2 confirmation.");
			}
			SanitizeFoveatedSettings(settings);
			if (settings.foveatedVendorDispatch && foveatedDispatchSupportedForMethod) {
				{
					Util::BlueFrameStyleWrapper maskStyle(true);
					ImGui::Checkbox("FOV Mask Visualization (required during setup)", &settings.foveatedPeripheryMaskVisualization);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Use this while tuning FOV masks.");
					ImGui::TextUnformatted("Green = DLSS center mask.");
					ImGui::TextUnformatted("With Peripheral TAA enabled: gold = TAA ring, blue = outer lightweight ring.");
					ImGui::TextUnformatted("With Peripheral TAA disabled: only the green DLSS center mask is shown.");
					ImGui::TextUnformatted("Enable this to calibrate Step 1 and Step 2 masks.");
					ImGui::TextUnformatted("Saved values from older versions do not bypass setup.");
					ImGui::TextUnformatted("Step 2 confirmation closes visualization automatically.");
				}

				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0.0f, 4.0f));
				ImGui::TextColored(ImVec4(0.80f, 0.88f, 1.00f, 1.0f), "FOV Setup Order (Required)");
				ImGui::TextUnformatted("0) Turn ON FOV Mask Visualization.");
				ImGui::TextUnformatted("1) Step 1: Configure the DLSS-only FOV core region (non-TAA path).");
				ImGui::TextUnformatted("2) Step 2: Configure Peripheral TAA extension outside that core.");
				ImGui::TextUnformatted("Flow: visualization ON -> Step 1 confirm -> Step 2 confirm (auto-closes visualization).");
				ImGui::TextUnformatted("Requirement for this version: Step 1 and Step 2 must both be confirmed.");
				const bool setupComplete = IsFoveatedSetupComplete(settings);
				if (!setupComplete) {
					ImGui::TextColored(ImVec4(0.95f, 0.76f, 0.33f, 1.0f), "Required setup: complete and confirm both Step 1 and Step 2 for this version.");
					if (!settings.foveatedPeripheryMaskVisualization)
						ImGui::TextDisabled("Start setup by enabling FOV Mask Visualization.");
					ImGui::TextDisabled("Normal foveated runtime stays disabled until setup is confirmed.");
				}

				bool step1Edited = false;
				ImGui::Dummy(ImVec2(0.0f, 4.0f));
				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.66f, 0.79f, 1.00f, 1.0f), "Step 1: DLSS FOV Core (Non-TAA)");
				ImGui::TextDisabled("Defines the DLSS-only core region used when Peripheral TAA is OFF.");
				ImGui::TextDisabled("Step 2 adds Peripheral TAA around this core.");
				ImGui::Dummy(ImVec2(0.0f, 2.0f));

				ImGui::BeginDisabled(settings.periphery_taa_enable);
				{
					Util::BlueFrameStyleWrapper foveatedAreaStyle;
					step1Edited |= ImGui::SliderFloat("DLSS Core FOV Area", &settings.foveatedCenterArea, FoveatedCommon::kCenterAreaMin, FoveatedCommon::kCenterAreaMax, "%.2f");
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Step 1 core size for DLSS-only (non-TAA) rendering.");
					ImGui::TextUnformatted("This mask is used by non-Peripheral-TAA path and by SSGI foveation.");
					ImGui::TextUnformatted("Lower values = smaller center mask and more performance.");
					ImGui::TextUnformatted("Range: low 0.30 (smallest center) to high 1.00 (largest center).");
					ImGui::TextUnformatted("Tune until the green center area touches the end of your visible field of view.");
				}
				{
					Util::BlueFrameStyleWrapper expandStyle;
					step1Edited |= ImGui::SliderFloat("DLSS Core Expand R/L", &settings.foveatedCenterHorizontalScale, FoveatedCommon::kCenterHorizontalScaleMin, FoveatedCommon::kCenterHorizontalScaleMax, "%.2f");
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Step 1 widens the DLSS-only core horizontally (left/right) without changing vertical size.");
					ImGui::TextUnformatted("Range: low 1.00 (no extra width) to high 2.00 (maximum extra width).");
				}
				settings.foveatedCenterArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
				settings.foveatedCenterHorizontalScale = ClampFoveatedCenterHorizontalScale(settings.foveatedCenterHorizontalScale);

				{
					Util::BlueFrameStyleWrapper maskSliderStyle;
					step1Edited |= ImGui::SliderFloat("DLSS Left Eye Offset X", &settings.foveatedLeftEyeMaskOffsetX, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Step 1 left-eye horizontal offset.");
						ImGui::TextUnformatted("+X moves right, -X moves left.");
					}
					step1Edited |= ImGui::SliderFloat("DLSS Left Eye Offset Y", &settings.foveatedLeftEyeMaskOffsetY, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Step 1 left-eye vertical offset.");
						ImGui::TextUnformatted("+Y moves down, -Y moves up.");
					}
					step1Edited |= ImGui::SliderFloat("DLSS Right Eye Offset X", &settings.foveatedRightEyeMaskOffsetX, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Step 1 right-eye horizontal offset.");
						ImGui::TextUnformatted("+X moves right, -X moves left.");
					}
					step1Edited |= ImGui::SliderFloat("DLSS Right Eye Offset Y", &settings.foveatedRightEyeMaskOffsetY, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Step 1 right-eye vertical offset.");
						ImGui::TextUnformatted("+Y moves down, -Y moves up.");
					}
					settings.foveatedLeftEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetX);
					settings.foveatedLeftEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetY);
					settings.foveatedRightEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetX);
					settings.foveatedRightEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetY);
				}
				if (step1Edited) {
					settings.foveatedStep1Confirmed = false;
					settings.foveatedStep2Confirmed = false;
					settings.foveatedSetupVersion = 0u;
					settings.periphery_taa_enable = false;
				}

				{
					Util::StyledButtonWrapper step1ConfirmButtonStyle(
						ImVec4(0.10f, 0.20f, 0.45f, 0.85f),
						ImVec4(0.14f, 0.28f, 0.58f, 0.90f),
						ImVec4(0.18f, 0.34f, 0.66f, 0.95f));
					if (ImGui::Button("Confirm DLSS FOV (Step 1 Complete)")) {
						settings.foveatedStep1Confirmed = true;
						settings.foveatedStep2Confirmed = false;
						settings.foveatedSetupVersion = 0u;
						settings.periphery_taa_enable = false;
						if (globals::state)
							globals::state->Save();
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Marks Step 1 complete for required setup and unlocks Step 2 controls.");
						ImGui::TextUnformatted("Changing Step 1 values later clears Step 2 confirmation.");
					}
				}
				ImGui::EndDisabled();

				if (settings.periphery_taa_enable)
					ImGui::TextDisabled("Step 1 DLSS FOV is locked while Peripheral TAA is enabled. Disable Peripheral TAA in Step 2 to edit Step 1.");

				if (!settings.foveatedStep1Confirmed)
					ImGui::TextDisabled("Complete and confirm Step 1 to unlock Step 2.");

				ImGui::BeginDisabled(!settings.foveatedStep1Confirmed);
				ImGui::Dummy(ImVec2(0.0f, 4.0f));
				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.96f, 0.82f, 0.40f, 1.0f), "Step 2: DLSS FOV + Peripheral TAA");
				ImGui::TextDisabled("Extends coverage beyond the Step 1 DLSS core using Peripheral TAA.");
				ImGui::TextDisabled("Confirm Step 2 to complete required setup.");
				bool step2Edited = false;
				{
					{
						Util::YellowFrameStyleWrapper taaStyle;
						ImGui::Checkbox("Peripheral TAA", &settings.periphery_taa_enable);
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::TextUnformatted("Enables periphery-only TAA around the Step 2 DLSS center mask.");
							ImGui::TextUnformatted("When ON, Step 1 controls are locked until this is turned OFF.");
							ImGui::TextUnformatted("This must be ON to confirm Step 2 setup.");
						}
					}
					{
						Util::StyledButtonWrapper step2CopyButtonStyle(
							ImVec4(0.46f, 0.34f, 0.08f, 0.85f),
							ImVec4(0.58f, 0.43f, 0.10f, 0.90f),
							ImVec4(0.68f, 0.51f, 0.12f, 0.95f));
						if (ImGui::Button("Copy Mask Positions From Step 1")) {
							step2Edited = true;
							settings.periphery_taa_left_eye_mask_offset_x = settings.foveatedLeftEyeMaskOffsetX;
							settings.periphery_taa_left_eye_mask_offset_y = settings.foveatedLeftEyeMaskOffsetY;
							settings.periphery_taa_right_eye_mask_offset_x = settings.foveatedRightEyeMaskOffsetX;
							settings.periphery_taa_right_eye_mask_offset_y = settings.foveatedRightEyeMaskOffsetY;
							settings.periphery_taa_center_area = FoveatedCommon::kCenterAreaMin;
							settings.periphery_taa_center_horizontal_scale = 1.0f;
						}
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Copies only left/right eye X/Y mask offsets from Step 1.");
						ImGui::TextUnformatted("Step 2 DLSS FOV Area is reset to 0.30 and DLSS Expand FOV Area R/L is reset to 1.00.");
						ImGui::TextUnformatted("Does not enable Peripheral TAA automatically.");
						ImGui::TextUnformatted("You must still tune Step 2 and confirm it.");
					}
					{
						Util::YellowFrameStyleWrapper step2DlssAreaStyle;
						step2Edited |= ImGui::SliderFloat("DLSS FOV Area##Step2", &settings.periphery_taa_center_area, FoveatedCommon::kCenterAreaMin, FoveatedCommon::kCenterAreaMax, "%.2f");
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Step 2: size of the DLSS center mask used before the TAA ring.");
						ImGui::TextUnformatted("Lower values = smaller DLSS FOV area and more performance.");
						ImGui::TextUnformatted("Range: low 0.30 (smallest DLSS FOV area) to high 1.00 (largest DLSS FOV area).");
						ImGui::TextUnformatted("Set this first, then expand TAA Peripheral Range until the gold ring reaches your visible FOV edge.");
					}
					{
						Util::YellowFrameStyleWrapper step2DlssExpandStyle;
						step2Edited |= ImGui::SliderFloat("DLSS Expand FOV Area R/L##Step2", &settings.periphery_taa_center_horizontal_scale, FoveatedCommon::kCenterHorizontalScaleMin, FoveatedCommon::kCenterHorizontalScaleMax, "%.2f");
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Step 2: widens the DLSS center mask horizontally (left/right) without changing vertical size.");
						ImGui::TextUnformatted("Range: low 1.00 (no extra width) to high 2.00 (maximum extra width).");
					}
					settings.periphery_taa_center_area = ClampFoveatedCenterArea(settings.periphery_taa_center_area);
					settings.periphery_taa_center_horizontal_scale = ClampFoveatedCenterHorizontalScale(settings.periphery_taa_center_horizontal_scale);
					{
						Util::YellowFrameStyleWrapper transitionStyle;
						step2Edited |= ImGui::SliderFloat(
							"Center Blend/TAA Transition",
							&settings.periphery_taa_center_blend_feather,
							kPeripheryTAACenterBlendFeatherMin,
							kPeripheryTAACenterBlendFeatherMax,
							"%.3f");
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Controls softness of the center-to-TAA transition edge.");
						ImGui::TextUnformatted("Lower = harder edge, higher = softer edge.");
						ImGui::Text("Range: low %.2f (harder transition) to high %.2f (softer transition).", kPeripheryTAACenterBlendFeatherMin, kPeripheryTAACenterBlendFeatherMax);
					}
					settings.periphery_taa_center_blend_feather = ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather);
					const float taaOuterRangeMin = GetPeripheryTAAOuterScaleFloor(
						settings.periphery_taa_center_area,
						settings.periphery_taa_center_horizontal_scale,
						settings.periphery_taa_center_blend_feather);
					{
						Util::YellowFrameStyleWrapper taaRangeStyle;
						step2Edited |= ImGui::SliderFloat(
							"TAA Peripheral Range",
							&settings.periphery_taa_outer_scale,
							taaOuterRangeMin,
							kPeripheryTAAOuterScaleMax,
							"%.2f");
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Controls how far Peripheral TAA extends outside the Step 2 DLSS FOV.");
						ImGui::Text("Range: low %.2f (minimum allowed by current DLSS FOV Area) to high %.2f (full range).", taaOuterRangeMin, kPeripheryTAAOuterScaleMax);
						ImGui::TextUnformatted("Lower values are faster.");
						ImGui::TextUnformatted("Increase until the gold ring reaches the edge of your visible field of view.");
					}
					{
						Util::YellowFrameStyleWrapper step2DlssOffsetStyle;
						step2Edited |= ImGui::SliderFloat("DLSS Left Eye Offset X##Step2", &settings.periphery_taa_left_eye_mask_offset_x, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::TextUnformatted("Step 2 left-eye horizontal offset for both DLSS center and TAA ring.");
							ImGui::TextUnformatted("+X moves right, -X moves left.");
						}
						step2Edited |= ImGui::SliderFloat("DLSS Left Eye Offset Y##Step2", &settings.periphery_taa_left_eye_mask_offset_y, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::TextUnformatted("Step 2 left-eye vertical offset for both DLSS center and TAA ring.");
							ImGui::TextUnformatted("+Y moves down, -Y moves up.");
						}
						step2Edited |= ImGui::SliderFloat("DLSS Right Eye Offset X##Step2", &settings.periphery_taa_right_eye_mask_offset_x, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::TextUnformatted("Step 2 right-eye horizontal offset for both DLSS center and TAA ring.");
							ImGui::TextUnformatted("+X moves right, -X moves left.");
						}
						step2Edited |= ImGui::SliderFloat("DLSS Right Eye Offset Y##Step2", &settings.periphery_taa_right_eye_mask_offset_y, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::TextUnformatted("Step 2 right-eye vertical offset for both DLSS center and TAA ring.");
							ImGui::TextUnformatted("+Y moves down, -Y moves up.");
						}
					}
					settings.periphery_taa_outer_scale = ClampPeripheryTAAOuterScaleForCenter(
						settings.periphery_taa_outer_scale,
						settings.periphery_taa_center_area,
						settings.periphery_taa_center_horizontal_scale,
						settings.periphery_taa_center_blend_feather);
					settings.periphery_taa_left_eye_mask_offset_x = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_left_eye_mask_offset_x);
					settings.periphery_taa_left_eye_mask_offset_y = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_left_eye_mask_offset_y);
					settings.periphery_taa_right_eye_mask_offset_x = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_right_eye_mask_offset_x);
					settings.periphery_taa_right_eye_mask_offset_y = ClampFoveatedMaskOffsetAdjustment(settings.periphery_taa_right_eye_mask_offset_y);
				}
				if (step2Edited) {
					settings.foveatedStep2Confirmed = false;
					settings.foveatedSetupVersion = 0u;
				}
				if (!settings.periphery_taa_enable)
					ImGui::TextDisabled("Enable Peripheral TAA to confirm Step 2 and finish required setup.");
				{
					Util::StyledButtonWrapper step2ConfirmButtonStyle(
						ImVec4(0.46f, 0.34f, 0.08f, 0.85f),
						ImVec4(0.58f, 0.43f, 0.10f, 0.90f),
						ImVec4(0.68f, 0.51f, 0.12f, 0.95f));
					ImGui::BeginDisabled(!settings.periphery_taa_enable);
					if (ImGui::Button("Confirm Peripheral TAA (Step 2 Complete)")) {
						settings.foveatedStep2Confirmed = true;
						settings.foveatedSetupVersion = kRequiredFoveatedSetupVersion;
						settings.foveatedPeripheryMaskVisualization = false;
						if (globals::state)
							globals::state->Save();
					}
					ImGui::EndDisabled();
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Marks Step 2 complete for this setup version and saves settings.");
						ImGui::TextUnformatted("Closes visualization and saves all FOV setup values to your profile.");
					}
				}
				if (!IsFoveatedSetupComplete(settings))
					ImGui::TextDisabled("Setup incomplete: runtime remains in setup mode until Step 1 and Step 2 are both confirmed.");
				ImGui::EndDisabled();
			}
			ImGui::EndDisabled();

			if (!foveatedDispatchSupportedForMethod) {
				ImGui::Separator();
				ImGui::TextUnformatted("SSGI Base FOV (Fallback)");
				ImGui::TextDisabled("DLSS foveated upscaling is unavailable on this backend.");
				ImGui::TextDisabled("These controls still drive SSGI foveated presets.");

				{
					Util::BlueFrameStyleWrapper ssgiBaseAreaStyle;
					ImGui::SliderFloat("SSGI Base FOV Area", &settings.foveatedCenterArea, FoveatedCommon::kCenterAreaMin, FoveatedCommon::kCenterAreaMax, "%.2f");
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Controls the SSGI center mask size used by SSGI foveated presets.");
					ImGui::TextUnformatted("Lower values = smaller center and more performance.");
					ImGui::TextUnformatted("Range: low 0.30 (smallest center) to high 1.00 (largest center).");
				}

				{
					Util::YellowFrameStyleWrapper ssgiBaseExpandStyle;
					ImGui::SliderFloat("SSGI Base Expand FOV Area R/L", &settings.foveatedCenterHorizontalScale, FoveatedCommon::kCenterHorizontalScaleMin, FoveatedCommon::kCenterHorizontalScaleMax, "%.2f");
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Widens SSGI center mask horizontally (left/right).");
					ImGui::TextUnformatted("Range: low 1.00 (no extra width) to high 2.00 (maximum extra width).");
				}

				{
					Util::YellowFrameStyleWrapper ssgiBaseOffsetStyle;
					ImGui::SliderFloat("SSGI Base Left Eye Offset X", &settings.foveatedLeftEyeMaskOffsetX, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Moves SSGI center mask horizontally for the left eye.");
					}
					ImGui::SliderFloat("SSGI Base Left Eye Offset Y", &settings.foveatedLeftEyeMaskOffsetY, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Moves SSGI center mask vertically for the left eye.");
					}
					ImGui::SliderFloat("SSGI Base Right Eye Offset X", &settings.foveatedRightEyeMaskOffsetX, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Moves SSGI center mask horizontally for the right eye.");
					}
					ImGui::SliderFloat("SSGI Base Right Eye Offset Y", &settings.foveatedRightEyeMaskOffsetY, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Moves SSGI center mask vertically for the right eye.");
					}
				}

				settings.foveatedCenterArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
				settings.foveatedCenterHorizontalScale = ClampFoveatedCenterHorizontalScale(settings.foveatedCenterHorizontalScale);
				settings.foveatedLeftEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetX);
				settings.foveatedLeftEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetY);
				settings.foveatedRightEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetX);
				settings.foveatedRightEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetY);
			}

			if (streamline.reflexSupportedOnCurrentAdapter)
				ImGui::Separator();
		}
	}

	const bool frameGenerationDx12PathActive = IsFrameGenerationDx12PathActive();

	if (!globals::game::isVR) {
		if (ImGui::TreeNodeEx("Frame Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Frame Generation interpolates real frames with generated ones for a smoother experience");
			ImGui::Text("Uses AMD FSR Frame Generation technology");
			if (HasFrameGenModule())
				ImGui::Text("AMD FSR Frame Generation is available.");
			ImGui::Text("Requires a D3D11 to D3D12 proxy which can create compatibility issues");
			ImGui::Text("Toggling this setting requires a restart to work correctly");

			bool onlyRequiresRestart = true;

			if (!isWindowed) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires windowed mode");
				ImGui::PopStyleColor();

				onlyRequiresRestart = false;
			}

			if (lowRefreshRate && !settings.frameGenerationForceEnable) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires a high refresh rate monitor or Force Enable Frame Generation");
				ImGui::PopStyleColor();

				onlyRequiresRestart = false;
			}

			if (fidelityFXMissing) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: FidelityFX DLLs are not loaded");
				ImGui::PopStyleColor();

				onlyRequiresRestart = false;
			}

			if (onlyRequiresRestart && settings.frameGenerationMode && !frameGenerationDx12PathActive) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires restart");
				ImGui::PopStyleColor();
			}

			if (!settings.frameGenerationMode && frameGenerationDx12PathActive) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires restart");
				ImGui::PopStyleColor();
			}

			std::string enabledLabel = "Enabled";
			const char* toggleModes[] = { "Disabled", "Enabled" };
			const char* toggleModesFG[] = { "Disabled", enabledLabel.c_str() };

			ImGui::SliderInt("Frame Generation", (int*)&settings.frameGenerationMode, 0, 1, toggleModesFG[settings.frameGenerationMode]);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Enables generated intermediate frames for higher apparent framerate.");
				ImGui::TextUnformatted("Range: 0 Disabled, 1 Enabled.");
			}

			if (!frameGenerationDx12PathActive)
				ImGui::BeginDisabled();

			ImGui::SliderInt("Frame Limit (Variable Refresh Rate)", (int*)&settings.frameLimitMode, 0, 1, std::format("{}", toggleModes[settings.frameLimitMode]).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Applies VRR-aware frame limiting for smoother pacing with Frame Generation.");
				ImGui::TextUnformatted("Range: 0 Disabled, 1 Enabled.");
			}

			if (!frameGenerationDx12PathActive)
				ImGui::EndDisabled();

			ImGui::Text("Allows frame generation to function on low refresh rate monitors");
			ImGui::SliderInt("Force Enable Frame Generation", (int*)&settings.frameGenerationForceEnable, 0, 1, std::format("{}", toggleModes[settings.frameGenerationForceEnable]).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Forces Frame Generation on unsupported/low-refresh setups.");
				ImGui::TextUnformatted("Range: 0 Disabled, 1 Enabled.");
			}

			ImGui::TreePop();
		}
	}

	if (streamline.reflexSupportedOnCurrentAdapter && ImGui::TreeNodeEx("NVIDIA Reflex", ImGuiTreeNodeFlags_DefaultOpen)) {
		const bool reflexAvailable = streamline.initialized && streamline.featureReflex;
		const bool markerOptimizationAvailable = reflexAvailable && streamline.featurePCL;
		const bool reflexBlockedByFrameGeneration = IsFrameGenerationDx12PathActive();
		const char* toggleModes[] = { "Disabled", "Enabled" };

		if (!reflexAvailable) {
			ImGui::TextDisabled("Reflex is not available. Ensure sl.reflex.dll is present and restart.");
		}

		if (reflexBlockedByFrameGeneration) {
			ImGui::TextDisabled("Reflex is disabled while Frame Generation is active on the DX12 swap chain.");
		}

		if (!reflexAvailable || reflexBlockedByFrameGeneration)
			ImGui::BeginDisabled();

		int lowLatencyMode = settings.reflexLowLatencyMode ? 1 : 0;
		ImGui::SliderInt("Low Latency Mode", &lowLatencyMode, 0, 1, toggleModes[lowLatencyMode]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Cuts input delay by syncing CPU work closer to the GPU.");
			ImGui::TextUnformatted("May reduce max FPS a little, but usually feels much more responsive.");
		}
		settings.reflexLowLatencyMode = lowLatencyMode > 0;

		if (!settings.reflexLowLatencyMode)
			ImGui::BeginDisabled();

		int lowLatencyBoost = settings.reflexLowLatencyBoost ? 1 : 0;
		ImGui::SliderInt("Low Latency Boost", &lowLatencyBoost, 0, 1, toggleModes[lowLatencyBoost]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Keeps GPU clocks higher to avoid latency spikes at low GPU load.");
			ImGui::TextUnformatted("Useful if frametime jumps and responsiveness feels inconsistent.");
			ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Increases power draw and heat, so leave Off unless needed.");
		}
		settings.reflexLowLatencyBoost = lowLatencyBoost > 0;

		if (!settings.reflexLowLatencyMode)
			ImGui::EndDisabled();

		if (!markerOptimizationAvailable)
			ImGui::BeginDisabled();

		int markersToOptimize = settings.reflexUseMarkersToOptimize ? 1 : 0;
		ImGui::SliderInt("Use Markers To Optimize", &markersToOptimize, 0, 1, toggleModes[markersToOptimize]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Uses frame markers for tighter Reflex timing.");
			ImGui::TextUnformatted("Try On first; turn Off if it causes stutter on your setup.");
		}
		settings.reflexUseMarkersToOptimize = markersToOptimize > 0;

		if (!markerOptimizationAvailable)
			ImGui::EndDisabled();

		if (!markerOptimizationAvailable) {
			ImGui::TextDisabled("Marker optimization unavailable (PCL not loaded).");
		}

		int useFPSLimit = settings.reflexUseFPSLimit ? 1 : 0;
		ImGui::SliderInt("Use FPS Limit", &useFPSLimit, 0, 1, toggleModes[useFPSLimit]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Uses Reflex's internal FPS cap for steadier frametimes.");
			ImGui::TextUnformatted("Can lower latency versus uncapped rendering.");
		}
		settings.reflexUseFPSLimit = useFPSLimit > 0;

		if (!settings.reflexUseFPSLimit)
			ImGui::BeginDisabled();

		if (!std::isfinite(settings.reflexFPSLimit))
			settings.reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
		ImGui::SliderFloat("FPS Limit", &settings.reflexFPSLimit, 20.0f, 240.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Set your frame cap target.");
			ImGui::TextUnformatted("Start about 2-3 FPS below refresh rate (e.g. 117 for 120 Hz).");
		}

		if (!settings.reflexUseFPSLimit)
			ImGui::EndDisabled();

		if (!reflexAvailable || reflexBlockedByFrameGeneration)
			ImGui::EndDisabled();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Backend Diagnostics")) {
		// Streamline log level selection
		const char* logLevels[] = { "Off", "Default", "Verbose" };
		int logLevelIdx = static_cast<int>(settings.streamlineLogLevel);
		if (ImGui::Combo("Streamline Logging", &logLevelIdx, logLevels, IM_ARRAYSIZE(logLevels))) {
			settings.streamlineLogLevel = static_cast<uint>(logLevelIdx);
		}
		ImGui::TextUnformatted("Changing this requires a restart to take effect.");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Streamline logging controls the verbosity of NVIDIA Streamline backend logs. Useful for debugging issues with DLSS/DLSS-G.");
		}

		// VR Debug visualization -- per-eye buffers and native inputs
		if (globals::game::isVR) {
			ImGui::Separator();
			static float debugRescale = 0.15f;
			ImGui::SliderFloat("View Resize", &debugRescale, 0.05f, 1.f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Scales debug buffer previews in the diagnostics panel.");
				ImGui::TextUnformatted("Range: low 0.05 (small previews) to high 1.00 (full-size previews).");
			}

			if (ImGui::TreeNode("Upscaling Intermediates")) {
				if (vrIntermediateColorIn[0] && vrIntermediateColorOut[0]) {
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[0], "Left Eye In", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[1], "Right Eye In", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[0], "Left Eye Out", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[1], "Right Eye Out", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[0], "Left Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[1], "Right Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[0], "Left Eye Reactive", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[1], "Right Eye Reactive", debugRescale)
				} else {
					ImGui::TextDisabled("VR intermediates not yet created (enter game world)");
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Native Inputs")) {
				auto renderer = globals::game::renderer;
				auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				auto& mvec = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

				auto DisplayRT = [&](const char* label, ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv) {
					if (srv && tex) {
						D3D11_TEXTURE2D_DESC desc;
						tex->GetDesc(&desc);
						char buf[128];
						snprintf(buf, sizeof(buf), "%s (%ux%u)", label, desc.Width, desc.Height);
						if (ImGui::TreeNode(buf)) {
							ImGui::Image(srv, { desc.Width * debugRescale, desc.Height * debugRescale });
							ImGui::TreePop();
						}
					}
				};

				DisplayRT("kMAIN (Color Input)", (ID3D11Texture2D*)main.texture, (ID3D11ShaderResourceView*)main.SRV);
				DisplayRT("Motion Vectors", (ID3D11Texture2D*)mvec.texture, (ID3D11ShaderResourceView*)mvec.SRV);
				DisplayRT("Depth", depth.texture, depth.depthSRV);

				if (reactiveMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(reactiveMaskTexture, "Reactive Mask", debugRescale)
				if (transparencyCompositionMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(transparencyCompositionMaskTexture, "Transparency Mask", debugRescale)

				ImGui::TreePop();
			}
		}

		ImGui::Separator();
		// FidelityFX section
		if (ImGui::Selectable("AMD FidelityFX DLLs (click to open folder)")) {
			ShellExecuteW(nullptr, L"open", FidelityFX::PluginDir, nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::string> headers = { "DLL Name", "Version" };
		std::vector<std::vector<std::string>> ffRows;
		for (const auto& [name, dllVersion] : FidelityFX::dllVersions)
			ffRows.push_back({ name, dllVersion });
		std::vector<Util::TableSortFunc> ffSorters = { nullptr, Util::VersionSortComparator };
		Util::ShowSortedStringTableStrings(
			"ffx_dll_versions",
			headers,
			ffRows,
			0,
			true,
			ffSorters);

		// Streamline section
		if (ImGui::Selectable("NVIDIA Streamline DLLs (click to open folder)")) {
			ShellExecuteW(nullptr, L"open", Streamline::PluginDir, nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::vector<std::string>> slRows;
		for (const auto& [name, dllVersion] : Streamline::dllVersions)
			slRows.push_back({ name, dllVersion });
		std::vector<Util::TableSortFunc> slSorters = { nullptr, Util::VersionSortComparator };
		Util::ShowSortedStringTableStrings(
			"sl_dll_versions",
			headers,
			slRows,
			0,
			true,
			slSorters);
		ImGui::TreePop();
	}
}

void Upscaling::SaveSettings(json& o_json)
{
	SanitizeUpscalingSettings(settings);
	o_json = settings;
	if (!IsVRRuntimeActive()) {
		StripVRSpecificUpscalingSettings(o_json);
	}
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		if (auto setting = iniSettingCollection->GetSetting("bUseTAA:Display"))
			iniSettingCollection->WriteSetting(setting);
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	settings = o_json;
	if (!IsVRRuntimeActive()) {
		ResetVRSpecificUpscalingSettings(settings);
	} else {
		// Force first-time setup on configs from older versions and keep setup state coherent.
		if (settings.foveatedSetupVersion < kRequiredFoveatedSetupVersion) {
			settings.foveatedSetupVersion = 0u;
			settings.foveatedStep1Confirmed = false;
			settings.foveatedStep2Confirmed = false;
			settings.periphery_taa_enable = false;
			settings.foveatedPeripheryMaskVisualization = false;
		}
		if (!settings.foveatedStep1Confirmed) {
			settings.foveatedStep2Confirmed = false;
			settings.foveatedSetupVersion = 0u;
			settings.periphery_taa_enable = false;
		}
		if (!settings.foveatedStep2Confirmed) {
			settings.foveatedSetupVersion = 0u;
		}
	}

	if (settings.upscaleMethod > static_cast<uint>(UpscaleMethod::kDLSS)) {
		logger::warn("[Upscaling] Loaded upscaleMethod {} out of range, clamping to {}", settings.upscaleMethod, static_cast<uint>(UpscaleMethod::kDLSS));
	}
	if (settings.upscaleMethodNoDLSS > static_cast<uint>(UpscaleMethod::kFSR)) {
		logger::warn("[Upscaling] Loaded upscaleMethodNoDLSS {} out of range, clamping to {}", settings.upscaleMethodNoDLSS, static_cast<uint>(UpscaleMethod::kFSR));
	}
	SanitizeUpscalingSettings(settings);
	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	if (!std::isfinite(settings.reflexFPSLimit)) {
		settings.reflexFPSLimit = 60.0f;
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} is not finite, resetting to {}",
			originalReflexFPSLimit,
			settings.reflexFPSLimit);
	}
	const float clampedReflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
	if (clampedReflexFPSLimit != settings.reflexFPSLimit) {
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} out of range, clamping to {}",
			settings.reflexFPSLimit,
			clampedReflexFPSLimit);
	}
	settings.reflexFPSLimit = clampedReflexFPSLimit;
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		if (auto setting = iniSettingCollection->GetSetting("bUseTAA:Display"))
			iniSettingCollection->ReadSetting(setting);
	}
}

void Upscaling::RestoreDefaultSettings()
{
	settings = {};
	SanitizeUpscalingSettings(settings);
}

void Upscaling::DataLoaded()
{
	// Fix screenshots fix from Engine Fixes
	RE::GetINISetting("bUseTAA:Display")->data.b = false;

	// The game defaults this to a non-zero value
	static auto fDRClampOffset = RE::GetINISetting("fDRClampOffset:Display");
	fDRClampOffset->data.f = 0.0f;
}

void Upscaling::Load()
{
	*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChainUpscaling = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainUpscaling, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
}

struct BSImageSpace_Init_FXAA
{
	static void thunk()
	{
		func();

		// Force FXAA off safely
		auto fxaaEnabled = reinterpret_cast<bool*>(REL::RelocationID(513281, 391028).address());
		*fxaaEnabled = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};
void Upscaling::PostPostLoad()
{
	bool isGOG = !GetModuleHandle(L"steam_api64.dll");
	stl::detour_thunk<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084));

	// Calculates resolution and jitter
	stl::write_thunk_call<Main_UpdateJitter>(REL::RelocationID(75460, 77245).address() + REL::Relocate(0xE5, isGOG ? 0x133 : 0xE2, 0x104));

	// Disables the original dynamic resolution system
	REL::safe_write(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2D, 0x2D, 0x25), REL::NOP5, sizeof(REL::NOP5));

	// Performs upscaling in between volumetric lighting and post processing
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7, 0x206));

	// Patches RSSetScissorRect calls to use dynamic resolution
	// This is a PC-specific function hence it was missing
	if (!globals::game::isVR)
		stl::detour_thunk<SetScissorRect>(REL::RelocationID(75564, 77365));

	// Patches facegen texture generation to not use dynamic resolution
	stl::detour_thunk<BSFaceGenManager_UpdatePendingCustomizationTextures>(REL::RelocationID(26455, 27041));

	// Patches precipitation camera to not use dynamic resolution
	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));

	// Forces FXAA off
	stl::detour_thunk<BSImageSpace_Init_FXAA>(REL::RelocationID(98974, 105626));

	logger::info("[Upscaling] Installed hooks");
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod() const
{
	if (streamline.featureDLSS)
		return (UpscaleMethod)settings.upscaleMethod;
	return (UpscaleMethod)settings.upscaleMethodNoDLSS;
}

void Upscaling::CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Creating texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		if (!reactiveMaskTexture) {
			reactiveMaskTexture = new Texture2D(texDesc);
			reactiveMaskTexture->CreateSRV(srvDesc);
			reactiveMaskTexture->CreateUAV(uavDesc);
		}

		if (!transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture = new Texture2D(texDesc);
			transparencyCompositionMaskTexture->CreateSRV(srvDesc);
			transparencyCompositionMaskTexture->CreateUAV(uavDesc);
		}
	}

	// Motion vector copy texture is used by DLSS and FSR encode pass.
	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		if (!motionVectorCopyTexture) {
			auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			D3D11_TEXTURE2D_DESC motionTexDesc{};
			motionVector.texture->GetDesc(&motionTexDesc);

			texDesc.Format = motionTexDesc.Format;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			motionVectorCopyTexture = new Texture2D(motionTexDesc);
			motionVectorCopyTexture->CreateSRV(srvDesc);
			motionVectorCopyTexture->CreateUAV(uavDesc);
		}

	}

	// RCAS sharpener texture - matches kMAIN format for HDR sharpening
	if (a_upscalemethod == UpscaleMethod::kDLSS) {
		if (!sharpenerTexture) {
			main.texture->GetDesc(&texDesc);
			main.SRV->GetDesc(&srvDesc);

			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			sharpenerTexture = new Texture2D(texDesc);
			sharpenerTexture->CreateSRV(srvDesc);
			sharpenerTexture->CreateUAV(uavDesc);
		}
	}
}

void Upscaling::DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Destroying texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	// Clean up D3D11 textures that are no longer needed
	// Only destroy textures when switching away from methods that use them
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		if (reactiveMaskTexture) {
			reactiveMaskTexture->srv = nullptr;
			reactiveMaskTexture->uav = nullptr;
			reactiveMaskTexture->resource = nullptr;

			delete reactiveMaskTexture;
			reactiveMaskTexture = nullptr;
		}

		if (transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture->srv = nullptr;
			transparencyCompositionMaskTexture->uav = nullptr;
			transparencyCompositionMaskTexture->resource = nullptr;

			delete transparencyCompositionMaskTexture;
			transparencyCompositionMaskTexture = nullptr;
		}
	}

	// Motion vector copy texture is used by DLSS/FSR - destroy when switching away from both.
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		if (motionVectorCopyTexture) {
			motionVectorCopyTexture->srv = nullptr;
			motionVectorCopyTexture->uav = nullptr;
			motionVectorCopyTexture->resource = nullptr;

			delete motionVectorCopyTexture;
			motionVectorCopyTexture = nullptr;
		}
	}

	// RCAS sharpener texture is only needed for DLSS.
	if (a_upscalemethod != UpscaleMethod::kDLSS) {
		if (sharpenerTexture) {
			sharpenerTexture->srv = nullptr;
			sharpenerTexture->uav = nullptr;
			sharpenerTexture->resource = nullptr;

			delete sharpenerTexture;
			sharpenerTexture = nullptr;
		}
	}
}

void Upscaling::CheckResources(UpscaleMethod a_upscalemethod)
{
	static auto previousUpscaleMode = UpscaleMethod::kTAA;
	static bool previousFrameGenMode = false;
	static bool previousFoveatedDispatch = false;
	static bool previousPeripheryTAA = false;
	static bool previousFSRRuntimePathActive = false;
	static uint32_t previousFoveatedCenterAreaMilli = static_cast<uint32_t>(std::round(GetFoveatedMaskProfileParams(settings, settings.periphery_taa_enable).centerArea * 1000.0f));
	static uint32_t previousFoveatedCenterHorizontalScaleMilli = static_cast<uint32_t>(std::round(GetFoveatedMaskProfileParams(settings, settings.periphery_taa_enable).centerHorizontalScale * 1000.0f));

	bool frameGenModeCurrent = (settings.frameGenerationMode && d3d12SwapChainActive);
	bool frameGenModeChanged = frameGenModeCurrent != previousFrameGenMode;
	bool upscaleModeChanged = (previousUpscaleMode != a_upscalemethod);
	const bool foveatedDispatchCurrent = IsFoveatedVendorDispatchEnabled(a_upscalemethod);
	const bool peripheryTAACurrent = IsPeripheryTAAEnabled(a_upscalemethod);
	const bool fsrRuntimePathCurrent = IsFSRRuntimePathActive(a_upscalemethod);
	const auto effectiveProfile = GetFoveatedMaskProfileParams(settings, peripheryTAACurrent);
	const uint32_t foveatedCenterAreaMilli = static_cast<uint32_t>(std::round(effectiveProfile.centerArea * 1000.0f));
	const uint32_t foveatedCenterHorizontalScaleMilli = static_cast<uint32_t>(std::round(effectiveProfile.centerHorizontalScale * 1000.0f));
	const bool compareFoveatedArea = foveatedDispatchCurrent || previousFoveatedDispatch;
	const bool foveatedDispatchChanged = previousFoveatedDispatch != foveatedDispatchCurrent ||
	                                     (compareFoveatedArea && (previousFoveatedCenterAreaMilli != foveatedCenterAreaMilli ||
	                                                              previousFoveatedCenterHorizontalScaleMilli != foveatedCenterHorizontalScaleMilli));
	const bool peripheryTAAChanged = previousPeripheryTAA != peripheryTAACurrent;
	const bool compareFSRRuntimePath = a_upscalemethod == UpscaleMethod::kFSR || previousUpscaleMode == UpscaleMethod::kFSR;
	const bool fsrRuntimePathChanged = compareFSRRuntimePath && previousFSRRuntimePathActive != fsrRuntimePathCurrent;

	if (upscaleModeChanged || frameGenModeChanged || foveatedDispatchChanged || peripheryTAAChanged || fsrRuntimePathChanged) {
		logger::debug("[Upscaling] Resource change detected - Upscale: {} ({}) -> {} ({}), FrameGen: {} -> {} (d3d12Active={}), FSRRuntimePath: {} -> {}",
			static_cast<int>(previousUpscaleMode), magic_enum::enum_name(previousUpscaleMode), static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod), previousFrameGenMode, frameGenModeCurrent, d3d12SwapChainActive,
			previousFSRRuntimePathActive, fsrRuntimePathCurrent);

		// Destroy previous upscaling method resources (only if they were actually active)
		if (upscaleModeChanged) {
			DestroyUpscalingTextureResources(a_upscalemethod);

			// Only destroy SDK resources if the previous method was actually performing upscaling
			if (previousUpscalingWasActive) {
				if (previousUpscaleMode == UpscaleMethod::kDLSS)
					streamline.DestroyDLSSResources();
				else if (previousUpscaleMode == UpscaleMethod::kFSR)
					fidelityFX.DestroyFSRResources();

				if (globals::game::isVR) {
					for (int i = 0; i < 2; i++) {
						vrIntermediateColorIn[i].reset();
						vrIntermediateColorOut[i].reset();
						vrIntermediateDepth[i].reset();
						vrIntermediateMotionVectors[i].reset();
						vrIntermediateReactiveMask[i].reset();
						vrIntermediateTransparencyMask[i].reset();
					}
				}
			}
			if (a_upscalemethod == UpscaleMethod::kFSR)
				fidelityFX.CreateFSRResources();
		}

		// Create new upscaling method resources
		if (upscaleModeChanged) {
			CreateUpscalingTextureResources(a_upscalemethod);
		}

		// Host FSR3.1 and runtime FSR4 keep separate temporal state; rebuild on path changes.
		if (!upscaleModeChanged && fsrRuntimePathChanged && a_upscalemethod == UpscaleMethod::kFSR) {
			fidelityFX.DestroyFSRResources();
			fidelityFX.CreateFSRResources();
			RequestHistoryReset();
		}

		if (upscaleModeChanged || foveatedDispatchChanged) {
			if (!foveatedDispatchCurrent)
				DestroyFoveatedResources();
		}

		if ((upscaleModeChanged || foveatedDispatchChanged || peripheryTAAChanged) && !peripheryTAACurrent) {
			DestroyPeripheryTAAResources();
		}

		// Update tracking for next call
		previousUpscaleMode = a_upscalemethod;
		previousFrameGenMode = (settings.frameGenerationMode && d3d12SwapChainActive);
		previousFoveatedDispatch = foveatedDispatchCurrent;
		previousPeripheryTAA = peripheryTAACurrent;
		previousFSRRuntimePathActive = fsrRuntimePathCurrent;
		previousFoveatedCenterAreaMilli = foveatedCenterAreaMilli;
		previousFoveatedCenterHorizontalScaleMilli = foveatedCenterHorizontalScaleMilli;
		previousUpscalingWasActive = IsUpscalingActive();
	}
}

ID3D11ComputeShader* Upscaling::GetEncodeTexturesCS()
{
	auto upscaleMethod = GetUpscaleMethod();
	uint methodIndex = (uint)upscaleMethod;

	if (!encodeTexturesCS[methodIndex]) {
		logger::debug("Compiling EncodeTexturesCS.hlsl for upscale method {}", methodIndex);

		std::vector<std::pair<const char*, const char*>> defines;

		// Add upscale method define
		switch (upscaleMethod) {
		case UpscaleMethod::kDLSS:
			defines.push_back({ "DLSS", "" });
			break;
		case UpscaleMethod::kFSR:
			defines.push_back({ "FSR", "" });
			break;
		default:
			// No define for NONE or TAA
			break;
		}

		encodeTexturesCS[methodIndex].attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0"));
	}
	return encodeTexturesCS[methodIndex].get();
}

ID3D11PixelShader* Upscaling::GetDepthRefractionUpscalePS()
{
	if (!depthRefractionUpscalePS) {
		logger::debug("Compiling DepthRefractionUpscalePS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		depthRefractionUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/DepthRefractionUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return depthRefractionUpscalePS.get();
}

ID3D11PixelShader* Upscaling::GetUnderwaterMaskUpscalePS()
{
	if (!underwaterMaskUpscalePS) {
		logger::debug("Compiling UnderwaterMaskPS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		underwaterMaskUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UnderwaterMaskUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return underwaterMaskUpscalePS.get();
}

ID3D11VertexShader* Upscaling::GetUpscaleVS()
{
	if (!upscaleVS) {
		logger::debug("Compiling UpscaleVS.hlsl");
		upscaleVS.attach((ID3D11VertexShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}

	return upscaleVS.get();
}

ID3D11ComputeShader* Upscaling::GetFoveatedPeripheryCS()
{
	if (!foveatedPeripheryCS) {
		logger::debug("Compiling FoveatedPeripheryCS.hlsl");
		foveatedPeripheryCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/FoveatedPeripheryCS.hlsl", {}, "cs_5_0"));
	}

	return foveatedPeripheryCS.get();
}

ID3D11ComputeShader* Upscaling::GetFoveatedCenterBlendCS()
{
	if (!foveatedCenterBlendCS) {
		logger::debug("Compiling FoveatedCenterBlendCS.hlsl");
		foveatedCenterBlendCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/FoveatedCenterBlendCS.hlsl", {}, "cs_5_0"));
	}

	return foveatedCenterBlendCS.get();
}

ID3D11ComputeShader* Upscaling::GetPeripheryTAACS()
{
	if (!peripheryTAACS) {
		logger::debug("Compiling PeripheryTAACS.hlsl");
		peripheryTAACS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/PeripheryTAACS.hlsl", {}, "cs_5_0"));
	}

	return peripheryTAACS.get();
}

eastl::unique_ptr<Texture2D> Upscaling::CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
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

bool Upscaling::IsFoveatedVendorDispatchEnabled(UpscaleMethod a_upscaleMethod) const
{
	if (!SupportsFoveatedVendorDispatch(a_upscaleMethod))
		return false;

	if (!settings.foveatedVendorDispatch)
		return false;

	if (!IsFoveatedSetupComplete(settings) && !settings.foveatedPeripheryMaskVisualization)
		return false;

	const bool usePeripheryTAAProfile = settings.periphery_taa_enable;
	const float centerArea = GetFoveatedMaskProfileParams(settings, usePeripheryTAAProfile).centerArea;
	// 1.0 is effectively full-frame vendor dispatch, so keep the default path.
	return centerArea < 0.999f;
}

bool Upscaling::IsFSRRuntimePathActive(UpscaleMethod a_upscaleMethod) const
{
	return a_upscaleMethod == UpscaleMethod::kFSR &&
	       fidelityFX.IsRuntimeUpscalerAvailable() &&
	       settings.fsr4RuntimeEnable;
}

bool Upscaling::IsPeripheryTAAEnabled(UpscaleMethod a_upscaleMethod) const
{
	return IsFoveatedVendorDispatchEnabled(a_upscaleMethod) && settings.periphery_taa_enable;
}

bool Upscaling::IsPeripheryTAAPathActive(UpscaleMethod a_upscaleMethod) const
{
	return IsPeripheryTAAEnabled(a_upscaleMethod) && !settings.foveatedPeripheryMaskVisualization;
}

float2 Upscaling::GetDefaultFoveatedMaskCenterOffset(uint32_t eyeIndex) const
{
	(void)eyeIndex;
	return { 0.0f, 0.0f };
}

std::array<float2, 2> Upscaling::GetDefaultFoveatedMaskCenterOffsets() const
{
	return { GetDefaultFoveatedMaskCenterOffset(0), GetDefaultFoveatedMaskCenterOffset(1) };
}

float2 Upscaling::GetResolvedFoveatedMaskCenterOffset(uint32_t eyeIndex, bool usePeripheryTAAProfile) const
{
	float2 resolved = GetDefaultFoveatedMaskCenterOffset(eyeIndex);
	const bool isLeftEye = eyeIndex == 0;
	const auto params = GetFoveatedMaskProfileParams(settings, usePeripheryTAAProfile);
	const float userAdjustX = isLeftEye ? params.leftOffsetX : params.rightOffsetX;
	const float userAdjustY = isLeftEye ? params.leftOffsetY : params.rightOffsetY;
	resolved.x += ClampFoveatedMaskOffsetAdjustment(userAdjustX);
	resolved.y += ClampFoveatedMaskOffsetAdjustment(userAdjustY);

	if (globals::game::isVR) {
		const float centerScale = params.centerArea;
		const float centerHorizontalScale = params.centerHorizontalScale;
		const float outwardExpansion = centerScale * 0.5f * std::max(0.0f, centerHorizontalScale - 1.0f);
		resolved.x += isLeftEye ? -outwardExpansion : outwardExpansion;
	}

	resolved.x = std::clamp(resolved.x, kFoveatedMaskOffsetResolvedMin, kFoveatedMaskOffsetResolvedMax);
	resolved.y = std::clamp(resolved.y, kFoveatedMaskOffsetResolvedMin, kFoveatedMaskOffsetResolvedMax);
	return resolved;
}

std::array<float2, 2> Upscaling::GetResolvedFoveatedMaskCenterOffsets(bool usePeripheryTAAProfile) const
{
	return { GetResolvedFoveatedMaskCenterOffset(0, usePeripheryTAAProfile), GetResolvedFoveatedMaskCenterOffset(1, usePeripheryTAAProfile) };
}

bool Upscaling::BuildFoveatedDispatchRects(uint32_t inputWidthPerEye, uint32_t inputHeight, uint32_t outputWidthPerEye, uint32_t outputHeight, bool isVR, float centerScale, float centerFeather, float centerHorizontalScale, bool usePeripheryTAAProfile)
{
	centerScale = ClampFoveatedCenterArea(centerScale);
	centerFeather = std::isfinite(centerFeather) ? std::max(0.0f, centerFeather) : FoveatedCommon::kCenterFeather;
	centerHorizontalScale = ClampFoveatedCenterHorizontalScale(centerHorizontalScale);

	auto& cache = foveatedRectCache;
	auto centerOffsets = GetResolvedFoveatedMaskCenterOffsets(usePeripheryTAAProfile);
	if (!isVR)
		centerOffsets[1] = { 0.0f, 0.0f };
	const bool cacheDirty =
		cache.inputWidthPerEye != inputWidthPerEye ||
		cache.inputHeight != inputHeight ||
		cache.outputWidthPerEye != outputWidthPerEye ||
		cache.outputHeight != outputHeight ||
		cache.isVR != isVR ||
		std::abs(cache.centerScale - centerScale) > 1e-6f ||
		std::abs(cache.centerFeather - centerFeather) > 1e-6f ||
		std::abs(cache.centerHorizontalScale - centerHorizontalScale) > 1e-6f ||
		std::abs(cache.centerOffsets[0].x - centerOffsets[0].x) > 1e-6f ||
		std::abs(cache.centerOffsets[0].y - centerOffsets[0].y) > 1e-6f ||
		(isVR && (std::abs(cache.centerOffsets[1].x - centerOffsets[1].x) > 1e-6f ||
		          std::abs(cache.centerOffsets[1].y - centerOffsets[1].y) > 1e-6f));

	if (!cacheDirty)
		return true;

	cache.inputWidthPerEye = inputWidthPerEye;
	cache.inputHeight = inputHeight;
	cache.outputWidthPerEye = outputWidthPerEye;
	cache.outputHeight = outputHeight;
	cache.isVR = isVR;
	cache.centerScale = centerScale;
	cache.centerFeather = centerFeather;
	cache.centerHorizontalScale = centerHorizontalScale;
	cache.centerOffsets = centerOffsets;
	cache.rects = {};

	auto buildRect = [&](uint32_t eyeIndex) {
		FoveatedDispatchRect rect{};
		if (!inputWidthPerEye || !inputHeight || !outputWidthPerEye || !outputHeight)
			return rect;

		const float2 centerOffset = centerOffsets[eyeIndex];
		const auto bounds = FoveatedCommon::BuildCenteredDispatchBounds(0, outputWidthPerEye, outputHeight, centerScale, centerOffset.x, centerOffset.y, centerFeather, centerHorizontalScale);
		const int minX = bounds.minX;
		const int maxX = bounds.maxX;
		const int minY = bounds.minY;
		const int maxY = bounds.maxY;

		if (maxX <= minX || maxY <= minY)
			return rect;

		rect.outputOffsetX = static_cast<uint32_t>(minX);
		rect.outputOffsetY = static_cast<uint32_t>(minY);
		rect.outputWidth = static_cast<uint32_t>(maxX - minX);
		rect.outputHeight = static_cast<uint32_t>(maxY - minY);

		const uint32_t outputRectMaxX = static_cast<uint32_t>(std::min<uint64_t>(outputWidthPerEye, static_cast<uint64_t>(rect.outputOffsetX) + rect.outputWidth));
		const uint32_t outputRectMaxY = static_cast<uint32_t>(std::min<uint64_t>(outputHeight, static_cast<uint64_t>(rect.outputOffsetY) + rect.outputHeight));
		const auto mappedInputRect = MapOutputRectToInputRect(
			rect.outputOffsetX,
			rect.outputOffsetY,
			outputRectMaxX,
			outputRectMaxY,
			outputWidthPerEye,
			outputHeight,
			inputWidthPerEye,
			inputHeight);
		if (!mappedInputRect.IsValid())
			return FoveatedDispatchRect{};

		rect.inputOffsetX = mappedInputRect.minX;
		rect.inputOffsetY = mappedInputRect.minY;
		rect.inputWidth = mappedInputRect.maxX - mappedInputRect.minX;
		rect.inputHeight = mappedInputRect.maxY - mappedInputRect.minY;

		(void)eyeIndex;
		return rect;
	};

	cache.rects[0] = buildRect(0);
	if (isVR)
		cache.rects[1] = buildRect(1);

	return true;
}

bool Upscaling::EnsureFoveatedTexture(eastl::unique_ptr<Texture2D>& texture, ID3D11Resource* source, uint32_t width, uint32_t height, bool copyBindFlags, bool createSRV, bool createUAV, bool createRTV, const char* name)
{
	if (!source || !width || !height)
		return false;

	D3D11_TEXTURE2D_DESC sourceDesc{};
	if (!TryGetTexture2DDesc(source, sourceDesc))
		return false;

	bool recreate = !texture;
	if (!recreate) {
		recreate = texture->desc.Width != width ||
		           texture->desc.Height != height ||
		           texture->desc.Format != sourceDesc.Format;
		if (createSRV && !texture->srv)
			recreate = true;
		if (createUAV && !texture->uav)
			recreate = true;
		if (createRTV && !texture->rtv)
			recreate = true;
	}

	if (recreate) {
		texture = CreateTextureFromSource(source, width, height, copyBindFlags, createSRV, createUAV, name);
		if (!texture)
			return false;
	}

	if (createRTV && !texture->rtv) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = texture->desc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		texture->CreateRTV(rtvDesc);
	}

	if (createSRV && !texture->srv) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texture->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		texture->CreateSRV(srvDesc);
	}

	if (createUAV && !texture->uav) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texture->desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		texture->CreateUAV(uavDesc);
	}

	return true;
}

bool Upscaling::EnsurePeripheryTAAResources(uint32_t outputWidthPerEye, uint32_t outputHeight, ID3D11Resource* colorSource)
{
	if (!outputWidthPerEye || !outputHeight || !colorSource)
		return false;

	D3D11_TEXTURE2D_DESC colorDesc{};
	if (!TryGetTexture2DDesc(colorSource, colorDesc))
		return false;

	bool recreatedResources = false;

	for (uint32_t eye = 0; eye < 2; ++eye) {
		const std::string suffix = eye == 0 ? "Left" : "Right";

		for (uint32_t historySlot = 0; historySlot < 2; ++historySlot) {
			auto& historyColorTexture = peripheryTAAHistoryColor[eye][historySlot];
			const bool recreateHistoryColor =
				!historyColorTexture ||
				historyColorTexture->desc.Width != outputWidthPerEye ||
				historyColorTexture->desc.Height != outputHeight ||
				historyColorTexture->desc.Format != colorDesc.Format ||
				!historyColorTexture->srv || !historyColorTexture->uav;
			recreatedResources = recreatedResources || recreateHistoryColor;

			if (!EnsureFoveatedTexture(
					historyColorTexture,
					colorSource,
					outputWidthPerEye,
					outputHeight,
					false,
					true,
					true,
					false,
					(std::format("Upscale_PeripheryTAA_HistoryColor_{}_{}", suffix, historySlot)).c_str())) {
				return false;
			}

			auto& velocityTexture = peripheryTAAVelocityHistory[eye][historySlot];
			const bool recreateVelocity =
				!velocityTexture ||
				velocityTexture->desc.Width != outputWidthPerEye ||
				velocityTexture->desc.Height != outputHeight ||
				velocityTexture->desc.Format != DXGI_FORMAT_R16G16_FLOAT ||
				!velocityTexture->srv || !velocityTexture->uav;
			if (recreateVelocity) {
				velocityTexture = CreateNamedTexture2D(
					outputWidthPerEye,
					outputHeight,
					DXGI_FORMAT_R16G16_FLOAT,
					true,
					true,
					(std::format("Upscale_PeripheryTAA_Velocity_{}_{}", suffix, historySlot)).c_str());
				recreatedResources = true;
			}

			auto& lockTexture = peripheryTAALockHistory[eye][historySlot];
			const bool recreateLock =
				!lockTexture ||
				lockTexture->desc.Width != outputWidthPerEye ||
				lockTexture->desc.Height != outputHeight ||
				lockTexture->desc.Format != DXGI_FORMAT_R16_FLOAT ||
				!lockTexture->srv || !lockTexture->uav;
			if (recreateLock) {
				lockTexture = CreateNamedTexture2D(
					outputWidthPerEye,
					outputHeight,
					DXGI_FORMAT_R16_FLOAT,
					true,
					true,
					(std::format("Upscale_PeripheryTAA_Lock_{}_{}", suffix, historySlot)).c_str());
				recreatedResources = true;
			}
		}
	}

	if (recreatedResources) {
		// Any recreated history surface invalidates temporal continuity.
		peripheryTAAHistoryReadIndex = 0;
		peripheryTAAHistoryValid = false;
	}

	return true;
}

bool Upscaling::EnsurePeripheryTAATileBuffer(uint32_t eyeIndex, uint32_t tileCapacity)
{
	if (eyeIndex >= 2 || tileCapacity == 0)
		return false;
	if (tileCapacity > std::numeric_limits<uint32_t>::max() / sizeof(PeripheryTAATile))
		return false;

	auto& tileBuffer = peripheryTAATileBuffer[eyeIndex];
	auto& tileCapacityCurrent = peripheryTAATileCapacity[eyeIndex];
	if (tileBuffer && tileCapacityCurrent >= tileCapacity && tileBuffer->srv)
		return true;

	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DYNAMIC;
	sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	sbDesc.StructureByteStride = sizeof(PeripheryTAATile);
	sbDesc.ByteWidth = static_cast<uint32_t>(sizeof(PeripheryTAATile) * tileCapacity);

	tileBuffer = eastl::make_unique<Buffer>(sbDesc);
	tileCapacityCurrent = tileCapacity;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = tileCapacity;
	tileBuffer->CreateSRV(srvDesc);

	peripheryTAATileCache[eyeIndex].uploaded = false;
	return tileBuffer->srv != nullptr;
}

bool Upscaling::BuildPeripheryTAATileList(uint32_t eyeIndex, uint32_t outputWidth, uint32_t outputHeight, float centerScale, float taaOuterScale, float centerHorizontalScale, float centerFeather, float centerOffsetX, float centerOffsetY, uint32_t coveragePadding, uint32_t& outTileCount)
{
	outTileCount = 0;
	if (eyeIndex >= 2 || outputWidth == 0 || outputHeight == 0)
		return false;

	const uint32_t tileSize = static_cast<uint32_t>(FoveatedCommon::kThreadGroupSize);
	const uint32_t tileColumns = (outputWidth + tileSize - 1u) / tileSize;
	const uint32_t tileRows = (outputHeight + tileSize - 1u) / tileSize;
	if (tileColumns != 0 && tileRows > (std::numeric_limits<uint32_t>::max() / tileColumns))
		return false;
	const uint32_t maxTileCount = tileColumns * tileRows;
	if (!EnsurePeripheryTAATileBuffer(eyeIndex, maxTileCount))
		return false;

	centerScale = ClampFoveatedCenterArea(centerScale);
	centerFeather = ClampPeripheryTAACenterBlendFeather(centerFeather);
	taaOuterScale = ClampPeripheryTAAOuterScaleForCenter(
		taaOuterScale,
		centerScale,
		centerHorizontalScale,
		centerFeather);
	centerHorizontalScale = ClampFoveatedCenterHorizontalScale(centerHorizontalScale);

	PeripheryTAATileCacheKey cacheKey{};
	cacheKey.outputWidth = outputWidth;
	cacheKey.outputHeight = outputHeight;
	cacheKey.coveragePadding = coveragePadding;
	cacheKey.centerScaleQ = QuantizePeripheryTAATileParam(centerScale);
	cacheKey.taaOuterScaleQ = QuantizePeripheryTAATileParam(taaOuterScale);
	cacheKey.centerHorizontalScaleQ = QuantizePeripheryTAATileParam(centerHorizontalScale);
	cacheKey.centerOffsetXQ = QuantizePeripheryTAATileParam(centerOffsetX);
	cacheKey.centerOffsetYQ = QuantizePeripheryTAATileParam(centerOffsetY);

	auto& cacheState = peripheryTAATileCache[eyeIndex];
	const bool keyMatches =
		cacheState.valid &&
		cacheState.key.outputWidth == cacheKey.outputWidth &&
		cacheState.key.outputHeight == cacheKey.outputHeight &&
		cacheState.key.coveragePadding == cacheKey.coveragePadding &&
		cacheState.key.centerScaleQ == cacheKey.centerScaleQ &&
		cacheState.key.taaOuterScaleQ == cacheKey.taaOuterScaleQ &&
		cacheState.key.centerHorizontalScaleQ == cacheKey.centerHorizontalScaleQ &&
		cacheState.key.centerOffsetXQ == cacheKey.centerOffsetXQ &&
		cacheState.key.centerOffsetYQ == cacheKey.centerOffsetYQ;

	if (!keyMatches) {
		cacheState.tiles.clear();
		cacheState.tiles.reserve(maxTileCount);

		const auto coverageBounds = FoveatedCommon::BuildCenteredDispatchBounds(0, outputWidth, outputHeight, taaOuterScale, centerOffsetX, centerOffsetY, 0.0f, centerHorizontalScale);
		const uint32_t coverageMinX = coverageBounds.minX > static_cast<int>(coveragePadding) ? static_cast<uint32_t>(coverageBounds.minX) - coveragePadding : 0u;
		const uint32_t coverageMinY = coverageBounds.minY > static_cast<int>(coveragePadding) ? static_cast<uint32_t>(coverageBounds.minY) - coveragePadding : 0u;
		const uint32_t coverageMaxX = coverageBounds.maxX > coverageBounds.minX ? std::min(outputWidth, static_cast<uint32_t>(coverageBounds.maxX) + coveragePadding) : 0u;
		const uint32_t coverageMaxY = coverageBounds.maxY > coverageBounds.minY ? std::min(outputHeight, static_cast<uint32_t>(coverageBounds.maxY) + coveragePadding) : 0u;
		const bool useRectangularCoverage = coveragePadding > 0 && coverageMaxX > coverageMinX && coverageMaxY > coverageMinY;

		for (uint32_t tileY = 0; tileY < outputHeight; tileY += tileSize) {
			const uint32_t maxY = std::min(tileY + tileSize, outputHeight);
			for (uint32_t tileX = 0; tileX < outputWidth; tileX += tileSize) {
				const uint32_t maxX = std::min(tileX + tileSize, outputWidth);
				if (useRectangularCoverage) {
					if (maxX <= coverageMinX || tileX >= coverageMaxX || maxY <= coverageMinY || tileY >= coverageMaxY)
						continue;
				} else {
					const float outerMinDistance = FoveatedMaskTileMinDistance(tileX, tileY, maxX, maxY, outputWidth, outputHeight, taaOuterScale, centerHorizontalScale, centerOffsetX, centerOffsetY);
					if (outerMinDistance > 1.0f + 1e-4f)
						continue;
				}

				const uint32_t centerTestMinX = tileX > coveragePadding ? tileX - coveragePadding : 0u;
				const uint32_t centerTestMinY = tileY > coveragePadding ? tileY - coveragePadding : 0u;
				const uint32_t centerTestMaxX = std::min(outputWidth, maxX + coveragePadding);
				const uint32_t centerTestMaxY = std::min(outputHeight, maxY + coveragePadding);
				const float centerMaxDistance = FoveatedMaskTileMaxDistance(centerTestMinX, centerTestMinY, centerTestMaxX, centerTestMaxY, outputWidth, outputHeight, centerScale, centerHorizontalScale, centerOffsetX, centerOffsetY);
				if (centerMaxDistance <= 1.0f - 1e-4f)
					continue;

				cacheState.tiles.push_back({ tileX, tileY });
			}
		}

		cacheState.key = cacheKey;
		cacheState.tileCount = static_cast<uint32_t>(cacheState.tiles.size());
		cacheState.valid = true;
		cacheState.uploaded = false;
	}

	outTileCount = cacheState.tileCount;
	if (outTileCount == 0)
		return true;
	if (cacheState.uploaded)
		return true;

	auto context = globals::d3d::context;
	auto& tileBuffer = peripheryTAATileBuffer[eyeIndex];
	const uint32_t tileCapacity = peripheryTAATileCapacity[eyeIndex];
	if (!context || !tileBuffer || !tileBuffer->resource || tileCapacity < outTileCount)
		return false;

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(tileBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	const size_t bytes = sizeof(PeripheryTAATile) * outTileCount;
	memcpy_s(mapped.pData, sizeof(PeripheryTAATile) * tileCapacity, cacheState.tiles.data(), bytes);
	context->Unmap(tileBuffer->resource.get(), 0);
	cacheState.uploaded = true;
	return true;
}

void Upscaling::DestroyFoveatedResources()
{
	for (uint32_t i = 0; i < 2; ++i) {
		foveatedCenterColorIn[i].reset();
		foveatedCenterColorOut[i].reset();
		foveatedCenterDepth[i].reset();
		foveatedCenterMotionVectors[i].reset();
		foveatedCenterReactiveMask[i].reset();
		foveatedCenterTransparencyMask[i].reset();
	}
	foveatedRectCache = {};
	DestroyPeripheryTAAResources();
}

void Upscaling::DestroyPeripheryTAAResources()
{
	for (uint32_t eye = 0; eye < 2; ++eye) {
		for (uint32_t historySlot = 0; historySlot < 2; ++historySlot) {
			peripheryTAAHistoryColor[eye][historySlot].reset();
			peripheryTAAVelocityHistory[eye][historySlot].reset();
			peripheryTAALockHistory[eye][historySlot].reset();
		}
		peripheryTAATileBuffer[eye].reset();
		peripheryTAATileCapacity[eye] = 0;
		peripheryTAATileCache[eye] = {};
	}
	peripheryTAAHistoryReadIndex = 0;
	peripheryTAAHistoryValid = false;
}

void Upscaling::DispatchFoveatedPeripheryPass(ID3D11ShaderResourceView* sourceSRV, ID3D11UnorderedAccessView* outputUAV, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t outputWidth, uint32_t outputHeight, uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight, float centerScale, float centerHorizontalScale, bool keepBindingsBound, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, float centerOffsetX, float centerOffsetY)
{
	auto* peripheryCS = GetFoveatedPeripheryCS();
	if (!peripheryCS || !sourceSRV || !outputUAV || !foveatedPeripheryCB)
		return;
	if (!dispatchWidth || !dispatchHeight)
		return;

	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!context || !deferred || !deferred->linearSampler)
		return;
	if (outputOffsetX >= outputWidth || outputOffsetY >= outputHeight)
		return;
	dispatchWidth = std::min(dispatchWidth, outputWidth - outputOffsetX);
	dispatchHeight = std::min(dispatchHeight, outputHeight - outputOffsetY);
	if (!dispatchWidth || !dispatchHeight)
		return;

	FoveatedPeripheryCB cbData{};
	cbData.outputDim = { static_cast<float>(outputWidth), static_cast<float>(outputHeight) };
	cbData.invOutputDim = {
		outputWidth > 0 ? 1.0f / static_cast<float>(outputWidth) : 0.0f,
		outputHeight > 0 ? 1.0f / static_cast<float>(outputHeight) : 0.0f
	};
	cbData.invSourceDim = {
		sourceWidth > 0 ? 1.0f / static_cast<float>(sourceWidth) : 0.0f,
		sourceHeight > 0 ? 1.0f / static_cast<float>(sourceHeight) : 0.0f
	};
	cbData.sourceScale = { sourceScaleX, sourceScaleY };
	cbData.sourceOffset = { sourceOffsetX, sourceOffsetY };
	cbData.dispatchDim = { static_cast<float>(dispatchWidth), static_cast<float>(dispatchHeight) };
	cbData.outputOffset = { static_cast<float>(outputOffsetX), static_cast<float>(outputOffsetY) };
	cbData.jitter = jitter;
	cbData.centerOffset = { centerOffsetX, centerOffsetY };
	cbData.pad0 = { 0.0f, 0.0f };
	centerScale = ClampFoveatedCenterArea(centerScale);
	centerHorizontalScale = ClampFoveatedCenterHorizontalScale(centerHorizontalScale);
	const bool visualizeMask = settings.foveatedPeripheryMaskVisualization;
	const bool showThreeZoneMask = visualizeMask && settings.periphery_taa_enable;
	const float centerFeather = showThreeZoneMask ? ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather) : FoveatedCommon::kCenterFeather;
	const float taaOuterScale = ClampPeripheryTAAOuterScaleForCenter(settings.periphery_taa_outer_scale, centerScale, centerHorizontalScale, centerFeather);
	cbData.tuning0 = {
		centerScale,
		centerFeather,
		centerHorizontalScale,
		0.0f
	};
	cbData.tuning1 = {
		visualizeMask ? 1.0f : 0.0f,
		showThreeZoneMask ? 1.0f : 0.0f,
		taaOuterScale,
		0.0f
	};
	foveatedPeripheryCB->Update(cbData);

	if (keepBindingsBound) {
		auto state = globals::state;
		if (state && state->frameAnnotations)
			state->BeginPerfEvent("Foveated Periphery");
		context->Dispatch((dispatchWidth + 7u) >> 3, (dispatchHeight + 7u) >> 3, 1);
		if (state && state->frameAnnotations)
			state->EndPerfEvent();
	} else {
		ID3D11Buffer* cb = foveatedPeripheryCB->CB();
		ID3D11SamplerState* samplers[1] = { deferred->linearSampler };
		ID3D11ShaderResourceView* srvs[1] = { sourceSRV };
		ID3D11UnorderedAccessView* uavs[1] = { outputUAV };

		context->CSSetShader(peripheryCS, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetSamplers(0, 1, samplers);
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		auto state = globals::state;
		if (state && state->frameAnnotations)
			state->BeginPerfEvent("Foveated Periphery");
		context->Dispatch((dispatchWidth + 7u) >> 3, (dispatchHeight + 7u) >> 3, 1);
		if (state && state->frameAnnotations)
			state->EndPerfEvent();

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11SamplerState* nullSampler[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

void Upscaling::DispatchPeripheryTAAPass(ID3D11ShaderResourceView* currentColorSRV, ID3D11ShaderResourceView* currentDepthSRV, ID3D11ShaderResourceView* currentMotionVectorSRV,
	ID3D11ShaderResourceView* currentReactiveSRV, ID3D11ShaderResourceView* currentTransparencySRV, ID3D11ShaderResourceView* historyColorSRV,
	ID3D11ShaderResourceView* historyVelocitySRV, ID3D11ShaderResourceView* historyLockSRV, ID3D11UnorderedAccessView* outputColorUAV, ID3D11UnorderedAccessView* outputHistoryColorUAV,
	ID3D11UnorderedAccessView* outputVelocityUAV, ID3D11UnorderedAccessView* outputLockUAV, ID3D11ShaderResourceView* tileListSRV, uint32_t tileCount,
	uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth,
	uint32_t outputHeight, uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight, const float4x4& currentViewProjInverse,
	const float4x4& previousViewProj, const float4& currentCameraPosAdjust, const float4& previousCameraPosAdjust, bool resetHistory, float centerScale, float centerHorizontalScale, float centerOffsetX, float centerOffsetY)
{
	// This custom periphery-only TAA path adapts MIT-licensed ideas from:
	// - Godot's TAA resolve / Spartan Engine lineage (taa_resolve.glsl, copyright Panos Karabelas)
	// - AMD FidelityFX FSR2/FSR3 lock/reactivity/luminance-instability heuristics.
	// - Temporal AA survey background: Yang, Liu, Salvi, "A Survey of Temporal Antialiasing Techniques" (2020).
	// The implementation below is purpose-built for Community Shaders VR periphery resolve and is not a verbatim copy.
	auto* peripheryTAA = GetPeripheryTAACS();
	if (!peripheryTAA || !peripheryTAACB)
		return;
	if (!currentColorSRV || !currentDepthSRV || !currentMotionVectorSRV || !currentReactiveSRV || !currentTransparencySRV)
		return;
	if (!historyColorSRV || !historyVelocitySRV || !historyLockSRV)
		return;
	if (!outputColorUAV || !outputHistoryColorUAV || !outputVelocityUAV || !outputLockUAV)
		return;
	if (!inputWidth || !inputHeight || !outputWidth || !outputHeight)
		return;
	const bool useTileList = tileListSRV && tileCount > 0;
	if (!useTileList && (!dispatchWidth || !dispatchHeight))
		return;

	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!context || !deferred || !deferred->linearSampler)
		return;

	uint32_t dispatchGroupsX = 0;
	uint32_t dispatchGroupsY = 0;
	if (useTileList) {
		dispatchGroupsX = std::min(tileCount, 65535u);
		dispatchGroupsY = (tileCount + dispatchGroupsX - 1u) / dispatchGroupsX;
		outputOffsetX = 0;
		outputOffsetY = 0;
		dispatchWidth = tileCount;
		dispatchHeight = 1;
	} else {
		if (outputOffsetX >= outputWidth || outputOffsetY >= outputHeight)
			return;

		dispatchWidth = std::min(dispatchWidth, outputWidth - outputOffsetX);
		dispatchHeight = std::min(dispatchHeight, outputHeight - outputOffsetY);
		if (!dispatchWidth || !dispatchHeight)
			return;

		dispatchGroupsX = (dispatchWidth + 7u) >> 3;
		dispatchGroupsY = (dispatchHeight + 7u) >> 3;
	}

	PeripheryTAACB cbData{};
	cbData.outputDim = { static_cast<float>(outputWidth), static_cast<float>(outputHeight) };
	cbData.invOutputDim = {
		outputWidth > 0 ? 1.0f / static_cast<float>(outputWidth) : 0.0f,
		outputHeight > 0 ? 1.0f / static_cast<float>(outputHeight) : 0.0f
	};
	cbData.inputDim = { static_cast<float>(inputWidth), static_cast<float>(inputHeight) };
	cbData.invInputDim = {
		inputWidth > 0 ? 1.0f / static_cast<float>(inputWidth) : 0.0f,
		inputHeight > 0 ? 1.0f / static_cast<float>(inputHeight) : 0.0f
	};
	cbData.dispatchDim = { static_cast<float>(dispatchWidth), static_cast<float>(dispatchHeight) };
	cbData.outputOffset = { static_cast<float>(outputOffsetX), static_cast<float>(outputOffsetY) };
	cbData.jitter = jitter;
	cbData.centerOffset = { centerOffsetX, centerOffsetY };
	centerScale = ClampFoveatedCenterArea(centerScale);
	centerHorizontalScale = ClampFoveatedCenterHorizontalScale(centerHorizontalScale);
	const float centerFeather = ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather);
	const float taaOuterScale = ClampPeripheryTAAOuterScaleForCenter(
		settings.periphery_taa_outer_scale,
		centerScale,
		centerHorizontalScale,
		centerFeather);
	const auto taaColorWriteBounds = FoveatedCommon::BuildCenteredDispatchBounds(
		0,
		outputWidth,
		outputHeight,
		taaOuterScale,
		centerOffsetX,
		centerOffsetY,
		0.0f,
		centerHorizontalScale);
	cbData.tuning0 = {
		centerScale,
		centerFeather,
		resetHistory ? 1.0f : 0.0f,
		taaOuterScale
	};
	cbData.tuning1 = {
		peripheryTAAHistoryValid && !resetHistory ? 1.0f : 0.0f,
		centerHorizontalScale,
		useTileList ? 1.0f : 0.0f,
		static_cast<float>(dispatchGroupsX)
	};
	cbData.tuning2 = {
		1.0f,
		1.25f,
		0.10f,
		0.92f
	};
	cbData.tuning3 = {
		static_cast<float>(taaColorWriteBounds.minX),
		static_cast<float>(taaColorWriteBounds.minY),
		static_cast<float>(taaColorWriteBounds.maxX),
		static_cast<float>(taaColorWriteBounds.maxY)
	};
	cbData.currentViewProjInverse = currentViewProjInverse;
	cbData.previousViewProj = previousViewProj;
	cbData.currentCameraPosAdjust = currentCameraPosAdjust;
	cbData.previousCameraPosAdjust = previousCameraPosAdjust;
	peripheryTAACB->Update(cbData);

	ID3D11Buffer* cb = peripheryTAACB->CB();
	ID3D11SamplerState* samplers[1] = { deferred->linearSampler };
	ID3D11ShaderResourceView* srvs[9] = {
		currentColorSRV,
		currentDepthSRV,
		currentMotionVectorSRV,
		currentReactiveSRV,
		currentTransparencySRV,
		historyColorSRV,
		historyVelocitySRV,
		historyLockSRV,
		tileListSRV
	};
	ID3D11UnorderedAccessView* uavs[4] = { outputColorUAV, outputVelocityUAV, outputLockUAV, outputHistoryColorUAV };

	context->CSSetShader(peripheryTAA, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(0, 1, samplers);
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	auto state = globals::state;
	if (state && state->frameAnnotations) {
		char buf[64];
		if (useTileList)
			snprintf(buf, sizeof(buf), "Periphery TAA Tiles %u", tileCount);
		else
			snprintf(buf, sizeof(buf), "Periphery TAA Rect %ux%u", dispatchWidth, dispatchHeight);
		state->BeginPerfEvent(buf);
	}
	context->Dispatch(dispatchGroupsX, dispatchGroupsY, 1);
	if (state && state->frameAnnotations)
		state->EndPerfEvent();

	ID3D11ShaderResourceView* nullSRV[9] = {};
	ID3D11UnorderedAccessView* nullUAV[4] = {};
	ID3D11SamplerState* nullSampler[1] = { nullptr };
	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(nullSRV), nullSRV);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAV), nullUAV, nullptr);
	context->CSSetSamplers(0, 1, nullSampler);
	context->CSSetConstantBuffers(0, 1, nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
}

void Upscaling::DispatchFoveatedBlendPass(ID3D11ShaderResourceView* centerSRV, ID3D11UnorderedAccessView* outputUAV, uint32_t outputWidthPerEye, uint32_t outputHeight, const FoveatedDispatchRect& rect, uint32_t dispatchOffsetX, uint32_t dispatchOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight, float centerScale, float centerHorizontalScale, const float2& centerOffset, float centerFeather)
{
	if (!centerSRV || !outputUAV || rect.outputWidth == 0 || rect.outputHeight == 0 || !foveatedCenterBlendCB)
		return;
	if (!dispatchWidth || !dispatchHeight)
		return;

	auto* blendCS = GetFoveatedCenterBlendCS();
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!blendCS || !context || !deferred || !deferred->linearSampler)
		return;
	if (dispatchOffsetX >= outputWidthPerEye || dispatchOffsetY >= outputHeight)
		return;

	dispatchWidth = std::min(dispatchWidth, outputWidthPerEye - dispatchOffsetX);
	dispatchHeight = std::min(dispatchHeight, outputHeight - dispatchOffsetY);
	if (!dispatchWidth || !dispatchHeight)
		return;

	const uint32_t rectMinX = rect.outputOffsetX;
	const uint32_t rectMinY = rect.outputOffsetY;
	const uint32_t rectMaxX = rect.outputOffsetX + rect.outputWidth;
	const uint32_t rectMaxY = rect.outputOffsetY + rect.outputHeight;

	const uint32_t dispatchMinX = std::max(dispatchOffsetX, rectMinX);
	const uint32_t dispatchMinY = std::max(dispatchOffsetY, rectMinY);
	const uint32_t dispatchMaxX = std::min(dispatchOffsetX + dispatchWidth, rectMaxX);
	const uint32_t dispatchMaxY = std::min(dispatchOffsetY + dispatchHeight, rectMaxY);
	if (dispatchMaxX <= dispatchMinX || dispatchMaxY <= dispatchMinY)
		return;

	const uint32_t actualDispatchWidth = dispatchMaxX - dispatchMinX;
	const uint32_t actualDispatchHeight = dispatchMaxY - dispatchMinY;
	const uint32_t sourceOffsetX = dispatchMinX - rectMinX;
	const uint32_t sourceOffsetY = dispatchMinY - rectMinY;

	FoveatedCenterBlendCB cbData{};
	cbData.invOutputDim = {
		outputWidthPerEye > 0 ? 1.0f / static_cast<float>(outputWidthPerEye) : 0.0f,
		outputHeight > 0 ? 1.0f / static_cast<float>(outputHeight) : 0.0f
	};
	cbData.centerScale = ClampFoveatedCenterArea(centerScale);
	cbData.centerFeather = std::isfinite(centerFeather) ? std::max(0.0f, centerFeather) : FoveatedCommon::kCenterFeather;
	cbData.centerOffset = centerOffset;
	cbData.outputOffset = { static_cast<float>(dispatchMinX), static_cast<float>(dispatchMinY) };
	cbData.dispatchDim = { static_cast<float>(actualDispatchWidth), static_cast<float>(actualDispatchHeight) };
	cbData.sourceOffset = { static_cast<float>(sourceOffsetX), static_cast<float>(sourceOffsetY) };
	cbData.invSourceDim = {
		rect.outputWidth > 0 ? 1.0f / static_cast<float>(rect.outputWidth) : 0.0f,
		rect.outputHeight > 0 ? 1.0f / static_cast<float>(rect.outputHeight) : 0.0f
	};
	cbData.pad0 = { ClampFoveatedCenterHorizontalScale(centerHorizontalScale), 0.0f };
	foveatedCenterBlendCB->Update(cbData);

	ID3D11Buffer* cb = foveatedCenterBlendCB->CB();
	ID3D11SamplerState* samplers[1] = { deferred->linearSampler };
	ID3D11ShaderResourceView* srvs[1] = { centerSRV };
	ID3D11UnorderedAccessView* uavs[1] = { outputUAV };

	context->CSSetShader(blendCS, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(0, 1, samplers);
	context->CSSetShaderResources(0, 1, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	auto state = globals::state;
	if (state && state->frameAnnotations)
		state->BeginPerfEvent("Foveated Center Blend");
	context->Dispatch((actualDispatchWidth + 7u) >> 3, (actualDispatchHeight + 7u) >> 3, 1);
	if (state && state->frameAnnotations)
		state->EndPerfEvent();

	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
	ID3D11SamplerState* nullSampler[1] = { nullptr };
	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
	context->CSSetSamplers(0, 1, nullSampler);
	context->CSSetConstantBuffers(0, 1, nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
}

bool Upscaling::DispatchSingleFoveatedVendorEye(UpscaleMethod a_upscaleMethod, uint32_t eyeIndex, ID3D11Resource* colorIn, ID3D11Resource* depthIn, ID3D11Resource* motionVectorsIn, ID3D11Resource* reactiveMaskIn, ID3D11Resource* transparencyMaskIn, uint32_t outputWidthPerEye, uint32_t outputHeight, float centerScale, float centerHorizontalScale, const float2& centerOffset, float centerFeather, uint32_t colorInputBaseOffsetX, uint32_t depthInputBaseOffsetX, uint32_t auxInputBaseOffsetX)
{
	if (a_upscaleMethod != UpscaleMethod::kDLSS)
		return false;

	if (eyeIndex > 1)
		return false;

	const auto& rect = foveatedRectCache.rects[eyeIndex];
	if (!rect.outputWidth || !rect.outputHeight || !rect.inputWidth || !rect.inputHeight)
		return false;

	const std::string suffix = eyeIndex == 0 ? "Left" : "Right";

	if (!EnsureFoveatedTexture(foveatedCenterColorIn[eyeIndex], colorIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_ColorIn_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterColorOut[eyeIndex], colorIn, rect.outputWidth, rect.outputHeight, false, true, false, false, ("Upscale_FoveatedCenter_ColorOut_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterDepth[eyeIndex], depthIn, rect.inputWidth, rect.inputHeight, true, false, false, false, ("Upscale_FoveatedCenter_Depth_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterMotionVectors[eyeIndex], motionVectorsIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_MVec_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterReactiveMask[eyeIndex], reactiveMaskIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_Reactive_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterTransparencyMask[eyeIndex], transparencyMaskIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_Transparency_" + suffix).c_str()))
		return false;

	auto context = globals::d3d::context;
	if (!context)
		return false;

	D3D11_BOX colorSrcBox{
		colorInputBaseOffsetX + rect.inputOffsetX,
		rect.inputOffsetY,
		0u,
		colorInputBaseOffsetX + rect.inputOffsetX + rect.inputWidth,
		rect.inputOffsetY + rect.inputHeight,
		1u
	};
	D3D11_BOX depthSrcBox{
		depthInputBaseOffsetX + rect.inputOffsetX,
		rect.inputOffsetY,
		0u,
		depthInputBaseOffsetX + rect.inputOffsetX + rect.inputWidth,
		rect.inputOffsetY + rect.inputHeight,
		1u
	};
	D3D11_BOX auxSrcBox{
		auxInputBaseOffsetX + rect.inputOffsetX,
		rect.inputOffsetY,
		0u,
		auxInputBaseOffsetX + rect.inputOffsetX + rect.inputWidth,
		rect.inputOffsetY + rect.inputHeight,
		1u
	};

	context->CopySubresourceRegion(foveatedCenterColorIn[eyeIndex]->resource.get(), 0, 0, 0, 0, colorIn, 0, &colorSrcBox);
	context->CopySubresourceRegion(foveatedCenterDepth[eyeIndex]->resource.get(), 0, 0, 0, 0, depthIn, 0, &depthSrcBox);
	context->CopySubresourceRegion(foveatedCenterMotionVectors[eyeIndex]->resource.get(), 0, 0, 0, 0, motionVectorsIn, 0, &auxSrcBox);
	context->CopySubresourceRegion(foveatedCenterReactiveMask[eyeIndex]->resource.get(), 0, 0, 0, 0, reactiveMaskIn, 0, &auxSrcBox);
	context->CopySubresourceRegion(foveatedCenterTransparencyMask[eyeIndex]->resource.get(), 0, 0, 0, 0, transparencyMaskIn, 0, &auxSrcBox);

	const float outputWidthPerEyeF = std::max(1.0f, static_cast<float>(outputWidthPerEye));
	const float outputHeightF = std::max(1.0f, static_cast<float>(outputHeight));
	const float rectCenterX = (static_cast<float>(rect.outputOffsetX) + static_cast<float>(rect.outputWidth) * 0.5f) / outputWidthPerEyeF;
	const float rectCenterY = (static_cast<float>(rect.outputOffsetY) + static_cast<float>(rect.outputHeight) * 0.5f) / outputHeightF;
	const float pinholeOffsetX = std::clamp((rectCenterX - 0.5f) * 2.0f, -1.0f, 1.0f);
	// Texture-space Y grows downward, while clip-space Y grows upward.
	const float pinholeOffsetY = std::clamp((0.5f - rectCenterY) * 2.0f, -1.0f, 1.0f);

	const bool dispatchOK = streamline.UpscaleRegion(
		eyeIndex,
		foveatedCenterColorIn[eyeIndex]->resource.get(),
		foveatedCenterColorOut[eyeIndex]->resource.get(),
		foveatedCenterDepth[eyeIndex]->resource.get(),
		foveatedCenterMotionVectors[eyeIndex]->resource.get(),
		foveatedCenterReactiveMask[eyeIndex]->resource.get(),
		foveatedCenterTransparencyMask[eyeIndex]->resource.get(),
		rect.inputWidth,
		rect.inputHeight,
		rect.outputWidth,
		rect.outputHeight,
		pinholeOffsetX,
		pinholeOffsetY);
	if (!dispatchOK)
		return false;

	if (!foveatedCenterColorOut[eyeIndex] || !foveatedCenterColorOut[eyeIndex]->resource || !foveatedCenterColorOut[eyeIndex]->srv)
		return false;
	if (!vrIntermediateColorOut[eyeIndex] || !vrIntermediateColorOut[eyeIndex]->uav || !vrIntermediateColorOut[eyeIndex]->resource)
		return false;

	const uint32_t rectMinX = rect.outputOffsetX;
	const uint32_t rectMinY = rect.outputOffsetY;
	const uint32_t rectMaxX = rect.outputOffsetX + rect.outputWidth;
	const uint32_t rectMaxY = rect.outputOffsetY + rect.outputHeight;

	ID3D11UnorderedAccessView* outputUAV = vrIntermediateColorOut[eyeIndex]->uav.get();
	ID3D11ShaderResourceView* centerSRV = foveatedCenterColorOut[eyeIndex]->srv.get();
	const float centerBlendFeather = std::isfinite(centerFeather) ?
		ClampPeripheryTAACenterBlendFeather(centerFeather) :
		ClampPeripheryTAACenterBlendFeather(FoveatedCommon::kCenterFeather);

	DispatchFoveatedBlendPass(
		centerSRV,
		outputUAV,
		outputWidthPerEye,
		outputHeight,
		rect,
		rectMinX,
		rectMinY,
		rectMaxX - rectMinX,
		rectMaxY - rectMinY,
		centerScale,
		centerHorizontalScale,
		centerOffset,
		centerBlendFeather);
	return true;
}

bool Upscaling::DispatchFoveatedVendorUpscaling(UpscaleMethod a_upscaleMethod, ID3D11Resource* colorTexture, ID3D11Resource* depthTexture, ID3D11Resource* motionVectors, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11ShaderResourceView* colorSRV)
{
	if (!globals::game::isVR)
		return false;
	if (a_upscaleMethod != UpscaleMethod::kDLSS)
		return false;

	if (!colorTexture || !depthTexture || !motionVectors || !reactiveMask || !transparencyMask)
		return false;

	auto state = globals::state;
	if (!state)
		return false;

	auto renderSize = Util::ConvertToDynamic(state->screenSize);
	const uint32_t outputWidthPerEye = static_cast<uint32_t>(state->screenSize.x / 2.0f);
	const uint32_t outputHeight = static_cast<uint32_t>(state->screenSize.y);
	const uint32_t inputWidthPerEye = static_cast<uint32_t>(renderSize.x / 2.0f);
	const uint32_t inputHeight = static_cast<uint32_t>(renderSize.y);

	const bool visualizeMask = settings.foveatedPeripheryMaskVisualization;
	const bool usePeripheryTAA = IsPeripheryTAAPathActive(a_upscaleMethod);
	const bool usePeripheryTAAProfile = IsPeripheryTAAEnabled(a_upscaleMethod);
	const auto foveatedProfile = GetFoveatedMaskProfileParams(settings, usePeripheryTAAProfile);
	const float centerScale = foveatedProfile.centerArea;
	const float centerHorizontalScale = foveatedProfile.centerHorizontalScale;
	const float effectiveCenterBlendFeather = usePeripheryTAA ? ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather) : FoveatedCommon::kCenterFeather;
	if (!BuildFoveatedDispatchRects(inputWidthPerEye, inputHeight, outputWidthPerEye, outputHeight, true, centerScale, effectiveCenterBlendFeather, centerHorizontalScale, usePeripheryTAAProfile))
		return false;

	auto* peripheryCS = usePeripheryTAA ? nullptr : GetFoveatedPeripheryCS();
	auto* peripheryTAA = usePeripheryTAA ? GetPeripheryTAACS() : nullptr;
	auto* blendCS = visualizeMask ? nullptr : GetFoveatedCenterBlendCS();
	if ((usePeripheryTAA && (!peripheryTAA || !peripheryTAACB)) ||
		(!usePeripheryTAA && (!peripheryCS || !foveatedPeripheryCB)) ||
		(!visualizeMask && (!blendCS || !foveatedCenterBlendCB)))
		return false;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	auto renderer = globals::game::renderer;
	if (!context || !deferred || !deferred->linearSampler || !renderer)
		return false;

	const bool useDirectSourcePath = settings.qualityMode == 0 && colorSRV != nullptr && !usePeripheryTAA;
	// In periphery-TAA mode the outside-ring fill must come from per-eye, HMD-cleared color,
	// otherwise hidden-area clear color can leak as side/bottom bars when TAA range < 1.0.
	// For non-TAA, keep combined-source sampling only on the direct-source (DLAA/native) path.
	// DLSS quality modes should sample per-eye inputs so hidden-area data cannot leak into periphery fills.
	const bool useCombinedPeripherySource = useDirectSourcePath;
	const bool requireFullPerEyeInputs = usePeripheryTAA || (!usePeripheryTAA && !useDirectSourcePath);
	if (requireFullPerEyeInputs) {
		PreparePerEyeInputs(colorTexture, depthTexture, motionVectors, reactiveMask, transparencyMask, false, true);
	}
	if (usePeripheryTAA && !EnsurePeripheryTAAResources(outputWidthPerEye, outputHeight, colorTexture))
		return false;

	const bool resetPeripheryTAA = usePeripheryTAA && (ShouldResetHistoryThisFrame() || !peripheryTAAHistoryValid);
	const uint32_t peripheryTAAWriteIndex = 1u - peripheryTAAHistoryReadIndex;

	for (uint32_t eye = 0; eye < 2; ++eye) {
		float4x4 currentViewProjInverse{};
		float4x4 previousViewProj{};
		float4 currentCameraPosAdjust{};
		float4 previousCameraPosAdjust{};
		if (usePeripheryTAA) {
			currentViewProjInverse = globals::game::frameBufferCached.GetCameraViewProjUnjittered(eye).Invert();
			previousViewProj = globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered(eye);
			currentCameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust(eye);
			previousCameraPosAdjust = globals::game::frameBufferCached.GetCameraPreviousPosAdjust(eye);
		}
		const float2 centerOffset = GetResolvedFoveatedMaskCenterOffset(eye, usePeripheryTAAProfile);
		const float taaOuterScale = usePeripheryTAA ? ClampPeripheryTAAOuterScaleForCenter(
			settings.periphery_taa_outer_scale,
			centerScale,
			centerHorizontalScale,
			effectiveCenterBlendFeather) :
			0.0f;
		const auto innerBounds = FoveatedCommon::BuildCenteredInscribedMaskRect(outputWidthPerEye, outputHeight, centerScale, centerOffset.x, centerOffset.y, centerHorizontalScale);
		const uint32_t innerMinX = static_cast<uint32_t>(innerBounds.minX);
		const uint32_t innerMaxX = static_cast<uint32_t>(innerBounds.maxX);
		const uint32_t innerMinY = static_cast<uint32_t>(innerBounds.minY);
		const uint32_t innerMaxY = static_cast<uint32_t>(innerBounds.maxY);
		const bool hasCenterInterior = innerMaxX > innerMinX && innerMaxY > innerMinY;
		constexpr uint32_t kCenterUnderlayHolePadding = 2u;
		const uint32_t underlayHoleMinX = hasCenterInterior ? std::min(innerMinX + kCenterUnderlayHolePadding, innerMaxX) : 0u;
		const uint32_t underlayHoleMinY = hasCenterInterior ? std::min(innerMinY + kCenterUnderlayHolePadding, innerMaxY) : 0u;
		const uint32_t underlayHoleMaxX = hasCenterInterior ? (innerMaxX > kCenterUnderlayHolePadding ? innerMaxX - kCenterUnderlayHolePadding : innerMinX) : 0u;
		const uint32_t underlayHoleMaxY = hasCenterInterior ? (innerMaxY > kCenterUnderlayHolePadding ? innerMaxY - kCenterUnderlayHolePadding : innerMinY) : 0u;
		const bool hasCenterUnderlayHole = underlayHoleMaxX > underlayHoleMinX && underlayHoleMaxY > underlayHoleMinY;
		const auto taaOuterBounds = FoveatedCommon::BuildCenteredDispatchBounds(0, outputWidthPerEye, outputHeight, taaOuterScale, centerOffset.x, centerOffset.y, 0.0f, centerHorizontalScale);
		const uint32_t taaOuterMinX = static_cast<uint32_t>(taaOuterBounds.minX);
		const uint32_t taaOuterMaxX = static_cast<uint32_t>(taaOuterBounds.maxX);
		const uint32_t taaOuterMinY = static_cast<uint32_t>(taaOuterBounds.minY);
		const uint32_t taaOuterMaxY = static_cast<uint32_t>(taaOuterBounds.maxY);
		const bool hasTaaOuterRegion = taaOuterMaxX > taaOuterMinX && taaOuterMaxY > taaOuterMinY;
		// Catmull-Rom history taps and 3x3 neighborhood sampling need a small pad
		// around the TAA outer region to keep transition-band history coherent.
		constexpr uint32_t kPeripheryHistoryPadding = 2u;
		const uint32_t taaHistoryMinX = hasTaaOuterRegion ? (taaOuterMinX > kPeripheryHistoryPadding ? taaOuterMinX - kPeripheryHistoryPadding : 0u) : 0u;
		const uint32_t taaHistoryMinY = hasTaaOuterRegion ? (taaOuterMinY > kPeripheryHistoryPadding ? taaOuterMinY - kPeripheryHistoryPadding : 0u) : 0u;
		const uint32_t taaHistoryMaxX = hasTaaOuterRegion ? std::min(outputWidthPerEye, taaOuterMaxX + kPeripheryHistoryPadding) : 0u;
		const uint32_t taaHistoryMaxY = hasTaaOuterRegion ? std::min(outputHeight, taaOuterMaxY + kPeripheryHistoryPadding) : 0u;
		const bool hasTaaHistoryRegion = taaHistoryMaxX > taaHistoryMinX && taaHistoryMaxY > taaHistoryMinY;
		const uint32_t taaDispatchMinX = hasTaaHistoryRegion ? taaHistoryMinX : taaOuterMinX;
		const uint32_t taaDispatchMinY = hasTaaHistoryRegion ? taaHistoryMinY : taaOuterMinY;
		const uint32_t taaDispatchMaxX = hasTaaHistoryRegion ? taaHistoryMaxX : taaOuterMaxX;
		const uint32_t taaDispatchMaxY = hasTaaHistoryRegion ? taaHistoryMaxY : taaOuterMaxY;

		if (!vrIntermediateColorOut[eye] || !vrIntermediateColorOut[eye]->uav || !vrIntermediateColorOut[eye]->resource) {
			return false;
		}
		if (!visualizeMask &&
			(!vrIntermediateMotionVectors[eye] || !vrIntermediateReactiveMask[eye] || !vrIntermediateTransparencyMask[eye])) {
			return false;
		}
		if (usePeripheryTAA) {
			if (!vrIntermediateColorIn[eye] || !vrIntermediateColorIn[eye]->srv || !vrIntermediateColorIn[eye]->resource ||
				!vrIntermediateDepth[eye] || !vrIntermediateDepth[eye]->srv || !vrIntermediateDepth[eye]->resource) {
				return false;
			}
		} else {
			if (!useDirectSourcePath && (!vrIntermediateColorIn[eye] || !vrIntermediateColorIn[eye]->srv || !vrIntermediateColorIn[eye]->resource))
				return false;
			if (!visualizeMask && !useDirectSourcePath && (!vrIntermediateDepth[eye] || !vrIntermediateDepth[eye]->srv || !vrIntermediateDepth[eye]->resource))
				return false;
		}

		ID3D11ShaderResourceView* peripherySourceSRV = useCombinedPeripherySource ? colorSRV : vrIntermediateColorIn[eye]->srv.get();
		const uint32_t peripherySourceWidth = useCombinedPeripherySource ? (inputWidthPerEye * 2u) : inputWidthPerEye;
		const uint32_t peripherySourceHeight = inputHeight;
		const float peripherySourceScaleX = useCombinedPeripherySource ? 0.5f : 1.0f;
		const float peripherySourceScaleY = 1.0f;
		const float peripherySourceOffsetX = useCombinedPeripherySource ? (eye == 1 ? 0.5f : 0.0f) : 0.0f;
		const float peripherySourceOffsetY = 0.0f;
		ID3D11UnorderedAccessView* outputColorUAV = vrIntermediateColorOut[eye]->uav.get();

		bool peripheryBindingsBound = false;
		auto bindPeripheryBindings = [&]() -> bool {
			if (peripheryBindingsBound)
				return true;

			auto* peripheryShader = GetFoveatedPeripheryCS();
			if (!peripheryShader || !foveatedPeripheryCB || !deferred || !deferred->linearSampler || !peripherySourceSRV || !outputColorUAV)
				return false;

			ID3D11Buffer* cb = foveatedPeripheryCB->CB();
			ID3D11SamplerState* samplers[1] = { deferred->linearSampler };
			ID3D11ShaderResourceView* srvs[1] = { peripherySourceSRV };
			ID3D11UnorderedAccessView* uavs[1] = { outputColorUAV };
			context->CSSetShader(peripheryShader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &cb);
			context->CSSetSamplers(0, 1, samplers);
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			peripheryBindingsBound = true;
			return true;
		};

		auto unbindPeripheryBindings = [&]() {
			if (!peripheryBindingsBound)
				return;

			ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
			ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
			ID3D11SamplerState* nullSampler[1] = { nullptr };
			ID3D11Buffer* nullCB[1] = { nullptr };
			context->CSSetShaderResources(0, 1, nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
			context->CSSetSamplers(0, 1, nullSampler);
			context->CSSetConstantBuffers(0, 1, nullCB);
			context->CSSetShader(nullptr, nullptr, 0);
			peripheryBindingsBound = false;
		};

		auto dispatchPeripheryBand = [&](uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight) {
			if (!dispatchWidth || !dispatchHeight)
				return;
			if (!bindPeripheryBindings())
				return;

			DispatchFoveatedPeripheryPass(
				peripherySourceSRV,
				vrIntermediateColorOut[eye]->uav.get(),
				peripherySourceWidth,
				peripherySourceHeight,
				outputWidthPerEye,
				outputHeight,
				outputOffsetX,
				outputOffsetY,
				dispatchWidth,
				dispatchHeight,
				centerScale,
				centerHorizontalScale,
				true,
				peripherySourceScaleX,
				peripherySourceScaleY,
				peripherySourceOffsetX,
				peripherySourceOffsetY,
				centerOffset.x,
				centerOffset.y);
		};

		auto dispatchPeripheryTAA = [&](ID3D11ShaderResourceView* tileListSRV, uint32_t tileCount, uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight) {
			DispatchPeripheryTAAPass(
				vrIntermediateColorIn[eye]->srv.get(),
				vrIntermediateDepth[eye]->srv.get(),
				vrIntermediateMotionVectors[eye]->srv.get(),
				vrIntermediateReactiveMask[eye]->srv.get(),
				vrIntermediateTransparencyMask[eye]->srv.get(),
				peripheryTAAHistoryColor[eye][peripheryTAAHistoryReadIndex]->srv.get(),
				peripheryTAAVelocityHistory[eye][peripheryTAAHistoryReadIndex]->srv.get(),
				peripheryTAALockHistory[eye][peripheryTAAHistoryReadIndex]->srv.get(),
				outputColorUAV,
				peripheryTAAHistoryColor[eye][peripheryTAAWriteIndex]->uav.get(),
				peripheryTAAVelocityHistory[eye][peripheryTAAWriteIndex]->uav.get(),
				peripheryTAALockHistory[eye][peripheryTAAWriteIndex]->uav.get(),
				tileListSRV,
				tileCount,
				inputWidthPerEye,
				inputHeight,
				outputWidthPerEye,
				outputHeight,
				outputOffsetX,
				outputOffsetY,
				dispatchWidth,
				dispatchHeight,
				currentViewProjInverse,
				previousViewProj,
				currentCameraPosAdjust,
				previousCameraPosAdjust,
				resetPeripheryTAA,
				centerScale,
				centerHorizontalScale,
				centerOffset.x,
				centerOffset.y);
		};

		auto dispatchPeripheryTAABand = [&](uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight) {
			if (!dispatchWidth || !dispatchHeight)
				return;
			dispatchPeripheryTAA(nullptr, 0, outputOffsetX, outputOffsetY, dispatchWidth, dispatchHeight);
		};

		auto dispatchRectMinusHole = [&](uint32_t outerMinX, uint32_t outerMinY, uint32_t outerMaxX, uint32_t outerMaxY, uint32_t holeMinX, uint32_t holeMinY, uint32_t holeMaxX, uint32_t holeMaxY, auto&& dispatchBand) {
			if (outerMaxX <= outerMinX || outerMaxY <= outerMinY)
				return;

			const uint32_t clampedHoleMinX = std::clamp(holeMinX, outerMinX, outerMaxX);
			const uint32_t clampedHoleMaxX = std::clamp(holeMaxX, outerMinX, outerMaxX);
			const uint32_t clampedHoleMinY = std::clamp(holeMinY, outerMinY, outerMaxY);
			const uint32_t clampedHoleMaxY = std::clamp(holeMaxY, outerMinY, outerMaxY);
			const bool hasHole = clampedHoleMaxX > clampedHoleMinX && clampedHoleMaxY > clampedHoleMinY;
			if (!hasHole) {
				dispatchBand(outerMinX, outerMinY, outerMaxX - outerMinX, outerMaxY - outerMinY);
				return;
			}

			const uint32_t outerWidth = outerMaxX - outerMinX;
			const uint32_t middleHeight = clampedHoleMaxY - clampedHoleMinY;
			dispatchBand(outerMinX, outerMinY, outerWidth, clampedHoleMinY - outerMinY);
			dispatchBand(outerMinX, clampedHoleMaxY, outerWidth, outerMaxY - clampedHoleMaxY);
			dispatchBand(outerMinX, clampedHoleMinY, clampedHoleMinX - outerMinX, middleHeight);
			dispatchBand(clampedHoleMaxX, clampedHoleMinY, outerMaxX - clampedHoleMaxX, middleHeight);
		};

		if (usePeripheryTAA) {
			if (!peripheryTAAHistoryColor[eye][peripheryTAAHistoryReadIndex] || !peripheryTAAHistoryColor[eye][peripheryTAAHistoryReadIndex]->srv ||
				!peripheryTAAHistoryColor[eye][peripheryTAAWriteIndex] || !peripheryTAAHistoryColor[eye][peripheryTAAWriteIndex]->uav ||
				!peripheryTAAVelocityHistory[eye][peripheryTAAHistoryReadIndex] || !peripheryTAAVelocityHistory[eye][peripheryTAAHistoryReadIndex]->srv ||
				!peripheryTAAVelocityHistory[eye][peripheryTAAWriteIndex] || !peripheryTAAVelocityHistory[eye][peripheryTAAWriteIndex]->uav ||
				!peripheryTAALockHistory[eye][peripheryTAAHistoryReadIndex] || !peripheryTAALockHistory[eye][peripheryTAAHistoryReadIndex]->srv ||
				!peripheryTAALockHistory[eye][peripheryTAAWriteIndex] || !peripheryTAALockHistory[eye][peripheryTAAWriteIndex]->uav) {
				return false;
			}

		}

		if (usePeripheryTAA) {
			if (hasTaaOuterRegion) {
				uint32_t tileCount = 0;
				const bool tileListBuilt = BuildPeripheryTAATileList(eye, outputWidthPerEye, outputHeight, centerScale, taaOuterScale, centerHorizontalScale, effectiveCenterBlendFeather, centerOffset.x, centerOffset.y, kPeripheryHistoryPadding, tileCount);
				const bool hasTileListSRV = peripheryTAATileBuffer[eye] && peripheryTAATileBuffer[eye]->srv;
				if (tileListBuilt && tileCount > 0 && hasTileListSRV) {
					dispatchPeripheryTAA(peripheryTAATileBuffer[eye]->srv.get(), tileCount, 0, 0, outputWidthPerEye, outputHeight);
				} else if (!tileListBuilt || tileCount == 0 || (tileCount > 0 && !hasTileListSRV)) {
					if (state->frameAnnotations)
						state->BeginPerfEvent("Periphery TAA Fallback Rect");
					if (hasCenterUnderlayHole) {
						dispatchRectMinusHole(
							taaDispatchMinX,
							taaDispatchMinY,
							taaDispatchMaxX,
							taaDispatchMaxY,
							underlayHoleMinX,
							underlayHoleMinY,
							underlayHoleMaxX,
							underlayHoleMaxY,
							dispatchPeripheryTAABand);
					} else {
						dispatchPeripheryTAABand(taaDispatchMinX, taaDispatchMinY, taaDispatchMaxX - taaDispatchMinX, taaDispatchMaxY - taaDispatchMinY);
					}
					if (state->frameAnnotations)
						state->EndPerfEvent();
				}

				// Fill outside the Peripheral TAA range so every visible per-eye
				// output pixel is initialized before the DLSS center blend.
				dispatchRectMinusHole(
					0,
					0,
					outputWidthPerEye,
					outputHeight,
					taaOuterMinX,
					taaOuterMinY,
					taaOuterMaxX,
					taaOuterMaxY,
					dispatchPeripheryBand);
			} else {
				dispatchPeripheryBand(0, 0, outputWidthPerEye, outputHeight);
			}
		} else if (visualizeMask) {
			dispatchPeripheryBand(0, 0, outputWidthPerEye, outputHeight);
		} else if (hasCenterUnderlayHole) {
			dispatchRectMinusHole(
				0,
				0,
				outputWidthPerEye,
				outputHeight,
				underlayHoleMinX,
				underlayHoleMinY,
				underlayHoleMaxX,
				underlayHoleMaxY,
				dispatchPeripheryBand);
		} else {
			dispatchPeripheryBand(0, 0, outputWidthPerEye, outputHeight);
		}

		unbindPeripheryBindings();

		if (visualizeMask)
			continue;

		const bool useCombinedCenterInputs = usePeripheryTAA || useDirectSourcePath;
		ID3D11Resource* centerColorInput = useCombinedCenterInputs ? colorTexture : vrIntermediateColorIn[eye]->resource.get();
		ID3D11Resource* centerDepthInput = useCombinedCenterInputs ? depthTexture : vrIntermediateDepth[eye]->resource.get();
		const uint32_t combinedEyeInputOffsetX = eye * inputWidthPerEye;

		if (!DispatchSingleFoveatedVendorEye(
				a_upscaleMethod,
				eye,
				centerColorInput,
				centerDepthInput,
				vrIntermediateMotionVectors[eye]->resource.get(),
				vrIntermediateReactiveMask[eye]->resource.get(),
				vrIntermediateTransparencyMask[eye]->resource.get(),
				outputWidthPerEye,
				outputHeight,
				centerScale,
				centerHorizontalScale,
				centerOffset,
				effectiveCenterBlendFeather,
				useCombinedCenterInputs ? combinedEyeInputOffsetX : 0u,
				useCombinedCenterInputs ? combinedEyeInputOffsetX : 0u,
				0u)) {
			return false;
		}

	}

	if (usePeripheryTAA) {
		peripheryTAAHistoryReadIndex = peripheryTAAWriteIndex;
		peripheryTAAHistoryValid = true;
	}

	FinalizePerEyeOutputs(colorTexture);
	return true;
}

void Upscaling::CreateVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
	ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc)
{
	// All buffers are per-eye: Streamline validates all extents against the input color texture
	// dimensions, so every tagged resource must be isolated per-eye at {0,0}.
	for (int i = 0; i < 2; i++) {
		std::string suffix = (i == 0) ? "Left" : "Right";

		vrIntermediateColorIn[i] = CreateTextureFromSource(colorSrc, inWidth, inHeight, false, true, true, ("Upscale_ColorIn_" + suffix).c_str());
		vrIntermediateColorOut[i] = CreateTextureFromSource(colorSrc, outWidth, outHeight, false, true, true, ("Upscale_ColorOut_" + suffix).c_str());

		// Depth: R32_TYPELESS base (matches kMAIN), with R32_FLOAT SRV for ClearHMDMaskCS.
		// CopySubresourceRegion requires matching typeless formats; SRV reinterprets as R32_FLOAT.
		{
			D3D11_TEXTURE2D_DESC depthDesc = {};
			depthDesc.Width = inWidth;
			depthDesc.Height = inHeight;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			vrIntermediateDepth[i] = eastl::make_unique<Texture2D>(depthDesc);

			Util::SetResourceName(vrIntermediateDepth[i]->resource.get(), ("Upscale_Depth_" + suffix).c_str());

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			vrIntermediateDepth[i]->CreateSRV(srvDesc);
		}

		vrIntermediateMotionVectors[i] = CreateTextureFromSource(mvecSrc, inWidth, inHeight, false, true, true, ("Upscale_MVec_" + suffix).c_str());
		vrIntermediateReactiveMask[i] = CreateTextureFromSource(reactiveSrc, inWidth, inHeight, false, true, true, ("Upscale_Reactive_" + suffix).c_str());
		vrIntermediateTransparencyMask[i] = CreateTextureFromSource(transparencySrc, inWidth, inHeight, false, true, true, ("Upscale_Transparency_" + suffix).c_str());
	}

	logger::info("[Upscaling] Created VR intermediate textures: per-eye in {}x{}, out {}x{}",
		inWidth, inHeight, outWidth, outHeight);
}

void Upscaling::EnsureVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
	ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc)
{
	bool hasAllIntermediates =
		vrIntermediateColorIn[0] && vrIntermediateColorIn[1] &&
		vrIntermediateColorOut[0] && vrIntermediateColorOut[1] &&
		vrIntermediateDepth[0] && vrIntermediateDepth[1] &&
		vrIntermediateMotionVectors[0] && vrIntermediateMotionVectors[1] &&
		vrIntermediateReactiveMask[0] && vrIntermediateReactiveMask[1] &&
		vrIntermediateTransparencyMask[0] && vrIntermediateTransparencyMask[1];

	bool needsRecreate = !hasAllIntermediates;
	if (!needsRecreate) {
		needsRecreate =
			(vrIntermediateColorIn[0]->desc.Width != inWidth || vrIntermediateColorIn[0]->desc.Height != inHeight ||
				vrIntermediateColorOut[0]->desc.Width != outWidth || vrIntermediateColorOut[0]->desc.Height != outHeight ||
				!vrIntermediateColorOut[0]->uav || !vrIntermediateColorOut[1]->uav ||
				!vrIntermediateMotionVectors[0]->uav || !vrIntermediateMotionVectors[1]->uav ||
				!vrIntermediateReactiveMask[0]->uav || !vrIntermediateReactiveMask[1]->uav ||
				!vrIntermediateTransparencyMask[0]->uav || !vrIntermediateTransparencyMask[1]->uav);
	}

	if (needsRecreate) {
		logger::info("[Upscaling] (Re)creating VR intermediates: per-eye in {}x{}, out {}x{}",
			inWidth, inHeight, outWidth, outHeight);
		CreateVRIntermediateTextures(inWidth, inHeight, outWidth, outHeight, colorSrc, mvecSrc, reactiveSrc, transparencySrc);
	}
}

void Upscaling::PreparePerEyeInputs(ID3D11Resource* colorSrc, ID3D11Resource* depthSrc, ID3D11Resource* mvecSrc,
	ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc, bool copyAuxiliaryInputs, bool copyDepthInput)
{
	if (!globals::game::isVR)
		return;

	auto state = globals::state;
	if (state->frameAnnotations)
		state->BeginPerfEvent("VR Upscaling Prepare");

	auto context = globals::d3d::context;
	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	uint32_t eyeWidthOut = (uint32_t)(screenSize.x / 2);
	uint32_t eyeHeightOut = (uint32_t)screenSize.y;
	uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
	uint32_t eyeHeightIn = (uint32_t)renderSize.y;

	EnsureVRIntermediateTextures(eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut, colorSrc, mvecSrc, reactiveSrc, transparencySrc);

	// Extract both eyes' required inputs from combined stereo buffers.
	// Reactive / transparency / encoded motion vectors can be pre-generated directly per-eye by the encode pass.
	for (uint32_t i = 0; i < 2; ++i) {
		uint32_t offsetXIn = (i == 1) ? eyeWidthIn : 0;
		D3D11_BOX srcBox = { offsetXIn, 0, 0, offsetXIn + eyeWidthIn, eyeHeightIn, 1 };

		context->CopySubresourceRegion(vrIntermediateColorIn[i]->resource.get(), 0, 0, 0, 0, colorSrc, 0, &srcBox);
		if (copyDepthInput)
			context->CopySubresourceRegion(vrIntermediateDepth[i]->resource.get(), 0, 0, 0, 0, depthSrc, 0, &srcBox);
		if (copyAuxiliaryInputs) {
			context->CopySubresourceRegion(vrIntermediateMotionVectors[i]->resource.get(), 0, 0, 0, 0, mvecSrc, 0, &srcBox);
			context->CopySubresourceRegion(vrIntermediateTransparencyMask[i]->resource.get(), 0, 0, 0, 0, transparencySrc, 0, &srcBox);
			context->CopySubresourceRegion(vrIntermediateReactiveMask[i]->resource.get(), 0, 0, 0, 0, reactiveSrc, 0, &srcBox);
		}
	}

	// Zero color where depth == 0 (HMD hidden area) in each per-eye buffer.
	// Bind CS/SRV/CB once for both eyes to reduce per-frame CPU overhead.
	auto& depthTexture = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!vrClearHMDMaskCS) {
		vrClearHMDMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/ClearHMDMaskCS.hlsl", {}, "cs_5_0"));

		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = 32;  // 8 uints
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = 0;
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, vrClearHMDMaskCB.put()));
	}

	if (vrClearHMDMaskCS && vrClearHMDMaskCB) {
		auto dispatchX = (eyeWidthIn + 7) / 8;
		auto dispatchY = (eyeHeightIn + 7) / 8;

		context->CSSetShader(vrClearHMDMaskCS.get(), nullptr, 0);

		ID3D11ShaderResourceView* srvs[1] = { depthTexture.depthSRV };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11Buffer* cbs[1] = { vrClearHMDMaskCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t depthOffset = (i == 1) ? eyeWidthIn : 0;
			uint32_t clearMaskParams[8] = {
				depthOffset,
				0,
				0,
				0,
				eyeWidthIn,
				eyeHeightIn,
				eyeWidthIn,
				eyeHeightIn
			};
			context->UpdateSubresource(vrClearHMDMaskCB.get(), 0, nullptr, clearMaskParams, 0, 0);

			ID3D11UnorderedAccessView* uavs[1] = { vrIntermediateColorIn[i]->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->Dispatch(dispatchX, dispatchY, 1);
		}

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	if (state->frameAnnotations)
		state->EndPerfEvent();
}

void Upscaling::FinalizePerEyeOutputs(ID3D11Resource* colorDst)
{
	if (!globals::game::isVR)
		return;

	auto state = globals::state;
	if (state->frameAnnotations)
		state->BeginPerfEvent("VR Upscaling Finalize");

	auto context = globals::d3d::context;
	auto screenSize = state->screenSize;

	uint32_t eyeWidthOut = (uint32_t)(screenSize.x / 2);
	uint32_t eyeHeightOut = (uint32_t)screenSize.y;

	// Write upscaled outputs back
	for (uint32_t i = 0; i < 2; ++i) {
		uint32_t offsetXOut = (i == 1) ? eyeWidthOut : 0;
		D3D11_BOX outBox = { 0, 0, 0, eyeWidthOut, eyeHeightOut, 1 };
		context->CopySubresourceRegion(colorDst, 0, offsetXOut, 0, 0, vrIntermediateColorOut[i]->resource.get(), 0, &outBox);
	}

	if (state->frameAnnotations)
		state->EndPerfEvent();
}

void Upscaling::ClearHMDMask(ID3D11UnorderedAccessView* colorUAV, ID3D11ShaderResourceView* depthSRV,
	uint32_t eyeWidth, uint32_t eyeHeight, uint32_t depthOffsetX, uint32_t colorOffsetX, uint32_t depthOffsetY, uint32_t colorOffsetY)
{
	if (!globals::game::isVR)
		return;

	auto context = globals::d3d::context;

	if (!vrClearHMDMaskCS) {
		vrClearHMDMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/ClearHMDMaskCS.hlsl", {}, "cs_5_0"));

		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = 32;  // 8 uints
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = 0;
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, vrClearHMDMaskCB.put()));
	}

	if (vrClearHMDMaskCS) {
		auto dispatchX = (eyeWidth + 7) / 8;
		auto dispatchY = (eyeHeight + 7) / 8;

		context->CSSetShader(vrClearHMDMaskCS.get(), nullptr, 0);

		ID3D11ShaderResourceView* srvs[1] = { depthSRV };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11UnorderedAccessView* uavs[1] = { colorUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		uint32_t clearMaskParams[8] = {
			depthOffsetX,
			colorOffsetX,
			depthOffsetY,
			colorOffsetY,
			eyeWidth,
			eyeHeight,
			eyeWidth,
			eyeHeight
		};
		context->UpdateSubresource(vrClearHMDMaskCB.get(), 0, nullptr, clearMaskParams, 0, 0);

		ID3D11Buffer* cbs[1] = { vrClearHMDMaskCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		context->Dispatch(dispatchX, dispatchY, 1);

		// Unbind
		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

int32_t GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}

// Calculate halton number for index and base.
static float Halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {
		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}

void GetJitterOffset(float* outX, float* outY, int32_t index, int32_t phaseCount)
{
	const float x = Halton((index % phaseCount) + 1, 2) - 0.5f;
	const float y = Halton((index % phaseCount) + 1, 3) - 0.5f;

	*outX = x;
	*outY = y;
}

void Upscaling::ConfigureTAA()
{
	auto upscaleMethod = GetUpscaleMethod();

	// When no upscaling method is active, preserve vanilla TAA state.
	// UpdateJitter (called immediately after this hook) owns the non-upscaling path.
	if (upscaleMethod == UpscaleMethod::kNONE)
		return;

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);

	// CS TAA replaces vanilla TAA, so disable water TAA there.
	// FSR/DLSS keep water TAA enabled.
	bool* enableWaterTAA = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISTemporalAA) + 0x38LL);
	*enableWaterTAA = upscaleMethod != UpscaleMethod::kTAA;

	BSImagespaceShaderISTemporalAA->taaEnabled = true;
}

void Upscaling::ConfigureUpscaling(RE::BSGraphics::State* a_viewport)
{
	auto upscaleMethod = GetUpscaleMethod();

	// Delete or create resources as necessary
	CheckResources(upscaleMethod);

	// Cache original TAA values for UI
	projectionPosScaleX = a_viewport->projectionPosScaleX;
	projectionPosScaleY = a_viewport->projectionPosScaleY;

	// Get full screen size
	auto state = globals::state;
	auto screenSize = state->screenSize;

	auto screenWidth = static_cast<int>(screenSize.x);
	auto screenHeight = static_cast<int>(screenSize.y);

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		float resolutionScaleBase = 1.0f / ffxFsr3GetUpscaleRatioFromQualityMode((FfxFsr3QualityMode)settings.qualityMode);

		auto renderWidth = static_cast<int>(screenWidth * resolutionScaleBase);
		auto renderHeight = static_cast<int>(screenHeight * resolutionScaleBase);

		resolutionScale.x = static_cast<float>(renderWidth) / static_cast<float>(screenWidth);
		resolutionScale.y = static_cast<float>(renderHeight) / static_cast<float>(screenHeight);

		auto phaseCount = GetJitterPhaseCount(renderWidth, screenWidth);

		GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);

		if (globals::game::isVR)
			a_viewport->projectionPosScaleX = -jitter.x / renderWidth;
		else
			a_viewport->projectionPosScaleX = -2.0f * jitter.x / renderWidth;

		a_viewport->projectionPosScaleY = 2.0f * jitter.y / renderHeight;
	} else {
		resolutionScale = { 1.0f, 1.0f };

		if (globals::game::isVR)
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth;
		else
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth / 2.0f;

		jitter.y = a_viewport->projectionPosScaleY * screenHeight / 2.0f;
	}

	auto& runtimeData = a_viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = dynamicResolutionWidthRatio;
	runtimeData.dynamicResolutionPreviousHeightRatio = dynamicResolutionHeightRatio;
	runtimeData.dynamicResolutionWidthRatio = resolutionScale.x;
	runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;

	dynamicResolutionWidthRatio = resolutionScale.x;
	dynamicResolutionHeightRatio = resolutionScale.y;

	// Disable dynamic resolution unless the game explicitly enables it
	if (!globals::game::isVR)
		runtimeData.dynamicResolutionLock = 1;
}

void Upscaling::SetupResources()
{
	QueryPerformanceFrequency(&qpf);

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
	depthStencilDesc.DepthEnable = true;                           // Enable depth testing
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;  // Write to all depth bits
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;          // Always pass depth test (write all depths)

	if (globals::game::isVR) {
		depthStencilDesc.StencilEnable = true;     // Enable stencil testing
		depthStencilDesc.StencilReadMask = 0xFF;   // Read all stencil bits
		depthStencilDesc.StencilWriteMask = 0xFF;  // Write to all stencil bits

		// Configure front-facing stencil operations
		depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;       // Replace on stencil fail
		depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;  // Replace on depth fail
		depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;    // Replace on pass
		depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;       // Always pass stencil test

		// Configure back-facing stencil operations (same as front)
		depthStencilDesc.BackFace.StencilFailOp = depthStencilDesc.FrontFace.StencilFailOp;
		depthStencilDesc.BackFace.StencilDepthFailOp = depthStencilDesc.FrontFace.StencilDepthFailOp;
		depthStencilDesc.BackFace.StencilPassOp = depthStencilDesc.FrontFace.StencilPassOp;
		depthStencilDesc.BackFace.StencilFunc = depthStencilDesc.FrontFace.StencilFunc;
	} else {
		depthStencilDesc.StencilEnable = false;  // Disable stencil testing
	}

	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthStencilDesc, upscaleDepthStencilState.put()));

	// Create jitter offset constant buffer for depth upscaling
	jitterCB = new ConstantBuffer(ConstantBufferDesc<JitterCB>());

	// Create upscaling data constant buffer for encode textures compute shader
	upscalingDataCB = new ConstantBuffer(ConstantBufferDesc<UpscalingDataCB>());
	foveatedPeripheryCB = new ConstantBuffer(ConstantBufferDesc<FoveatedPeripheryCB>());
	foveatedCenterBlendCB = new ConstantBuffer(ConstantBufferDesc<FoveatedCenterBlendCB>());
	peripheryTAACB = new ConstantBuffer(ConstantBufferDesc<PeripheryTAACB>());

	// Create blend state for depth upscaling
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = false;
	blendDesc.IndependentBlendEnable = false;
	blendDesc.RenderTarget[0].BlendEnable = false;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, upscaleBlendState.put()));

	// Create rasterizer state for fullscreen rendering
	D3D11_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.FrontCounterClockwise = false;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, upscaleRasterizerState.put()));

	CheckResources(GetUpscaleMethod());

	rcas.Initialize();

	if (d3d12SwapChainActive)
		dx12SwapChain.CreateSharedResources();

	copyDepthToSharedBufferPS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
}

void Upscaling::ClearShaderCache()
{
	for (int i = 0; i < 5; ++i) {
		encodeTexturesCS[i] = nullptr;  // com_ptr automatically releases
	}

	depthRefractionUpscalePS = nullptr;  // com_ptr automatically releases
	underwaterMaskUpscalePS = nullptr;   // com_ptr automatically releases
	upscaleVS = nullptr;                 // com_ptr automatically releases
	foveatedPeripheryCS = nullptr;       // com_ptr automatically releases
	foveatedCenterBlendCS = nullptr;     // com_ptr automatically releases
	peripheryTAACS = nullptr;            // com_ptr automatically releases
}

void Upscaling::CopySharedD3D12Resources()
{
	globals::state->BeginPerfEvent("Copy Shared D3D12 Resources");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	context->CopyResource(dx12SwapChain.motionVectorBufferShared12->resource11, motionVector.texture);

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	{
		// Set up viewport for fullscreen rendering
		auto screenSize = globals::state->screenSize;

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set up Input Assembler for fullscreen triangle
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader
		context->VSSetShader(GetUpscaleVS(), nullptr, 0);

		// Set up rasterizer and blend states
		context->RSSetState(upscaleRasterizerState.get());
		context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

		// Set up pixel shader resources
		ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(views), views);

		// Set render target view for pixel shader output
		ID3D11RenderTargetView* rtvs[1] = { dx12SwapChain.depthBufferShared12->rtv };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(copyDepthToSharedBufferPS.get(), nullptr, 0);

		context->Draw(3, 0);
	}

	// Clean up
	ID3D11ShaderResourceView* views[1] = { nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(views), views);

	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetShader(nullptr, nullptr, 0);

	globals::state->EndPerfEvent();
}

void UpdateCameraData()
{
	using func_t = decltype(&UpdateCameraData);
	static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
	func();
}

void Upscaling::PostDisplay()
{
	auto viewport = globals::game::graphicsState;

	viewport->projectionPosScaleX = projectionPosScaleX;
	viewport->projectionPosScaleY = projectionPosScaleY;

	auto& runtimeData = viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;

	globals::game::renderer->UpdateViewPort(0, 0, 1);
	UpdateCameraData();

	if (d3d12SwapChainActive)
		SetUIBuffer();

	globals::state->UpdateSharedData(false, false);
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	if (d3d12SwapChainActive) {
		// Use frame latency waitable object if available for better frame pacing
		HANDLE waitableObject = GetFrameLatencyWaitableObject();

		// Wait for the next frame presentation slot
		WaitForSingleObject(waitableObject, INFINITE);

		if (settings.frameLimitMode) {
			// Fall back to the original timing method
			// Use integer arithmetic for more precise timing
			int64_t targetFrameTimeNS = int64_t(1000000000.0 / (refreshRate * (settings.frameGenerationMode && !globals::game::ui->GameIsPaused() ? 0.5 : 1.0)));
			int64_t targetFrameTicks = (targetFrameTimeNS * qpf.QuadPart) / 1000000000LL;

			static LARGE_INTEGER lastFrame = {};
			LARGE_INTEGER timeNow;
			QueryPerformanceCounter(&timeNow);

			int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
			if (delta < targetFrameTicks) {
				TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
			}
			QueryPerformanceCounter(&lastFrame);
		}
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						// get the refresh rate
						UINT numerator = p.targetInfo.refreshRate.Numerator;
						UINT denominator = p.targetInfo.refreshRate.Denominator;
						return (double)numerator / (double)denominator;
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}

bool Upscaling::IsFrameGenerationActive() const
{
	return IsFrameGenerationDx12PathActive() && settings.frameGenerationMode && fidelityFX.isFrameGenActive;
}

bool Upscaling::IsFrameGenerationDx12PathActive() const
{
	// Frame generation in this implementation runs via the DX12 swap-chain proxy path.
	return d3d12SwapChainActive && !globals::game::isVR;
}

bool Upscaling::IsUpscalingActive() const
{
	auto method = GetUpscaleMethod();

	// Only consider vendor upscalers (FSR/DLSS) as "active" when the
	// selected method actually produces a downscale. If the renderer is
	// currently running at 1:1 (no downscale), treat upscaling as inactive.
	if (!(method == UpscaleMethod::kFSR || method == UpscaleMethod::kDLSS)) {
		return false;
	}

	// resolutionScale.x represents renderWidth / displayWidth.
	return resolutionScale.x < .99f;
}

void Upscaling::RequestHistoryReset()
{
	historyResetRequested = true;
}

bool Upscaling::ShouldResetHistoryThisFrame() const
{
	return historyResetThisFrame;
}

void Upscaling::LatchHistoryResetForCurrentFrame()
{
	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (historyResetLatchedFrame == frame)
		return;

	historyResetLatchedFrame = frame;
	historyResetThisFrame = historyResetRequested;
	historyResetRequested = false;
}

void Upscaling::UpdateHistoryResetState(UpscaleMethod a_upscaleMethod)
{
	auto state = globals::state;
	if (!state)
		return;

	const bool inWorld = state->inWorld;
	const bool inMapMenu = globals::game::ui ? globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME) : false;
	const float2 screenSize = state->screenSize;
	const bool foveatedDispatchEnabled = IsFoveatedVendorDispatchEnabled(a_upscaleMethod);
	const bool peripheryTAAEnabled = IsPeripheryTAAEnabled(a_upscaleMethod);
	const bool peripheryTAAPathActive = IsPeripheryTAAPathActive(a_upscaleMethod);
	const bool fsrRuntimePathActive = IsFSRRuntimePathActive(a_upscaleMethod);
	const auto foveatedProfile = GetFoveatedMaskProfileParams(settings, peripheryTAAEnabled);
	const float foveatedCenterArea = foveatedProfile.centerArea;
	const float foveatedCenterHorizontalScale = foveatedProfile.centerHorizontalScale;
	const float peripheryTAACenterBlendFeather = ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather);
	const float peripheryTAAOuterScale = ClampPeripheryTAAOuterScaleForCenter(
		settings.periphery_taa_outer_scale,
		foveatedCenterArea,
		foveatedCenterHorizontalScale,
		peripheryTAACenterBlendFeather);
	const auto foveatedCenterOffsets = GetResolvedFoveatedMaskCenterOffsets(peripheryTAAEnabled);

	auto cameraCutDetected = []() {
		constexpr float kCameraCutDistanceThreshold = 2500.0f;  // ~35m teleport/cut in Skyrim units
		const float cutDistanceSq = kCameraCutDistanceThreshold * kCameraCutDistanceThreshold;

		auto exceededThreshold = [&](uint32_t eyeIndex) {
			const auto& currentPos = globals::game::frameBufferCached.GetCameraPosAdjust(eyeIndex);
			const auto& previousPos = globals::game::frameBufferCached.GetCameraPreviousPosAdjust(eyeIndex);
			const float dx = currentPos.x - previousPos.x;
			const float dy = currentPos.y - previousPos.y;
			const float dz = currentPos.z - previousPos.z;
			return (dx * dx + dy * dy + dz * dz) > cutDistanceSq;
		};

		if (globals::game::isVR)
			return exceededThreshold(0) || exceededThreshold(1);
		return exceededThreshold(0);
	};

	bool shouldReset = false;
	if (!historyResetTrackingInitialized) {
		shouldReset = true;
		historyResetTrackingInitialized = true;
	} else {
		const bool screenSizeChanged =
			std::abs(screenSize.x - previousHistoryScreenSize.x) > 0.5f ||
			std::abs(screenSize.y - previousHistoryScreenSize.y) > 0.5f;
		const bool scaleChanged =
			std::abs(resolutionScale.x - previousHistoryResolutionScale.x) > 1e-4f ||
			std::abs(resolutionScale.y - previousHistoryResolutionScale.y) > 1e-4f;
		const bool worldStateChanged =
			inWorld != previousHistoryInWorld ||
			inMapMenu != previousHistoryInMapMenu;
		const bool methodChanged = a_upscaleMethod != previousHistoryUpscaleMethod;
		const bool fsrRuntimePathChanged = fsrRuntimePathActive != previousHistoryFSRRuntimePathActive;
		const bool compareFoveatedArea = foveatedDispatchEnabled || previousHistoryFoveatedDispatch;
		const bool foveatedOffsetsChanged =
			compareFoveatedArea &&
			(std::abs(foveatedCenterOffsets[0].x - previousHistoryFoveatedCenterOffsets[0].x) > 1e-4f ||
			 std::abs(foveatedCenterOffsets[0].y - previousHistoryFoveatedCenterOffsets[0].y) > 1e-4f ||
			 std::abs(foveatedCenterOffsets[1].x - previousHistoryFoveatedCenterOffsets[1].x) > 1e-4f ||
			 std::abs(foveatedCenterOffsets[1].y - previousHistoryFoveatedCenterOffsets[1].y) > 1e-4f);
		const bool foveatedChanged =
			foveatedDispatchEnabled != previousHistoryFoveatedDispatch ||
			(compareFoveatedArea && std::abs(foveatedCenterArea - previousHistoryFoveatedCenterArea) > 1e-4f) ||
			(compareFoveatedArea && std::abs(foveatedCenterHorizontalScale - previousHistoryFoveatedCenterHorizontalScale) > 1e-4f) ||
			foveatedOffsetsChanged;
		const bool longFrameGap = globals::game::deltaTime &&
								  std::isfinite(*globals::game::deltaTime) &&
								  *globals::game::deltaTime > 0.20f;
		const bool cameraCut = inWorld && cameraCutDetected();

		const bool effectivePeripheryTAAChanged =
			peripheryTAAEnabled != previousHistoryPeripheryTAA ||
			peripheryTAAPathActive != previousHistoryPeripheryTAAPathActive ||
			(peripheryTAAPathActive && (
				std::abs(peripheryTAAOuterScale - previousHistoryPeripheryTAAOuterScale) > 1e-4f ||
				std::abs(peripheryTAACenterBlendFeather - previousHistoryPeripheryTAACenterBlendFeather) > 1e-4f));

		shouldReset = screenSizeChanged || scaleChanged || worldStateChanged || methodChanged || fsrRuntimePathChanged || foveatedChanged || effectivePeripheryTAAChanged || longFrameGap || cameraCut;
	}

	if (state->pendingPostLoadRuntimeReset)
		shouldReset = true;

	if (shouldReset)
		RequestHistoryReset();

	previousHistoryScreenSize = screenSize;
	previousHistoryResolutionScale = resolutionScale;
	previousHistoryInWorld = inWorld;
	previousHistoryInMapMenu = inMapMenu;
	previousHistoryUpscaleMethod = a_upscaleMethod;
	previousHistoryFoveatedDispatch = foveatedDispatchEnabled;
	previousHistoryFoveatedCenterArea = foveatedCenterArea;
	previousHistoryFoveatedCenterHorizontalScale = foveatedCenterHorizontalScale;
	previousHistoryFoveatedCenterOffsets = foveatedCenterOffsets;
	previousHistoryPeripheryTAA = peripheryTAAEnabled;
	previousHistoryPeripheryTAAPathActive = peripheryTAAPathActive;
	previousHistoryPeripheryTAAOuterScale = peripheryTAAOuterScale;
	previousHistoryPeripheryTAACenterBlendFeather = peripheryTAACenterBlendFeather;
	previousHistoryFSRRuntimePathActive = fsrRuntimePathActive;
}

/**
 * @brief Retrieves the current frame time for frame generation.
 *
 * Returns the frame time from the D3D12 swap chain if frame generation is active; otherwise, returns 0.
 *
 * @return float The current frame time in seconds, or 0 if frame generation is inactive.
 */
float Upscaling::GetFrameGenerationFrameTime() const
{
	if (!IsFrameGenerationActive())
		return 0.0f;

	// Get the current frame time from D3D12 swapchain
	if (dx12SwapChain.swapChain) {
		// Get frame time from the D3D12 SwapChain
		return GetFrameTime();
	}

	return 0.0f;
}

// Unified interface methods
void Upscaling::LoadUpscalingSDKs()
{
	// Initialize upscaling SDK components during plugin startup
	// This ensures all SDKs are available before any D3D device creation
	streamline.LoadInterposer();
	fidelityFX.LoadFFX();
}

void Upscaling::SetUIBuffer()
{
	dx12SwapChain.SetUIBuffer();
}

HANDLE Upscaling::GetFrameLatencyWaitableObject() const
{
	return dx12SwapChain.GetFrameLatencyWaitableObject();
}

float Upscaling::GetFrameTime() const
{
	return dx12SwapChain.GetFrameTime();
}

// Backend interface methods
bool Upscaling::IsBackendInitialized() const
{
	return streamline.initialized;
}

void Upscaling::CheckBackendFeatures(IDXGIAdapter* adapter)
{
	streamline.CheckFeatures(adapter);
}

void Upscaling::UpgradeBackendInterface(void** ppInterface)
{
	streamline.slUpgradeInterface(ppInterface);
}

void Upscaling::SetBackendD3DDevice(ID3D11Device* device)
{
	streamline.slSetD3DDevice(device);
}

void Upscaling::PostBackendDevice()
{
	streamline.PostDevice();
}

// Module availability methods
bool Upscaling::HasFrameGenModule() const
{
	return fidelityFX.featureFSR3FG;
}

// Proxy interface methods
void Upscaling::SetProxyD3D11Device(ID3D11Device* device)
{
	dx12SwapChain.SetD3D11Device(device);
}

void Upscaling::SetProxyD3D11DeviceContext(ID3D11DeviceContext* context)
{
	dx12SwapChain.SetD3D11DeviceContext(context);
}

void Upscaling::CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc)
{
	dx12SwapChain.CreateSwapChain(adapter, swapChainDesc);
}

void Upscaling::CreateProxyInterop()
{
	dx12SwapChain.CreateInterop();
}

IDXGISwapChain* Upscaling::GetProxySwapChain()
{
	return dx12SwapChain.GetSwapChainProxy();
}

void Upscaling::Upscale()
{
	auto upscaleMethod = GetUpscaleMethod();
	UpdateHistoryResetState(upscaleMethod);
	LatchHistoryResetForCurrentFrame();

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	const bool requiresEncodedMotionVectors = upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kFSR;
	const bool requiresCombinedEncodedMotionVectors = requiresEncodedMotionVectors && !globals::game::isVR;
	if (requiresCombinedEncodedMotionVectors && (!motionVectorCopyTexture || !motionVectorCopyTexture->uav || !motionVectorCopyTexture->resource)) {
		logger::error("[Upscaling] Missing encoded motion-vector resources for method {}", magic_enum::enum_name(upscaleMethod));
		return;
	}

	auto dispatchCount = Util::GetScreenDispatchCount(true);
	const bool foveatedDispatchRequested = IsFoveatedVendorDispatchEnabled(upscaleMethod);
	bool encodedVRFoveatedRegions = false;

	auto encodeUpscalingTextures = [&](bool forceFullVREncode, const char* eventName) {
		encodedVRFoveatedRegions = false;
		state->BeginPerfEvent(eventName);

		auto& temporalAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
		auto& normals = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[2]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
		ID3D11ShaderResourceView* views[4] = { temporalAAMask.SRV, normals.SRV, motionVector.SRV, depth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		auto upscalingBuffer = upscalingDataCB->CB();
		context->CSSetConstantBuffers(0, 1, &upscalingBuffer);
		context->CSSetShader(GetEncodeTexturesCS(), nullptr, 0);

		if (globals::game::isVR) {
			const uint32_t eyeWidthOut = static_cast<uint32_t>(state->screenSize.x / 2);
			const uint32_t eyeHeightOut = static_cast<uint32_t>(state->screenSize.y);
			const uint32_t eyeWidthIn = static_cast<uint32_t>(renderSize.x / 2);
			const uint32_t eyeHeightIn = static_cast<uint32_t>(renderSize.y);

			EnsureVRIntermediateTextures(eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut,
				main.texture, motionVector.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get());

			auto dispatchEyeEncode = [&](uint32_t eye, uint32_t inputMinX, uint32_t inputMinY, uint32_t inputMaxX, uint32_t inputMaxY) {
				if (eye >= 2 || inputMaxX <= inputMinX || inputMaxY <= inputMinY)
					return;

				inputMinX = std::min(inputMinX, eyeWidthIn);
				inputMinY = std::min(inputMinY, eyeHeightIn);
				inputMaxX = std::min(inputMaxX, eyeWidthIn);
				inputMaxY = std::min(inputMaxY, eyeHeightIn);
				if (inputMaxX <= inputMinX || inputMaxY <= inputMinY)
					return;

				const uint32_t dispatchWidth = inputMaxX - inputMinX;
				const uint32_t dispatchHeight = inputMaxY - inputMinY;
				UpscalingDataCB upscalingData{};
				upscalingData.dispatchDim = { static_cast<float>(dispatchWidth), static_cast<float>(dispatchHeight) };
				upscalingData.trueSamplingDim = renderSize;
				upscalingData.invTrueSamplingDim = { renderSize.x > 0.0f ? 1.0f / renderSize.x : 0.0f, renderSize.y > 0.0f ? 1.0f / renderSize.y : 0.0f };
				upscalingData.seamCenterX = renderSize.x * 0.5f;
				upscalingData.seamHalfWidthPx = 2.0f;
				upscalingData.maskDepthThreshold = 1e-6f;
				upscalingData.vrSeamHardening = 1.0f;
				upscalingData.sourceOffset = { static_cast<float>(eye * eyeWidthIn + inputMinX), static_cast<float>(inputMinY) };
				upscalingData.outputOffset = { static_cast<float>(inputMinX), static_cast<float>(inputMinY) };
				upscalingDataCB->Update(upscalingData);

				ID3D11UnorderedAccessView* uavs[3] = {
					vrIntermediateReactiveMask[eye]->uav.get(),
					vrIntermediateTransparencyMask[eye]->uav.get(),
					vrIntermediateMotionVectors[eye]->uav.get()
				};
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
				context->Dispatch((dispatchWidth + 7u) >> 3, (dispatchHeight + 7u) >> 3, 1);
			};

			auto dispatchFullEyes = [&]() {
				for (uint32_t eye = 0; eye < 2; ++eye) {
					dispatchEyeEncode(eye, 0, 0, eyeWidthIn, eyeHeightIn);
				}
			};

			struct EncodeRegion
			{
				uint32_t minX = 0;
				uint32_t minY = 0;
				uint32_t maxX = 0;
				uint32_t maxY = 0;
				bool valid = false;
			};

			auto includeInputRect = [&](EncodeRegion& region, uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) {
				minX = std::min(minX, eyeWidthIn);
				minY = std::min(minY, eyeHeightIn);
				maxX = std::min(maxX, eyeWidthIn);
				maxY = std::min(maxY, eyeHeightIn);
				if (maxX <= minX || maxY <= minY)
					return;

				if (!region.valid) {
					region.minX = minX;
					region.minY = minY;
					region.maxX = maxX;
					region.maxY = maxY;
					region.valid = true;
				} else {
					region.minX = std::min(region.minX, minX);
					region.minY = std::min(region.minY, minY);
					region.maxX = std::max(region.maxX, maxX);
					region.maxY = std::max(region.maxY, maxY);
				}
			};

			const bool useRegionEncode = !forceFullVREncode && foveatedDispatchRequested && IsPeripheryTAAPathActive(upscaleMethod);
			bool dispatchedRegionEncode = false;
			if (useRegionEncode) {
				const bool usePeripheryTAAProfile = IsPeripheryTAAEnabled(upscaleMethod);
				const auto foveatedProfile = GetFoveatedMaskProfileParams(settings, usePeripheryTAAProfile);
				const float centerScale = foveatedProfile.centerArea;
				const float centerHorizontalScale = foveatedProfile.centerHorizontalScale;
				const float centerFeather = ClampPeripheryTAACenterBlendFeather(settings.periphery_taa_center_blend_feather);

				if (BuildFoveatedDispatchRects(eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut, true, centerScale, centerFeather, centerHorizontalScale, usePeripheryTAAProfile)) {
					std::array<EncodeRegion, 2> regions{};
					bool allRegionsValid = true;
					for (uint32_t eye = 0; eye < 2; ++eye) {
						const auto& rect = foveatedRectCache.rects[eye];
						if (!rect.inputWidth || !rect.inputHeight) {
							allRegionsValid = false;
							break;
						}

						includeInputRect(regions[eye], rect.inputOffsetX, rect.inputOffsetY, rect.inputOffsetX + rect.inputWidth, rect.inputOffsetY + rect.inputHeight);

						const float2 centerOffset = GetResolvedFoveatedMaskCenterOffset(eye, usePeripheryTAAProfile);
						const float taaOuterScale = ClampPeripheryTAAOuterScaleForCenter(
							settings.periphery_taa_outer_scale,
							centerScale,
							centerHorizontalScale,
							centerFeather);
						const auto taaOuterBounds = FoveatedCommon::BuildCenteredDispatchBounds(0, eyeWidthOut, eyeHeightOut, taaOuterScale, centerOffset.x, centerOffset.y, 0.0f, centerHorizontalScale);
						if (taaOuterBounds.maxX > taaOuterBounds.minX && taaOuterBounds.maxY > taaOuterBounds.minY) {
							constexpr uint32_t kCopyPadding = 2u;
							const auto mappedInputRect = MapOutputRectToInputRect(
								static_cast<uint32_t>(taaOuterBounds.minX),
								static_cast<uint32_t>(taaOuterBounds.minY),
								static_cast<uint32_t>(taaOuterBounds.maxX),
								static_cast<uint32_t>(taaOuterBounds.maxY),
								eyeWidthOut,
								eyeHeightOut,
								eyeWidthIn,
								eyeHeightIn,
								kCopyPadding);
							if (mappedInputRect.IsValid()) {
								includeInputRect(regions[eye], mappedInputRect.minX, mappedInputRect.minY, mappedInputRect.maxX, mappedInputRect.maxY);
							}
						}

						if (!regions[eye].valid) {
							allRegionsValid = false;
							break;
						}
					}

					if (allRegionsValid) {
						for (uint32_t eye = 0; eye < 2; ++eye) {
							dispatchEyeEncode(eye, regions[eye].minX, regions[eye].minY, regions[eye].maxX, regions[eye].maxY);
						}
						dispatchedRegionEncode = true;
						encodedVRFoveatedRegions = true;
					}
				}
			}

			if (!dispatchedRegionEncode) {
				dispatchFullEyes();
			}
		} else {
			UpscalingDataCB upscalingData{};
			upscalingData.dispatchDim = renderSize;
			upscalingData.trueSamplingDim = renderSize;
			upscalingData.invTrueSamplingDim = { renderSize.x > 0.0f ? 1.0f / renderSize.x : 0.0f, renderSize.y > 0.0f ? 1.0f / renderSize.y : 0.0f };
			upscalingData.seamCenterX = renderSize.x * 0.5f;
			upscalingData.seamHalfWidthPx = 2.0f;
			upscalingData.maskDepthThreshold = 1e-6f;
			upscalingData.vrSeamHardening = 0.0f;
			upscalingData.sourceOffset = { 0.0f, 0.0f };
			upscalingData.outputOffset = { 0.0f, 0.0f };
			upscalingDataCB->Update(upscalingData);

			ID3D11UnorderedAccessView* uavs[3] = {
				reactiveMaskTexture->uav.get(),
				transparencyCompositionMaskTexture->uav.get(),
				requiresCombinedEncodedMotionVectors ? motionVectorCopyTexture->uav.get() : nullptr
			};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		}

		ID3D11ShaderResourceView* nullSRV[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(nullSRV), nullSRV);

		ID3D11UnorderedAccessView* nullUAV[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAV), nullUAV, nullptr);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);

		state->EndPerfEvent();
	};

	encodeUpscalingTextures(false, "Encode Upscaling Textures");

	{
		state->BeginPerfEvent("Upscaling");
		ID3D11Resource* motionVectorResource = globals::game::isVR ? motionVector.texture : motionVectorCopyTexture->resource.get();
		bool dispatched = false;
		static bool loggedFoveatedFallback = false;

		if (foveatedDispatchRequested) {
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			dispatched = DispatchFoveatedVendorUpscaling(
				upscaleMethod,
				main.texture,
				depth.texture,
				motionVectorResource,
				reactiveMaskTexture->resource.get(),
				transparencyCompositionMaskTexture->resource.get(),
				main.SRV);
			if (!dispatched) {
				if (!loggedFoveatedFallback) {
					logger::warn("[Upscaling] Foveated vendor dispatch failed; falling back to full-frame {} dispatch.",
						magic_enum::enum_name(upscaleMethod));
					loggedFoveatedFallback = true;
				}
			} else {
				loggedFoveatedFallback = false;
			}
		} else {
			loggedFoveatedFallback = false;
		}

		if (!dispatched) {
			if (encodedVRFoveatedRegions) {
				encodeUpscalingTextures(true, "Encode Upscaling Textures (Fallback Full)");
			}
			if (upscaleMethod == UpscaleMethod::kDLSS) {
				streamline.Upscale(main.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVectorResource);
			} else if (upscaleMethod == UpscaleMethod::kFSR) {
				fidelityFX.Upscale(main.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVectorResource, settings.sharpnessFSR);
			}
		}

		state->EndPerfEvent();
	}
}

void Upscaling::PerformUpscaling()
{
	Upscale();
	UpscaleDepth();

	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();

	// Disable dynamic resolution past this point
	runtimeData.dynamicResolutionLock = 1;

	// Updates the PerFrame constant buffer so that dynamic resolution settings are disabled
	UpdateCameraData();
}

void Upscaling::UpscaleDepth()
{
	// Optimization overview:
	// 1) Early validation exits before issuing GPU work.
	// 2) Wide-kernel depth mode uses hysteresis to avoid frequent toggles.
	// 3) Resource copies are skipped for aliased src/dst to reduce copy churn.

	// (1) Early validation exits
	if (!IsUpscalingActive()) {
		return;
	}

	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!state || !renderer || !context || !deferred || !deferred->linearSampler || !jitterCB || !upscaleRasterizerState || !upscaleBlendState || !upscaleDepthStencilState) {
		return;
	}

	auto screenSize = state->screenSize;
	if (screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
		return;
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
	auto& refractionNormals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
	auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];
	auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];

	if (!depth.texture || !depth.views[0] || !depthCopy.texture || !depthCopy.depthSRV ||
		!refractionNormals.texture || !refractionNormals.textureCopy || !refractionNormals.SRVCopy || !refractionNormals.RTV || !saoCameraZ.RTV ||
		!underwaterMask.texture || !underwaterMask.textureCopy || !underwaterMask.SRVCopy || !underwaterMask.RTV) {
		return;
	}
	if (globals::game::isVR && (!depthCopy.views[0] || !depthCopy.stencilSRV)) {
		return;
	}

	auto* fullscreenVS = GetUpscaleVS();
	auto* depthUpscalePS = GetDepthRefractionUpscalePS();
	auto* underwaterMaskPS = GetUnderwaterMaskUpscalePS();
	if (!fullscreenVS || !depthUpscalePS || !underwaterMaskPS) {
		return;
	}

	state->BeginPerfEvent("Render Target Upscaling");

	// Set up Input Assembler for fullscreen triangle (no vertex/index buffers needed)
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Set up vertex shader that generates fullscreen triangle using SV_VertexID
	context->VSSetShader(fullscreenVS, nullptr, 0);

	// Set up viewport for fullscreen rendering
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = screenSize.x;
	viewport.Height = screenSize.y;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	// Set rasterizer and blend state
	context->RSSetState(upscaleRasterizerState.get());
	context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

	ID3D11SamplerState* samplers[] = { deferred->linearSampler };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// Set up jitter/depth-kernel constant buffer for upscaling
	JitterCB jitterData;
	jitterData.jitter = jitter;
	// (2) Wide-kernel hysteresis
	{
		constexpr float kEnterWideKernelRatio = 1.55f;
		constexpr float kExitWideKernelRatio = 1.45f;
		const float minScale = std::max(std::min(resolutionScale.x, resolutionScale.y), FLT_EPSILON);
		const float upscaleRatio = 1.0f / minScale;

		if (depthUpscaleUseWideKernel) {
			if (upscaleRatio < kExitWideKernelRatio) {
				depthUpscaleUseWideKernel = false;
			}
		} else {
			if (upscaleRatio > kEnterWideKernelRatio) {
				depthUpscaleUseWideKernel = true;
			}
		}

		jitterData.useWideKernel = depthUpscaleUseWideKernel ? 1.0f : 0.0f;
		jitterData.pad0 = 0.0f;
	}

	jitterCB->Update(jitterData);
	auto bufferArray = jitterCB->CB();
	context->PSSetConstantBuffers(0, 1, &bufferArray);

	// (3) Skip aliased copies
	const auto copyIfNonAliased = [&](ID3D11Resource* dst, ID3D11Resource* src) {
		if (dst && src && dst != src) {
			context->CopyResource(dst, src);
		}
	};

	{
		// Sometimes this is not already copied e.g. map menu.
		// Skip alias copies to reduce unnecessary copy churn.
		copyIfNonAliased(depthCopy.texture, depth.texture);

		// Clear stencil to be 0xFF
		if (globals::game::isVR) {
			context->ClearDepthStencilView(depthCopy.views[0], D3D11_CLEAR_STENCIL, 1.0f, 0xFF);
		}

		// Set depth stencil state to write 0x00
		context->OMSetDepthStencilState(upscaleDepthStencilState.get(), 0x00);

		copyIfNonAliased(refractionNormals.textureCopy, refractionNormals.texture);

		ID3D11ShaderResourceView* srvs[] = { refractionNormals.SRVCopy, depthCopy.depthSRV, depthCopy.stencilSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { refractionNormals.RTV, saoCameraZ.RTV };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, depth.views[0]);

		context->PSSetShader(depthUpscalePS, nullptr, 0);
		context->Draw(3, 0);

		// Depth copy is also used on VR.
		if (globals::game::isVR) {
			copyIfNonAliased(depthCopy.texture, depth.texture);
		}
	}

	{
		viewport.Width = screenSize.x * 0.5f;
		viewport.Height = screenSize.y * 0.5f;
		context->RSSetViewports(1, &viewport);

		copyIfNonAliased(underwaterMask.textureCopy, underwaterMask.texture);

		context->OMSetDepthStencilState(nullptr, 0x00);

		ID3D11ShaderResourceView* srvs[] = { underwaterMask.SRVCopy };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { underwaterMask.RTV };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(underwaterMaskPS, nullptr, 0);
		context->Draw(3, 0);
	}

	ID3D11ShaderResourceView* nullPSResources[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);

	state->EndPerfEvent();
}

void Upscaling::ApplySharpening()
{
	if (settings.sharpnessDLSS <= 0.0f)
		return;

	if (!sharpenerTexture)
		return;

	float currentSharpness = (-2.0f * settings.sharpnessDLSS) + 2.0f;
	currentSharpness = exp2(-currentSharpness);

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	ID3D11Resource* mainResource = nullptr;
	main.SRV->GetResource(&mainResource);

	if (!mainResource)
		return;

	context->OMSetRenderTargets(0, nullptr, nullptr);

	rcas.ApplySharpen(main.SRV, sharpenerTexture->uav.get(), currentSharpness);
	context->CopyResource(mainResource, sharpenerTexture->resource.get());

	mainResource->Release();

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void Upscaling::Main_UpdateJitter::thunk(RE::BSGraphics::State* a_state)
{
	globals::features::upscaling.ConfigureTAA();
	func(a_state);
	globals::features::upscaling.ConfigureUpscaling(a_state);
}

void Upscaling::MenuManagerDrawInterfaceStartHook::thunk(int64_t a1)
{
	globals::features::upscaling.PostDisplay();
	func(a1);
}

void Upscaling::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	auto& upscaling = globals::features::upscaling;
	auto upscaleMethod = upscaling.GetUpscaleMethod();

	if (upscaling.d3d12SwapChainActive && upscaling.settings.frameGenerationMode)
		upscaling.CopySharedD3D12Resources();

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA)
		upscaling.PerformUpscaling();

	if (upscaleMethod == UpscaleMethod::kDLSS)
		upscaling.ApplySharpening();

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);

	if (upscaleMethod == UpscaleMethod::kNONE) {
		// Keep vanilla TAA/water stabilization state untouched when no upscaler is active.
		func(a_this, a3, a_target, a_4, a_5);
		return;
	}

	BSImagespaceShaderISTemporalAA->taaEnabled = upscaleMethod == UpscaleMethod::kTAA;

	func(a_this, a3, a_target, a_4, a_5);

	BSImagespaceShaderISTemporalAA->taaEnabled = false;
}

void Upscaling::SetScissorRect::thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom)
{
	auto viewport = globals::game::graphicsState;
	auto& runtimeData = viewport->GetRuntimeData();

	if (!runtimeData.dynamicResolutionLock) {
		a_left = static_cast<int>(a_left * runtimeData.dynamicResolutionWidthRatio);
		a_right = static_cast<int>(a_right * runtimeData.dynamicResolutionWidthRatio);

		a_top = static_cast<int>(a_top * runtimeData.dynamicResolutionHeightRatio);
		a_bottom = static_cast<int>(a_bottom * runtimeData.dynamicResolutionHeightRatio);
	}

	func(This, a_left, a_top, a_right, a_bottom);
}

void Upscaling::Main_RenderPrecipitation::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

void Upscaling::BSFaceGenManager_UpdatePendingCustomizationTextures::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}
