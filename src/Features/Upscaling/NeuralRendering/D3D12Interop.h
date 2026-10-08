#pragma once

#include "../DX12SwapChain.h"

#include <array>
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

namespace NR
{
	/** @brief Owns the same-adapter NR queue and D3D11/D3D12 synchronization. */
	class D3D12Interop
	{
	public:
		/** @brief Command-allocator ring depth; a slot is reused once its submission retires. */
		static constexpr uint32_t kFramesInFlight = 3;

		/** @brief Creates the device, queue, ring and shared fence on the renderer's adapter. */
		void Initialize();
		/** @brief Creates a named, view-bearing D3D11 texture shared with the NR device. */
		std::unique_ptr<WrappedResource> CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name);
		/** @brief Acquires a retired allocator and queues the D3D11 input dependency. */
		ID3D12GraphicsCommandList* Begin();
		/** @brief Submits NR commands and queues the D3D11 output dependency. */
		void End();
		/**
		 * @brief Returns a command list that runs on a second compute queue concurrently with Begin()'s list,
		 *        submitted by the same End(). Only a compute queue overlaps NR's launch-bound kernels; a second
		 *        direct queue serializes with the first. Call between Begin() and End().
		 * @return Null when the queue cannot be created; the caller then records on Begin()'s list.
		 */
		ID3D12GraphicsCommandList* ParallelList();
		/**
		 * @brief Retires all submitted work on both APIs.
		 *        Throws rather than releasing anything: on a failure the caller must keep the
		 *        resources alive, because the GPU may still reference them.
		 */
		void Drain();

		/** @brief Returns the device used by NGX. */
		ID3D12Device* Device() const { return device.get(); }
		/** @brief LUID of the adapter that device was created on, for the caller's success log. */
		[[nodiscard]] LUID AdapterLuid() const { return adapterLuid; }
		/** @brief Asks the device whether it was removed, so a caller can release instead of waiting. */
		[[nodiscard]] bool DeviceRemoved() const { return device && FAILED(device->GetDeviceRemovedReason()); }
		/** @brief Returns the most recently issued shared-fence value. */
		uint64_t SubmittedFence() const { return fence.value; }
		/** @brief Samples GPU progress without waiting. */
		uint64_t CompletedFence() const { return fence.fence12 ? fence.fence12->GetCompletedValue() : 0; }

	private:
		struct Commands
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> list;
			uint64_t completion = 0;
		};

		/** @brief Throws for a failed HRESULT, naming device removal when the device reports it. */
		void Check(HRESULT result);
		/** @brief Creates a named allocator and closed command list of one type for every slot of a ring. */
		void CreateRing(std::array<Commands, kFramesInFlight>& ring, D3D12_COMMAND_LIST_TYPE type, const wchar_t* allocatorName, const wchar_t* listName);
		/** @brief Blocks the CPU until completion retires; throws on a timeout or a removed device. */
		void Wait(uint64_t completion);
		/** @brief Releases every interop resource; drain first or the GPU may still read them. */
		void Reset();

		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D11DeviceContext4> context;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12CommandQueue> parallelQueue;
		std::array<Commands, kFramesInFlight> parallelCommands;
		uint64_t inputReady = 0;
		bool parallelUsed = false;
		bool parallelFailed = false;
		SharedFence fence;
		std::array<Commands, kFramesInFlight> commands;
		LUID adapterLuid{};
		uint32_t cursor = 0;
	};
}
