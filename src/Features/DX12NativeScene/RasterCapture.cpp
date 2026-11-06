#include "RasterCapture.h"
#include "GpuPass.h"
#include "Utils/D3D.h"

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

	void RasterCapture::Capture(const GraphicsBackend& a_backend, const std::array<ID3D11Texture2D*, static_cast<size_t>(RasterFrame::Input::Count)>& a_inputs, ID3D11ShaderResourceView* a_depth, const globals::FrameBuffer* a_camera, uint32_t a_frame, uint64_t a_budget)
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
			if (done == UINT64_MAX) throw std::runtime_error("Raster device removed");
			for (const auto& frame : slots) {
				if (frame && !frame->completionCounted && frame->readyValue && frame->readyValue != UINT64_MAX && frame->readyValue <= done) {
					frame->completionCounted = true;
					++completions;
				}
			}
			auto available = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
				const auto completed = producer.fence12->GetCompletedValue();
				if (!slot) return completed != UINT64_MAX;
				if (slot.use_count() != 1 || slot->readyValue == UINT64_MAX || completed == UINT64_MAX || completed < slot->readyValue) return false;
				return std::ranges::all_of(slot->consumers, [](const auto& consumer) {
					const auto done = consumer.first->GetCompletedValue();
					return done != UINT64_MAX && done >= consumer.second;
				});
			});
			if (available == slots.end()) { ++busyDeferrals; throw std::runtime_error("Raster snapshots are still in use"); }
			uint64_t trackedBytes = 0;
			for (const auto& frame : slots) if (frame) {
				if (trackedBytes > UINT64_MAX - frame->allocationBytes) throw std::runtime_error("Raster frame pool byte count overflow");
				trackedBytes += frame->allocationBytes;
			}
			const uint64_t budget = videoBudget.Query(backend.d3d12Device.get(), trackedBytes, a_budget).effectiveBytes;
			std::array<D3D11_TEXTURE2D_DESC, static_cast<size_t>(RasterFrame::Input::Count)> descriptions{};
			uint32_t width = 0, height = 0;
			for (size_t i = 0; i < a_inputs.size(); ++i) {
				if (!a_inputs[i]) throw std::runtime_error("Opaque raster input unavailable");
				a_inputs[i]->GetDesc(&descriptions[i]);
				const auto& desc = descriptions[i];
				if (!desc.Width || !desc.Height || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
					throw std::runtime_error("Opaque raster input layout unsupported");
				if (!width) { width = desc.Width; height = desc.Height; }
				if (desc.Width != width || desc.Height != height) throw std::runtime_error("Opaque raster input dimensions differ");
			}
			if (!a_depth) throw std::runtime_error("Opaque depth unavailable");
			D3D11_SHADER_RESOURCE_VIEW_DESC depthView{};
			a_depth->GetDesc(&depthView);
			winrt::com_ptr<ID3D11Resource> depthResource;
			a_depth->GetResource(depthResource.put());
			auto depthTexture = depthResource.try_as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC depthDesc{};
			if (depthTexture) depthTexture->GetDesc(&depthDesc);
			if (!depthTexture || depthView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || depthDesc.Width != width || depthDesc.Height != height ||
				depthDesc.ArraySize != 1 || depthDesc.MipLevels != 1 || depthDesc.SampleDesc.Count != 1)
				throw std::runtime_error("Opaque depth layout unsupported");
			if (!depthShader)
				depthShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DX12NativeScene\\CaptureDepth.cs.hlsl", {}, "cs_5_0")));
			if (!depthShader) throw std::runtime_error("Opaque depth capture shader unavailable");
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			depthDesc.CPUAccessFlags = depthDesc.MiscFlags = 0;
			auto& slot = *available;
			bool recreate = !slot || slot->width != width || slot->height != height;
			if (slot && !recreate)
				for (size_t i = 0; i < descriptions.size(); ++i) recreate |= slot->formats[i] != descriptions[i].Format;
			uint64_t slotBytes = recreate ? 0 : slot->allocationBytes;
			if (recreate) {
				for (const auto& source : descriptions) {
					D3D12_RESOURCE_DESC desc{};
					desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					desc.Width = source.Width;
					desc.Height = source.Height;
					desc.DepthOrArraySize = desc.MipLevels = 1;
					desc.Format = source.Format;
					desc.SampleDesc.Count = 1;
					const auto allocation = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
					if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > budget || slotBytes > budget - allocation.SizeInBytes)
						throw std::runtime_error("Raster snapshot exceeds memory budget or format is unsupported");
					slotBytes += allocation.SizeInBytes;
				}
				D3D12_RESOURCE_DESC depthAllocation{};
				depthAllocation.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				depthAllocation.Width = width;
				depthAllocation.Height = height;
				depthAllocation.DepthOrArraySize = depthAllocation.MipLevels = 1;
				depthAllocation.Format = DXGI_FORMAT_R32_FLOAT;
				depthAllocation.SampleDesc.Count = 1;
				depthAllocation.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				const auto depthBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &depthAllocation).SizeInBytes;
				if (depthBytes == UINT64_MAX || depthBytes > budget || slotBytes > budget - depthBytes)
					throw std::runtime_error("Raster depth exceeds memory budget");
				slotBytes += depthBytes;
			}
			uint64_t otherBytes = 0;
			for (auto it = slots.begin(); it != slots.end(); ++it)
				if (it != available && *it) otherBytes += (*it)->allocationBytes;
			if (otherBytes > budget || slotBytes > budget - otherBytes) throw std::runtime_error("Raster snapshot exceeds memory budget");
			if (recreate) {
				slot.reset();
				auto replacement = std::make_shared<RasterFrame>();
				replacement->width = width;
				replacement->height = height;
				replacement->allocationBytes = slotBytes;
				constexpr std::array names{ "LitColor", "LitSpecular", "Albedo", "NormalRoughness", "Masks", "Masks2", "Reflectance", "Motion" };
				for (size_t i = 0; i < descriptions.size(); ++i) {
					auto desc = descriptions[i];
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = desc.MiscFlags = 0;
					replacement->inputs[i] = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), std::format("DX12NativeScene::Raster{}", names[i]));
					replacement->formats[i] = desc.Format;
				}
				replacement->depth = std::make_unique<WrappedResource>(depthDesc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::RasterDepth");
				slot = std::move(replacement);
			}
			slot->consumers.clear();
			slot->readyValue = UINT64_MAX;
			slot->completionCounted = false;
			for (size_t i = 0; i < a_inputs.size(); ++i)
				backend.d3d11Context->CopyResource(slot->inputs[i]->resource11, a_inputs[i]);
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
			slot->sourceDepthFormat = depthView.Format;
			slot->cameraValid = a_camera != nullptr;
			if (a_camera) slot->camera = *a_camera;
			slot->readyFence = producer.fence12;
			slot->readyValue = ready;
			slot->frame = a_frame;
			current = slot;
			++submissions;
			status.clear();
		} catch (const std::exception& error) { status = error.what(); }
	}

	std::shared_ptr<const RasterFrame> RasterCapture::Get(uint32_t a_frame) const
	{
		return current && current->frame == a_frame ? current : nullptr;
	}

	json RasterCapture::GetDiagnostics() const
	{
		const auto completed = producer.fence12 ? producer.fence12->GetCompletedValue() : 0;
		return { { "status", status }, { "submissions", submissions }, { "gpuCompletions", completions }, { "offered", bool(current) },
			{ "frame", current ? current->frame : 0 }, { "width", current ? current->width : 0 }, { "height", current ? current->height : 0 },
			{ "cameraValid", current && current->cameraValid }, { "frameSlots", slots.size() }, { "busyDeferrals", busyDeferrals },
			{ "dxgiAvailable", videoBudget.last.dxgiAvailable }, { "effectiveFrameBudgetBytes", videoBudget.last.effectiveBytes },
			{ "dxgiLocalBudgetBytes", videoBudget.last.localBudget }, { "dxgiLocalUsageBytes", videoBudget.last.localUsage },
			{ "gpuComplete", current && completed != UINT64_MAX && completed >= current->readyValue } };
	}

	bool RasterCapture::RetainThrough(const std::shared_ptr<const RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_frame || !a_fence || !a_value || a_value == UINT64_MAX) return false;
		for (const auto& slot : slots) {
			if (slot.get() != a_frame.get()) continue;
			winrt::com_ptr<ID3D12Fence> fence;
			fence.copy_from(a_fence);
			slot->consumers.emplace_back(std::move(fence), a_value);
			return true;
		}
		return false;
	}

	void RasterCapture::Reset() { current.reset(); }
}
