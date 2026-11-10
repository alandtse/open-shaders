#pragma once
// Local-video-memory budget policy adapted from the earlier open-RXRT tracer.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstdint>

namespace DX12Native
{
    struct VideoMemoryBudget
    {
        static constexpr uint64_t kMiB = 1024ull * 1024ull;
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
            result.effectiveBytes = manualLimitBytes ? manualLimitBytes : 512ull * kMiB;
            if (!budget) return result;
            result.dxgiAvailable = true;
            result.localBudget = budget;
            result.localUsage = usage;
            result.headroom = budget > usage ? budget - usage : 0ull;
            result.reserve = std::max(256ull * kMiB, std::min(1024ull * kMiB, budget / 16ull));
            const auto usable = result.headroom > result.reserve ? result.headroom - result.reserve : 0ull;
            const auto allowance = trackedBytes > UINT64_MAX - usable ? UINT64_MAX : trackedBytes + usable;
            result.effectiveBytes = manualLimitBytes ? std::min(manualLimitBytes, allowance) : allowance;
            return result;
        }

        Snapshot Query(ID3D12Device* device, uint64_t trackedBytes, uint64_t manualLimitBytes = 0)
        {
            last = Evaluate(0, 0, trackedBytes, manualLimitBytes);
            if (!device) return last;
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
                info.Budget == 0) return last;
            last = Evaluate(info.Budget, info.CurrentUsage, trackedBytes, manualLimitBytes);
            return last;
        }

    };
}
