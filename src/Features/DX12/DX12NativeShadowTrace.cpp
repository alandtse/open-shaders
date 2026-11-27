#include "DX12NativeShadowTrace.h"
#include "DX12GpuRetirement.h"
#include "DX12NativeLightWorker.h"
#include "Utils/FileSystem.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <directx/d3dx12.h>
#include <ranges>
#include <stdexcept>
#include <tuple>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kRecords = kLightRecordsPerEmitter;
		constexpr uint64_t kMaxShadowBytecodeBytes = 4 * VideoMemoryBudget::kMiB;
		constexpr uint32_t kShadowThreadGroupSize = 8;
		struct Constants
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, lightCount, frameIndex;
			uint32_t physicalWidth, physicalHeight, lightSlotCount;
		};
		static_assert(sizeof(Constants) <= D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
		winrt::com_ptr<ID3D12Resource> UploadBuffer(ID3D12Device* device, UINT64 bytes)
		{
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
			winrt::com_ptr<ID3D12Resource> result;
			DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(result.put())));
			return result;
		}
		void WriteUpload(ID3D12Resource* resource, const void* bytes, size_t count)
		{
			const D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			DX::ThrowIfFailed(resource->Map(0, &noRead, &mapped));
			std::memcpy(mapped, bytes, count);
			resource->Unmap(0, nullptr);
		}
	}

	ShadowMaskTrace::~ShadowMaskTrace()
	{
		current.reset();
		for (auto& slot : slots) {
			if (!slot)
				continue;
			std::vector<FenceDependency> dependencies;
			dependencies.push_back(FencePoint(slot->readyFence ? slot->readyFence.get() : completion.get(), slot->readyValue));
			RetireAfterFences(std::tuple{ roots[slot->pipelineVariant >= kShadowVariantCount], pipelines[slot->pipelineVariant], std::move(slot) }, backend.d3d12Device, std::move(dependencies));
		}
	}

	void ShadowMaskTrace::Initialize(const GraphicsBackend& requested, uint32_t variant)
	{
		if (pipelines[variant])
			return;
		if (!completion)
			backend = requested;
		auto* device = backend.d3d12Device.get();
		if (!device || !backend.commandQueue)
			throw std::runtime_error("DX12 Native Scene queue unavailable");
		const bool slices = variant >= kShadowVariantCount;
		auto& root = roots[slices];
		constexpr const char* names[]{ "ShadowOffload.dxil", "ShadowRTQuality.dxil", "ShadowRTPerformance.dxil",
			"ShadowRTQualitySlices.dxil", "ShadowRTPerformanceSlices.dxil" };
		const auto shader = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath() / "DX12NativeScene" / names[variant], kMaxShadowBytecodeBytes);

		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, slices ? 2u : 1u, 0, 0, 0 };
		D3D12_ROOT_PARAMETER params[5]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[0].Descriptor.ShaderRegister = 0;  // t0 TLAS
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 1, &ranges[0] };  // t1 raster depth
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable = { 1, &ranges[1] };  // u0 raw mask
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[3].Descriptor.ShaderRegister = 2;  // t2 light records
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[4].Descriptor.ShaderRegister = 0;  // b0 camera and sample settings
		const D3D12_ROOT_SIGNATURE_DESC signature{
			UINT(std::size(params)), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE
		};
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
			serialized.put(), errors.put()));
		auto newRoot = root;
		if (!newRoot)
			DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
				serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
		pipelineDesc.pRootSignature = newRoot.get();
		pipelineDesc.CS = { shader.data(), shader.size() };
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(newPipeline.put())));
		auto newCompletion = completion;
		if (!newCompletion)
			DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(newCompletion.put())));
		if (slices && !visibilityReady.fence12)
			visibilityReady.Create(device, backend.d3d11Device.get(), "DX12NativeScene::LightVisibilityReady");
		root = std::move(newRoot);
		pipelines[variant] = std::move(newPipeline);
		completion = std::move(newCompletion);
	}

	void ShadowMaskTrace::BeginFrame(uint32_t frame)
	{
		if (current && current->frame != frame)
			current.reset();
		if (!completion)
			return;
		const uint64_t finished = completion->GetCompletedValue();
		if (finished == UINT64_MAX)
			return;
		for (auto& slot : slots) {
			if (slot && slot.use_count() == 1 && slot->readyValue != UINT64_MAX && slot->readyValue <= finished) {
				slot->raster.reset();
				slot->scene.reset();
				slot->nativeLights.reset();
			}
		}
	}

	void ShadowMaskTrace::Dispatch(const GraphicsBackend& requested,
		std::shared_ptr<const RasterFrame> raster, std::shared_ptr<const TopLevel> scene,
		std::span<const DX12::PreparedLight> lights, uint32_t frame, const ShadowTraceControls& controls)
	{
		if (current && current->frame == frame)
			return;
		current.reset();
		ReportSkipped(Outcome::None);
		const auto quality = controls.quality;
		const bool slices = controls.captureLightVisibility && quality != kShadowQualityOffloading;
		const auto variant = ShadowTraceVariant(quality, slices);
		const auto fallback = [&] {
			auto combined = controls;
			combined.captureLightVisibility = false;
			Dispatch(requested, raster, scene, lights, frame, combined);
		};
		const bool uploadLights = !controls.nativeLights || controls.uploadLights;
		const auto& pipeline = pipelines[variant];
		if (initializationFailed[variant]) {
			if (slices)
				return fallback();
			return ReportSkipped(Outcome::InitializationFailed);
		}
		if (!raster || !raster->depth || !raster->depth->resource || !raster->readyFence ||
			!raster->cameraValid) {
			return ReportSkipped(Outcome::NoRaster);
		}
		if (!scene || !scene->acceleration || !scene->producerFence) {
			return ReportSkipped(Outcome::NoScene);
		}
		if (controls.nativeLights && (!controls.nativeLights->buffer || controls.nativeLights->scope != scene->scope))
			return ReportSkipped(Outcome::NoLights);
		if (lights.empty()) {
			return ReportSkipped(Outcome::NoLights);
		}
		std::shared_ptr<ShadowMaskFrame> attempted;
		bool submitted = false;
		try {
			Initialize(requested, variant);
			const uint64_t finished = completion->GetCompletedValue();
			if (finished == UINT64_MAX)
				return ReportSkipped(Outcome::Failed);
			// Retire idle experimental allocations when leaving the experiment.
			if (!slices) {
				for (auto& slot : slots)
					if (slot && slot->lightVisibility && slot.use_count() == 1 && slot->readyValue <= finished)
						slot.reset();
			}
			auto it = std::ranges::find_if(slots, [&](const auto& slot) {
				if (!slot)
					return true;
				return slot.use_count() == 1 && slot->readyValue <= finished;
			});
			if (it == slots.end()) {
				return ReportSkipped(Outcome::NoFreeSlot);
			}
			auto& slot = *it;
			const UINT width = controls.viewportWidth, height = controls.viewportHeight;
			if (!width || !height || width > raster->width || height > raster->height) {
				return ReportSkipped(Outcome::ViewportMismatch);
			}
			const uint32_t maskWidth = ShadowPhysicalWidth(ShadowLogicalExtent(width, quality), quality);
			const uint32_t maskHeight = ShadowLogicalExtent(height, quality);
			const uint32_t lightCount = uint32_t(std::min(lights.size(), size_t(kMaxShadowLights)));
			const uint64_t lightBytes = uint64_t(std::bit_ceil(lightCount)) * kRecords * sizeof(std::array<float, 4>);
			const uint32_t lightSlotCount = ShadowVisibilityCount(lightCount, slices);
			// Always create an array SRV, including when only one light is submitted.
			const uint32_t lightCapacity = ShadowVisibilityCapacity(lightSlotCount);
			const bool fresh = !slot || slot->maskWidth != maskWidth || slot->maskHeight != maskHeight ||
			                   slot->pipelineVariant != variant || slot->lightVisibilityCapacity != lightCapacity;
			if (fresh) {
				auto replacement = std::make_shared<ShadowMaskFrame>();
				D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ DXGI_FORMAT_R32G32_FLOAT };
				DX::ThrowIfFailed(requested.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
				if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
					throw std::runtime_error("Offload mask format unavailable");
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
					DXGI_FORMAT_R32G32_FLOAT, maskWidth, maskHeight, 1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				uint64_t allocation = requested.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				if (!allocation || allocation == UINT64_MAX)
					throw std::runtime_error("Native shadowmask allocation unsupported");
				if (slices) {
					D3D12_FEATURE_DATA_FORMAT_SUPPORT sliceSupport{ DXGI_FORMAT_R8_UNORM };
					DX::ThrowIfFailed(requested.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &sliceSupport, sizeof(sliceSupport)));
					if (!(sliceSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) || !(sliceSupport.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
						throw std::runtime_error("Native light visibility format unavailable");
					const auto sliceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, maskWidth, maskHeight,
						static_cast<UINT16>(lightCapacity), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
					const uint64_t sliceBytes = requested.d3d12Device->GetResourceAllocationInfo(0, 1, &sliceDesc).SizeInBytes;
					if (!sliceBytes || sliceBytes == UINT64_MAX || allocation > UINT64_MAX - sliceBytes)
						throw std::runtime_error("Native light visibility allocation unsupported");
					allocation += sliceBytes;
				}
				uint64_t tracked = 0;
				for (const auto& existing : slots) {
					if (!existing)
						continue;
					if (tracked > UINT64_MAX - existing->allocationBytes)
						throw std::runtime_error("Native shadowmask size overflow");
					tracked += existing->allocationBytes;
				}
				const uint64_t allowance = videoBudget.Query(requested.d3d12Device.get(), tracked).effectiveBytes;
				if (allocation > allowance || tracked > allowance - allocation)
					throw std::runtime_error("Native shadowmask allocation deferred by DXGI budget");
				replacement->allocationBytes = allocation;
				replacement->pipelineVariant = variant;
				replacement->lightVisibilityCapacity = lightCapacity;
				DX::ThrowIfFailed(requested.d3d12Device->CreateCommittedResource(&heap,
					D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					nullptr, IID_PPV_ARGS(replacement->mask.put())));
				replacement->mask->SetName(L"DX12NativeScene::RawShadowMask");
				if (slices) {
					D3D11_TEXTURE2D_DESC sliceDesc{};
					sliceDesc.Width = maskWidth;
					sliceDesc.Height = maskHeight;
					sliceDesc.MipLevels = 1;
					sliceDesc.ArraySize = lightCapacity;
					sliceDesc.Format = DXGI_FORMAT_R8_UNORM;
					sliceDesc.SampleDesc.Count = 1;
					sliceDesc.Usage = D3D11_USAGE_DEFAULT;
					sliceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					replacement->lightVisibility = std::make_unique<WrappedResource>(sliceDesc,
						requested.d3d11Device.get(), requested.d3d12Device.get(), "DX12NativeScene::LightVisibility");
				}
				D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
				descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				descriptors.NumDescriptors = slices ? 3u : 2u;
				DX::ThrowIfFailed(requested.d3d12Device->CreateDescriptorHeap(
					&descriptors, IID_PPV_ARGS(replacement->descriptors.put())));
				if (slices) {
					descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
					descriptors.NumDescriptors = 1;
					DX::ThrowIfFailed(requested.d3d12Device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(replacement->clearDescriptors.put())));
				}
				replacement->cameraUpload = UploadBuffer(requested.d3d12Device.get(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				if (uploadLights)
					replacement->lightUpload = UploadBuffer(requested.d3d12Device.get(), lightBytes);
				DX::ThrowIfFailed(requested.d3d12Device->CreateCommandAllocator(
					D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				DX::ThrowIfFailed(requested.d3d12Device->CreateCommandList(0,
					D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(),
					pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				if (uploadLights && (!slot->lightUpload || slot->lightUpload->GetDesc().Width < lightBytes)) {
					slot->lightRecordsUploaded = false;
					slot->lightUpload = UploadBuffer(requested.d3d12Device.get(), lightBytes);
				}
				DX::ThrowIfFailed(slot->allocator->Reset());
				DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->readyValue = UINT64_MAX;
			slot->readyFence = completion;
			attempted = slot;
			// Retain the arguments locally for a combined-mask retry if the optional producer fails.
			slot->raster = raster;
			slot->scene = scene;
			slot->nativeLights = controls.nativeLights;
			slot->uploadedLights = uploadLights;
			slot->frame = frame;
			slot->width = width;
			slot->height = height;
			slot->maskWidth = maskWidth;
			slot->maskHeight = maskHeight;
			slot->quality = quality;
			slot->lightVisibilityValue = 0;
			slot->visibilityLights.clear();
			if (slices)
				slot->visibilityLights.assign(lights.begin(), lights.begin() + lightSlotCount);
			const Constants camera{
				slot->raster->camera.CameraProjInverse,
				slot->raster->camera.CameraViewInverse,
				slot->raster->camera.CameraPosAdjust,
				width, height, lightCount, frame, maskWidth, maskHeight, lightSlotCount
			};
			WriteUpload(slot->cameraUpload.get(), &camera, sizeof(camera));

			if (slot->uploadedLights) {
				auto& records = slot->lightRecords;
				const size_t recordCount = size_t(lightCount) * kRecords;
				bool changed = !slot->lightRecordsUploaded || records.size() != recordCount;
				slot->lightRecordsUploaded = false;
				records.resize(recordCount);
				for (size_t i = 0; i < lightCount; ++i) {
					const auto packed = PackNativeLight(lights[i]);
					auto* destination = records.data() + i * kRecords;
					if (std::memcmp(destination, packed.data(), sizeof(packed)) != 0) {
						std::memcpy(destination, packed.data(), sizeof(packed));
						changed = true;
					}
				}
				if (changed)
					WriteUpload(slot->lightUpload.get(), records.data(), records.size() * sizeof(records.front()));
				slot->lightRecordsUploaded = true;
			}
			slot->debugLights.clear();
			slot->debugPreparedCount = 0;
			if (controls.captureDebugLights) {
				slot->debugLights.assign(lights.begin(), lights.begin() + lightCount);
				slot->debugPreparedCount = std::min(controls.debugPreparedCount, lightCount);
			}

			const UINT step = requested.d3d12Device->GetDescriptorHandleIncrementSize(
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
			D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
			depth.Format = DXGI_FORMAT_R32_FLOAT;
			depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			depth.Texture2D.MipLevels = 1;
			requested.d3d12Device->CreateShaderResourceView(
				slot->raster->depth->resource.get(), &depth, cpu);
			cpu.ptr += step;
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32G32_FLOAT;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			requested.d3d12Device->CreateUnorderedAccessView(slot->mask.get(), nullptr, &uav, cpu);

			ID3D12Resource* lightMask = slices ? slot->lightVisibility->resource.get() : nullptr;
			D3D12_CPU_DESCRIPTOR_HANDLE clearCpu{};
			if (slices) {
				cpu.ptr += step;
				D3D12_UNORDERED_ACCESS_VIEW_DESC lightUav{};
				lightUav.Format = DXGI_FORMAT_R8_UNORM;
				lightUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				lightUav.Texture2DArray.ArraySize = lightCapacity;
				clearCpu = slot->clearDescriptors->GetCPUDescriptorHandleForHeapStart();
				requested.d3d12Device->CreateUnorderedAccessView(lightMask, nullptr, &lightUav, cpu);
				requested.d3d12Device->CreateUnorderedAccessView(lightMask, nullptr, &lightUav, clearCpu);
			}
			auto* command = slot->commands.get();
			ID3D12DescriptorHeap* heaps[]{ slot->descriptors.get() };
			command->SetDescriptorHeaps(1, heaps);
			command->SetComputeRootSignature(roots[slices].get());
			command->SetPipelineState(pipeline.get());
			command->SetComputeRootShaderResourceView(0, slot->scene->acceleration->GetGPUVirtualAddress());
			auto gpu = slot->descriptors->GetGPUDescriptorHandleForHeapStart();
			command->SetComputeRootDescriptorTable(1, gpu);
			gpu.ptr += step;
			command->SetComputeRootDescriptorTable(2, gpu);
			command->SetComputeRootShaderResourceView(3, (slot->uploadedLights ? slot->lightUpload : slot->nativeLights->buffer)->GetGPUVirtualAddress());
			command->SetComputeRootConstantBufferView(4, slot->cameraUpload->GetGPUVirtualAddress());

			const auto depthStart = CD3DX12_RESOURCE_BARRIER::Transition(
				slot->raster->depth->resource.get(), D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			command->ResourceBarrier(1, &depthStart);
			if (!fresh) {
				const auto maskStart = CD3DX12_RESOURCE_BARRIER::Transition(
					slot->mask.get(), D3D12_RESOURCE_STATE_COMMON,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				command->ResourceBarrier(1, &maskStart);
			}
			if (slices) {
				const auto start = CD3DX12_RESOURCE_BARRIER::Transition(lightMask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				command->ResourceBarrier(1, &start);
				auto lightGpu = slot->descriptors->GetGPUDescriptorHandleForHeapStart();
				lightGpu.ptr += 2ull * step;
				constexpr float lit[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
				command->ClearUnorderedAccessViewFloat(lightGpu, clearCpu, lightMask, lit, 0, nullptr);
				const auto cleared = CD3DX12_RESOURCE_BARRIER::UAV(lightMask);
				command->ResourceBarrier(1, &cleared);
			}
			command->Dispatch((maskWidth + kShadowThreadGroupSize - 1) / kShadowThreadGroupSize,
				(maskHeight + kShadowThreadGroupSize - 1) / kShadowThreadGroupSize, 1);
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(slot->mask.get());
			command->ResourceBarrier(1, &uavBarrier);
			if (slices) {
				const auto written = CD3DX12_RESOURCE_BARRIER::UAV(lightMask);
				command->ResourceBarrier(1, &written);
				const auto end = CD3DX12_RESOURCE_BARRIER::Transition(lightMask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
				command->ResourceBarrier(1, &end);
			}
			const std::array end{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->mask.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
			};
			command->ResourceBarrier(UINT(end.size()), end.data());
			DX::ThrowIfFailed(command->Close());

			// These are queue-side dependencies, never CPU waits or render-thread stalls.
			DX::ThrowIfFailed(requested.commandQueue->Wait(slot->raster->readyFence.get(), slot->raster->readyValue));
			DX::ThrowIfFailed(requested.commandQueue->Wait(slot->scene->producerFence.get(), slot->scene->producerValue));
			ID3D12CommandList* lists[]{ command };
			requested.commandQueue->ExecuteCommandLists(1, lists);
			submitted = true;
			if (slices) {
				const auto ready = visibilityReady.Next();
				DX::ThrowIfFailed(requested.commandQueue->Signal(visibilityReady.fence12.get(), ready));
				slot->lightVisibilityFence = visibilityReady.fence11;
				slot->lightVisibilityValue = ready;
			}
			const uint64_t value = nextFence + 1;
			DX::ThrowIfFailed(requested.commandQueue->Signal(completion.get(), value));
			nextFence = value;
			slot->readyValue = value;
			current = slot;
			lastLightCount.store(lightCount, std::memory_order_relaxed);
			lastVisibilityCount.store(lightSlotCount, std::memory_order_relaxed);
			lastOutcome.store(Outcome::Dispatched, std::memory_order_relaxed);
			failureLogged = false;
		} catch (...) {
			ReportSkipped(Outcome::Failed);
			initializationFailed[variant] = !pipeline;
			if (attempted) {
				if (submitted && completion && backend.commandQueue) {
					// ExecuteCommandLists succeeded. A queue-tail marker retires the
					// submission even if its original Signal failed.
					const uint64_t rescue = nextFence + 1;
					if (const HRESULT hr = backend.commandQueue->Signal(completion.get(), rescue); SUCCEEDED(hr)) {
						nextFence = rescue;
						attempted->readyValue = rescue;
					} else {
						logger::warn("[DX12NativeScene] Shadow recovery signal failed; retaining slot: {}", DX::com_exception(hr).what());
					}
				} else if (!submitted) {
					attempted->readyValue = 0;
					attempted->raster.reset();
					attempted->scene.reset();
					attempted->nativeLights.reset();
					// A recorded but unsubmitted fresh resource still has its initial state.
					// Drop this slot rather than reuse it with the submitted-state assumption.
					if (slices) {
						for (auto& entry : slots)
							if (entry == attempted)
								entry.reset();
					}
				}
			}
			if (!failureLogged) {
				failureLogged = true;
				logger::warn("[DX12NativeScene] Shadow dispatch failed: {}", Util::DescribeCurrentD3DException());
			}
			if (slices)
				fallback();
		}
	}

}
