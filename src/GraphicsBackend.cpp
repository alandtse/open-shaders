#include "GraphicsBackend.h"

#include <dxgi1_2.h>
#include <mutex>

HRESULT GraphicsBackend::GetSharedDevice(IDXGIAdapter* a_adapter, ID3D12Device** a_device)
{
	if (!a_adapter || !a_device)
		return E_INVALIDARG;
	static std::mutex mutex;
	static winrt::com_ptr<ID3D12Device> shared;
	std::scoped_lock lock(mutex);
	DXGI_ADAPTER_DESC adapterDesc{};
	const auto result = a_adapter->GetDesc(&adapterDesc);
	if (FAILED(result))
		return result;
	if (shared) {
		const auto luid = shared->GetAdapterLuid();
		if (luid.HighPart != adapterDesc.AdapterLuid.HighPart || luid.LowPart != adapterDesc.AdapterLuid.LowPart)
			return DXGI_ERROR_UNSUPPORTED;
		const auto removed = shared->GetDeviceRemovedReason();
		if (FAILED(removed))
			return removed;
	} else {
		const auto created = D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(shared.put()));
		if (FAILED(created))
			return created;
	}
	shared.copy_to(a_device);
	return S_OK;
}

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
		!check(GetSharedDevice(adapter.get(), backend.d3d12Device.put()), "Create DX12 device"))
		return std::nullopt;
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (!check(backend.d3d12Device->CreateCommandQueue(&desc, IID_PPV_ARGS(backend.commandQueue.put())), "Create DX12 queue"))
		return std::nullopt;
	backend.commandQueue->SetName(L"GraphicsBackend::CommandQueue");
	a_failure.clear();
	return backend;
}
