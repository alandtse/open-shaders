#include "GraphicsBackend.h"

#include <dxgi1_2.h>

std::optional<GraphicsBackend> GraphicsBackend::Create(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string& a_failure)
{
	GraphicsBackend backend;
	if (!a_device || !a_context) {
		a_failure = "DX11 device or context unavailable";
		return std::nullopt;
	}
	auto check = [&](HRESULT result, const char* operation) {
		if (FAILED(result))
			a_failure = std::format("{} failed: 0x{:08X}", operation, static_cast<uint32_t>(result));
		return SUCCEEDED(result);
	};
	winrt::com_ptr<IDXGIDevice> dxgiDevice;
	winrt::com_ptr<IDXGIAdapter> adapter;
	if (!check(a_device->QueryInterface(IID_PPV_ARGS(backend.d3d11Device.put())), "Query DX11 device") ||
		!check(a_context->QueryInterface(IID_PPV_ARGS(backend.d3d11Context.put())), "Query DX11 context") ||
		!check(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())), "Query DXGI device") ||
		!check(dxgiDevice->GetAdapter(adapter.put()), "Get DX11 adapter") ||
		!check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(backend.d3d12Device.put())), "Create DX12 device"))
		return std::nullopt;
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (!check(backend.d3d12Device->CreateCommandQueue(&desc, IID_PPV_ARGS(backend.commandQueue.put())), "Create DX12 queue"))
		return std::nullopt;
	backend.commandQueue->SetName(L"GraphicsBackend::CommandQueue");
	a_failure.clear();
	return backend;
}
