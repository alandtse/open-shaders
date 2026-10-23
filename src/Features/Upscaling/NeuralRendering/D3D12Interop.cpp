#include "D3D12Interop.h"

#include "../Streamline.h"
#include "Globals.h"

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
				throw std::runtime_error("requires an NVIDIA RTX GPU");
			// The level is a minimum: the runtime hands back the adapter's existing device.
			DX::ThrowIfFailed(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			DX::ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())));
			DX::ThrowIfFailed(queue->SetName(L"NeuralRendering::Queue"));
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
			DX::ThrowIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(slot.allocator.put())));
			DX::ThrowIfFailed(device->CreateCommandList(0, type, slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.list.put())));
			DX::ThrowIfFailed(slot.allocator->SetName(allocatorName));
			DX::ThrowIfFailed(slot.list->SetName(listName));
			DX::ThrowIfFailed(slot.list->Close());
		}
	}

	void D3D12Interop::Reset()
	{
		commands = {};
		parallelCommands = {};
		parallelQueue = nullptr;
		parallelFence.Reset();
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

	void D3D12Interop::Wait(SharedFence& target, uint64_t completion)
	{
		if (!target.fence12)
			return;
		DX::ThrowIfFailed(device->GetDeviceRemovedReason());
		DWORD waitError = 0;
		const auto outcome = target.CpuWaitOutcome(completion, SharedFence::kFenceTimeoutMs, &waitError);
		if (outcome == SharedFence::WaitOutcome::kComplete)
			return;
		DX::ThrowIfFailed(device->GetDeviceRemovedReason());
		if (outcome == SharedFence::WaitOutcome::kFailed)
			throw std::runtime_error(std::format("NR GPU fence {} wait failed (error 0x{:08X})", completion, waitError));
		throw std::runtime_error(std::format("NR GPU fence {} did not retire within {} ms", completion, SharedFence::kFenceTimeoutMs));
	}

	ID3D12GraphicsCommandList* D3D12Interop::Begin()
	{
		auto& slot = commands[cursor];
		Wait(fence, slot.completion);
		DX::ThrowIfFailed(slot.allocator->Reset());
		DX::ThrowIfFailed(slot.list->Reset(slot.allocator.get(), nullptr));
		const auto ready = fence.Next();
		inputReady = ready;
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
		parallelSubmitted = parallelUsed;
		if (parallelUsed) {
			auto& parallel = parallelCommands[cursor];
			DX::ThrowIfFailed(parallel.list->Close());
			ID3D12CommandList* parallelLists[]{ parallel.list.get() };
			parallelQueue->ExecuteCommandLists(1, parallelLists);
			const auto parallelDone = parallelFence.Next();
			parallel.completion = parallelDone;
			DX::ThrowIfFailed(parallelQueue->Signal(parallelFence.fence12.get(), parallelDone));
			DX::ThrowIfFailed(queue->Wait(parallelFence.fence12.get(), parallelDone));
			parallelUsed = false;
		}
		const auto complete = fence.Next();
		DX::ThrowIfFailed(queue->Signal(fence.fence12.get(), complete));
		slot.completion = complete;
		DX::ThrowIfFailed(context->Wait(fence.fence11.get(), complete));
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
				DX::ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(parallelQueue.put())));
				DX::ThrowIfFailed(parallelQueue->SetName(L"NeuralRendering::ParallelQueue"));
				CreateRing(parallelCommands, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"NeuralRendering::ParallelAllocator", L"NeuralRendering::ParallelCommands");
				parallelFence.Create(device.get(), device11.get(), "NeuralRendering::ParallelFence");
			} catch (const winrt::hresult_error& error) {
				logger::warn("[NeuralRendering] parallel eye queue unavailable, using one queue: {}", winrt::to_string(error.message()));
				parallelCommands = {};
				parallelQueue = nullptr;
				parallelFence.Reset();
				parallelFailed = true;
				return nullptr;
			} catch (const std::exception& error) {
				logger::warn("[NeuralRendering] parallel eye queue unavailable, using one queue: {}", error.what());
				parallelCommands = {};
				parallelQueue = nullptr;
				parallelFence.Reset();
				parallelFailed = true;
				return nullptr;
			}
		}
		auto& slot = parallelCommands[cursor];
		Wait(parallelFence, slot.completion);
		DX::ThrowIfFailed(slot.allocator->Reset());
		DX::ThrowIfFailed(slot.list->Reset(slot.allocator.get(), nullptr));
		if (!parallelUsed)
			DX::ThrowIfFailed(parallelQueue->Wait(fence.fence12.get(), inputReady));
		parallelUsed = true;
		return slot.list.get();
	}

	void D3D12Interop::Drain()
	{
		if (!fence.fence12 || !fence.fence11)
			return;
		const auto ready = fence.Next();
		DX::ThrowIfFailed(context->Signal(fence.fence11.get(), ready));
		context->Flush();
		DX::ThrowIfFailed(queue->Wait(fence.fence12.get(), ready));
		if (parallelQueue) {
			// End may fail after parallel submission but before joining it.
			const auto parallelComplete = parallelFence.Next();
			DX::ThrowIfFailed(parallelQueue->Signal(parallelFence.fence12.get(), parallelComplete));
			DX::ThrowIfFailed(queue->Wait(parallelFence.fence12.get(), parallelComplete));
		}
		const auto complete = fence.Next();
		DX::ThrowIfFailed(queue->Signal(fence.fence12.get(), complete));
		Wait(fence, complete);
	}
}
