#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace DX12Native
{
	// Shared CPU-side limit for prepared lights submitted to the native shadow RayQuery.
	inline constexpr uint32_t kMaxShadowLights = DX12::kMaxPreparedLights;
	inline constexpr float kDefaultShadowEmitterRadiusMultiplier = 0.2f;
	inline constexpr float kMaximumShadowEmitterRadiusMultiplier = 8.0f;

	/** @brief Shadow-only behavior. Live matched values override companion fallbacks. */
	struct ShadowTraceControls
	{
		uint32_t viewportWidth = 0, viewportHeight = 0;
		uint32_t densityMode = 0;
		bool nearLightExclusion = false;
		float nearLightRadius = 0.0f;  // 0..512 game units; exclusion sphere centered at light.
		float reachMultiplier = 1.0f;  // 1.0 means the vanilla-provided light radius.
		float fadeRange = 0.50f;       // Fraction of native shadow reach.
		float falloffCurve = 1.0f;
		bool stochasticSampling = true;
		float emitterRadiusMultiplier = kDefaultShadowEmitterRadiusMultiplier;
	};

	/** @brief Full render-size raw visibility; the composite owns reconstruction. */
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
		// RGBA32F: x weighted visibility, y 1 sampled / -1 omitted / 0 invalid,
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
			initializationFailed = false;
			failureLogged = false;
		}

	private:
		void Initialize(const GraphicsBackend& backend);
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		std::array<std::shared_ptr<ShadowMaskFrame>, 4> slots;
		std::shared_ptr<ShadowMaskFrame> current;
		uint64_t nextFence = 0;
		VideoMemoryBudget videoBudget;
		bool initializationFailed = false;
		bool failureLogged = false;
	};
}
