#include "PCH.h"
#include "NativeCompositePass.h"
#include "GpuPass.h"
#include "Globals.h"
#include <directx/d3dx12.h>
#include <fstream>
#include <limits>

bool NativeComposite::InitializeShadow(ID3D12Device* a_device)
{
	if (shadowPipeline && shadowPipelineDevice == a_device) return true;
	shadowPipelineFailure.clear();
	try {
		std::ifstream file("Data/Shaders/NativeComposite/ShadowLayer.dxil", std::ios::binary | std::ios::ate);
		if (!file) {
			shadowPipelineFailure = "ShadowLayer.dxil missing: Data/Shaders/NativeComposite/ShadowLayer.dxil";
			return false;
		}
		const auto size = file.tellg();
		if (size <= 0 || size > 16 * 1024 * 1024) {
			shadowPipelineFailure = "ShadowLayer.dxil size is invalid (empty or >16 MiB)";
			return false;
		}
		std::vector<char> shader(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(shader.data(), size)) {
			shadowPipelineFailure = "ShadowLayer.dxil could not be read completely";
			return false;
		}
		CD3DX12_DESCRIPTOR_RANGE ranges[2];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER parameters[3];
		parameters[0].InitAsDescriptorTable(1, &ranges[0]);
		parameters[1].InitAsDescriptorTable(1, &ranges[1]);
		parameters[2].InitAsConstants(40, 0);
		const CD3DX12_ROOT_SIGNATURE_DESC desc(3, parameters);
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		DX::ThrowIfFailed(a_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = newRoot.get(); pso.CS = { shader.data(), shader.size() };
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(a_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
		shadowRoot = std::move(newRoot); shadowPipeline = std::move(newPipeline); shadowPipelineDevice = a_device;
		return true;
	} catch (const std::exception& error) {
		shadowPipelineFailure = std::string("ShadowLayer.dxil root signature or PSO creation failed: ") + error.what();
		return false;
	} catch (...) {
		shadowPipelineFailure = "ShadowLayer.dxil root signature or PSO creation failed (unknown error)";
		return false;
	}
}

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
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
		CD3DX12_ROOT_PARAMETER parameters[3];
		parameters[0].InitAsDescriptorTable(1, &ranges[0]);
		parameters[1].InitAsDescriptorTable(1, &ranges[1]);
		parameters[2].InitAsConstants(8, 0);
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
	const std::shared_ptr<const NativeLightingFeature::Contribution>& a_lighting,
	const std::shared_ptr<const DX12Native::ShadowFrame>& a_shadow,
	const std::shared_ptr<const DX12Native::RasterFrame>& a_raster,
	const ShadowSettings& a_shadowSettings,
	float a_strength, const Policy& a_policy, uint64_t a_revision)
{
	CS_GPU_PASS("NativeComposite::RasterPassThrough");
	status = "Invalid capture input";
	shadowStatus = "Independent shadow layer disabled or contribution is zero";
	if (!a_device11 || !a_context11 || !a_device12 || !a_queue || !a_source || !a_width || !a_height) {
		shadowStatus = "Shadow composite missing device, queue, input texture or dimensions";
		return {};
	}
	winrt::com_ptr<ID3D11Device> sourceDevice;
	a_source->GetDevice(sourceDevice.put());
	winrt::com_ptr<ID3D12Device> queueDevice;
	if (sourceDevice.get() != a_device11 || FAILED(a_queue->GetDevice(IID_PPV_ARGS(queueDevice.put()))) ||
		queueDevice.get() != a_device12 || a_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
		shadowStatus = "Shadow composite DX11/DX12 device or command queue mismatch";
		return {};
	}

	D3D11_TEXTURE2D_DESC desc{};
	a_source->GetDesc(&desc);
	if (desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
		desc.Width < a_width || desc.Height < a_height || desc.Format == DXGI_FORMAT_UNKNOWN) {
		shadowStatus = "Shadow composite input has unsupported dimensions, sample count or format";
		return {};
	}
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ desc.Format };
	if (FAILED(a_device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
		!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) {
		status = "Unsupported scene format";
		shadowStatus = "Composite scene format cannot be sampled by the DX12 device";
		return {};
	}
	const bool supportsLighting = (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
		(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
	const bool shadowRequested = (a_shadowSettings.contribution * a_shadowSettings.blend > 0.0f) || a_shadowSettings.preview;
	bool applyShadow = false;
	// A raster-only submission must not conceal the exact shadow admission gate
	// that prevented this current frame from consuming the independent mask.
	if (shadowRequested) {
		if (!supportsLighting) shadowStatus = "Composite scene format does not support typed UAV load/store";
		else if (!a_shadow) shadowStatus = "Current-frame shadow mask not published by DX12 Native Scene";
		else if (!a_raster) shadowStatus = "Current-frame raster depth snapshot not published";
		else if (!a_shadow->mask || !a_shadow->mask->resource) shadowStatus = "Shadow mask texture unavailable";
		else if (!a_shadow->readyFence || !a_shadow->readyValue || a_shadow->readyValue == UINT64_MAX)
			shadowStatus = "Shadow producer fence missing or invalid";
		else if (a_shadow->frame != a_frame) shadowStatus = "Shadow mask belongs to another frame";
		else if (a_shadow->width != a_width || a_shadow->height != a_height)
			shadowStatus = "Shadow mask dimensions differ from native composite";
		else if (a_shadow->retainedBackend.d3d12Device.get() != a_device12)
			shadowStatus = "Shadow mask belongs to a different DX12 device";
		else if (!a_raster->depth || !a_raster->depth->resource) shadowStatus = "Raster depth texture unavailable";
		else if (!a_raster->cameraValid) shadowStatus = "Raster depth has no validated current-frame camera";
		else if (!a_raster->readyFence || !a_raster->readyValue || a_raster->readyValue == UINT64_MAX)
			shadowStatus = "Raster capture producer fence missing or invalid";
		else if (a_raster->frame != a_frame) shadowStatus = "Raster depth belongs to another frame";
		// RasterCapture retains the full DX11 allocation, whereas the shadow
		// trace and composite operate on the active dynamic-resolution subrect.
		// A larger raster texture is valid: the shader reads only [0, a_width)
		// x [0, a_height). Reject undersized allocations, not extra capacity.
		else if (a_raster->width < a_width || a_raster->height < a_height)
			shadowStatus = "Raster depth allocation is smaller than active composite resolution";
		else if (a_raster->sourceDepthFormat == DXGI_FORMAT_UNKNOWN)
			shadowStatus = "Captured raster depth source format unavailable";
		else if (a_transfer == Transfer::Unknown)
			shadowStatus = "Composite scene transfer/color space is unknown";
		else if (!InitializeShadow(a_device12))
			shadowStatus = shadowPipelineFailure.empty() ? "ShadowLayer.dxil pipeline initialization failed" : shadowPipelineFailure;
		else {
			applyShadow = true;
			shadowStatus = "Current-frame shadow admitted; awaiting GPU submission";
		}
	}
	const bool blend = supportsLighting && a_lighting && a_lighting->readyFence && a_lighting->readyValue &&
		a_lighting->readyValue != UINT64_MAX && a_lighting->backend.d3d12Device.get() == a_device12 &&
		a_lighting->textures[0] && a_lighting->textures[1] && a_lighting->textures[2] && a_lighting->textures[7] && a_lighting->textures[8] && a_lighting->textures[9] &&
		a_lighting->width == a_width && a_lighting->height == a_height && (a_policy.visual != 0 || a_transfer != Transfer::Unknown) && InitializeLighting(a_device12);

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
		if (!value.countersRead) {
			void* mapped = nullptr;
			const D3D12_RANGE range{ 0, 5 * sizeof(uint32_t) };
			if (SUCCEEDED(value.counterReadback->Map(0, &range, &mapped))) {
				const auto* values = static_cast<const uint32_t*>(mapped);
				acceptedPixels += values[0]; modifiedPixels += values[1]; visualizationPixels += values[2];
				effectiveRecoveredPixels += values[3]; finiteOnlyPixels += values[4];
				const D3D12_RANGE noWrite{ 0, 0 };
				value.counterReadback->Unmap(0, &noWrite);
				++completedComposites;
			} else ++counterReadFailures;
			value.countersRead = true;
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
		status = "Waiting for available frame resources (four fenced composite slots in flight)";
		if (applyShadow) shadowStatus = "Shadow compositor frame slots are still occupied";
		return {};
	}

	bool submitted = false;
	bool producerSignalled = false;
	try {
		const bool recreate = !slot->frame || slot->device != a_device12 || slot->device11 != a_device11 ||
			slot->sourceDesc.Width != desc.Width || slot->sourceDesc.Height != desc.Height || slot->sourceDesc.Format != desc.Format || slot->supportsLighting != supportsLighting;
		if (recreate) {
			// Reserve two full-size images: one DX11/DX12 shared input and
			// one DX12-owned composited output. Guard incremental slot growth
			// with the same adapter-local DXGI headroom policy as native tracing.
			const auto allocationDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, 1, 1, 1, 0,
				supportsLighting ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
			const auto allocation = a_device12->GetResourceAllocationInfo(0, 1, &allocationDesc).SizeInBytes;
			if (allocation == UINT64_MAX || allocation > (UINT64_MAX - 2ull * 1024 * 1024) / 2)
				throw std::runtime_error("Composite allocation size unsupported");
			const uint64_t required = allocation * 2 + 2ull * 1024 * 1024;
			uint64_t retained = 0;
			for (const auto& candidate : slots) if (candidate && candidate.get() != slot) {
				if (retained > UINT64_MAX - candidate->frameBytes) throw std::runtime_error("Composite retained size overflow");
				retained += candidate->frameBytes;
			}
			const uint64_t tracked = retained > UINT64_MAX - slot->frameBytes ? UINT64_MAX : retained + slot->frameBytes;
			const uint64_t allowance = videoBudget.Query(a_device12, tracked).effectiveBytes;
			if (required > allowance || retained > allowance - required)
				throw std::runtime_error("Composite frame allocation deferred by DXGI memory budget");
			auto replacement = std::make_unique<Slot>();
			replacement->frameBytes = required;
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
				heapDesc.NumDescriptors = 3;
				DX::ThrowIfFailed(a_device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(replacement->shadowDescriptors.put())));
				heapDesc.NumDescriptors = 8;
				DX::ThrowIfFailed(a_device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(replacement->lightingDescriptors.put())));
				const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(5 * sizeof(uint32_t));
				const auto counterBuffer = CD3DX12_RESOURCE_DESC::Buffer(5 * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
				const auto readback = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
				DX::ThrowIfFailed(a_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &counterBuffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(replacement->counters.put())));
				DX::ThrowIfFailed(a_device12->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(replacement->counterReset.put())));
				DX::ThrowIfFailed(a_device12->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(replacement->counterReadback.put())));
				void* mapped = nullptr;
				const D3D12_RANGE noRead{ 0, 0 };
				DX::ThrowIfFailed(replacement->counterReset->Map(0, &noRead, &mapped));
				std::memset(mapped, 0, 5 * sizeof(uint32_t));
				replacement->counterReset->Unmap(0, nullptr);
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
				(blend && !(applyShadow && !a_shadowSettings.afterLighting)) ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON)
		};
		slot->commands->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
		auto recordShadow = [&]() {
			slot->shadowRoot = shadowRoot;
			slot->shadowPipeline = shadowPipeline;
			const auto step = a_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = slot->shadowDescriptors->GetCPUDescriptorHandleForHeapStart();
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			a_device12->CreateShaderResourceView(a_shadow->mask->resource.get(), &srv, cpu);
			cpu.ptr += step;
			srv.Format = DXGI_FORMAT_R32_FLOAT;
			a_device12->CreateShaderResourceView(a_raster->depth->resource.get(), &srv, cpu);
			cpu.ptr += step;
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uav.Format = desc.Format;
			a_device12->CreateUnorderedAccessView(slot->frame->color.get(), nullptr, &uav, cpu);
			const std::array start{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(a_shadow->mask->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(a_raster->depth->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
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
			struct Constants {
				uint32_t width, height; float receiverTolerance, radiusPixels;
				uint32_t legacyTransfer, preview; float strength, blend;
				DirectX::SimpleMath::Matrix inverseProjection, inverseView;
			};
			static_assert(sizeof(Constants) == 40 * sizeof(uint32_t));
			const auto& camera = a_raster->camera;
			const Constants constants{ a_width, a_height, a_shadowSettings.receiverTolerance, a_shadowSettings.radiusPixels,
				a_transfer == Transfer::LegacyScene ? 1u : 0u, a_shadowSettings.preview ? 1u : 0u,
				a_shadowSettings.contribution, a_shadowSettings.blend,
				camera.CameraProjInverse, camera.CameraViewInverse };
			slot->commands->SetComputeRoot32BitConstants(2, 40, &constants, 0);
			slot->commands->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
			const std::array end{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(a_shadow->mask->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(a_raster->depth->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
			};
			slot->commands->ResourceBarrier(static_cast<UINT>(end.size()), end.data());
		};
		if (applyShadow && !a_shadowSettings.afterLighting) recordShadow();
		if (blend) {
			if (applyShadow && !a_shadowSettings.afterLighting) {
				// Shadow pass resolved the raster back to COMMON before native direct lighting.
				const auto startDirect = CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(),
					D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				slot->commands->ResourceBarrier(1, &startDirect);
			}
			slot->root = root;
			slot->pipeline = pipeline;
			const auto step = a_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = slot->lightingDescriptors->GetCPUDescriptorHandleForHeapStart();
			std::array<ID3D12Resource*, 6> resources{ a_lighting->textures[0].get(), a_lighting->textures[1].get(), a_lighting->textures[2].get(), a_lighting->textures[7].get(), a_lighting->textures[8].get(), a_lighting->textures[9].get() };
			std::array<D3D12_RESOURCE_BARRIER, 6> lightingBarriers{};
			for (size_t i = 0; i < resources.size(); ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = i >= 2 && i <= 4 ? DXGI_FORMAT_R32_UINT : i == 5 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
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
			cpu.ptr += step;
			D3D12_UNORDERED_ACCESS_VIEW_DESC counterUav{};
			counterUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			counterUav.Buffer.NumElements = 5;
			counterUav.Buffer.StructureByteStride = sizeof(uint32_t);
			a_device12->CreateUnorderedAccessView(slot->counters.get(), nullptr, &counterUav, cpu);
			slot->commands->CopyBufferRegion(slot->counters.get(), 0, slot->counterReset.get(), 0, 5 * sizeof(uint32_t));
			const auto counterBegin = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			slot->commands->ResourceBarrier(1, &counterBegin);
			slot->commands->ResourceBarrier(static_cast<UINT>(lightingBarriers.size()), lightingBarriers.data());
			ID3D12DescriptorHeap* heaps[]{ slot->lightingDescriptors.get() };
			slot->commands->SetDescriptorHeaps(1, heaps);
			slot->commands->SetPipelineState(slot->pipeline.get());
			slot->commands->SetComputeRootSignature(slot->root.get());
			auto gpu = slot->lightingDescriptors->GetGPUDescriptorHandleForHeapStart();
			slot->commands->SetComputeRootDescriptorTable(0, gpu);
			gpu.ptr += 6ull * step;
			slot->commands->SetComputeRootDescriptorTable(1, gpu);
			struct Constants { uint32_t width, height; float strength; uint32_t legacy; uint32_t diffuse, specular, visual, reserved; };
			const Constants constants{ a_width, a_height, a_strength, a_transfer == Transfer::LegacyScene ? 1u : 0u, a_policy.diffuse, a_policy.specular, a_policy.visual, 0 };
			slot->commands->SetComputeRoot32BitConstants(2, 8, &constants, 0);
			slot->commands->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
			const auto counterEnd = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			slot->commands->ResourceBarrier(1, &counterEnd);
			slot->commands->CopyBufferRegion(slot->counterReadback.get(), 0, slot->counters.get(), 0, 5 * sizeof(uint32_t));
			const auto counterRestore = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			slot->commands->ResourceBarrier(1, &counterRestore);
			for (size_t i = 0; i < resources.size(); ++i)
				lightingBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(resources[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			slot->commands->ResourceBarrier(static_cast<UINT>(lightingBarriers.size()), lightingBarriers.data());
			const auto outputBarrier = CD3DX12_RESOURCE_BARRIER::Transition(slot->frame->color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			slot->commands->ResourceBarrier(1, &outputBarrier);
		}
		if (applyShadow && a_shadowSettings.afterLighting) recordShadow();
		DX::ThrowIfFailed(slot->commands->Close());
		if (applyShadow) {
			DX::ThrowIfFailed(a_queue->Wait(a_shadow->readyFence.get(), a_shadow->readyValue));
			DX::ThrowIfFailed(a_queue->Wait(a_raster->readyFence.get(), a_raster->readyValue));
		}
		if (blend)
			DX::ThrowIfFailed(a_queue->Wait(a_lighting->readyFence.get(), a_lighting->readyValue));
		ID3D12CommandList* lists[]{ slot->commands.get() };
		const uint64_t done = slot->fence.Next();
		if (blend && !globals::features::nativeLighting.RetainContribution(a_lighting, slot->fence.fence12.get(), done)) {
			if (applyShadow) shadowStatus = "Shadow submission blocked by native-lighting retention failure";
			slot->untracked = true;
			return {};
		}
		if (applyShadow && !globals::features::dx12NativeScene.RetainShadowMask(a_shadow, slot->fence.fence12.get(), done)) {
			shadowStatus = "Shadow mask consumer-fence retention failed";
			slot->untracked = true;
			return {};
		}
		if (applyShadow && !globals::features::dx12NativeScene.RetainRasterFrame(a_raster, slot->fence.fence12.get(), done)) {
			shadowStatus = "Raster depth consumer-fence retention failed";
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
		frame.shadowSubmitted = applyShadow;
		slot->countersRead = !blend;
		frame.validityRevision = a_revision ^ (blend ? (a_lighting->validityRevision * 0x9e3779b97f4a7c15ull) : 0) ^
			(applyShadow ? (a_shadowSettings.revision * 0xbf58476d1ce4e5b9ull) : 0);
		status = "Submitted";
		if (applyShadow) shadowStatus = "Independent shadow layer submitted";
		return slot->frame;
	} catch (const std::exception& error) {
		status = error.what();
		if (applyShadow) shadowStatus = std::string("Shadow composite submission failed: ") + error.what();
	} catch (...) {
		status = "Capture failed";
		if (applyShadow) shadowStatus = "Shadow composite submission failed (unknown exception)";
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
