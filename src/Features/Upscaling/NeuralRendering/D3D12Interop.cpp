#include "D3D12Interop.h"

#include "../Streamline.h"
#include "Globals.h"

#include <format>

namespace NR
{
	void D3D12Interop::Check(HRESULT result)
	{
		if (SUCCEEDED(result))
			return;
		if (DeviceRemoved())
			throw std::runtime_error(std::format("NR D3D12 device removed (0x{:08X})", static_cast<uint32_t>(device->GetDeviceRemovedReason())));
		throw std::runtime_error(std::format("initialization failed: D3D12 call failed (HRESULT 0x{:08X})", static_cast<uint32_t>(result)));
	}

	void D3D12Interop::Initialize()
	{
		if (device)
			return;
		try {
			Check(globals::d3d::device->QueryInterface(device11.put()));
			Check(globals::d3d::context->QueryInterface(context.put()));
			winrt::com_ptr<IDXGIDevice> dxgi;
			Check(device11->QueryInterface(dxgi.put()));
			winrt::com_ptr<IDXGIAdapter> adapter;
			Check(dxgi->GetAdapter(adapter.put()));
			DXGI_ADAPTER_DESC desc{};
			Check(adapter->GetDesc(&desc));
			if (desc.VendorId != Streamline::kNvidiaVendorId)
				throw std::runtime_error("requires an NVIDIA RTX GPU");
			// The level is a minimum: the runtime hands back the adapter's existing device.
			Check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())));
			Check(queue->SetName(L"NeuralRendering::Queue"));
			for (auto& slot : commands) {
				Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put())));
				Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.list.put())));
				Check(slot.allocator->SetName(L"NeuralRendering::Allocator"));
				Check(slot.list->SetName(L"NeuralRendering::Commands"));
				Check(slot.list->Close());
			}
			fence.Create(device.get(), device11.get(), "NeuralRendering::Fence");
			adapterLuid = desc.AdapterLuid;
		} catch (...) {
			// A half-built interop would make the next attempt reuse a device with no queue behind it.
			Reset();
			throw;
		}
	}

	void D3D12Interop::Reset()
	{
		commands = {};
		fence.Reset();
		queue = nullptr;
		device = nullptr;
		context = nullptr;
		device11 = nullptr;
		adapterLuid = {};
		cursor = 0;
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
		if (!fence.fence12)
			return;
		Check(device->GetDeviceRemovedReason());
		DWORD waitError = 0;
		const auto outcome = fence.CpuWaitOutcome(completion, kFenceTimeoutMs, &waitError);
		if (outcome == SharedFence::WaitOutcome::kComplete)
			return;
		Check(device->GetDeviceRemovedReason());
		if (outcome == SharedFence::WaitOutcome::kFailed)
			throw std::runtime_error(std::format("NR GPU fence {} wait failed (error 0x{:08X})", completion, waitError));
		throw std::runtime_error(std::format("NR GPU fence {} did not retire within {} ms", completion, kFenceTimeoutMs));
	}

	ID3D12GraphicsCommandList* D3D12Interop::Begin()
	{
		auto& slot = commands[cursor];
		Wait(slot.completion);
		winrt::check_hresult(slot.allocator->Reset());
		winrt::check_hresult(slot.list->Reset(slot.allocator.get(), nullptr));
		const auto ready = fence.Next();
		winrt::check_hresult(context->Signal(fence.fence11.get(), ready));
		context->Flush();
		winrt::check_hresult(queue->Wait(fence.fence12.get(), ready));
		return slot.list.get();
	}

	void D3D12Interop::End()
	{
		auto& slot = commands[cursor];
		winrt::check_hresult(slot.list->Close());
		ID3D12CommandList* lists[]{ slot.list.get() };
		queue->ExecuteCommandLists(1, lists);
		const auto complete = fence.Next();
		winrt::check_hresult(queue->Signal(fence.fence12.get(), complete));
		slot.completion = complete;
		winrt::check_hresult(context->Wait(fence.fence11.get(), complete));
		cursor = (cursor + 1) % static_cast<uint32_t>(commands.size());
	}

	void D3D12Interop::Drain()
	{
		if (!fence.fence12 || !fence.fence11)
			return;
		const auto ready = fence.Next();
		winrt::check_hresult(context->Signal(fence.fence11.get(), ready));
		context->Flush();
		winrt::check_hresult(queue->Wait(fence.fence12.get(), ready));
		const auto complete = fence.Next();
		winrt::check_hresult(queue->Signal(fence.fence12.get(), complete));
		Wait(complete);
	}
}
