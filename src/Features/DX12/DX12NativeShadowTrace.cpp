#include "DX12NativeShadowTrace.h"
#include "DX12GpuRetirement.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <tuple>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kRecords = 3;
		constexpr std::streamoff kMaxShadowBytecodeBytes = 4 * VideoMemoryBudget::kMiB;
		constexpr uint32_t kShadowThreadGroupSize = 8;
		constexpr float kMinRayDistance = 0.5f;
		constexpr float kMaxRayDistance = 60000.0f;

		struct Constants
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, frame, density;
			uint32_t lightCount;
			float minimum = kMinRayDistance, maximum = kMaxRayDistance;
			uint32_t reserved = 0;
			float shadowReachMultiplier = 1.0f, shadowFadeRange = 0.5f, shadowFalloffCurve = 1.0f;
			float nearLightRadius = 0.0f;
			uint32_t nearLightExclusion = 0, padding0 = 0;
			uint32_t padding1 = 0, padding2 = 0;
		};
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
			for (const auto& consumer : slot->consumers)
				dependencies.push_back({ consumer.first, consumer.second });
			RetireAfterFences(std::tuple{ backend, root, pipeline, std::move(slot) }, backend.d3d12Device, std::move(dependencies));
		}
	}

	void ShadowMaskTrace::Initialize(const GraphicsBackend& requested)
	{
		if (pipeline)
			return;
		backend = requested;
		auto* device = backend.d3d12Device.get();
		if (!device || !backend.commandQueue)
			throw std::runtime_error("DX12 Native Scene queue unavailable");
		std::ifstream bytecode(Util::PathHelpers::GetShadersPath() / "DX12NativeScene" / "ShadowMask.dxil",
			std::ios::binary | std::ios::ate);
		if (!bytecode || bytecode.tellg() <= 0 || bytecode.tellg() > kMaxShadowBytecodeBytes)
			throw std::runtime_error("Native Scene shadowmask DXIL unavailable");
		std::vector<char> shader(static_cast<size_t>(bytecode.tellg()));
		bytecode.seekg(0);
		if (!bytecode.read(shader.data(), std::streamsize(shader.size())))
			throw std::runtime_error("Native Scene shadowmask DXIL read failed");

		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
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
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
			serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
		pipelineDesc.pRootSignature = newRoot.get();
		pipelineDesc.CS = { shader.data(), shader.size() };
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(newPipeline.put())));
		winrt::com_ptr<ID3D12Fence> newCompletion;
		DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(newCompletion.put())));
		root = std::move(newRoot);
		pipeline = std::move(newPipeline);
		completion = std::move(newCompletion);
	}

	void ShadowMaskTrace::Dispatch(const GraphicsBackend& requested,
		std::shared_ptr<const RasterFrame> raster, std::shared_ptr<const TopLevel> scene,
		std::span<const DX12::PreparedLight> lights, uint32_t frame, const ShadowTraceControls& controls)
	{
		if (current && current->frame == frame)
			return;
		current.reset();
		if (initializationFailed)
			return;
		if (!raster || !raster->depth || !raster->depth->resource || !raster->readyFence ||
			!raster->cameraValid) {
			return;
		}
		if (!scene || !scene->acceleration || !scene->producerFence) {
			return;
		}
		if (lights.empty()) {
			return;
		}
		std::shared_ptr<ShadowMaskFrame> attempted;
		bool submitted = false;
		try {
			Initialize(requested);
			const uint64_t finished = completion->GetCompletedValue();
			auto it = std::ranges::find_if(slots, [&](const auto& slot) {
				if (!slot)
					return true;
				return slot.use_count() == 1 && slot->readyValue <= finished &&
				       std::ranges::all_of(slot->consumers, [](const auto& c) {
						   return c.first->GetCompletedValue() >= c.second;
					   });
			});
			if (it == slots.end()) {
				return;
			}
			auto& slot = *it;
			const UINT width = controls.viewportWidth, height = controls.viewportHeight;
			if (!width || !height || width > raster->width || height > raster->height) {
				return;
			}
			const bool fresh = !slot || slot->width != width || slot->height != height;
			if (fresh) {
				auto replacement = std::make_shared<ShadowMaskFrame>();
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
					DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const uint64_t allocation = requested.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				if (!allocation || allocation == UINT64_MAX)
					throw std::runtime_error("Native shadowmask allocation unsupported");
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
				DX::ThrowIfFailed(requested.d3d12Device->CreateCommittedResource(&heap,
					D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					nullptr, IID_PPV_ARGS(replacement->mask.put())));
				replacement->mask->SetName(L"DX12NativeScene::RawShadowMask");
				D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
				descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				descriptors.NumDescriptors = 2;
				DX::ThrowIfFailed(requested.d3d12Device->CreateDescriptorHeap(
					&descriptors, IID_PPV_ARGS(replacement->descriptors.put())));
				replacement->cameraUpload = UploadBuffer(requested.d3d12Device.get(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				replacement->lightUpload = UploadBuffer(requested.d3d12Device.get(),
					sizeof(std::array<float, 4>) * kMaxShadowLights * kRecords);
				DX::ThrowIfFailed(requested.d3d12Device->CreateCommandAllocator(
					D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				DX::ThrowIfFailed(requested.d3d12Device->CreateCommandList(0,
					D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(),
					pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				DX::ThrowIfFailed(slot->allocator->Reset());
				DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->consumers.clear();
			slot->readyValue = UINT64_MAX;
			slot->readyFence = completion;
			attempted = slot;
			slot->raster = std::move(raster);
			slot->scene = std::move(scene);
			slot->frame = frame;
			slot->width = width;
			slot->height = height;
			const Constants camera{
				slot->raster->camera.CameraProjInverse,
				slot->raster->camera.CameraViewInverse,
				slot->raster->camera.CameraPosAdjust,
				width, height, frame, std::min(controls.densityMode, 7u),
				uint32_t(std::min(lights.size(), size_t(kMaxShadowLights))),
				kMinRayDistance, kMaxRayDistance, 0u,
				std::clamp(controls.reachMultiplier, 0.1f, 64.0f),
				std::clamp(controls.fadeRange, 0.0f, 1.0f),
				std::clamp(controls.falloffCurve, 0.0f, 2.5f),
				std::clamp(controls.nearLightRadius, 0.0f, 512.0f),
				controls.nearLightExclusion ? 1u : 0u,
				0u, 0u, 0u
			};
			static_assert(sizeof(Constants) <= D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
			WriteUpload(slot->cameraUpload.get(), &camera, sizeof(camera));

			std::array<std::array<float, 4>, kMaxShadowLights * kRecords> records{};
			for (size_t i = 0; i < camera.lightCount; ++i) {
				const auto& light = lights[i];
				records[i * kRecords + 0] = { light.position.x, light.position.y, light.position.z, light.radius };
				records[i * kRecords + 1] = { light.color.x, light.color.y, light.color.z, light.fade };
				records[i * kRecords + 2] = { light.emitterRadius, light.falloffExponent, float(light.flags), 0 };
			}
			WriteUpload(slot->lightUpload.get(), records.data(), sizeof(records));

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
			uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			requested.d3d12Device->CreateUnorderedAccessView(slot->mask.get(), nullptr, &uav, cpu);

			auto* command = slot->commands.get();
			ID3D12DescriptorHeap* heaps[]{ slot->descriptors.get() };
			command->SetDescriptorHeaps(1, heaps);
			command->SetComputeRootSignature(root.get());
			command->SetPipelineState(pipeline.get());
			command->SetComputeRootShaderResourceView(0, slot->scene->acceleration->GetGPUVirtualAddress());
			auto gpu = slot->descriptors->GetGPUDescriptorHandleForHeapStart();
			command->SetComputeRootDescriptorTable(1, gpu);
			gpu.ptr += step;
			command->SetComputeRootDescriptorTable(2, gpu);
			command->SetComputeRootShaderResourceView(3, slot->lightUpload->GetGPUVirtualAddress());
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
			// The output remains full-resolution regardless of sparse trace density.
			command->Dispatch((width + kShadowThreadGroupSize - 1) / kShadowThreadGroupSize,
				(height + kShadowThreadGroupSize - 1) / kShadowThreadGroupSize, 1);
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(slot->mask.get());
			command->ResourceBarrier(1, &uavBarrier);
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
			const uint64_t value = nextFence + 1;
			DX::ThrowIfFailed(requested.commandQueue->Signal(completion.get(), value));
			nextFence = value;
			slot->readyValue = value;
			current = slot;
			failureLogged = false;
		} catch (...) {
			initializationFailed = !pipeline;
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
				}
			}
			if (!failureLogged) {
				failureLogged = true;
				try {
					throw;
				} catch (const winrt::hresult_error& error) {
					logger::warn("[DX12NativeScene] Shadow dispatch failed (HRESULT 0x{:08X}): {}", static_cast<uint32_t>(error.code()), winrt::to_string(error.message()));
				} catch (const std::exception& error) {
					logger::warn("[DX12NativeScene] Shadow dispatch failed: {}", error.what());
				} catch (...) {
					logger::warn("[DX12NativeScene] Shadow dispatch failed with an unknown exception");
				}
			}
		}
	}

	bool ShadowMaskTrace::RetainThrough(const std::shared_ptr<const ShadowMaskFrame>& frame,
		ID3D12Fence* fence, uint64_t value)
	{
		if (!frame || !fence || !value)
			return false;
		for (const auto& slot : slots) {
			if (slot.get() != frame.get())
				continue;
			winrt::com_ptr<ID3D12Fence> keep;
			keep.copy_from(fence);
			slot->consumers.emplace_back(std::move(keep), value);
			return true;
		}
		return false;
	}
}
