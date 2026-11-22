#pragma once

#include "DX12SwapChain.h"
#include "Features/DX12/VideoMemoryBudget.h"
#include "GraphicsInterop.h"
#include "Streamline.h"

#include <array>
#include <d3d11_4.h>
#include <d3d12.h>
#include <memory>
#include <optional>
#include <string>
#include <winrt/base.h>

/** @brief DLSS Super Resolution on the DX12 Streamline instance, reading Native Composite's final color once. */
class NativeDLSS
{
public:
	/**
	 * @brief Upscales this frame's Native Composite color into a_colorOut through a GPU-fenced DX12 dispatch.
	 * @param a_colorOut DX11 display-resolution target that receives the upscaled scene.
	 * @param a_motionVectors Dilated motion vectors the DX11 encoder wrote.
	 * @param a_reactiveMask Reactive mask the DX11 encoder wrote.
	 * @param a_transparencyMask Transparency composition mask the DX11 encoder wrote.
	 * @param a_extentIn Render-resolution input extent.
	 * @param a_extentOut Display-resolution output extent.
	 * @return False when the native path cannot run this frame; the caller then evaluates DLSS on DX11.
	 */
	bool Upscale(ID3D11Resource* a_colorOut, ID3D11Resource* a_motionVectors, ID3D11Resource* a_reactiveMask,
		ID3D11Resource* a_transparencyMask, const sl::Extent& a_extentIn, const sl::Extent& a_extentOut);

	/** @brief Waits for submitted native work, then frees the DX12 DLSS feature and the shared textures. */
	void Reset();

	/** @brief True when the last DLSS frame was upscaled on DX12. */
	bool IsActive() const { return active; }

	/** @brief Why the native path stopped for this session; empty while it may run. */
	const std::string& GetFailure() const { return failure; }

	/** @brief Parks the shared textures and command lists instead of releasing them while the GPU may still read them. */
	~NativeDLSS();

private:
	enum Shared : uint32_t
	{
		kDepth,
		kMotionVectors,
		kReactiveMask,
		kTransparencyMask,
		kOutput,
		kSharedCount
	};

	struct CommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
		uint64_t fenceValue = 0;
	};

	/** @brief Keys the DLSS feature the way Streamline does; a change recreates it. */
	struct FeatureKey
	{
		uint32_t qualityMode = 0;
		uint32_t preset = 0;
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
		bool operator==(const FeatureKey&) const = default;
	};

	// One more than the back buffers DLSS-G can queue, so the oldest list has always finished.
	std::array<CommandContext, DX12SwapChain::kMaxBackBuffers + 1> commandContexts;
	std::array<std::unique_ptr<WrappedResource>, kSharedCount> shared;
	std::array<D3D11_TEXTURE2D_DESC, kSharedCount> sharedDescs{};
	uint64_t sharedBytes = 0;
	DX12Native::VideoMemoryBudget videoBudget;
	SharedFence fence;
	uint64_t lastSubmitted = 0;
	std::optional<FeatureKey> featureKey;
	bool active = false;
	std::string failure;

	bool EnsureStreamline();
	bool EnsureCommandContexts();
	bool EnsureSharedResources(const std::array<ID3D11Resource*, kSharedCount>& a_sources);
	CommandContext* AcquireCommandContext();
	/** @brief True once the GPU finished the last submitted dispatch; false when the bounded wait gave up. */
	bool WaitForSubmitted();
	void ParkInFlight();
	void Latch(std::string a_reason);
};
