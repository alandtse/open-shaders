#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "VideoMemoryBudget.h"
#include "../../../package/Shaders/Common/NativeShadowQuality.hlsli"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace DX12Native
{
	// Shared CPU-side limit for prepared lights submitted to the native shadow RayQuery.
	inline constexpr uint32_t kMaxShadowLights = DX12::kMaxPreparedLights;
	inline constexpr uint32_t kDefaultShadowQualityMode = kShadowQualityQuality;
	inline constexpr uint32_t kShadowShaderCount = kShadowQualityCount + 2;
	inline constexpr uint32_t ShadowShaderIndex(bool offloading, uint32_t quality)
	{
		return offloading ? kShadowQualityCount + (quality == kShadowQualityFull ? 0u : 1u) :
		                    (quality < kShadowQualityCount ? quality : kDefaultShadowQualityMode);
	}
	inline constexpr float kDefaultShadowReachMultiplier = 1.25f;
	inline constexpr float kDefaultShadowFadeRange = 0.36f;

	/** @brief Shadow-only behavior. Live matched values override companion fallbacks. */
	struct ShadowTraceControls
	{
		uint32_t viewportWidth = 0, viewportHeight = 0;
		uint32_t qualityMode = kDefaultShadowQualityMode;
		bool nearLightExclusion = false;
		float nearLightRadius = 0.0f;  // 0..512 game units; exclusion sphere centered at light.
		float reachMultiplier = kDefaultShadowReachMultiplier;  // 1.0 means vanilla light radius.
		float fadeRange = kDefaultShadowFadeRange;  // Fraction of native shadow reach.
		float falloffCurve = 1.0f;
		bool captureDebugLights = false;
		bool offloading = false;
	};

	/** @brief Physical raw texture and logical sampling grid for this frame. */
	struct ShadowSamplingLayout
	{
		uint32_t internalWidth = 0, internalHeight = 0;
		uint32_t logicalWidth = 0, logicalHeight = 0;
		uint32_t physicalWidth = 0, physicalHeight = 0;
		uint32_t qualityMode = kDefaultShadowQualityMode, checkerboardOrigin = 0;
		bool offloading = false;
	};

	/** @brief Packed raw visibility; the composite owns reconstruction. */
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
		uint32_t frame = 0;
		ShadowSamplingLayout sampling;
		std::vector<DX12::PreparedLight> debugLights;
		std::vector<std::array<float, 4>> lightRecords;
		bool lightRecordsUploaded = false;
		// RGBA32F: x weighted visibility, y 1 lit receiver / 0 neutral or invalid,
		// z receiver distance, w accumulated light weight.
	};

	/** @brief Native Scene DXR1.1 RayQuery dispatch, independent of Native Lighting and FSR. */
	class ShadowMaskTrace
	{
	public:
		~ShadowMaskTrace();
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
			initializationFailed = {};
			failureLogged = false;
		}

	private:
		void Initialize(const GraphicsBackend& backend, uint32_t variant);
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D12RootSignature> root;
		std::array<winrt::com_ptr<ID3D12PipelineState>, kShadowShaderCount> pipelines;
		std::array<std::shared_ptr<ShadowMaskFrame>, 4> slots;
		std::shared_ptr<ShadowMaskFrame> current;
		uint64_t nextFence = 0;
		VideoMemoryBudget videoBudget;
		std::array<bool, kShadowShaderCount> initializationFailed{};
		bool failureLogged = false;
	};
}
