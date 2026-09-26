#include "D3D12Interop.h"

#include "Lifecycle.h"

#include "../../../Globals.h"
#include "../Streamline.h"

#include <format>

namespace NR
{
	void D3D12Interop::Initialize()
	{
		if (device)
			return;
		try {
			DX::ThrowIfFailed(globals::d3d::device->QueryInterface(device11.put()));
			DX::ThrowIfFailed(globals::d3d::context->QueryInterface(context.put()));
			winrt::com_ptr<IDXGIDevice> dxgi;
			DX::ThrowIfFailed(device11->QueryInterface(dxgi.put()));
			winrt::com_ptr<IDXGIAdapter> adapter;
			DX::ThrowIfFailed(dxgi->GetAdapter(adapter.put()));
			DXGI_ADAPTER_DESC desc{};
			DX::ThrowIfFailed(adapter->GetDesc(&desc));
			if (desc.VendorId != Streamline::kNvidiaVendorId)
				throw RuntimeError(FailureKind::kUnsupportedAdapter, "Neural Rendering requires an NVIDIA adapter");
			// The level is a minimum: the runtime hands back the adapter's existing device.
			DX::ThrowIfFailed(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			DX::ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())));
			DX::ThrowIfFailed(queue->SetName(L"NeuralRendering::Queue"));
			for (auto& slot : commands) {
				DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put())));
				DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.list.put())));
				DX::ThrowIfFailed(slot.allocator->SetName(L"NeuralRendering::Allocator"));
				DX::ThrowIfFailed(slot.list->SetName(L"NeuralRendering::Commands"));
				DX::ThrowIfFailed(slot.list->Close());
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

	SharedTexture D3D12Interop::CreateTexture(uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, const std::string& a_name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.Format = a_format;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		// NTHANDLE is required for a D3D12 OpenSharedHandle on the same memory.
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		SharedTexture result;
		result.texture = std::make_unique<Texture2D>(desc, a_name.c_str());
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = a_format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		result.texture->CreateUAV(uav);
		winrt::com_ptr<IDXGIResource1> sharedResource;
		DX::ThrowIfFailed(result.texture->resource->QueryInterface(sharedResource.put()));
		winrt::handle shared;
		DX::ThrowIfFailed(sharedResource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, shared.put()));
		DX::ThrowIfFailed(device->OpenSharedHandle(shared.get(), IID_PPV_ARGS(result.resource.put())));
		DX::ThrowIfFailed(result.resource->SetName(winrt::to_hstring(a_name).c_str()));
		return result;
	}

	void D3D12Interop::Wait(uint64_t a_completion)
	{
		if (!fence.fence12)
			return;
		const auto removed = device->GetDeviceRemovedReason();
		if (FAILED(removed))
			throw RuntimeError(FailureKind::kDeviceRemoved, std::format("NR D3D12 device removed (0x{:08X})", static_cast<uint32_t>(removed)));
		DWORD waitError = 0;
		const auto outcome = fence.CpuWaitOutcome(a_completion, kFenceTimeoutMs, &waitError);
		if (outcome == SharedFence::WaitOutcome::kComplete)
			return;
		const auto afterWait = device->GetDeviceRemovedReason();
		if (FAILED(afterWait))
			throw RuntimeError(FailureKind::kDeviceRemoved, std::format("NR D3D12 device removed (0x{:08X})", static_cast<uint32_t>(afterWait)));
		if (outcome == SharedFence::WaitOutcome::kFailed)
			throw RuntimeError(FailureKind::kWaitFailed, std::format("NR GPU fence {} wait failed (Win32 error {})", a_completion, waitError));
		throw RuntimeError(FailureKind::kDrainTimeout, std::format("NR GPU fence {} did not retire within {} ms", a_completion, kFenceTimeoutMs));
	}

	ID3D12GraphicsCommandList* D3D12Interop::Begin()
	{
		auto& slot = commands[cursor];
		Wait(slot.completion);
		DX::ThrowIfFailed(slot.allocator->Reset());
		DX::ThrowIfFailed(slot.list->Reset(slot.allocator.get(), nullptr));
		const auto ready = fence.Next();
		DX::ThrowIfFailed(context->Signal(fence.fence11.get(), ready));
		context->Flush();
		DX::ThrowIfFailed(queue->Wait(fence.fence12.get(), ready));
		return slot.list.get();
	}

	void D3D12Interop::End()
	{
		auto& slot = commands[cursor];
		DX::ThrowIfFailed(slot.list->Close());
		ID3D12CommandList* lists[]{ slot.list.get() };
		queue->ExecuteCommandLists(1, lists);
		const auto complete = fence.Next();
		DX::ThrowIfFailed(queue->Signal(fence.fence12.get(), complete));
		slot.completion = complete;
		DX::ThrowIfFailed(context->Wait(fence.fence11.get(), complete));
		cursor = (cursor + 1) % static_cast<uint32_t>(commands.size());
	}

	void D3D12Interop::Drain()
	{
		if (!fence.fence12 || !fence.fence11)
			return;
		const auto ready = fence.Next();
		DX::ThrowIfFailed(context->Signal(fence.fence11.get(), ready));
		context->Flush();
		DX::ThrowIfFailed(queue->Wait(fence.fence12.get(), ready));
		const auto complete = fence.Next();
		DX::ThrowIfFailed(queue->Signal(fence.fence12.get(), complete));
		Wait(complete);
	}
}
