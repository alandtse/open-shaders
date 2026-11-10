#include "ShadowTrace.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include <directx/d3dx12.h>
#include <fstream>
#include <cstring>

namespace DX12Native
{
	void ShadowTrace::CreatePipeline()
	{
		if (pipeline) return;
		std::ifstream file("Data/Shaders/DX12NativeScene/ShadowTrace.dxil", std::ios::binary | std::ios::ate);
		if (!file) throw std::runtime_error("Packaged shadow trace DXIL unavailable");
		const std::streamoff size = file.tellg();
		constexpr std::streamoff kMaximumShaderBytes = 16 * 1024 * 1024;
		if (size <= 0 || size > kMaximumShaderBytes) throw std::runtime_error("Invalid directional DXIL size");
		std::vector<char> bytecode(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(bytecode.data(), std::streamsize(bytecode.size()))) throw std::runtime_error("Cannot read directional DXIL");
		D3D12_DESCRIPTOR_RANGE ranges[6]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
		ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ShadowBindings::kMaximumInstances, 0, 1, 0 };
		ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0 };
		ranges[4] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ShadowBindings::kMaximumInstances, 0, 2, 0 };
		ranges[5] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumTexturePools, 0, 3, 0 };
		D3D12_ROOT_PARAMETER parameters[12]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[0].Descriptor.ShaderRegister = 0;
		for (uint32_t i = 0; i < 3; ++i) {
			parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[i + 1].DescriptorTable = { 1, &ranges[i] };
		}
		parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[4].Descriptor.ShaderRegister = 2;
		parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[5].Constants = { 0, 0, 36 };
		parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[6].DescriptorTable = { 1, &ranges[3] };
		for (uint32_t i = 0; i < 2; ++i) {
			parameters[7 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[7 + i].DescriptorTable = { 1, &ranges[4 + i] };
			parameters[9 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			parameters[9 + i].Descriptor.ShaderRegister = 4 + i;
		}
        // Extra root CBV uses two DWORDs; remains within the 64-DWORD limit.
        parameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[11].Descriptor.ShaderRegister = 1;
        D3D12_STATIC_SAMPLER_DESC samplers[4]{};
		for (uint32_t i = 0; i < 4; ++i) {
			samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samplers[i].AddressU = (i & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].AddressV = (i & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].MaxAnisotropy = 1;
			samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			samplers[i].ShaderRegister = i;
		}
		const D3D12_ROOT_SIGNATURE_DESC desc{ uint32_t(std::size(parameters)), parameters, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
		DX::ThrowIfFailed(backend.d3d12Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
		root->SetName(L"DX12NativeScene::ShadowRoot");
		D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
		state.pRootSignature = root.get();
		state.CS = { bytecode.data(), bytecode.size() };
		DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline.put())));
		pipeline->SetName(L"DX12NativeScene::ShadowPipeline");
	}

	bool ShadowTrace::Initialize(const GraphicsBackend& a_backend)
	{
		if (failed) return false;
		if (producer.fence12 && (backend.d3d12Device.get() != a_backend.d3d12Device.get() ||
			backend.commandQueue.get() != a_backend.commandQueue.get() || backend.d3d11Device.get() != a_backend.d3d11Device.get())) {
			status = "Shadow backend changed; restart required";
			failed = true;
			return false;
		}
		if (pipeline) return true;
		try {
			backend = a_backend;
			if (!backend.d3d12Device || !backend.commandQueue || !backend.d3d11Device || !backend.d3d11Context)
				throw std::runtime_error("Shadow backend unavailable");
			D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
			DX::ThrowIfFailed(backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
			if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) throw std::runtime_error("Shadow geometry requires resource binding tier 3");
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 raytracing{};
			DX::ThrowIfFailed(backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &raytracing, sizeof(raytracing)));
			if (raytracing.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) throw std::runtime_error("Shadow tracing requires DXR 1.1");
			D3D12_FEATURE_DATA_SHADER_MODEL model{ D3D_SHADER_MODEL_6_5 };
			DX::ThrowIfFailed(backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
			if (model.HighestShaderModel < D3D_SHADER_MODEL_6_5) throw std::runtime_error("Shadow tracing requires shader model 6.5");
			if (!producer.fence12) producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "DX12NativeScene::ShadowProducer");
			if (!completion) {
				DX::ThrowIfFailed(backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
				completion->SetName(L"DX12NativeScene::ShadowCompletion");
			}
			CreatePipeline();
			return true;
		} catch (const std::exception& error) {
			if (producer.fence12 && !producer.fence11) producer.Reset();
			status = error.what();
			failed = true;
			return false;
		}
	}

	ShadowTrace::~ShadowTrace()
	{
		current.reset();
		const auto done = completion ? completion->GetCompletedValue() : 0;
		const auto produced = producer.fence12 ? producer.fence12->GetCompletedValue() : 0;
		for (auto& frame : frames) {
			if (!frame) continue;
			const bool pendingConsumer = std::ranges::any_of(frame->consumers, [](const auto& consumer) {
				const auto value = consumer.first->GetCompletedValue();
				return value == UINT64_MAX || value < consumer.second;
			});
			if (done == UINT64_MAX || produced == UINT64_MAX || frame->readyValue == UINT64_MAX || frame->producerValue == UINT64_MAX ||
				frame->readyValue > done || frame->producerValue > produced || pendingConsumer)
				(void)new std::shared_ptr<ShadowFrame>(std::move(frame));
		}
	}

	void ShadowTrace::Reset()
	{
		current.reset();
		status.clear();
		failed = false;
	}

	bool ShadowTrace::RetireScenesExcept(const std::shared_ptr<const TopLevel>& a_scene)
	{
		const auto done = completion ? completion->GetCompletedValue() : 0;
		const auto produced = producer.fence12 ? producer.fence12->GetCompletedValue() : 0;
		if (done == UINT64_MAX || produced == UINT64_MAX) return false;
		for (auto& old : frames) {
			if (!old || !old->scene || old->scene == a_scene) continue;
			const bool busy = old.use_count() != 1 || old->readyValue == UINT64_MAX || old->producerValue == UINT64_MAX ||
				old->readyValue > done || old->producerValue > produced || std::ranges::any_of(old->consumers, [](const auto& consumer) {
					const auto value = consumer.first->GetCompletedValue();
					return value == UINT64_MAX || value < consumer.second;
				});
			if (busy) return false;
			old.reset();
		}
		return true;
	}

	void ShadowTrace::ReloadShaders()
	{
		Reset();
		pipeline = nullptr;
		root = nullptr;
	}

	std::shared_ptr<const ShadowFrame> ShadowTrace::Get(uint32_t a_frame) const
	{
		return current && current->frame == a_frame ? current : nullptr;
	}

	json ShadowTrace::GetDiagnostics() const
	{
		const auto completed = completion ? completion->GetCompletedValue() : 0;
		return { { "status", status }, { "submissions", submissions }, { "gpuCompletions", completions },
			{ "frameSlots", frames.size() }, { "busyDeferrals", busyDeferrals },
			{ "dxgiAvailable", videoBudget.last.dxgiAvailable }, { "effectiveFrameBudgetBytes", videoBudget.last.effectiveBytes },
			{ "dxgiLocalBudgetBytes", videoBudget.last.localBudget }, { "dxgiLocalUsageBytes", videoBudget.last.localUsage }, { "offeredFrame", current ? current->frame : 0 },
			{ "offered", bool(current) }, { "gpuComplete", current && completed != UINT64_MAX && completed >= current->readyValue },
			{ "readyMaterials", current ? current->bindings->materials->readyMaterials : 0 },
			{ "rejectedMaterials", current ? current->bindings->materials->rejectedMaterials : 0 },
			{ "lightCount", current ? current->lightSnapshot.size() : 0 },
			{ "traceDensityMode", current ? current->controls.traceDensityMode : 0u },
			{ "rayPreparationAPI", "DX12 SM6.5 fused RayQuery" },
			{ "fullWidth", current ? current->width : 0u }, { "fullHeight", current ? current->height : 0u } };
	}

	bool ShadowTrace::RetainThrough(const std::shared_ptr<const ShadowFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_frame || !a_fence || !a_value || a_value == UINT64_MAX) return false;
		for (const auto& frame : frames) {
			if (frame.get() != a_frame.get()) continue;
			winrt::com_ptr<ID3D12Fence> fence;
			fence.copy_from(a_fence);
			frame->consumers.emplace_back(std::move(fence), a_value);
			return true;
		}
		return false;
	}

    void ShadowTrace::UploadLightSnapshot(ShadowFrame& a_frame)
    {
        CS_GPU_PASS("DX12NativeScene::UploadShadowLightSnapshot");
        // DX11 is source transport only. Native ray directions and shadow
        // evaluation are computed exclusively by the DX12 SM6.5 dispatch.
		constexpr uint32_t kMaximumLights = kMaximumNativePointLights;
        constexpr uint32_t kLightTexels = kNativeLightRecordTexels;
        constexpr uint32_t kLightWidth = kNativeShadowLightTextureWidth;
        static_assert(kMaximumLights * kLightTexels == kLightWidth);
		std::array<std::array<float, 4>, kLightWidth * 2> snapshot{};
		for (size_t i = 0; i < a_frame.lightSnapshot.size(); ++i) {
			const auto& light = a_frame.lightSnapshot[i];
			auto* record = snapshot.data() + i * kLightTexels;
			record[1] = { light.radius, light.radius > 0 ? 1.0f / light.radius : 0.0f, light.radius * 0.1f, light.sourceRadius };
			record[2] = { light.position.x, light.position.y, light.position.z, 1 };
			record[5] = { 0, light.supported ? 0.0f : 7.0f, light.supported ? 1.0f : 0.0f, 0 };
			record[6] = { 0, 0, 0, light.supported ? 1.0f : 0.0f };
			record[7] = { light.radiance.x, light.radiance.y, light.radiance.z, light.supported ? 1.0f : 0.0f };
		}
		auto* context = backend.d3d11Context.get();
		context->UpdateSubresource(a_frame.lights->resource11, 0, nullptr, snapshot.data(), kLightWidth * sizeof(snapshot[0]), 0);
		const auto signal = producer.Next();
		DX::ThrowIfFailed(context->Signal(producer.fence11.get(), signal));
		context->Flush();
		DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), signal));
		a_frame.producerValue = signal;
	}
	void ShadowTrace::Record(ShadowFrame& a_frame, uint32_t a_descriptorBase)
	{
		CS_GPU_PASS("DX12NativeScene::TraceShadowMask");
		if (a_frame.allocator) DX::ThrowIfFailed(a_frame.allocator->Reset());
		else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_frame.allocator.put())));
		if (a_frame.commands) DX::ThrowIfFailed(a_frame.commands->Reset(a_frame.allocator.get(), pipeline.get()));
		else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_frame.allocator.get(), pipeline.get(), IID_PPV_ARGS(a_frame.commands.put())));
		const auto step = backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpu = a_frame.bindings->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(a_descriptorBase) * step;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		backend.d3d12Device->CreateShaderResourceView(nullptr, &srv, cpu);
		cpu.ptr += step;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		backend.d3d12Device->CreateUnorderedAccessView(a_frame.mask->resource.get(), nullptr, &uav, cpu);
		cpu.ptr += step;
		backend.d3d12Device->CreateShaderResourceView(a_frame.lights->resource.get(), &srv, cpu);
		auto* commands = a_frame.commands.get();
		ID3D12DescriptorHeap* heaps[]{ a_frame.bindings->heap.get() };

        // Generate rays in the DX12 RayQuery shader itself.
        // The shader shares the shadow output's full-resolution pixel grid.
		commands->SetDescriptorHeaps(1, heaps);
		commands->SetComputeRootSignature(root.get());
        if (!a_frame.cameraConstants)
            throw std::runtime_error("Native shadow camera constants unavailable");
        commands->SetComputeRootConstantBufferView(11,
            a_frame.cameraConstants->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(0, a_frame.scene->result->GetGPUVirtualAddress());
		auto gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(a_descriptorBase) * step;
		commands->SetComputeRootDescriptorTable(1, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(2, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(6, gpu);
		gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(ShadowBindings::kGeometryDescriptorBase) * step;
		commands->SetComputeRootDescriptorTable(3, gpu);
		commands->SetComputeRootShaderResourceView(4, a_frame.bindings->offsets->GetGPUVirtualAddress());
		gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(ShadowBindings::kAttributeDescriptorBase) * step;
		commands->SetComputeRootDescriptorTable(7, gpu);
		gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(ShadowBindings::kTextureDescriptorBase) * step;
		commands->SetComputeRootDescriptorTable(8, gpu);
		commands->SetComputeRootShaderResourceView(9, a_frame.bindings->materials->surfaces->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(10, a_frame.bindings->materials->materials->GetGPUVirtualAddress());
		struct Constants
		{
			uint32_t width, height, frame, count;
			float3 direction;
			float maximum;
			float3 origin;
			float minimum, radius;
			uint32_t padding[3];
			float4 strengths;
			uint32_t exportSignals, signalPadding[3];
			uint32_t indirect[4];
			float4 falloff, stochastic;
		};
		const Constants constants{ a_frame.width, a_frame.height, a_frame.frame, uint32_t(a_frame.scene->bottoms.size()), {}, 60000.0f,
			a_frame.cameraOrigin, 0.5f, 0.0f, { 1u, 0u, uint32_t(a_frame.lightSnapshot.size()) }, { 1, 1, 1, 1 }, 0u, { 4u, a_frame.controls.traceDensityMode, 0u },
			{ 0u, a_frame.bindings->materials->roomCount, a_frame.bindings->materials->roomsKnown ? 1u : 0u, a_frame.controls.normalInfluence },
			{ a_frame.controls.reachMultiplier, a_frame.controls.falloffFraction, a_frame.controls.falloffExponent, a_frame.controls.distanceFade ? 1.0f : 0.0f },
			{ a_frame.controls.stochastic ? 1.0f : 0.0f, a_frame.controls.emitterRadius, 0.0f, 0.0f } };
		static_assert(sizeof(Constants) == 36 * sizeof(uint32_t));
		commands->SetComputeRoot32BitConstants(5, 36, &constants, 0);
		std::array begin{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.lights->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.mask->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
		commands->Dispatch((a_frame.width + 7) / 8, (a_frame.height + 7) / 8, 1);
		const auto ordered = CD3DX12_RESOURCE_BARRIER::UAV(a_frame.mask->resource.get());
		commands->ResourceBarrier(1, &ordered);
		std::array end{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.lights->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.mask->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
		};
		commands->ResourceBarrier(uint32_t(end.size()), end.data());
		DX::ThrowIfFailed(commands->Close());
		ID3D12CommandList* lists[]{ commands };
		backend.commandQueue->ExecuteCommandLists(1, lists);
		const auto signal = ++nextFence;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), signal));
		a_frame.readyValue = signal;
	}
	void ShadowTrace::Dispatch(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene, std::shared_ptr<const ShadowBindings> a_bindings, std::span<const PreparedPointLight> a_lights,
		const globals::FrameBuffer& a_camera, std::array<uint32_t, 2> a_size, uint32_t a_frame, uint64_t a_budget, const ShadowTraceControls& a_controls)
	{
		CS_GPU_PASS("DX12NativeScene::ShadowMaskDispatch");
		if (current && current->frame == a_frame) return;
		current.reset();
		if (!a_scene || !a_scene->result || a_scene->instances.empty() || a_lights.empty() || a_lights.size() > kMaximumNativePointLights ||
			!a_size[0] || !a_size[1] || a_size[0] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_size[1] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			status = "Prepared scene, lights or shadow dimensions unavailable";
			return;
		}
		if (std::ranges::none_of(a_lights, [](const auto& light) { return light.supported; })) {
			status = "No supported prepared point lights";
			return;
		}
		if (!Initialize(a_backend)) return;
		const auto done = completion->GetCompletedValue();
		const auto produced = producer.fence12->GetCompletedValue();
		if (done == UINT64_MAX || produced == UINT64_MAX) { status = "Shadow device removed"; return; }
		for (const auto& frame : frames) {
			if (frame && !frame->completionCounted && frame->readyValue && frame->readyValue != UINT64_MAX && frame->readyValue <= done) {
				frame->completionCounted = true;
				++completions;
			}
		}
		if (!RetireScenesExcept(a_scene)) { status = "Previous shadow scene is still in use"; return; }
		auto tables = std::move(a_bindings);
		if (!tables || tables->scene != a_scene) {
			status = "Prepared scene material bindings unavailable";
			return;
		}
		auto available = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) {
			if (!frame) return true;
			if (frame.use_count() != 1 || frame->readyValue == UINT64_MAX || frame->producerValue == UINT64_MAX ||
				frame->readyValue > done || frame->producerValue > produced) return false;
			return std::ranges::all_of(frame->consumers, [](const auto& consumer) {
				const auto completed = consumer.first->GetCompletedValue();
				return completed != UINT64_MAX && completed >= consumer.second;
			});
		});
		if (available == frames.end()) { ++busyDeferrals; status = "Shadow frames are still in use"; return; }
		uint64_t trackedBytes = tables->bytes;
		for (const auto& frame : frames) if (frame) {
			if (trackedBytes > UINT64_MAX - frame->bytes) { status = "Shadow budget accounting overflow"; return; }
			trackedBytes += frame->bytes;
			if (frame->bindings && frame->bindings != tables) {
				if (trackedBytes > UINT64_MAX - frame->bindings->bytes) { status = "Shadow binding budget accounting overflow"; return; }
				trackedBytes += frame->bindings->bytes;
			}
		}
		const uint64_t budget = videoBudget.Query(backend.d3d12Device.get(), trackedBytes, a_budget).effectiveBytes;
		try {
			// ShadowTrace.dxil builds camera rays directly inside DX12 RayQuery.
			const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, a_size[0], a_size[1], 1, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
			const auto lightDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kNativeShadowLightTextureWidth, 2, 1, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
			const auto textureBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes;
			const auto lightBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &lightDesc).SizeInBytes;
            // Receiver rays are generated in the same DX12 shader; reserve
            // only the full-size shadow mask plus the tiny light snapshot.
            if (textureBytes == UINT64_MAX || lightBytes == UINT64_MAX ||
                textureBytes > budget || lightBytes > budget - textureBytes)
                throw std::runtime_error("Shadow frame allocation exceeds DXGI budget");
            const uint64_t bytes = textureBytes + lightBytes;
			uint64_t retained = tables->bytes;
			for (auto it = frames.begin(); it != frames.end(); ++it) {
				if (it == available || !*it) continue;
				if ((*it)->bytes > budget || retained > budget - (*it)->bytes) throw std::runtime_error("Shadow frames exceed budget");
				retained += (*it)->bytes;
				if ((*it)->bindings && (*it)->bindings != tables) {
					if ((*it)->bindings->bytes > budget || retained > budget - (*it)->bindings->bytes) throw std::runtime_error("Retired shadow bindings exceed budget");
					retained += (*it)->bindings->bytes;
				}
			}
			if (retained > budget || bytes > budget - retained) throw std::runtime_error("Shadow frame budget exhausted");
			auto& slot = *available;
			if (!slot || slot->width != a_size[0] || slot->height != a_size[1]) {
				slot.reset();
				auto frame = std::make_shared<ShadowFrame>();
				frame->width = a_size[0]; frame->height = a_size[1]; frame->bytes = bytes;
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = frame->width; desc.Height = frame->height;
				desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
				desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				frame->mask = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::ShadowMask");
				desc.Width = kNativeShadowLightTextureWidth; desc.Height = 2;
				frame->lights = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::ShadowLights");
				slot = std::move(frame);
			}
			slot->consumers.clear();
			slot->scene = std::move(a_scene);
			slot->bindings = std::move(tables);
			slot->frame = a_frame;
			slot->lightSnapshot.assign(a_lights.begin(), a_lights.end());
			slot->controls = a_controls;
			slot->cameraOrigin = { a_camera.CameraPosAdjust.x + a_camera.CameraViewInverse._14,
				a_camera.CameraPosAdjust.y + a_camera.CameraViewInverse._24, a_camera.CameraPosAdjust.z + a_camera.CameraViewInverse._34 };
			slot->rayProjInverse = a_camera.CameraProjInverse;
			slot->rayViewInverse = a_camera.CameraViewInverse;
            if (!slot->cameraConstants) {
                const auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(slot->cameraConstants.put())));
                slot->cameraConstants->SetName(L"DX12NativeScene::ShadowNativeCamera");
            }
            struct NativeCameraConstants {
                DirectX::SimpleMath::Matrix projectionInverse, viewInverse;
            };
            static_assert(sizeof(NativeCameraConstants) == 128);
            const NativeCameraConstants cameraData{ slot->rayProjInverse, slot->rayViewInverse };
            void* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(slot->cameraConstants->Map(0, &noRead, &mapped));
            std::memcpy(mapped, &cameraData, sizeof(cameraData));
            slot->cameraConstants->Unmap(0, nullptr);
			slot->retainedBackend = backend;
			slot->root = root; slot->pipeline = pipeline;
			slot->producerFence = producer.fence12; slot->producerFence11 = producer.fence11; slot->readyFence = completion;
			slot->readyValue = slot->producerValue = UINT64_MAX;
			slot->completionCounted = false;
			UploadLightSnapshot(*slot); // DX11 input transfer only; all shadow math is DX12.
			Record(*slot, uint32_t(available - frames.begin()) * ShadowBindings::kFrameDescriptorCount);
			current = slot;
			++submissions;
			status = slot->bindings->materials->failure;
		} catch (const std::exception& error) { status = error.what(); }
	}
}
