#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "VideoMemoryBudget.h"
#include "../../../package/Shaders/Common/NativeShadowRT.hlsli"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace DX12Native
{
	// Shared CPU-side limit for prepared lights submitted to the native shadow RayQuery.
	inline constexpr uint32_t kMaxShadowLights = DX12::kMaxPreparedLights;

	/** @brief Shadow-only behavior. Live matched values override companion fallbacks. */
	struct ShadowTraceControls
	{
		uint32_t viewportWidth = 0, viewportHeight = 0;
		uint32_t quality = kShadowQualityOffloading;
		bool captureDebugLights = false;
		uint32_t debugPreparedCount = 0;
	};

	/** @brief Quarter-resolution raw visibility and receiver distance; the composite reconstructs the full view. */
	struct ShadowMaskFrame
	{
		std::shared_ptr<const TopLevel> scene;
		std::shared_ptr<const RasterFrame> raster;
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
		// RG32F: weighted visibility and receiver distance (zero for invalid receivers).
	};

	/** @brief Native Scene DXR1.1 RayQuery dispatch, independent of Native Lighting and FSR. */
	class ShadowMaskTrace
	{
	public:
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
		void Reset()
		{
			current.reset();
			initializationFailed.fill(false);
			failureLogged = false;
		}

	private:
		void Initialize(const GraphicsBackend& backend, uint32_t variant);
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D12RootSignature> root;
		std::array<winrt::com_ptr<ID3D12PipelineState>, kShadowVariantCount> pipelines;
		std::array<std::shared_ptr<ShadowMaskFrame>, 4> slots;
		std::shared_ptr<ShadowMaskFrame> current;
		uint64_t nextFence = 0;
		VideoMemoryBudget videoBudget;
		std::array<bool, kShadowVariantCount> initializationFailed{};
		bool failureLogged = false;
	};
}
