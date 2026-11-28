#pragma once

#include "DX12FenceRetirementPolicy.h"

#include <Windows.h>
#include <cstdint>
#include <d3d12.h>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <winrt/base.h>

namespace DX12Native
{
	// Completion sentinels mean device removal, never successful GPU work.
	inline bool GpuFenceRetired(ID3D12Fence* fence, uint64_t value)
	{
		return FenceValueRetired(fence != nullptr, value && fence ? fence->GetCompletedValue() : 0, value);
	}

	inline bool GpuDeviceRemoved(ID3D12Device* device)
	{
		return device && FAILED(device->GetDeviceRemovedReason());
	}

	struct FenceDependency
	{
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t value = 0;
	};

	inline FenceDependency FencePoint(ID3D12Fence* fence, uint64_t value)
	{
		FenceDependency result;
		if (fence)
			result.fence.copy_from(fence);
		result.value = value;
		return result;
	}

	namespace Detail
	{
		// The OS wait owns this registration until a single completion callback.
		// No plugin thread or BS pool slot waits on a GPU fence.
		struct FenceWait
		{
			FenceDependency dependency;
			std::function<void()> complete;
			HANDLE event = nullptr;
			PTP_WAIT wait = nullptr;

			~FenceWait()
			{
				if (wait)
					CloseThreadpoolWait(wait);
				if (event)
					CloseHandle(event);
			}

			static void CALLBACK OnReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) noexcept
			{
				std::unique_ptr<FenceWait> registration(static_cast<FenceWait*>(context));
				try {
					registration->complete();
				} catch (...) {
					OutputDebugStringA("DX12 fence completion callback failed\n");
				}
			}
		};
	}

	/** @brief Register a one-shot fence callback; false means no callback was registered. */
	inline bool NotifyOnFence(FenceDependency dependency, std::function<void()> complete)
	{
		if (!dependency.fence || !dependency.value || !complete)
			return false;
		auto registration = std::make_unique<Detail::FenceWait>();
		registration->dependency = std::move(dependency);
		registration->complete = std::move(complete);
		registration->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!registration->event)
			return false;
		registration->wait = CreateThreadpoolWait(&Detail::FenceWait::OnReady, registration.get(), nullptr);
		if (!registration->wait)
			return false;
		if (FAILED(registration->dependency.fence->SetEventOnCompletion(
				registration->dependency.value, registration->event)))
			return false;

		// Transfer ownership before arming: an already-signalled fence may invoke
		// its callback immediately. Neither handle is accessed after this call.
		auto* armed = registration.release();
		SetThreadpoolWait(armed->wait, armed->event, nullptr);
		return true;
	}

	namespace Detail
	{
		template <class Owner>
		struct Retirement
		{
			Owner owner;
			winrt::com_ptr<ID3D12Device> device;
			std::vector<FenceDependency> dependencies;
			size_t next = 0;

			static void Continue(std::shared_ptr<Retirement> pending)
			{
				if (GpuDeviceRemoved(pending->device.get()))
					return;
				while (pending->next < pending->dependencies.size()) {
					const auto& dependency = pending->dependencies[pending->next];
					if (GpuFenceRetired(dependency.fence.get(), dependency.value)) {
						++pending->next;
						continue;
					}

					// A UINT64_MAX dependency is deliberately held until device removal:
					// it represents submitted work for which no completion marker exists.
					try {
						if (NotifyOnFence(dependency, [pending] { Continue(pending); }))
							return;
					} catch (...) {
						// Preserve the owner even if allocating a wait registration fails.
					}
					if (GpuDeviceRemoved(pending->device.get()) ||
						GpuFenceRetired(dependency.fence.get(), dependency.value)) {
						++pending->next;
						continue;
					}

					// An untrackable submission or OS registration failure cannot be
					// treated as completion. Quarantine for process lifetime rather than
					// free resources the GPU may still access or start a polling thread.
					OutputDebugStringA("DX12 GPU retirement registration failed; retaining resources until process exit\n");
					(void)new std::shared_ptr<Retirement>(std::move(pending));
					return;
				}
			}
		};
	}

	/** @brief Retain a GPU owner until every producer and consumer fence retires. */
	template <class Owner>
	void RetireAfterFences(Owner held, winrt::com_ptr<ID3D12Device> device,
		std::vector<FenceDependency> dependencies)
	{
		using Record = Detail::Retirement<Owner>;
		Record::Continue(std::make_shared<Record>(Record{
			std::move(held), std::move(device), std::move(dependencies) }));
	}
}
