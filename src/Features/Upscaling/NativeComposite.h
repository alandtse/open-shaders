#pragma once

#include "GraphicsInterop.h"
#include <array>
#include <memory>
#include <vector>

/** @brief Copies the completed raster scene into a DX12-owned frame. */
class NativeComposite
{
public:
	/** @brief Interpretation of the source values; the pass performs no conversion. */
	enum class Transfer { Unknown, Linear, LegacyScene };
	enum class Gamut { Unknown };
	struct Frame
	{
		winrt::com_ptr<ID3D12Resource> color;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12Fence> producerFence;
		uint64_t producerValue = 0;
		uint32_t frame = 0, width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		Transfer transfer = Transfer::Unknown;
		Gamut gamut = Gamut::Unknown;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	};

	/** @brief Captures one frame without waiting for the GPU on the CPU. */
	std::shared_ptr<const Frame> Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
		ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, Transfer a_transfer);
	/** @brief Retains a frame until a downstream consumer's GPU fence completes. */
	bool Retain(const std::shared_ptr<const Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
	/** @brief Returns a short explanation of the latest capture result. */
	const std::string& Status() const { return status; }
	~NativeComposite();

private:
	struct Slot
	{
		std::shared_ptr<Frame> frame;
		std::unique_ptr<WrappedResource> shared;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		SharedFence fence;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		ID3D12Device* device = nullptr;
		ID3D11Device* device11 = nullptr;
		D3D11_TEXTURE2D_DESC sourceDesc{};
		bool untracked = false;
	};
	std::array<std::unique_ptr<Slot>, 3> slots;
	std::string status = "Not captured";
};
