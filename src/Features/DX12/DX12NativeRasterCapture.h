#pragma once

#include "Globals.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"
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
		/** @brief Invalidates publication while retaining slots for GPU retirement. */
		void Reset();

	private:
		std::array<std::shared_ptr<RasterFrame>, 4> slots;
		std::shared_ptr<RasterFrame> current;
		SharedFence producer;
		winrt::com_ptr<ID3D11ComputeShader> depthShader;
		GraphicsBackend backend;
		VideoMemoryBudget videoBudget;
		bool failureLogged = false;
	};
}
