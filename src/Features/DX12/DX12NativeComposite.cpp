#include "DX12NativeComposite.h"
#include "DX12Control.h"
#include "DX12GpuRetirement.h"

#include "Features/DX12/DX12NativeScene.h"
#include "Globals.h"
#include "GpuPass.h"
#include "PCH.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <array>
#include <directx/d3dx12.h>
#include <fstream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace DX12Native
{
	namespace
	{
		constexpr std::streamoff kMaxShadowBytecodeBytes = 16 * VideoMemoryBudget::kMiB;
		constexpr uint64_t kSlotOverheadBytes = 2 * VideoMemoryBudget::kMiB;
		constexpr uint32_t kShadowConstantDwords = 42;
		constexpr uint32_t kShadowThreadGroupSize = 8;
		constexpr float kReceiverTolerance = 16.0f;
		constexpr float kReconstructionRadiusPixels = 2.0f;
	}

	bool NativeComposite::InitializeShadow(ID3D12Device* device)
	{
		if (shadowPipeline && shadowDevice.get() == device)
			return true;
		if (failedShadowDevice.get() == device)
			return false;
		try {
			std::ifstream file(Util::PathHelpers::GetShadersPath() / "NativeComposite" / "ShadowLayer.dxil", std::ios::binary | std::ios::ate);
			if (!file || file.tellg() <= 0 || file.tellg() > kMaxShadowBytecodeBytes)
				throw std::runtime_error("ShadowLayer.dxil missing, empty or exceeds the bytecode limit");
			std::vector<char> shader(static_cast<size_t>(file.tellg()));
			file.seekg(0);
			if (!file.read(shader.data(), std::streamsize(shader.size())))
				throw std::runtime_error("ShadowLayer.dxil read failed");
			CD3DX12_DESCRIPTOR_RANGE ranges[2];
			ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
			ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
			CD3DX12_ROOT_PARAMETER parameters[3];
			parameters[0].InitAsDescriptorTable(1, &ranges[0]);
			parameters[1].InitAsDescriptorTable(1, &ranges[1]);
			parameters[2].InitAsConstants(kShadowConstantDwords, 0);
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
			shadowDevice.copy_from(device);
			failedShadowDevice = nullptr;
			return true;
		} catch (const winrt::hresult_error& error) {
			logger::error("[NativeComposite] Shadow initialization failed (HRESULT 0x{:08X}): {}", static_cast<uint32_t>(error.code()), winrt::to_string(error.message()));
		} catch (const std::exception& error) {
			logger::error("[NativeComposite] Shadow initialization failed: {}", error.what());
		} catch (...) {
			logger::error("[NativeComposite] Shadow initialization failed with an unknown exception");
		}
		failedShadowDevice.copy_from(device);
		return false;
	}

	std::shared_ptr<const NativeComposite::Frame> NativeComposite::Capture(
		ID3D11Device5* device11, ID3D11DeviceContext4* context11,
		ID3D12Device* device12, ID3D12CommandQueue* queue, ID3D11Texture2D* source,
		uint32_t frame, uint32_t width, uint32_t height, Transfer transfer,
		const std::shared_ptr<const ShadowMaskFrame>& shadow,
		bool returnColorToDX11)
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
		if (!applyShadow)
			returnColorToDX11 = false;

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
		try {
			const bool recreate = !slot->frame || slot->backend.d3d12Device.get() != device12 ||
			                      slot->backend.d3d11Device.get() != device11 ||
			                      slot->backend.d3d11Context.get() != context11 ||
			                      slot->backend.commandQueue.get() != queue || slot->sourceDesc.Width != desc.Width ||
			                      slot->sourceDesc.Height != desc.Height || slot->sourceDesc.Format != desc.Format ||
			                      slot->supportsShadow != supportsShadow ||
			                      slot->hasReturn != returnColorToDX11;
			if (recreate) {
				// Reuse open-RT's two-image, four-slot DXGI budget and lifetime policy.
				const auto allocationDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format,
					desc.Width, desc.Height, 1, 1, 1, 0,
					supportsShadow ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
				const auto allocation = device12->GetResourceAllocationInfo(0, 1, &allocationDesc).SizeInBytes;
				const uint64_t imageCount = returnColorToDX11 ? 3ull : 2ull;
				if (allocation == UINT64_MAX || allocation > (UINT64_MAX - kSlotOverheadBytes) / imageCount)
					throw std::runtime_error("Composite allocation size unsupported");
				const uint64_t required = allocation * imageCount + kSlotOverheadBytes;
				uint64_t retained = 0;
				for (const auto& candidate : slots)
					if (candidate && candidate.get() != slot) {
						if (retained > UINT64_MAX - candidate->frameBytes)
							throw std::runtime_error("Composite retained size overflow");
						retained += candidate->frameBytes;
					}
				const uint64_t current = slot->frameBytes;
				const uint64_t tracked = retained > UINT64_MAX - current ? UINT64_MAX : retained + current;
				const auto before = videoBudget.Query(device12, tracked);
				const uint64_t allowance = before.effectiveBytes;
				if (required > allowance || tracked > allowance - required)
					throw std::runtime_error("Composite frame allocation deferred by DXGI memory budget");

				auto replacement = std::make_unique<Slot>();
				replacement->frameBytes = required;
				replacement->frame = std::make_shared<Frame>();
				replacement->backend.d3d12Device.copy_from(device12);
				replacement->backend.d3d11Device.copy_from(device11);
				replacement->backend.d3d11Context.copy_from(context11);
				replacement->backend.commandQueue.copy_from(queue);
				replacement->sourceDesc = desc;
				replacement->supportsShadow = supportsShadow;
				replacement->hasReturn = returnColorToDX11;
				auto sharedDesc = desc;
				sharedDesc.Usage = D3D11_USAGE_DEFAULT;
				sharedDesc.BindFlags = 0;
				sharedDesc.CPUAccessFlags = 0;
				sharedDesc.MiscFlags = 0;
				replacement->shared = std::make_unique<WrappedResource>(sharedDesc, device11, device12, "NativeComposite::RasterInput");
				if (returnColorToDX11)
					replacement->returned = std::make_unique<WrappedResource>(sharedDesc, device11, device12, "NativeComposite::RasterReturn");
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
				logger::info("[NativeComposite] {}x{} slot: {} images, {} MiB; DXGI local usage {} MiB before, {} MiB after, of {} MiB",
					desc.Width, desc.Height, imageCount, required / VideoMemoryBudget::kMiB, before.localUsage / VideoMemoryBudget::kMiB,
					videoBudget.Query(device12, tracked + required).localUsage / VideoMemoryBudget::kMiB, before.localBudget / VideoMemoryBudget::kMiB);
			}

			// Own the upstream inputs until this composite's GPU work retires.
			// Strong references prevent the producer pools from recycling them
			// without registering a fence that could be abandoned on failure.
			slot->shadowInput = applyShadow ? shadow : nullptr;
			slot->rasterInput = applyShadow ? raster : nullptr;
			slot->frame->valid = false;
			slot->frame->producerFence = nullptr;
			slot->frame->producerValue = 0;
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
					uint32_t legacyTransfer, alignmentPadding;
					float strength, blend;
					DirectX::SimpleMath::Matrix inverseProjection, inverseView;
					uint32_t spatialReconstruction, reconstructionGridSize;
				};
				static_assert(sizeof(Constants) == kShadowConstantDwords * sizeof(uint32_t));
				const auto& camera = raster->camera;
				// Exactly the open-RT default independent shadow settings.
				const Constants constants{ width, height, kReceiverTolerance, kReconstructionRadiusPixels,
					transfer == Transfer::LegacyScene ? 1u : 0u,
					0u,  // padding: matrices retain 16-byte alignment.
					globals::features::dx12Control.GetShadowStrength(),
					globals::features::dx12Control.GetShadowBlend(),
					camera.CameraProjInverse, camera.CameraViewInverse,
					globals::features::dx12Control.UseSpatialReconstruction() ? 1u : 0u,
					globals::features::dx12Control.GetReconstructionGridSize() };
				slot->commands->SetComputeRoot32BitConstants(2, kShadowConstantDwords, &constants, 0);
				slot->commands->Dispatch((width + kShadowThreadGroupSize - 1) / kShadowThreadGroupSize,
					(height + kShadowThreadGroupSize - 1) / kShadowThreadGroupSize, 1);
				const std::array end{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(shadow->mask.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(raster->depth->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
				};
				slot->commands->ResourceBarrier(static_cast<UINT>(end.size()), end.data());
			}

			if (applyShadow && returnColorToDX11 && slot->returned) {
				const std::array beginReturn{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(slot->returned->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
				};
				slot->commands->ResourceBarrier(UINT(beginReturn.size()), beginReturn.data());
				const auto destination = CD3DX12_TEXTURE_COPY_LOCATION(slot->returned->resource.get(), 0);
				const auto origin = CD3DX12_TEXTURE_COPY_LOCATION(slot->frame->color.get(), 0);
				const D3D12_BOX region{ 0, 0, 0, width, height, 1 };
				slot->commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, &region);
				const std::array finishReturn{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(slot->returned->resource.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
				};
				slot->commands->ResourceBarrier(UINT(finishReturn.size()), finishReturn.data());
			}
			DX::ThrowIfFailed(slot->commands->Close());
			if (applyShadow) {
				DX::ThrowIfFailed(queue->Wait(shadow->readyFence.get(), shadow->readyValue));
				DX::ThrowIfFailed(queue->Wait(raster->readyFence.get(), raster->readyValue));
			}
			const uint64_t done = slot->fence.Next();
			ID3D12CommandList* lists[]{ slot->commands.get() };
			queue->ExecuteCommandLists(1, lists);
			submitted = true;
			DX::ThrowIfFailed(queue->Signal(slot->fence.fence12.get(), done));
			auto& output = *slot->frame;
			output.producerFence = slot->fence.fence12;
			output.producerValue = done;
			if (applyShadow && returnColorToDX11 && slot->returned) {
				CS_GPU_PASS("NativeComposite::SceneReturn");
				// The slot is reused only once this second signal completes, so the return texture outlives the DX11 copy.
				DX::ThrowIfFailed(context11->Wait(slot->fence.fence11.get(), done));
				const D3D11_BOX sourceRegion{ 0, 0, 0, width, height, 1 };
				context11->CopySubresourceRegion(source, 0, 0, 0, 0,
					slot->returned->resource11, 0, &sourceRegion);
				const uint64_t copied = slot->fence.Next();
				DX::ThrowIfFailed(context11->Signal(slot->fence.fence11.get(), copied));
				output.producerValue = copied;
			}
			output.frame = frame;
			output.width = width;
			output.height = height;
			output.format = desc.Format;
			output.valid = true;
			captureFailureLogged = false;
			return slot->frame;
		} catch (...) {
			if (submitted && slot->fence.fence12) {
				// The command list was queued. A tail Signal on that queue tracks
				// its completion even when the original Signal failed.
				const uint64_t tail = slot->fence.Next();
				if (const HRESULT hr = queue->Signal(slot->fence.fence12.get(), tail); SUCCEEDED(hr)) {
					slot->frame->producerFence = slot->fence.fence12;
					slot->frame->producerValue = tail;
				} else {
					slot->untracked = true;
					logger::warn("[NativeComposite] DX12 recovery signal failed; retaining slot: {}", DX::com_exception(hr).what());
				}
			} else if (producerSignalled && slot->fence.fence11) {
				// No DX12 work was submitted, but the DX11 copy may still be
				// pending. Fence it before allowing this slot to be recycled.
				const uint64_t tail = slot->fence.Next();
				if (const HRESULT hr = context11->Signal(slot->fence.fence11.get(), tail); SUCCEEDED(hr)) {
					context11->Flush();
					slot->frame->producerFence = slot->fence.fence12;
					slot->frame->producerValue = tail;
				} else {
					slot->untracked = true;
					logger::warn("[NativeComposite] DX11 recovery signal failed; retaining slot: {}", DX::com_exception(hr).what());
				}
			}
			if (!std::exchange(captureFailureLogged, true)) {
				try {
					throw;
				} catch (const winrt::hresult_error& error) {
					logger::warn("[NativeComposite] Capture failed (HRESULT 0x{:08X}): {}", static_cast<uint32_t>(error.code()), winrt::to_string(error.message()));
				} catch (const std::exception& error) {
					logger::warn("[NativeComposite] Capture failed, the frame keeps its plain color: {}", error.what());
				} catch (...) {
					logger::warn("[NativeComposite] Capture failed with an unknown exception");
				}
			}
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
		for (auto& slot : slots) {
			if (!slot)
				continue;
			std::vector<FenceDependency> dependencies;
			if (slot->untracked)
				dependencies.push_back(FencePoint(slot->fence.fence12.get(), UINT64_MAX));
			else if (slot->frame)
				dependencies.push_back(FencePoint(slot->fence.fence12.get(), slot->frame->producerValue));
			for (const auto& consumer : slot->consumers)
				dependencies.push_back({ consumer.first, consumer.second });
			auto device = slot->backend.d3d12Device;
			// Borrowed Frame objects independently own their published color and
			// fence. With no slot reuse after destruction, only submitted GPU
			// dependencies require retaining the other slot resources.
			RetireAfterFences(std::move(slot), std::move(device), std::move(dependencies));
		}
	}
}
