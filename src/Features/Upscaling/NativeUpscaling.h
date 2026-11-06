#pragma once

#include "Features/NativeComposite/NativeCompositePass.h"

/** @brief Frame-scoped inputs for the DX12 runtime upscaler. */
struct NativeUpscaleInputs
{
	std::shared_ptr<const NativeComposite::Frame> color;
	ID3D12Resource* depth = nullptr;
	ID3D12Resource* motion = nullptr;
	ID3D12Resource* reactive = nullptr;
	ID3D12Resource* transparency = nullptr;
	ID3D12Resource* output = nullptr;
	uint32_t frame = 0, renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
	float jitterX = 0, jitterY = 0, motionScaleX = 0, motionScaleY = 0;
	float deltaMilliseconds = 0, cameraNear = 0, cameraFar = 0, verticalFov = 0;
	bool resetHistory = false;
};

/** @brief Current-frame runtime FSR result and the fence completing its producer work. */
struct NativeUpscaleOutput
{
	winrt::com_ptr<ID3D12Resource> color;
	winrt::com_ptr<ID3D12Fence> producerFence;
	uint64_t producerValue = 0;
	uint32_t frame = 0, width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
