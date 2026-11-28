#pragma once

#include "../../../package/Shaders/Common/NativeShadowRT.hlsli"
#include "DX12NativeAcceleration.h"
#include "DX12NativeMovers.h"
#include "DX12NativeRasterCapture.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace DX12Native
{
	struct NativeLightPublication;
	// Shared CPU-side limit for prepared lights submitted to the native shadow RayQuery.
	inline constexpr uint32_t kMaxShadowLights = DX12::kMaxPreparedLights;

	/** @brief Shadow-only behavior. Live matched values override companion fallbacks. */
	struct ShadowTraceControls
	{
		uint32_t viewportWidth = 0, viewportHeight = 0;
		uint32_t quality = kShadowQualityOffloading;
		bool captureDebugLights = false;
		uint32_t debugPreparedCount = 0;
		std::shared_ptr<const NativeLightPublication> nativeLights;
		bool uploadLights = false;
		bool captureLightVisibility = false;
		MoverScene* movers = nullptr;  // Rigid movers traced alongside the static scene this frame.
	};

	/** @brief Quarter-resolution raw visibility and receiver distance; the composite reconstructs the full view. */
	struct ShadowMaskFrame
	{
		std::shared_ptr<const TopLevel> scene;
		std::shared_ptr<const RasterFrame> raster;
		std::shared_ptr<const NativeLightPublication> nativeLights;
		winrt::com_ptr<ID3D12Resource> mask, cameraUpload, lightUpload;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t frame = 0, width = 0, height = 0;
		uint32_t maskWidth = 0, maskHeight = 0;
		uint32_t quality = kShadowQualityOffloading;
		std::vector<DX12::PreparedLight> debugLights;
		uint32_t debugPreparedCount = 0;
		std::vector<std::array<float, 4>> lightRecords;
		bool lightRecordsUploaded = false;
		bool uploadedLights = false;
		uint32_t pipelineVariant = 0;
		// R8 visibility per submitted light (slice = light index), read by DX11 lighting. The next
		// frame's raster fence orders DX12 reuse behind those reads.
		std::unique_ptr<WrappedResource> lightVisibility;
		winrt::com_ptr<ID3D12DescriptorHeap> clearDescriptors;
		winrt::com_ptr<ID3D11Fence> lightVisibilityFence;
		uint64_t lightVisibilityValue = 0;
		uint32_t lightVisibilityCapacity = 0, lightVisibilityWidth = 0;
		std::vector<DX12::PreparedLight> visibilityLights;
		// RG32F: weighted visibility and receiver distance (zero for invalid receivers).
	};

	/** @brief Native Scene DXR1.1 RayQuery dispatch, independent of Native Lighting and FSR. */
	class ShadowMaskTrace
	{
	public:
		enum class Outcome : uint8_t
		{
			None,
			InitializationFailed,
			NoRaster,
			NoScene,
			NoLights,
			NoFreeSlot,
			ViewportMismatch,
			Failed,
			Dispatched
		};
		Outcome LastOutcome() const { return lastOutcome.load(std::memory_order_relaxed); }
		uint32_t LastLightCount() const { return lastLightCount.load(std::memory_order_relaxed); }
		uint32_t LastVisibilityCount() const { return lastVisibilityCount.load(std::memory_order_relaxed); }
		void ReportSkipped(Outcome outcome)
		{
			lastLightCount.store(0, std::memory_order_relaxed);
			lastVisibilityCount.store(0, std::memory_order_relaxed);
			lastOutcome.store(outcome, std::memory_order_relaxed);
		}
		~ShadowMaskTrace();
		/** @brief Release completed, unborrowed inputs before the next raster capture. */
		void BeginFrame(uint32_t frame);
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const RasterFrame> raster,
			std::shared_ptr<const TopLevel> scene, std::span<const DX12::PreparedLight> lights,
			uint32_t frame, const ShadowTraceControls& controls);
		std::shared_ptr<const ShadowMaskFrame> Get(uint32_t frame) const
		{
			return current && current->frame == frame ? current : nullptr;
		}
		/** @brief The frame before a_frame when it shares a_frame's mask layout, so lighting can average both. */
		std::shared_ptr<const ShadowMaskFrame> GetPrevious(uint32_t frame) const
		{
			const auto now = Get(frame);
			return now && previous && previous->frame + 1 == frame && previous->lightVisibility &&
			               previous->lightVisibilityWidth == now->lightVisibilityWidth &&
			               previous->maskHeight == now->maskHeight && previous->quality == now->quality ?
			           previous :
			           nullptr;
		}
		void Reset()
		{
			current.reset();
			previous.reset();
			ReportSkipped(Outcome::None);
			initializationFailed.fill(false);
			failureLogged = sliceFailureLogged = false;
		}

	private:
		void Initialize(const GraphicsBackend& backend, uint32_t variant);
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Fence> completion;
		SharedFence visibilityReady;
		std::array<winrt::com_ptr<ID3D12RootSignature>, 2> roots;
		std::array<winrt::com_ptr<ID3D12PipelineState>, kShadowTracePipelineCount> pipelines;
		std::array<std::shared_ptr<ShadowMaskFrame>, 4> slots;
		std::shared_ptr<ShadowMaskFrame> current, previous;  // Previous stays unreused while lighting may still read it.
		uint64_t nextFence = 0;
		VideoMemoryBudget videoBudget;
		std::array<bool, kShadowTracePipelineCount> initializationFailed{};
		bool failureLogged = false, sliceFailureLogged = false;
		std::atomic<Outcome> lastOutcome{ Outcome::None };
		std::atomic<uint32_t> lastLightCount{ 0 };
		std::atomic<uint32_t> lastVisibilityCount{ 0 };
	};
}
