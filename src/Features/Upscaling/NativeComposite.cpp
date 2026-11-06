#include "PCH.h"
#include "NativeComposite.h"
#include "GpuPass.h"
#include <directx/d3dx12.h>
#include <limits>

std::shared_ptr<const NativeComposite::Frame> NativeComposite::Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
	ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
	uint32_t a_frame, uint32_t a_width, uint32_t a_height, Transfer a_transfer)
{
	CS_GPU_PASS("NativeComposite::RasterPassThrough");
	status = "Invalid capture input";
	if (!a_device11 || !a_context11 || !a_device12 || !a_queue || !a_source || !a_width || !a_height)
		return {};
	winrt::com_ptr<ID3D11Device> sourceDevice;
	a_source->GetDevice(sourceDevice.put());
	winrt::com_ptr<ID3D12Device> queueDevice;
	if (sourceDevice.get() != a_device11 || FAILED(a_queue->GetDevice(IID_PPV_ARGS(queueDevice.put()))) ||
		queueDevice.get() != a_device12 || a_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
		return {};

	D3D11_TEXTURE2D_DESC desc{};
	a_source->GetDesc(&desc);
	if (desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
		desc.Width < a_width || desc.Height < a_height || desc.Format == DXGI_FORMAT_UNKNOWN)
		return {};
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ desc.Format };
	if (FAILED(a_device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
		!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) {
		status = "Unsupported scene format";
		return {};
	}

	Slot* slot = nullptr;
	for (auto& candidate : slots) {
		if (!candidate)
			candidate = std::make_unique<Slot>();
		auto& value = *candidate;
		if (value.untracked || (value.frame && value.frame.use_count() != 1))
			continue;
		if (value.frame && value.frame->producerValue) {
			const uint64_t completed = value.fence.fence12 ? value.fence.fence12->GetCompletedValue() : UINT64_MAX;
			if (completed == UINT64_MAX || completed < value.frame->producerValue)
				continue;
		}
		std::erase_if(value.consumers, [](const auto& consumer) {
			return consumer.first && consumer.first->GetCompletedValue() != UINT64_MAX && consumer.first->GetCompletedValue() >= consumer.second;
		});
		if (value.consumers.empty()) {
			slot = &value;
			break;
		}
	}
	if (!slot) {
		status = "Waiting for available frame resources";
		return {};
	}

	bool submitted = false;
	bool producerSignalled = false;
	try {
		const bool recreate = !slot->frame || slot->device != a_device12 || slot->device11 != a_device11 ||
			slot->sourceDesc.Width != desc.Width || slot->sourceDesc.Height != desc.Height || slot->sourceDesc.Format != desc.Format;
		if (recreate) {
			auto replacement = std::make_unique<Slot>();
			replacement->frame = std::make_shared<Frame>();
			replacement->device = a_device12;
			replacement->device11 = a_device11;
			replacement->sourceDesc = desc;
			auto sharedDesc = desc;
			sharedDesc.Usage = D3D11_USAGE_DEFAULT;
			sharedDesc.BindFlags = 0;
			sharedDesc.CPUAccessFlags = 0;
			sharedDesc.MiscFlags = 0;
			replacement->shared = std::make_unique<WrappedResource>(sharedDesc, a_device11, a_device12, "NativeComposite::RasterInput");
			replacement->fence.Create(a_device12, a_device11, "NativeComposite::Fence");
			DX::ThrowIfFailed(a_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
			DX::ThrowIfFailed(a_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), nullptr, IID_PPV_ARGS(replacement->commands.put())));
			DX::ThrowIfFailed(replacement->commands->Close());
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			const auto texture = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height);
			DX::ThrowIfFailed(a_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(replacement->frame->color.put())));
			replacement->frame->color->SetName(L"NativeComposite::FinalSceneDX12");
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.NumDescriptors = 1;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			DX::ThrowIfFailed(a_device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(replacement->frame->descriptors.put())));
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			a_device12->CreateShaderResourceView(replacement->frame->color.get(), &srv, replacement->frame->descriptors->GetCPUDescriptorHandleForHeapStart());
			*slot = std::move(*replacement);
		}

		producerSignalled = true;
		a_context11->CopyResource(slot->shared->resource11, a_source);
		const uint64_t inputValue = slot->fence.Next();
		DX::ThrowIfFailed(a_context11->Signal(slot->fence.fence11.get(), inputValue));
		DX::ThrowIfFailed(a_queue->Wait(slot->fence.fence12.get(), inputValue));
		DX::ThrowIfFailed(slot->allocator->Reset());
		DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), nullptr));
		const std::array barriers{
			CD3DX12_RESOURCE_BARRIER::Transition(slot->shared->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		slot->commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		const auto dst = CD3DX12_TEXTURE_COPY_LOCATION(slot->frame->color.get(), 0);
		const auto src = CD3DX12_TEXTURE_COPY_LOCATION(slot->shared->resource.get(), 0);
		const D3D12_BOX box{ 0, 0, 0, a_width, a_height, 1 };
		slot->commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		const std::array endBarriers{
			CD3DX12_RESOURCE_BARRIER::Transition(slot->shared->resource.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
		};
		slot->commands->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
		DX::ThrowIfFailed(slot->commands->Close());
		ID3D12CommandList* lists[]{ slot->commands.get() };
		a_queue->ExecuteCommandLists(1, lists);
		submitted = true;
		const uint64_t done = slot->fence.Next();
		DX::ThrowIfFailed(a_queue->Signal(slot->fence.fence12.get(), done));
		auto& frame = *slot->frame;
		frame.producerFence = slot->fence.fence12;
		frame.producerValue = done;
		frame.frame = a_frame;
		frame.width = a_width;
		frame.height = a_height;
		frame.format = desc.Format;
		frame.transfer = a_transfer;
		status = "Submitted";
		return slot->frame;
	} catch (const std::exception& error) {
		status = error.what();
	} catch (...) {
		status = "Capture failed";
	}
	if (submitted || producerSignalled)
		slot->untracked = true;
	return {};
}

bool NativeComposite::Retain(const std::shared_ptr<const Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
{
	if (!a_frame || !a_fence || !a_value)
		return false;
	for (auto& slot : slots) {
		if (slot && slot->frame == a_frame) {
			winrt::com_ptr<ID3D12Fence> fence;
			fence.copy_from(a_fence);
			slot->consumers.emplace_back(std::move(fence), a_value);
			return true;
		}
	}
	return false;
}

NativeComposite::~NativeComposite()
{
	static auto* pending = new std::vector<std::unique_ptr<Slot>>;
	for (auto& slot : slots) {
		if (!slot)
			continue;
		if (slot->untracked || (slot->frame && slot->frame.use_count() > 1) || !slot->consumers.empty() ||
			(slot->frame && slot->frame->producerValue && slot->fence.fence12 &&
				slot->fence.fence12->GetCompletedValue() < slot->frame->producerValue))
			pending->push_back(std::move(slot));
	}
}
