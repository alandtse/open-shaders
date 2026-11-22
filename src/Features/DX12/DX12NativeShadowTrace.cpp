#include "DX12NativeShadowTrace.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>
#include <stdexcept>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kMaxLights = 64;
		constexpr uint32_t kRecords = 3;

		void Check(HRESULT result)
		{
			if (FAILED(result))
				throw std::runtime_error("DX12 Native Scene shadowmask operation failed");
		}
		struct Constants
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, frame, density;
			uint32_t lightCount;
			float minimum = 0.5f, maximum = 60000.0f;
			uint32_t reserved = 0;
			uint32_t emitterSamples = 1;
			float shadowReachMultiplier = 1.0f, shadowFadeRange = 0.5f, shadowFalloffCurve = 1.0f;
			float nearLightRadius = 0.0f;
			uint32_t nearLightExclusion = 0, stochastic = 0, padding = 0;
			uint32_t debugGeometryMode = 0;
		};
		winrt::com_ptr<ID3D12Resource> UploadBuffer(ID3D12Device* device, UINT64 bytes)
		{
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
			winrt::com_ptr<ID3D12Resource> result;
			Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
				&desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(result.put())));
			return result;
		}
		void WriteUpload(ID3D12Resource* resource, const void* bytes, size_t count)
		{
			const D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			Check(resource->Map(0, &noRead, &mapped));
			std::memcpy(mapped, bytes, count);
			resource->Unmap(0, nullptr);
		}
	}

	ShadowMaskTrace::~ShadowMaskTrace()
	{
		current.reset();
		const uint64_t completed = completion ? completion->GetCompletedValue() : 0;
		for (auto& slot : slots)
			if (slot && (slot->readyValue > completed ||
							std::ranges::any_of(slot->consumers, [](const auto& c) {
								return c.first->GetCompletedValue() < c.second;
							})))
				(void)new std::shared_ptr<ShadowMaskFrame>(std::move(slot));
	}

	void ShadowMaskTrace::Initialize(const GraphicsBackend& requested)
	{
		if (pipeline)
			return;
		backend = requested;
		auto* device = backend.d3d12Device.get();
		if (!device || !backend.commandQueue)
			throw std::runtime_error("DX12 Native Scene queue unavailable");
		std::ifstream bytecode("Data/Shaders/DX12NativeScene/ShadowMask.dxil",
			std::ios::binary | std::ios::ate);
		if (!bytecode || bytecode.tellg() <= 0 || bytecode.tellg() > 4 * 1024 * 1024)
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
		Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
			serialized.put(), errors.put()));
		Check(device->CreateRootSignature(0, serialized->GetBufferPointer(),
			serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
		pipelineDesc.pRootSignature = root.get();
		pipelineDesc.CS = { shader.data(), shader.size() };
		Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(pipeline.put())));
		Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
	}

	void ShadowMaskTrace::Dispatch(const GraphicsBackend& requested,
		std::shared_ptr<const RasterFrame> raster, std::shared_ptr<const TopLevel> scene,
		std::span<const DX12::PreparedLight> lights, uint32_t frame, const ShadowTraceControls& controls)
	{
		if (current && current->frame == frame)
			return;
		current.reset();
		if (!raster || !raster->depth || !raster->depth->resource || !raster->readyFence ||
			!raster->cameraValid) {
			status = "not submitted: raster depth, camera or capture fence unavailable";
			return;
		}
		if (!scene || !scene->acceleration || !scene->producerFence) {
			status = "not submitted: native TLAS unavailable";
			return;
		}
		if (lights.empty() && controls.debugGeometryMode == 0u) {
			status = "not submitted: no prepared lights";
			return;
		}
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
				status = "not submitted: all raw shadowmask slots in flight";
				return;
			}
			auto& slot = *it;
			const UINT width = controls.viewportWidth, height = controls.viewportHeight;
			if (!width || !height || width > raster->width || height > raster->height) {
				status = "not submitted: active viewport missing or outside captured raster dimensions";
				return;
			}
			const bool fresh = !slot || slot->width != width || slot->height != height;
			if (fresh) {
				auto replacement = std::make_shared<ShadowMaskFrame>();
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
					DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				Check(requested.d3d12Device->CreateCommittedResource(&heap,
					D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					nullptr, IID_PPV_ARGS(replacement->mask.put())));
				replacement->mask->SetName(L"DX12NativeScene::RawShadowMask");
				D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
				descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				descriptors.NumDescriptors = 2;
				Check(requested.d3d12Device->CreateDescriptorHeap(
					&descriptors, IID_PPV_ARGS(replacement->descriptors.put())));
				replacement->cameraUpload = UploadBuffer(requested.d3d12Device.get(), 256);
				replacement->lightUpload = UploadBuffer(requested.d3d12Device.get(),
					sizeof(std::array<float, 4>) * kMaxLights * kRecords);
				Check(requested.d3d12Device->CreateCommandAllocator(
					D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				Check(requested.d3d12Device->CreateCommandList(0,
					D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(),
					pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				Check(slot->allocator->Reset());
				Check(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->consumers.clear();
			slot->readyValue = UINT64_MAX;
			slot->raster = std::move(raster);
			slot->scene = std::move(scene);
			slot->frame = frame;
			slot->width = width;
			slot->height = height;
			slot->densityMode = std::min(controls.densityMode, 7u);
			const Constants camera{
				slot->raster->camera.CameraProjInverse,
				slot->raster->camera.CameraViewInverse,
				slot->raster->camera.CameraPosAdjust,
				width, height, frame, slot->densityMode,
				uint32_t(std::min(lights.size(), size_t(kMaxLights))),
				0.5f, 60000.0f, 0u,
				controls.stochastic ? std::clamp(controls.emitterSamples, 1u, 16u) : 1u,
				std::clamp(controls.reachMultiplier, 0.1f, 64.0f),
				std::clamp(controls.fadeRange, 0.0f, 1.0f),
				std::clamp(controls.falloffCurve, 0.0f, 2.5f),
				std::clamp(controls.nearLightRadius, 0.0f, 512.0f),
				controls.nearLightExclusion ? 1u : 0u,
				controls.stochastic ? 1u : 0u, 0u,
				std::min(controls.debugGeometryMode, 3u)
			};
			static_assert(sizeof(Constants) <= 256);
			WriteUpload(slot->cameraUpload.get(), &camera, sizeof(camera));

			std::array<std::array<float, 4>, kMaxLights * kRecords> records{};
			for (size_t i = 0; i < camera.lightCount; ++i) {
				const auto& light = lights[i];
				records[i * 3 + 0] = { light.position.x, light.position.y, light.position.z, light.radius };
				records[i * 3 + 1] = { light.color.x, light.color.y, light.color.z, light.fade };
				records[i * 3 + 2] = { light.emitterRadius, light.falloffExponent, float(light.flags), 0 };
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
			command->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(slot->mask.get());
			command->ResourceBarrier(1, &uavBarrier);
			const std::array end{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->mask.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
			};
			command->ResourceBarrier(UINT(end.size()), end.data());
			Check(command->Close());

			// These are queue-side dependencies, never CPU waits or render-thread stalls.
			Check(requested.commandQueue->Wait(slot->raster->readyFence.get(), slot->raster->readyValue));
			Check(requested.commandQueue->Wait(slot->scene->producerFence.get(), slot->scene->producerValue));
			ID3D12CommandList* lists[]{ command };
			requested.commandQueue->ExecuteCommandLists(1, lists);
			const uint64_t value = ++nextFence;
			Check(requested.commandQueue->Signal(completion.get(), value));
			slot->readyFence = completion;
			slot->readyValue = value;
			current = slot;
			status = "GPU dispatch submitted (fence completion shown separately)";
		} catch (const std::exception& e) {
			status = e.what();
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
