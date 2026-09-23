#pragma once

#include "Buffer.h"

#include <array>
#include <d3d11_4.h>
#include <d3d12.h>

namespace NR
{
	struct SharedTexture
	{
		std::unique_ptr<Texture2D> texture;
		winrt::com_ptr<ID3D12Resource> resource;
	};

	/** @brief Owns the same-adapter NR queue and D3D11/D3D12 synchronization. */
	class D3D12Interop
	{
	public:
		/** @brief Creates a queue and shared fence on the renderer's adapter. */
		void Initialize();
		/** @brief Creates a typed D3D11 texture and opens it on the NR device. */
		SharedTexture CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name);
		/** @brief Acquires a retired allocator and queues the D3D11 input dependency. */
		ID3D12GraphicsCommandList* Begin();
		/** @brief Submits NR commands and queues the D3D11 output dependency. */
		void End();
		/** @brief Retires both APIs' work before resources or features are destroyed. */
		void Drain();
		/** @brief Returns the device used by NGX. */
		ID3D12Device* Device() const { return device.get(); }
		/** @brief Returns the most recently issued shared-fence value. */
		uint64_t SubmittedFence() const { return value; }
		/** @brief Samples GPU progress without waiting. */
		uint64_t CompletedFence() const { return fence ? fence->GetCompletedValue() : 0; }

	private:
		struct Commands
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> list;
			uint64_t completion = 0;
		};
		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D11DeviceContext4> context;
		winrt::com_ptr<ID3D11Fence> fence11;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12Fence> fence;
		winrt::handle event;
		std::array<Commands, 3> commands;
		uint32_t cursor = 0;
		uint64_t value = 0;
		void Wait(uint64_t completion);
	};
}
