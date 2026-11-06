#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>
#include <optional>
#include <string>
#include <dxgi1_2.h>

/** @brief Retains a DX12 queue on the adapter used by the engine's DX11 device. */
struct GraphicsBackend
{
	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;

	/** @brief Creates an independent backend or reports the failed operation. */
	static std::optional<GraphicsBackend> Create(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string& a_failure);
	/** @brief Returns the shared DX12 device for the renderer adapter. */
	static HRESULT GetSharedDevice(IDXGIAdapter* a_adapter, ID3D12Device** a_device);
};
