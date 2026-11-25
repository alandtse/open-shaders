#include "DX12NativeRasterCapture.h"

#include "DX12GpuRetirement.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include <algorithm>
#include <directx/d3dx12.h>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace DX12Native
{
	RasterCapture::~RasterCapture()
	{
		current.reset();
		for (auto& slot : slots) {
			if (!slot)
				continue;
			std::vector<FenceDependency> dependencies;
			dependencies.push_back(FencePoint(slot->readyFence ? slot->readyFence.get() : producer.fence12.get(), slot->readyValue));
			RetireAfterFences(std::pair{ backend, std::move(slot) }, backend.d3d12Device, std::move(dependencies));
		}
	}

	void RasterCapture::Capture(const GraphicsBackend& a_backend, ID3D11ShaderResourceView* a_depth, const globals::FrameBuffer* a_camera, uint32_t a_frame)
	{
		CS_GPU_PASS("DX12NativeScene::CaptureOpaqueRaster");
		current.reset();
		std::shared_ptr<RasterFrame> captureSlot;
		bool gpuCopyStarted = false;
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
				return slot.use_count() == 1 && slot->readyValue != UINT64_MAX &&
				       completed != UINT64_MAX && completed >= slot->readyValue;
			});
			if (available == slots.end())
				return;
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
			if (!depthShader) {
				const auto shaderPath = Util::PathHelpers::GetShadersPath() / "DX12NativeScene" / "CaptureDepth.cs.hlsl";
				depthShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(shaderPath.c_str(), {}, "cs_5_0")));
			}
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
				// Include the old slot: replacement must coexist with it during allocation.
				uint64_t tracked = 0;
				for (const auto& retained : slots) {
					if (!retained)
						continue;
					if (tracked > UINT64_MAX - retained->allocationBytes)
						throw std::runtime_error("Raster depth tracked size overflow");
					tracked += retained->allocationBytes;
				}
				const uint64_t allowance = videoBudget.Query(backend.d3d12Device.get(), tracked).effectiveBytes;
				if (allocation.SizeInBytes > allowance || tracked > allowance - allocation.SizeInBytes)
					throw std::runtime_error("Raster depth allocation deferred by DXGI budget");
				auto replacement = std::make_shared<RasterFrame>();
				replacement->width = width;
				replacement->height = height;
				replacement->allocationBytes = allocation.SizeInBytes;
				replacement->depth = std::make_unique<WrappedResource>(
					depthDesc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::RasterDepth");
				slot = std::move(replacement);
			}
			slot->readyValue = UINT64_MAX;
			slot->readyFence = producer.fence12;
			captureSlot = slot;
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
			gpuCopyStarted = true;
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			write = priorWrite.get();
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			auto* read = priorRead.get();
			context->CSSetShaderResources(0, 1, &read);
			context->CSSetShader(priorShader.get(), nullptr, 0);
			const auto ready = producer.Next();
			DX::ThrowIfFailed(backend.d3d11Context->Signal(producer.fence11.get(), ready));
			// A failed DX12 queue Wait must not strand a successfully signaled frame.
			slot->readyValue = ready;
			backend.d3d11Context->Flush();
			DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), ready));
			slot->cameraValid = a_camera != nullptr;
			if (a_camera)
				slot->camera = *a_camera;
			slot->readyFence = producer.fence12;
			slot->readyValue = ready;
			slot->frame = a_frame;
			current = slot;
			failureLogged = false;
		} catch (...) {
			// A DX11 dispatch may still be queued if a later step fails.
			// Attach a producer fence rather than quarantining the slot forever.
			if (captureSlot && captureSlot->readyValue == UINT64_MAX) {
				if (gpuCopyStarted && producer.fence11) {
					const uint64_t rescue = producer.Next();
					if (const HRESULT hr = backend.d3d11Context->Signal(producer.fence11.get(), rescue); SUCCEEDED(hr)) {
						backend.d3d11Context->Flush();
						captureSlot->readyValue = rescue;
					} else {
						logger::warn("[DX12NativeScene] Raster recovery signal failed; retaining slot: {}", DX::com_exception(hr).what());
					}
				} else if (!gpuCopyStarted) {
					captureSlot->readyValue = 0;
				}
			}
			if (!failureLogged) {
				failureLogged = true;
				try {
					throw;
				} catch (const winrt::hresult_error& error) {
					logger::warn("[DX12NativeScene] Raster capture failed (HRESULT 0x{:08X}): {}", static_cast<uint32_t>(error.code()), winrt::to_string(error.message()));
				} catch (const std::exception& error) {
					logger::warn("[DX12NativeScene] Raster capture failed: {}", error.what());
				} catch (...) {
					logger::warn("[DX12NativeScene] Raster capture failed with an unknown exception");
				}
			}
		}
	}

	std::shared_ptr<const RasterFrame> RasterCapture::Get(uint32_t a_frame) const
	{
		return current && current->frame == a_frame ? current : nullptr;
	}

	void RasterCapture::Reset()
	{
		current.reset();
		failureLogged = false;
	}
}
