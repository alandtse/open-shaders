#pragma once
// Local-video-memory budget policy adapted from the earlier open-RXRT tracer.
#include <algorithm>
#include <atomic>
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
		static constexpr uint32_t kDefaultHeadroomPercent = 10;
		inline static std::atomic<uint32_t> globalHeadroomPercent{ kDefaultHeadroomPercent };

		static uint32_t HeadroomPercent()
		{
			return globalHeadroomPercent.load(std::memory_order_relaxed);
		}

		static void SetHeadroomPercent(uint32_t percent)
		{
			globalHeadroomPercent.store(std::clamp(percent, 1u, 50u), std::memory_order_relaxed);
		}
		struct Snapshot
		{
			uint64_t localBudget = 0, localUsage = 0, headroom = 0, reserve = 0;
			uint64_t effectiveBytes = 0;
			bool dxgiAvailable = false;
		};
		winrt::com_ptr<IDXGIAdapter3> adapter;
		LUID adapterLuid{};
		Snapshot last{};

		/** @brief Applies DXGI headroom and an optional manual limit without double-counting tracked allocations. */
		static Snapshot Evaluate(uint64_t budget, uint64_t usage, uint64_t trackedBytes, uint64_t manualLimitBytes = 0)
		{
			Snapshot result{};
			// Never guess a 512 MiB fallback without DXGI adapter telemetry.
			// Consumers defer new allocations until the budget can be queried.
			result.effectiveBytes = 0;
			if (!budget)
				return result;
			result.dxgiAvailable = true;
			result.localBudget = budget;
			result.localUsage = usage;
			result.headroom = budget > usage ? budget - usage : 0ull;
			// Reserve a globally configured fraction of the *total* adapter
			// budget, not a per-feature slice. Re-query each time so the
			// existing shader, AS and material budgets adapt at runtime.
			const uint64_t percent = HeadroomPercent();
			result.reserve = (budget / 100) * percent + ((budget % 100) * percent) / 100;
			const auto usable = result.headroom > result.reserve ? result.headroom - result.reserve : 0ull;
			const auto allowance = trackedBytes > UINT64_MAX - usable ? UINT64_MAX : trackedBytes + usable;
			result.effectiveBytes = manualLimitBytes ? std::min(manualLimitBytes, allowance) : allowance;
			return result;
		}

		Snapshot Query(ID3D12Device* device, uint64_t trackedBytes, uint64_t manualLimitBytes = 0)
		{
			last = Evaluate(0, 0, trackedBytes, manualLimitBytes);
			if (!device)
				return last;
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
				return last;
			last = Evaluate(info.Budget, info.CurrentUsage, trackedBytes, manualLimitBytes);
			return last;
		}
	};
}
