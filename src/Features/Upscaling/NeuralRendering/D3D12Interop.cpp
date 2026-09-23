#include "D3D12Interop.h"

#include "Globals.h"
#include "Utils/D3D.h"

namespace NR
{
	void D3D12Interop::Initialize()
	{
		winrt::check_hresult(globals::d3d::device->QueryInterface(device11.put()));
		winrt::check_hresult(globals::d3d::context->QueryInterface(context.put()));
		winrt::com_ptr<IDXGIDevice> dxgi;
		winrt::check_hresult(device11->QueryInterface(dxgi.put()));
		winrt::com_ptr<IDXGIAdapter> adapter;
		winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
		DXGI_ADAPTER_DESC desc{};
		winrt::check_hresult(adapter->GetDesc(&desc));
		if (desc.VendorId != 0x10DE)
			throw std::runtime_error("Neural Rendering requires an NVIDIA adapter");
		// Match the bridge used by OptiScaler: create a D3D12 device on the game's
		// D3D11 adapter, using the baseline level accepted by the DX11-on-DX12 path.
		winrt::check_hresult(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		winrt::check_hresult(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())));
		winrt::check_hresult(queue->SetName(L"NeuralRendering::Queue"));
		for (auto& slot : commands) {
			winrt::check_hresult(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put())));
			winrt::check_hresult(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.list.put())));
			winrt::check_hresult(slot.allocator->SetName(L"NeuralRendering::Allocator"));
			winrt::check_hresult(slot.list->SetName(L"NeuralRendering::Commands"));
			winrt::check_hresult(slot.list->Close());
		}
		winrt::check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.put())));
		winrt::check_hresult(fence->SetName(L"NeuralRendering::Fence"));
		winrt::handle shared;
		winrt::check_hresult(device->CreateSharedHandle(fence.get(), nullptr, GENERIC_ALL, nullptr, shared.put()));
		winrt::check_hresult(device11->OpenSharedFence(shared.get(), IID_PPV_ARGS(fence11.put())));
		Util::SetResourceName(fence11.get(), "NeuralRendering::Fence");
		event.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		if (!event)
			winrt::throw_last_error();
		logger::info("[NeuralRendering] D3D12 device on renderer adapter LUID {:08X}:{:08X}",
			desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
	}

	SharedTexture D3D12Interop::CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.Format = format;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		SharedTexture result;
		result.texture = std::make_unique<Texture2D>(desc, name.c_str());
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		result.texture->CreateUAV(uav);
		winrt::com_ptr<IDXGIResource1> sharedResource;
		winrt::check_hresult(result.texture->resource->QueryInterface(sharedResource.put()));
		winrt::handle shared;
		winrt::check_hresult(sharedResource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, shared.put()));
		winrt::check_hresult(device->OpenSharedHandle(shared.get(), IID_PPV_ARGS(result.resource.put())));
		winrt::check_hresult(result.resource->SetName(winrt::to_hstring(name).c_str()));
		return result;
	}

	void D3D12Interop::Wait(uint64_t completion)
	{
		winrt::check_hresult(device->GetDeviceRemovedReason());
		if (fence->GetCompletedValue() >= completion)
			return;
		winrt::check_hresult(fence->SetEventOnCompletion(completion, event.get()));
		while (WaitForSingleObject(event.get(), 100) == WAIT_TIMEOUT)
			winrt::check_hresult(device->GetDeviceRemovedReason());
		winrt::check_hresult(device->GetDeviceRemovedReason());
		if (fence->GetCompletedValue() < completion)
			winrt::throw_last_error();
	}

	ID3D12GraphicsCommandList* D3D12Interop::Begin()
	{
		auto& slot = commands[cursor];
		Wait(slot.completion);
		winrt::check_hresult(slot.allocator->Reset());
		winrt::check_hresult(slot.list->Reset(slot.allocator.get(), nullptr));
		const auto ready = ++value;
		winrt::check_hresult(context->Signal(fence11.get(), ready));
		context->Flush();
		winrt::check_hresult(queue->Wait(fence.get(), ready));
		return slot.list.get();
	}

	void D3D12Interop::End()
	{
		auto& slot = commands[cursor];
		winrt::check_hresult(slot.list->Close());
		ID3D12CommandList* lists[]{ slot.list.get() };
		queue->ExecuteCommandLists(1, lists);
		const auto complete = ++value;
		winrt::check_hresult(queue->Signal(fence.get(), complete));
		slot.completion = complete;
		winrt::check_hresult(context->Wait(fence11.get(), complete));
		cursor = (cursor + 1) % static_cast<uint32_t>(commands.size());
	}

	void D3D12Interop::Drain()
	{
		if (!fence || !fence11 || !event)
			return;
		const auto ready = ++value;
		winrt::check_hresult(context->Signal(fence11.get(), ready));
		context->Flush();
		winrt::check_hresult(queue->Wait(fence.get(), ready));
		const auto complete = ++value;
		winrt::check_hresult(queue->Signal(fence.get(), complete));
		Wait(complete);
	}
}
