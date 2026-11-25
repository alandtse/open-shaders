#pragma once
// Local-video-memory budget policy adapted from the earlier open-RXRT tracer.
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>

namespace DX12Native
{
	struct VideoMemoryBudget
	{
		static constexpr uint64_t kMiB = 1024ull * 1024ull;
		// One process-wide policy shared by every native DX12 budget consumer.
		// DXGI CurrentUsage already includes all resources on this adapter:
		// never sum independently tracked allocations to compute the reserve.
		static constexpr uint32_t kHeadroomPercent = 10;

		struct Snapshot
		{
			uint64_t localBudget = 0, localUsage = 0;
			uint64_t effectiveBytes = 0;
		};
		winrt::com_ptr<IDXGIAdapter3> adapter;
		LUID adapterLuid{};

		/** @brief Applies DXGI headroom without double-counting tracked allocations. */
		static Snapshot Evaluate(uint64_t budget, uint64_t usage, uint64_t trackedBytes)
		{
			Snapshot result{};
			// Never guess a 512 MiB fallback without DXGI adapter telemetry.
			// Consumers defer new allocations until the budget can be queried.
			if (!budget)
				return result;
			result.localBudget = budget;
			result.localUsage = usage;
			const auto headroom = budget > usage ? budget - usage : 0ull;
			// Reserve a fixed fraction of the *total* adapter
			// budget, not a per-feature slice. Re-query each time so the
			// existing shader, AS and material budgets adapt at runtime.
			const uint64_t percent = kHeadroomPercent;
			const auto reserve = (budget / 100) * percent + ((budget % 100) * percent) / 100;
			const auto usable = headroom > reserve ? headroom - reserve : 0ull;
			result.effectiveBytes = trackedBytes > UINT64_MAX - usable ? UINT64_MAX : trackedBytes + usable;
			return result;
		}

		Snapshot Query(ID3D12Device* device, uint64_t trackedBytes)
		{
			if (!device)
				return {};
			const auto luid = device->GetAdapterLuid();
			if (!adapter || adapterLuid.LowPart != luid.LowPart || adapterLuid.HighPart != luid.HighPart) {
				adapter = nullptr;
				winrt::com_ptr<IDXGIFactory4> factory;
				if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))) &&
					SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()))))
					adapterLuid = luid;
			}
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ||
				info.Budget == 0)
				return {};
			return Evaluate(info.Budget, info.CurrentUsage, trackedBytes);
		}
	};
}
