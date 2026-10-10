#include "D3D12Interop.h"

#include "../Streamline.h"
#include "Globals.h"
#include "Runtime.h"

#include <format>

namespace NR
{
	void D3D12Interop::Check(HRESULT result)
	{
		if (SUCCEEDED(result))
			return;
		if (DeviceRemoved())
			throw std::runtime_error(std::format("NR D3D12 device removed (0x{:08X})", static_cast<uint32_t>(device->GetDeviceRemovedReason())));
		throw std::runtime_error(std::format("{}D3D12 call failed (HRESULT 0x{:08X})", kInitializationPrefix, static_cast<uint32_t>(result)));
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
			CreateRing(commands, D3D12_COMMAND_LIST_TYPE_DIRECT, L"NeuralRendering::Allocator", L"NeuralRendering::Commands");
			fence.Create(device.get(), device11.get(), "NeuralRendering::Fence");
			adapterLuid = desc.AdapterLuid;
		} catch (...) {
			// A half-built interop would make the next attempt reuse a device with no queue behind it.
			Reset();
			throw;
		}
	}

	void D3D12Interop::CreateRing(std::array<Commands, kFramesInFlight>& ring, D3D12_COMMAND_LIST_TYPE type, const wchar_t* allocatorName, const wchar_t* listName)
	{
		for (auto& slot : ring) {
			Check(device->CreateCommandAllocator(type, IID_PPV_ARGS(slot.allocator.put())));
			Check(device->CreateCommandList(0, type, slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.list.put())));
			Check(slot.allocator->SetName(allocatorName));
			Check(slot.list->SetName(listName));
			Check(slot.list->Close());
		}
	}

	void D3D12Interop::Reset()
	{
		commands = {};
		parallelCommands = {};
		parallelQueue = nullptr;
		parallelUsed = parallelFailed = false;
		fence.Reset();
		queue = nullptr;
		device = nullptr;
		context = nullptr;
		device11 = nullptr;
		adapterLuid = {};
		cursor = 0;
	}

	std::unique_ptr<WrappedResource> D3D12Interop::CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.Format = format;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return std::make_unique<WrappedResource>(desc, device11.get(), device.get(), name);
	}

	void D3D12Interop::Wait(uint64_t completion)
	{
		if (!fence.fence12)
			return;
		Check(device->GetDeviceRemovedReason());
		DWORD waitError = 0;
		const auto outcome = fence.CpuWaitOutcome(completion, SharedFence::kFenceTimeoutMs, &waitError);
		if (outcome == SharedFence::WaitOutcome::kComplete)
			return;
		Check(device->GetDeviceRemovedReason());
		if (outcome == SharedFence::WaitOutcome::kFailed)
			throw std::runtime_error(std::format("NR GPU fence {} wait failed (error 0x{:08X})", completion, waitError));
		throw std::runtime_error(std::format("NR GPU fence {} did not retire within {} ms", completion, SharedFence::kFenceTimeoutMs));
	}

	ID3D12GraphicsCommandList* D3D12Interop::Begin()
	{
		auto& slot = commands[cursor];
		Wait(slot.completion);
		winrt::check_hresult(slot.allocator->Reset());
		winrt::check_hresult(slot.list->Reset(slot.allocator.get(), nullptr));
		const auto ready = fence.Next();
		inputReady = ready;
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
		parallelSubmitted = parallelUsed;
		if (parallelUsed) {
			auto& parallel = parallelCommands[cursor];
			winrt::check_hresult(parallel.list->Close());
			ID3D12CommandList* parallelLists[]{ parallel.list.get() };
			parallelQueue->ExecuteCommandLists(1, parallelLists);
			const auto parallelDone = fence.Next();
			winrt::check_hresult(parallelQueue->Signal(fence.fence12.get(), parallelDone));
			parallel.completion = parallelDone;
			// The D3D11 wait below covers this work only because the main queue's completion follows it.
			winrt::check_hresult(queue->Wait(fence.fence12.get(), parallelDone));
			parallelUsed = false;
		}
		const auto complete = fence.Next();
		winrt::check_hresult(queue->Signal(fence.fence12.get(), complete));
		slot.completion = complete;
		winrt::check_hresult(context->Wait(fence.fence11.get(), complete));
		cursor = (cursor + 1) % static_cast<uint32_t>(commands.size());
	}

	ID3D12GraphicsCommandList* D3D12Interop::ParallelList()
	{
		if (parallelFailed)
			return nullptr;
		if (!parallelQueue) {
			try {
				D3D12_COMMAND_QUEUE_DESC queueDesc{};
				queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
				Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(parallelQueue.put())));
				Check(parallelQueue->SetName(L"NeuralRendering::ParallelQueue"));
				CreateRing(parallelCommands, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"NeuralRendering::ParallelAllocator", L"NeuralRendering::ParallelCommands");
			} catch (const std::exception& error) {
				logger::warn("[NeuralRendering] parallel eye queue unavailable, using one queue: {}", error.what());
				parallelCommands = {};
				parallelQueue = nullptr;
				parallelFailed = true;
				return nullptr;
			}
		}
		auto& slot = parallelCommands[cursor];
		Wait(slot.completion);
		winrt::check_hresult(slot.allocator->Reset());
		winrt::check_hresult(slot.list->Reset(slot.allocator.get(), nullptr));
		if (!parallelUsed)
			winrt::check_hresult(parallelQueue->Wait(fence.fence12.get(), inputReady));
		parallelUsed = true;
		return slot.list.get();
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
