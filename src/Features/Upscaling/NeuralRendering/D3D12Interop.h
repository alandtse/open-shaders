#pragma once

#include "../../../Buffer.h"

#include "../DX12SwapChain.h"

#include <array>
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

namespace NR
{
	/** @brief A D3D11 texture with a D3D12 handle onto the same memory, for one NR resource. */
	struct SharedTexture
	{
		std::unique_ptr<Texture2D> texture;
		winrt::com_ptr<ID3D12Resource> resource;
	};

	/** @brief Owns the NR queue and the D3D11/D3D12 synchronization on the renderer's adapter. */
	class D3D12Interop
	{
	public:
		/** @brief CPU bound for one fence value; a wedged GPU must not hang the render thread. */
		static constexpr DWORD kFenceTimeoutMs = 5000;
		/** @brief Command-allocator ring depth; a slot is reused once its submission retires. */
		static constexpr uint32_t kFramesInFlight = 3;

		/** @brief Creates the device, queue, ring and shared fence on the renderer's adapter. */
		void Initialize();
		/** @brief Releases every interop resource; drain first or the GPU may still read them. */
		void Reset();
		/** @brief Creates a D3D11 texture and opens the same memory on the D3D12 device. */
		SharedTexture CreateTexture(uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, const std::string& a_name);
		/** @brief Acquires a retired allocator and queues the D3D11 input dependency. */
		ID3D12GraphicsCommandList* Begin();
		/** @brief Submits the recorded commands and queues the D3D11 output dependency. */
		void End();
		/**
		 * @brief Retires all submitted work on both APIs.
		 *        Throws rather than releasing anything: on a failure the caller must keep the
		 *        resources alive, because the GPU may still reference them.
		 */
		void Drain();

		/** @brief Returns the device Feature 18 is initialized on. */
		[[nodiscard]] ID3D12Device* Device() const { return device.get(); }
		/**
		 * @brief Counts successful Initialize calls, so a caller can tell a rebuilt device from the
		 *        one it opened its shared textures on.
		 */
		[[nodiscard]] uint32_t Generation() const { return generation; }
		/** @brief LUID of the adapter that device was created on, for the caller's success log. */
		[[nodiscard]] LUID AdapterLuid() const { return adapterLuid; }
		/** @brief True once Initialize has completed and Reset has not run since. */
		[[nodiscard]] bool Initialized() const { return static_cast<bool>(device); }
		/** @brief Returns the most recently issued shared-fence value; read from any thread. */
		[[nodiscard]] uint64_t Submitted() const { return fence.value.load(std::memory_order_relaxed); }
		/** @brief Samples GPU progress without waiting. */
		[[nodiscard]] uint64_t Completed() const { return fence.fence12 ? fence.fence12->GetCompletedValue() : 0; }

	private:
		struct Commands
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> list;
			uint64_t completion = 0;
		};

		/** @brief Blocks the CPU until a_completion retires; throws on a timeout or a removed device. */
		void Wait(uint64_t a_completion);

		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D11DeviceContext4> context;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		SharedFence fence;
		std::array<Commands, kFramesInFlight> commands;
		LUID adapterLuid{};
		uint32_t cursor = 0;
		uint32_t generation = 0;
	};
}
