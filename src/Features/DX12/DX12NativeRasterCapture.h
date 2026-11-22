#pragma once

#include "Globals.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include <array>
#include <memory>
#include <vector>

namespace DX12Native
{
	/** @brief Retains captured DX11 depth and camera until the GPU consumers finish. */
	struct RasterFrame
	{
		std::unique_ptr<WrappedResource> depth;
		globals::FrameBuffer camera{};
		bool cameraValid = false;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t frame = 0, width = 0, height = 0;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
	};

	/** @brief Captures only raster depth and camera into fenced DX12 frame slots. */
	class RasterCapture
	{
	public:
		~RasterCapture();
		/** @brief Captures one coherent frame; callers retain returned frames until GPU consumption completes. */
		void Capture(const GraphicsBackend& a_backend, ID3D11ShaderResourceView* a_depth, const globals::FrameBuffer* a_camera, uint32_t a_frame);
		/** @brief Borrows a matching snapshot, whose ready fence must be waited on by the consumer queue. */
		std::shared_ptr<const RasterFrame> Get(uint32_t a_frame) const;
		/** @brief Retains a borrowed snapshot until a submitted consumer fence completes. */
		bool RetainThrough(const std::shared_ptr<const RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Invalidates publication while retaining slots for GPU retirement. */
		void Reset();
		/** @brief Drops the depth shader for a later shader-cache retry. */
		void ReloadShader()
		{
			Reset();
			depthShader = nullptr;
		}

	private:
		std::array<std::shared_ptr<RasterFrame>, 4> slots;
		std::shared_ptr<RasterFrame> current;
		SharedFence producer;
		winrt::com_ptr<ID3D11ComputeShader> depthShader;
		GraphicsBackend backend;
	};
}
