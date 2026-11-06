#include "PCH.h"
#include "NativeCompositePass.h"
#include "GpuPass.h"
#include "Globals.h"
#include <directx/d3dx12.h>
#include <fstream>
#include <limits>

bool NativeComposite::InitializeLighting(ID3D12Device* a_device)
{
	if (pipeline && pipelineDevice == a_device)
		return true;
	try {
		std::ifstream file("Data/Shaders/NativeComposite/AddLighting.dxil", std::ios::binary | std::ios::ate);
		if (!file) return false;
		const auto size = file.tellg();
		if (size <= 0 || size > 16 * 1024 * 1024) return false;
		std::vector<char> shader(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(shader.data(), size)) return false;
		CD3DX12_DESCRIPTOR_RANGE ranges[2];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER parameters[3];
		parameters[0].InitAsDescriptorTable(1, &ranges[0]);
		parameters[1].InitAsDescriptorTable(1, &ranges[1]);
		parameters[2].InitAsConstants(4, 0);
		const CD3DX12_ROOT_SIGNATURE_DESC desc(3, parameters);
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		DX::ThrowIfFailed(a_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = newRoot.get();
		pso.CS = { shader.data(), shader.size() };
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(a_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
		root = std::move(newRoot);
		pipeline = std::move(newPipeline);
		pipelineDevice = a_device;
		return true;
	} catch (...) {
		return false;
	}
}

std::shared_ptr<const NativeComposite::Frame> NativeComposite::Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
	ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
	uint32_t a_frame, uint32_t a_width, uint32_t a_height, Transfer a_transfer,
	const std::shared_ptr<const NativeLightingFeature::Contribution>& a_lighting, float a_strength)
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
	const bool supportsLighting = (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
		(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
	const bool blend = supportsLighting && a_lighting && a_lighting->readyFence && a_lighting->readyValue &&
		a_lighting->readyValue != UINT64_MAX && a_lighting->backend.d3d12Device.get() == a_device12 &&
		a_lighting->textures[0] && a_lighting->textures[1] && a_lighting->textures[2] &&
		a_lighting->width == a_width && a_lighting->height == a_height && a_transfer != Transfer::Unknown && InitializeLighting(a_device12);

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
			slot->sourceDesc.Width != desc.Width || slot->sourceDesc.Height != desc.Height || slot->sourceDesc.Format != desc.Format || slot->supportsLighting != supportsLighting;
		if (recreate) {
			auto replacement = std::make_unique<Slot>();
			replacement->frame = std::make_shared<Frame>();
			replacement->device = a_device12;
			replacement->device11 = a_device11;
			replacement->sourceDesc = desc;
			replacement->supportsLighting = supportsLighting;
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
			const auto texture = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, 1, 1, 1, 0,
				supportsLighting ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
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
			if (supportsLighting) {
				heapDesc.NumDescriptors = 4;
				DX::ThrowIfFailed(a_device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(replacement->lightingDescriptors.put())));
			}
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
			CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COPY_DEST,
				blend ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON)
		};
		slot->commands->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
		if (blend) {
			slot->root = root;
			slot->pipeline = pipeline;
			const auto step = a_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = slot->lightingDescriptors->GetCPUDescriptorHandleForHeapStart();
			std::array<ID3D12Resource*, 3> resources{ a_lighting->textures[0].get(), a_lighting->textures[1].get(), a_lighting->textures[2].get() };
			std::array<D3D12_RESOURCE_BARRIER, 3> lightingBarriers{};
			for (size_t i = 0; i < resources.size(); ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = i == 2 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				a_device12->CreateShaderResourceView(resources[i], &srv, cpu);
				cpu.ptr += step;
				lightingBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(resources[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			a_device12->CreateUnorderedAccessView(slot->frame->color.get(), nullptr, &uav, cpu);
			slot->commands->ResourceBarrier(static_cast<UINT>(lightingBarriers.size()), lightingBarriers.data());
			ID3D12DescriptorHeap* heaps[]{ slot->lightingDescriptors.get() };
			slot->commands->SetDescriptorHeaps(1, heaps);
			slot->commands->SetPipelineState(slot->pipeline.get());
			slot->commands->SetComputeRootSignature(slot->root.get());
			auto gpu = slot->lightingDescriptors->GetGPUDescriptorHandleForHeapStart();
			slot->commands->SetComputeRootDescriptorTable(0, gpu);
			gpu.ptr += 3ull * step;
			slot->commands->SetComputeRootDescriptorTable(1, gpu);
			struct Constants { uint32_t width, height; float strength; uint32_t legacy; };
			const Constants constants{ a_width, a_height, a_strength, a_transfer == Transfer::LegacyScene ? 1u : 0u };
			slot->commands->SetComputeRoot32BitConstants(2, 4, &constants, 0);
			slot->commands->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
			for (size_t i = 0; i < resources.size(); ++i)
				lightingBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(resources[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			slot->commands->ResourceBarrier(static_cast<UINT>(lightingBarriers.size()), lightingBarriers.data());
			const auto outputBarrier = CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			slot->commands->ResourceBarrier(1, &outputBarrier);
		}
		DX::ThrowIfFailed(slot->commands->Close());
		if (blend)
			DX::ThrowIfFailed(a_queue->Wait(a_lighting->readyFence.get(), a_lighting->readyValue));
		ID3D12CommandList* lists[]{ slot->commands.get() };
		const uint64_t done = slot->fence.Next();
		if (blend && !globals::features::nativeLighting.RetainContribution(a_lighting, slot->fence.fence12.get(), done)) {
			slot->untracked = true;
			return {};
		}
		a_queue->ExecuteCommandLists(1, lists);
		submitted = true;
		DX::ThrowIfFailed(a_queue->Signal(slot->fence.fence12.get(), done));
		auto& frame = *slot->frame;
		frame.producerFence = slot->fence.fence12;
		frame.producerValue = done;
		frame.frame = a_frame;
		frame.width = a_width;
		frame.height = a_height;
		frame.format = desc.Format;
		frame.transfer = a_transfer;
		frame.valid = true;
		frame.lightingSubmitted = blend;
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
