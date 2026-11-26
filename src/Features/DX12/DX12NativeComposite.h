#pragma once

#include "DX12NativeShadowTrace.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <memory>
#include <optional>
#include <string_view>
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
			winrt::com_ptr<ID3D12Resource> color, reconstructedShadowMask;
			winrt::com_ptr<ID3D12Fence> producerFence;
			uint64_t producerValue = 0;
			uint32_t frame = 0, width = 0, height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool valid = false;
		};

		std::shared_ptr<const Frame> Capture(ID3D11Texture2D* source,
			uint32_t frame, uint32_t width, uint32_t height, Transfer transfer,
			const std::shared_ptr<const ShadowMaskFrame>& shadow);
		/** @brief Deliver shadowed color to the original DX11 input only when a consumer needs it. */
		bool ReturnToDX11(const std::shared_ptr<const Frame>& frame, ID3D11Resource* target);
		/** @brief Prevent reuse or fallback waits if a registered consumer could not signal. */
		void MarkUntracked(const std::shared_ptr<const Frame>& frame);
		bool Retain(const std::shared_ptr<const Frame>& frame, ID3D12Fence* fence, uint64_t value);
		~NativeComposite();
		/** @brief Allow a new initialization attempt after a scene reset. */
		void ResetFailures()
		{
			failedShadowDevice = nullptr;
			captureFailureLogged = false;
			lastCaptureRejection = {};
			returnFailureLogged = false;
		}

	private:
		struct Slot
		{
			std::shared_ptr<Frame> frame;
			// Retain producer textures until the composite GPU work completes.
			std::shared_ptr<const ShadowMaskFrame> shadowInput;
			std::shared_ptr<const RasterFrame> rasterInput;
			std::unique_ptr<WrappedResource> shared;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12CommandAllocator> returnAllocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> returnCommands;
			winrt::com_ptr<ID3D12DescriptorHeap> shadowDescriptors;
			winrt::com_ptr<ID3D12Resource> reconstructedMask;
			uint32_t maskWidth = 0, maskHeight = 0;
			winrt::com_ptr<ID3D12RootSignature> shadowRoot;
			winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
			SharedFence fence;
			std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
			GraphicsBackend backend;
			winrt::com_ptr<ID3D11Texture2D> source;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			bool supportsShadow = false, untracked = false;
			uint64_t returnValue = 0;
			uint64_t frameBytes = 0;
		};

		std::array<std::unique_ptr<Slot>, 4> slots;
		winrt::com_ptr<ID3D12RootSignature> shadowRoot;
		winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
		winrt::com_ptr<ID3D12Device> shadowDevice;
		winrt::com_ptr<ID3D12Device> failedShadowDevice;
		VideoMemoryBudget videoBudget;
		std::optional<GraphicsBackend> executionBackend;
		std::string_view lastCaptureRejection;
		bool returnFailureLogged = false;
		bool EnsureBackend(ID3D11Texture2D* source);
		void LogRejection(std::string_view reason);
		bool captureFailureLogged = false;
		bool InitializeShadow(ID3D12Device* device);
	};
}
