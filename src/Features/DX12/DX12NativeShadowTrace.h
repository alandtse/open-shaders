#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace DX12Native
{
	/** @brief Shadow-only behavior. Prepared lights keep their vanilla-supplied values. */
	struct ShadowTraceControls
	{
		uint32_t densityMode = 0;
		uint32_t debugGeometryMode = 0;  // 0 normal, 1 all TLAS, 2 companion, 3 runtime.
		uint32_t emitterSamples = 4;
		bool stochastic = false;
		bool nearLightExclusion = false;
		float nearLightRadius = 0.0f;  // 0..512 game units; exclusion sphere centered at light.
		float reachMultiplier = 1.0f;  // 1.0 means the vanilla-provided light radius.
		float fadeRange = 0.50f;       // Fraction of native shadow reach.
		float falloffCurve = 1.0f;
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
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		uint64_t readyValue = 0;
		uint32_t frame = 0, width = 0, height = 0, densityMode = 0;
		// RGBA32F: x weighted visibility, y 1 sampled / -1 missing / 0 invalid,
		// z raster depth, w accumulated weight.
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
		bool RetainThrough(const std::shared_ptr<const ShadowMaskFrame>& frame, ID3D12Fence* fence, uint64_t value);
		void Reset() { current.reset(); }
		const std::string& Status() const { return status; }

	private:
		void Initialize(const GraphicsBackend& backend);
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		std::array<std::shared_ptr<ShadowMaskFrame>, 4> slots;
		std::shared_ptr<ShadowMaskFrame> current;
		uint64_t nextFence = 0;
		std::string status;
	};
}
