#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <d3d12.h>
#include <thread>
#include <utility>
#include <winrt/base.h>

namespace DX12Native
{
	// Completion sentinels mean device removal, never successful GPU work.
	inline bool GpuFenceRetired(ID3D12Fence* fence, uint64_t value)
	{
		if (!value)
			return true;
		if (!fence)
			return false;
		const uint64_t completed = fence->GetCompletedValue();
		return completed == UINT64_MAX || (value != UINT64_MAX && completed >= value);
	}

	inline bool GpuDeviceRemoved(ID3D12Device* device)
	{
		return device && FAILED(device->GetDeviceRemovedReason());
	}

	// Keep each outstanding frame owned until its producer and consumer GPU
	// dependencies retire. The bounded pool size limits shutdown tasks to four
	// per capture; unlike deliberately leaked pointers, they release on completion.
	template <class Owner, class CanRetire>
	void RetireWhenGpuDone(Owner held, winrt::com_ptr<ID3D12Device> device, CanRetire canRetire)
	{
		std::thread([held = std::move(held), device = std::move(device),
						canRetire = std::move(canRetire)]() mutable {
			while (!GpuDeviceRemoved(device.get()) && !canRetire(*held))
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
		}).detach();
	}
}
