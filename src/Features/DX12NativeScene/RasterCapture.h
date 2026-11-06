#pragma once

#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include "Globals.h"
#include <array>
#include <memory>
#include <vector>

namespace DX12Native
{
	/** @brief Retains copied opaque raster resources until every GPU consumer completes. */
	struct RasterFrame
	{
		enum class Input : size_t { LitColor, LitSpecular, Albedo, NormalRoughness, Masks, Masks2, Reflectance, Motion, Count };
		std::array<std::unique_ptr<WrappedResource>, static_cast<size_t>(Input::Count)> inputs;
		std::array<DXGI_FORMAT, static_cast<size_t>(Input::Count)> formats{};
		std::unique_ptr<WrappedResource> depth;
		DXGI_FORMAT sourceDepthFormat = DXGI_FORMAT_UNKNOWN;
		globals::FrameBuffer camera{};
		bool cameraValid = false;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t frame = 0, width = 0, height = 0;
		bool completionCounted = false;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
	};

	/** @brief Copies opaque inputs from DX11 into bounded, independently fenced DX12 slots. */
	class RasterCapture
	{
	public:
		~RasterCapture();
		/** @brief Captures one coherent frame; callers retain returned frames until GPU consumption completes. */
		void Capture(const GraphicsBackend& a_backend, const std::array<ID3D11Texture2D*, static_cast<size_t>(RasterFrame::Input::Count)>& a_inputs, ID3D11ShaderResourceView* a_depth, const globals::FrameBuffer* a_camera, uint32_t a_frame, uint64_t a_budget);
		/** @brief Borrows a matching snapshot, whose ready fence must be waited on by the consumer queue. */
		std::shared_ptr<const RasterFrame> Get(uint32_t a_frame) const;
		/** @brief Retains a borrowed snapshot until a submitted consumer fence completes. */
		bool RetainThrough(const std::shared_ptr<const RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Invalidates publication while retaining slots for GPU retirement. */
		void Reset();
		/** @brief Drops the depth shader for a later shader-cache retry. */
		void ReloadShader() { Reset(); depthShader = nullptr; }
		/** @brief Returns the latest capture failure or availability status. */
		const std::string& Status() const { return status; }
		/** @brief Reports source capture and producer completion independently of consumption. */
		json GetDiagnostics() const;
	private:
		std::array<std::shared_ptr<RasterFrame>, 2> slots;
		std::shared_ptr<RasterFrame> current;
		SharedFence producer;
		winrt::com_ptr<ID3D11ComputeShader> depthShader;
		GraphicsBackend backend;
		std::string status;
		uint64_t submissions = 0, completions = 0;
	};
}
