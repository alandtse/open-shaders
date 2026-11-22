#include "DX12NativeRasterCapture.h"

#include "GpuPass.h"
#include "Utils/D3D.h"
#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace DX12Native
{
	RasterCapture::~RasterCapture()
	{
		current.reset();
		const auto completed = producer.fence12 ? producer.fence12->GetCompletedValue() : 0;
		for (auto& slot : slots) {
			if (slot && (slot->readyValue == UINT64_MAX || completed == UINT64_MAX || slot->readyValue > completed ||
							std::ranges::any_of(slot->consumers, [](const auto& consumer) { const auto done = consumer.first->GetCompletedValue(); return done == UINT64_MAX || done < consumer.second; })))
				(void)new std::shared_ptr<RasterFrame>(std::move(slot));
		}
	}

	void RasterCapture::Capture(const GraphicsBackend& a_backend, ID3D11ShaderResourceView* a_depth, const globals::FrameBuffer* a_camera, uint32_t a_frame)
	{
		CS_GPU_PASS("DX12NativeScene::CaptureOpaqueRaster");
		current.reset();
		try {
			if (!a_backend.d3d11Device || !a_backend.d3d11Context || !a_backend.d3d12Device || !a_backend.commandQueue)
				throw std::runtime_error("DX11/DX12 backend unavailable");
			if (!producer.fence12) {
				backend = a_backend;
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "DX12NativeScene::RasterReady");
			}
			if (backend.d3d11Device.get() != a_backend.d3d11Device.get() || backend.d3d12Device.get() != a_backend.d3d12Device.get() || backend.commandQueue.get() != a_backend.commandQueue.get())
				throw std::runtime_error("Raster backend changed; restart required");
			const auto done = producer.fence12->GetCompletedValue();
			if (done == UINT64_MAX)
				throw std::runtime_error("Raster device removed");
			auto available = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
				const auto completed = producer.fence12->GetCompletedValue();
				if (!slot)
					return completed != UINT64_MAX;
				if (slot.use_count() != 1 || slot->readyValue == UINT64_MAX || completed == UINT64_MAX || completed < slot->readyValue)
					return false;
				return std::ranges::all_of(slot->consumers, [](const auto& consumer) {
					const auto done = consumer.first->GetCompletedValue();
					return done != UINT64_MAX && done >= consumer.second;
				});
			});
			if (available == slots.end()) {
				throw std::runtime_error("Raster snapshots are still in use");
			}
			if (!a_depth)
				throw std::runtime_error("Opaque depth unavailable");
			D3D11_SHADER_RESOURCE_VIEW_DESC depthView{};
			a_depth->GetDesc(&depthView);
			winrt::com_ptr<ID3D11Resource> depthResource;
			a_depth->GetResource(depthResource.put());
			auto depthTexture = depthResource.try_as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC depthDesc{};
			if (depthTexture)
				depthTexture->GetDesc(&depthDesc);
			if (!depthTexture || depthView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
				!depthDesc.Width || !depthDesc.Height || depthDesc.ArraySize != 1 ||
				depthDesc.MipLevels != 1 || depthDesc.SampleDesc.Count != 1)
				throw std::runtime_error("Opaque depth layout unsupported");
			if (!depthShader)
				depthShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DX12NativeScene\\CaptureDepth.cs.hlsl", {}, "cs_5_0")));
			if (!depthShader)
				throw std::runtime_error("Opaque depth capture shader unavailable");
			const uint32_t width = depthDesc.Width, height = depthDesc.Height;
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			depthDesc.CPUAccessFlags = depthDesc.MiscFlags = 0;
			auto& slot = *available;
			const bool recreateBase = !slot || slot->width != width || slot->height != height;
			if (recreateBase) {
				const auto depthAllocation = CD3DX12_RESOURCE_DESC::Tex2D(
					DXGI_FORMAT_R32_FLOAT, width, height, 1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto allocation = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &depthAllocation);
				if (allocation.SizeInBytes == UINT64_MAX || !allocation.SizeInBytes)
					throw std::runtime_error("Raster depth allocation unsupported");
				auto replacement = std::make_shared<RasterFrame>();
				replacement->width = width;
				replacement->height = height;
				replacement->allocationBytes = allocation.SizeInBytes;
				replacement->depth = std::make_unique<WrappedResource>(
					depthDesc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::RasterDepth");
				slot = std::move(replacement);
			}
			slot->consumers.clear();
			slot->readyValue = UINT64_MAX;
			auto* context = backend.d3d11Context.get();
			winrt::com_ptr<ID3D11ComputeShader> priorShader;
			winrt::com_ptr<ID3D11ShaderResourceView> priorRead;
			winrt::com_ptr<ID3D11UnorderedAccessView> priorWrite;
			context->CSGetShader(priorShader.put(), nullptr, nullptr);
			context->CSGetShaderResources(0, 1, priorRead.put());
			context->CSGetUnorderedAccessViews(0, 1, priorWrite.put());
			auto* write = slot->depth->uav;
			context->CSSetShaderResources(0, 1, &a_depth);
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			context->CSSetShader(depthShader.get(), nullptr, 0);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			write = priorWrite.get();
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			auto* read = priorRead.get();
			context->CSSetShaderResources(0, 1, &read);
			context->CSSetShader(priorShader.get(), nullptr, 0);
			const auto ready = producer.Next();
			DX::ThrowIfFailed(backend.d3d11Context->Signal(producer.fence11.get(), ready));
			backend.d3d11Context->Flush();
			DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), ready));
			slot->cameraValid = a_camera != nullptr;
			if (a_camera)
				slot->camera = *a_camera;
			slot->readyFence = producer.fence12;
			slot->readyValue = ready;
			slot->frame = a_frame;
			current = slot;
		} catch (const std::exception&) {
			// Missing capture leaves no published frame; do not disrupt raster.
		}
	}

	std::shared_ptr<const RasterFrame> RasterCapture::Get(uint32_t a_frame) const
	{
		return current && current->frame == a_frame ? current : nullptr;
	}

	bool RasterCapture::RetainThrough(const std::shared_ptr<const RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_frame || !a_fence || !a_value || a_value == UINT64_MAX)
			return false;
		for (const auto& slot : slots) {
			if (slot.get() != a_frame.get())
				continue;
			winrt::com_ptr<ID3D12Fence> fence;
			fence.copy_from(a_fence);
			slot->consumers.emplace_back(std::move(fence), a_value);
			return true;
		}
		return false;
	}

	void RasterCapture::Reset() { current.reset(); }
}
