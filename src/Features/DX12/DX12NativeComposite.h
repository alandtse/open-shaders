#pragma once

#include "DX12NativeShadowTrace.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <memory>
#include <vector>

namespace DX12Native
{
	/** @brief Native Composite owns the final pre-upscale DX12 color, as in open-RT. */
	class NativeComposite
	{
	public:
		enum class Transfer
		{
			Unknown,
			Linear,
			LegacyScene
		};

		struct Frame
		{
			winrt::com_ptr<ID3D12Resource> color;
			winrt::com_ptr<ID3D12Fence> producerFence;
			uint64_t producerValue = 0;
			uint32_t frame = 0, width = 0, height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool valid = false;
			bool IsComplete() const
			{
				if (!valid || !producerFence || !producerValue)
					return false;
				const auto completed = producerFence->GetCompletedValue();
				return completed != UINT64_MAX && completed >= producerValue;
			}
		};

		std::shared_ptr<const Frame> Capture(ID3D11Device5* device11,
			ID3D11DeviceContext4* context11, ID3D12Device* device12,
			ID3D12CommandQueue* queue, ID3D11Texture2D* source,
			uint32_t frame, uint32_t width, uint32_t height, Transfer transfer,
			const std::shared_ptr<const ShadowMaskFrame>& shadow,
			bool returnColorToDX11);
		bool Retain(const std::shared_ptr<const Frame>& frame, ID3D12Fence* fence, uint64_t value);
		~NativeComposite();
		/** @brief Allow a new initialization attempt after a scene reset. */
		void ResetFailures()
		{
			failedShadowDevice = nullptr;
			captureFailureLogged = false;
		}

	private:
		struct Slot
		{
			std::shared_ptr<Frame> frame;
			// Retain producer textures until the composite GPU work completes.
			std::shared_ptr<const ShadowMaskFrame> shadowInput;
			std::shared_ptr<const RasterFrame> rasterInput;
			std::unique_ptr<WrappedResource> shared;
			std::unique_ptr<WrappedResource> returned;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12DescriptorHeap> shadowDescriptors;
			winrt::com_ptr<ID3D12RootSignature> shadowRoot;
			winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
			SharedFence fence;
			std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
			ID3D12Device* device = nullptr;
			ID3D11Device* device11 = nullptr;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			bool supportsShadow = false, hasReturn = false, untracked = false;
			uint64_t frameBytes = 0;
		};

		std::array<std::unique_ptr<Slot>, 4> slots;
		winrt::com_ptr<ID3D12RootSignature> shadowRoot;
		winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
		ID3D12Device* shadowDevice = nullptr;
		winrt::com_ptr<ID3D12Device> failedShadowDevice;
		VideoMemoryBudget videoBudget;
		bool captureFailureLogged = false;
		bool InitializeShadow(ID3D12Device* device);
	};
}
