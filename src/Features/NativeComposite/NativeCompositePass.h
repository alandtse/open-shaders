#pragma once

#include "GraphicsInterop.h"
#include "Features/NativeLighting/NativeLighting.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

/** @brief Copies the completed raster scene into a DX12-owned frame. */
class NativeComposite
{
public:
	/** @brief Interpretation of the source values; the pass performs no conversion. */
	enum class Transfer { Unknown, Linear, LegacyScene };
	enum class Gamut { Unknown };
	struct Policy
	{
		uint32_t diffuse = 1; // Effective validity: display reference-compatible recovered contributions.
		uint32_t specular = 1;
		uint32_t visual = 0;
	};
	struct Frame
	{
		winrt::com_ptr<ID3D12Resource> color;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12Fence> producerFence;
		uint64_t producerValue = 0;
		uint64_t validityRevision = 0;
		uint32_t frame = 0, width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		Transfer transfer = Transfer::Unknown;
		Gamut gamut = Gamut::Unknown;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		bool valid = false;
		bool lightingSubmitted = false;
		bool shadowSubmitted = false;
		/** @brief Reports whether the producer fence completed without device removal. */
		bool IsComplete() const
		{
			if (!valid || !producerFence || !producerValue)
				return false;
			const auto completed = producerFence->GetCompletedValue();
			return completed != UINT64_MAX && completed >= producerValue;
		}
	};

	/** @brief Captures one frame without waiting for the GPU on the CPU. */
	std::shared_ptr<const Frame> Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
		ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, Transfer a_transfer,
		const std::shared_ptr<const NativeLightingFeature::Contribution>& a_lighting,
		const std::shared_ptr<const DX12Native::ShadowFrame>& a_shadow,
		const std::shared_ptr<const DX12Native::RasterFrame>& a_raster,
		const DX12NativeScene::ShadowLayerSettings& a_shadowSettings,
		float a_strength, const Policy& a_policy, uint64_t a_revision);
	/** @brief Retains a frame until a downstream consumer's GPU fence completes. */
	bool Retain(const std::shared_ptr<const Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
	/** @brief Returns a short explanation of the latest capture result. */
	const std::string& Status() const { return status; }
	uint64_t CompletedComposites() const { return completedComposites.load(); }
	uint64_t AcceptedPixels() const { return acceptedPixels.load(); }
	uint64_t ModifiedPixels() const { return modifiedPixels.load(); }
	uint64_t VisualizationPixels() const { return visualizationPixels.load(); }
	uint64_t EffectiveRecoveredPixels() const { return effectiveRecoveredPixels.load(); }
	uint64_t FiniteOnlyPixels() const { return finiteOnlyPixels.load(); }
	uint64_t CounterReadFailures() const { return counterReadFailures.load(); }
	~NativeComposite();

private:
	struct Slot
	{
		std::shared_ptr<Frame> frame;
		std::unique_ptr<WrappedResource> shared;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12DescriptorHeap> lightingDescriptors, shadowDescriptors;
		winrt::com_ptr<ID3D12RootSignature> shadowRoot;
		winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Resource> counters, counterReset, counterReadback;
		SharedFence fence;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		ID3D12Device* device = nullptr;
		ID3D11Device* device11 = nullptr;
		D3D11_TEXTURE2D_DESC sourceDesc{};
		bool supportsLighting = false;
		uint64_t frameBytes = 0;
		bool countersRead = true;
		bool untracked = false;
	};
	std::array<std::unique_ptr<Slot>, 4> slots;
	winrt::com_ptr<ID3D12RootSignature> root;
	winrt::com_ptr<ID3D12PipelineState> pipeline;
	ID3D12Device* pipelineDevice = nullptr, *shadowPipelineDevice = nullptr;
	winrt::com_ptr<ID3D12RootSignature> shadowRoot;
	winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
	std::atomic<uint64_t> completedComposites{ 0 }, acceptedPixels{ 0 }, modifiedPixels{ 0 }, visualizationPixels{ 0 }, counterReadFailures{ 0 };
	std::atomic<uint64_t> effectiveRecoveredPixels{ 0 }, finiteOnlyPixels{ 0 };
	DX12Native::VideoMemoryBudget videoBudget;
	bool InitializeLighting(ID3D12Device* a_device);
	bool InitializeShadow(ID3D12Device* a_device);
	std::string status = "Not captured";
};
