#include "DX12NativeComposite.h"
#include "DX12Control.h"

#include "Features/DX12/DX12NativeScene.h"
#include "Globals.h"
#include "PCH.h"

#include <algorithm>
#include <array>
#include <directx/d3dx12.h>
#include <fstream>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace DX12Native
{
	bool NativeComposite::InitializeShadow(ID3D12Device* device)
	{
		if (shadowPipeline && shadowDevice == device)
			return true;
		try {
			std::ifstream file("Data/Shaders/NativeComposite/ShadowLayer.dxil", std::ios::binary | std::ios::ate);
			if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024)
				return false;
			std::vector<char> shader(static_cast<size_t>(file.tellg()));
			file.seekg(0);
			if (!file.read(shader.data(), std::streamsize(shader.size())))
				return false;
			CD3DX12_DESCRIPTOR_RANGE ranges[2];
			ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
			ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
			CD3DX12_ROOT_PARAMETER parameters[3];
			parameters[0].InitAsDescriptorTable(1, &ranges[0]);
			parameters[1].InitAsDescriptorTable(1, &ranges[1]);
			parameters[2].InitAsConstants(42, 0);
			const CD3DX12_ROOT_SIGNATURE_DESC desc(3, parameters);
			winrt::com_ptr<ID3DBlob> serialized, errors;
			DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
			winrt::com_ptr<ID3D12RootSignature> newRoot;
			DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = newRoot.get();
			pso.CS = { shader.data(), shader.size() };
			winrt::com_ptr<ID3D12PipelineState> newPipeline;
			DX::ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
			shadowRoot = std::move(newRoot);
			shadowPipeline = std::move(newPipeline);
			shadowDevice = device;
			return true;
		} catch (...) {
			return false;
		}
	}

	std::shared_ptr<const NativeComposite::Frame> NativeComposite::Capture(
		ID3D11Device5* device11, ID3D11DeviceContext4* context11,
		ID3D12Device* device12, ID3D12CommandQueue* queue, ID3D11Texture2D* source,
		uint32_t frame, uint32_t width, uint32_t height, Transfer transfer,
		const std::shared_ptr<const ShadowMaskFrame>& shadow)
	{
		if (!device11 || !context11 || !device12 || !queue || !source || !width || !height)
			return {};
		winrt::com_ptr<ID3D11Device> sourceDevice;
		source->GetDevice(sourceDevice.put());
		winrt::com_ptr<ID3D12Device> queueDevice;
		if (sourceDevice.get() != device11 ||
			FAILED(queue->GetDevice(IID_PPV_ARGS(queueDevice.put()))) ||
			queueDevice.get() != device12 ||
			queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
			return {};

		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		if (desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
			desc.Width < width || desc.Height < height || desc.Format == DXGI_FORMAT_UNKNOWN)
			return {};
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ desc.Format };
		if (FAILED(device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
			!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
			return {};
		const bool supportsShadow = (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
		                            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
		                            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
		const auto raster = shadow ? shadow->raster : nullptr;
		// The native scene's frame counter is used when fetching the mask; the
		// compositor frame number is the upscaler's existing frame counter.
		const bool applyShadow = supportsShadow && transfer != Transfer::Unknown &&
		                         shadow && shadow->mask && shadow->readyFence && shadow->readyValue &&
		                         shadow->readyValue != UINT64_MAX &&
		                         shadow->width >= width && shadow->height >= height &&
		                         raster && raster->depth && raster->depth->resource &&
		                         raster->cameraValid && raster->readyFence && raster->readyValue &&
		                         raster->readyValue != UINT64_MAX &&
		                         raster->width >= width && raster->height >= height &&
		                         InitializeShadow(device12);

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
				const auto completed = consumer.first ? consumer.first->GetCompletedValue() : UINT64_MAX;
				return completed != UINT64_MAX && completed >= consumer.second;
			});
			if (value.consumers.empty()) {
				slot = &value;
				break;
			}
		}
		if (!slot)
			return {};

		bool producerSignalled = false;
		bool submitted = false;
		bool geometryPreviewApplied = false;
		try {
			const bool recreate = !slot->frame || slot->device != device12 ||
			                      slot->device11 != device11 || slot->sourceDesc.Width != desc.Width ||
			                      slot->sourceDesc.Height != desc.Height || slot->sourceDesc.Format != desc.Format ||
			                      slot->supportsShadow != supportsShadow;
			if (recreate) {
				// Reuse open-RT's two-image, four-slot DXGI budget and lifetime policy.
				const auto allocationDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format,
					desc.Width, desc.Height, 1, 1, 1, 0,
					supportsShadow ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
				const auto allocation = device12->GetResourceAllocationInfo(0, 1, &allocationDesc).SizeInBytes;
				if (allocation == UINT64_MAX || allocation > (UINT64_MAX - 2ull * 1024 * 1024) / 2)
					throw std::runtime_error("Composite allocation size unsupported");
				const uint64_t required = allocation * 2 + 2ull * 1024 * 1024;
				uint64_t retained = 0;
				for (const auto& candidate : slots)
					if (candidate && candidate.get() != slot) {
						if (retained > UINT64_MAX - candidate->frameBytes)
							throw std::runtime_error("Composite retained size overflow");
						retained += candidate->frameBytes;
					}
				const uint64_t current = slot->frameBytes;
				const uint64_t tracked = retained > UINT64_MAX - current ? UINT64_MAX : retained + current;
				const uint64_t allowance = videoBudget.Query(device12, tracked).effectiveBytes;
				if (required > allowance || retained > allowance - required)
					throw std::runtime_error("Composite frame allocation deferred by DXGI memory budget");

				auto replacement = std::make_unique<Slot>();
				replacement->frameBytes = required;
				replacement->frame = std::make_shared<Frame>();
				replacement->device = device12;
				replacement->device11 = device11;
				replacement->sourceDesc = desc;
				replacement->supportsShadow = supportsShadow;
				auto sharedDesc = desc;
				sharedDesc.Usage = D3D11_USAGE_DEFAULT;
				sharedDesc.BindFlags = 0;
				sharedDesc.CPUAccessFlags = 0;
				sharedDesc.MiscFlags = 0;
				replacement->shared = std::make_unique<WrappedResource>(sharedDesc, device11, device12, "NativeComposite::RasterInput");
				replacement->fence.Create(device12, device11, "NativeComposite::Fence");
				DX::ThrowIfFailed(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				DX::ThrowIfFailed(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					replacement->allocator.get(), nullptr, IID_PPV_ARGS(replacement->commands.put())));
				DX::ThrowIfFailed(replacement->commands->Close());
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				const auto texture = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format,
					desc.Width, desc.Height, 1, 1, 1, 0,
					supportsShadow ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
				DX::ThrowIfFailed(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
					&texture, D3D12_RESOURCE_STATE_COMMON, nullptr,
					IID_PPV_ARGS(replacement->frame->color.put())));
				replacement->frame->color->SetName(L"NativeComposite::FinalSceneDX12");
				if (supportsShadow) {
					D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
					heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
					heapDesc.NumDescriptors = 3;
					heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
					DX::ThrowIfFailed(device12->CreateDescriptorHeap(&heapDesc,
						IID_PPV_ARGS(replacement->shadowDescriptors.put())));
				}
				*slot = std::move(*replacement);
			}

			producerSignalled = true;
			context11->CopyResource(slot->shared->resource11, source);
			const uint64_t inputValue = slot->fence.Next();
			DX::ThrowIfFailed(context11->Signal(slot->fence.fence11.get(), inputValue));
			DX::ThrowIfFailed(queue->Wait(slot->fence.fence12.get(), inputValue));
			DX::ThrowIfFailed(slot->allocator->Reset());
			DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), nullptr));
			const std::array barriers{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->shared->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
			};
			slot->commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
			const auto dst = CD3DX12_TEXTURE_COPY_LOCATION(slot->frame->color.get(), 0);
			const auto src = CD3DX12_TEXTURE_COPY_LOCATION(slot->shared->resource.get(), 0);
			const D3D12_BOX box{ 0, 0, 0, width, height, 1 };
			slot->commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			const std::array endCopy{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->shared->resource.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
			};
			slot->commands->ResourceBarrier(static_cast<UINT>(endCopy.size()), endCopy.data());

			if (applyShadow) {
				const uint32_t previewMode = globals::features::dx12Control.GetShadowPreview();
				geometryPreviewApplied = previewMode >= 3u && previewMode <= 5u;
				slot->shadowRoot = shadowRoot;
				slot->shadowPipeline = shadowPipeline;
				const auto step = device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				auto cpu = slot->shadowDescriptors->GetCPUDescriptorHandleForHeapStart();
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				device12->CreateShaderResourceView(shadow->mask.get(), &srv, cpu);
				cpu.ptr += step;
				srv.Format = DXGI_FORMAT_R32_FLOAT;
				device12->CreateShaderResourceView(raster->depth->resource.get(), &srv, cpu);
				cpu.ptr += step;
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uav.Format = desc.Format;
				device12->CreateUnorderedAccessView(slot->frame->color.get(), nullptr, &uav, cpu);
				const std::array start{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
					CD3DX12_RESOURCE_BARRIER::Transition(shadow->mask.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(raster->depth->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
				};
				slot->commands->ResourceBarrier(static_cast<UINT>(start.size()), start.data());
				ID3D12DescriptorHeap* heaps[]{ slot->shadowDescriptors.get() };
				slot->commands->SetDescriptorHeaps(1, heaps);
				slot->commands->SetPipelineState(slot->shadowPipeline.get());
				slot->commands->SetComputeRootSignature(slot->shadowRoot.get());
				auto gpu = slot->shadowDescriptors->GetGPUDescriptorHandleForHeapStart();
				slot->commands->SetComputeRootDescriptorTable(0, gpu);
				gpu.ptr += 2ull * step;
				slot->commands->SetComputeRootDescriptorTable(1, gpu);
				struct Constants
				{
					uint32_t width, height;
					float receiverTolerance, radiusPixels;
					uint32_t legacyTransfer, preview;
					float strength, blend;
					DirectX::SimpleMath::Matrix inverseProjection, inverseView;
					uint32_t spatialReconstruction, reconstructionGridSize;
				};
				static_assert(sizeof(Constants) == 42 * sizeof(uint32_t));
				const auto& camera = raster->camera;
				// Exactly the open-RT default independent shadow settings.
				const Constants constants{ width, height, 16.0f, 2.0f,
					transfer == Transfer::LegacyScene ? 1u : 0u,
					previewMode,
					globals::features::dx12Control.GetShadowStrength(),
					globals::features::dx12Control.GetShadowBlend(),
					camera.CameraProjInverse, camera.CameraViewInverse,
					globals::features::dx12Control.UseSpatialReconstruction() ? 1u : 0u,
					globals::features::dx12Control.GetReconstructionGridSize() };
				slot->commands->SetComputeRoot32BitConstants(2, 42, &constants, 0);
				slot->commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
				const std::array end{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(shadow->mask.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(raster->depth->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
				};
				slot->commands->ResourceBarrier(static_cast<UINT>(end.size()), end.data());
			}

			DX::ThrowIfFailed(slot->commands->Close());
			if (applyShadow) {
				DX::ThrowIfFailed(queue->Wait(shadow->readyFence.get(), shadow->readyValue));
				DX::ThrowIfFailed(queue->Wait(raster->readyFence.get(), raster->readyValue));
			}
			const uint64_t done = slot->fence.Next();
			if (applyShadow &&
				(!globals::features::dx12NativeScene.RetainShadowMask(shadow, slot->fence.fence12.get(), done) ||
					!globals::features::dx12NativeScene.RetainRasterFrame(raster, slot->fence.fence12.get(), done))) {
				slot->untracked = true;
				return {};
			}
			ID3D12CommandList* lists[]{ slot->commands.get() };
			queue->ExecuteCommandLists(1, lists);
			submitted = true;
			DX::ThrowIfFailed(queue->Signal(slot->fence.fence12.get(), done));
			auto& output = *slot->frame;
			output.producerFence = slot->fence.fence12;
			output.producerValue = done;
			output.frame = frame;
			output.width = width;
			output.height = height;
			output.format = desc.Format;
			output.valid = true;
			output.geometryPreviewApplied = geometryPreviewApplied;
			return slot->frame;
		} catch (...) {
			if (submitted || producerSignalled)
				slot->untracked = true;
			return {};
		}
	}

	bool NativeComposite::Retain(const std::shared_ptr<const Frame>& frame, ID3D12Fence* fence, uint64_t value)
	{
		if (!frame || !fence || !value)
			return false;
		for (auto& slot : slots) {
			if (slot && slot->frame == frame) {
				winrt::com_ptr<ID3D12Fence> keep;
				keep.copy_from(fence);
				slot->consumers.emplace_back(std::move(keep), value);
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
			if (slot->untracked || (slot->frame && slot->frame.use_count() > 1) ||
				!slot->consumers.empty() ||
				(slot->frame && slot->frame->producerValue && slot->fence.fence12 &&
					slot->fence.fence12->GetCompletedValue() < slot->frame->producerValue))
				pending->push_back(std::move(slot));
		}
	}
}
