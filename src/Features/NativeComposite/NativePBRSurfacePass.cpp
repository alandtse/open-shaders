#include "NativePBRSurfacePass.h"

#include "Features/NativePBR/MaterialReadiness.h"
#include "Features/NativePBR/MaterialService.h"
#include "GpuPass.h"
#include "PCH.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace
{
	constexpr auto kInputFormats = NativePBR::kSurfaceTransportFormats;
	constexpr std::array<DXGI_FORMAT, NativePBRSurfacePass::kSurfaceOutputs> kOutputFormats{
		DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT
	};
	constexpr uint32_t kFrameDescriptors = NativePBRSurfacePass::kSurfaceInputs + 2;
	constexpr uint32_t kTotalDescriptors = kFrameDescriptors + NativePBRSurfacePass::kMaterialPages + NativePBRSurfacePass::kSurfaceOutputs;
	constexpr uint64_t kDescriptorReserveBytes = 1024 * 1024;

	D3D12_CPU_DESCRIPTOR_HANDLE Offset(D3D12_CPU_DESCRIPTOR_HANDLE a_handle, uint32_t a_count, uint32_t a_step)
	{
		a_handle.ptr += uint64_t(a_count) * a_step;
		return a_handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE Offset(D3D12_GPU_DESCRIPTOR_HANDLE a_handle, uint32_t a_count, uint32_t a_step)
	{
		a_handle.ptr += uint64_t(a_count) * a_step;
		return a_handle;
	}

	uint64_t ResourceBytes(ID3D12Device* a_device, const D3D12_RESOURCE_DESC& a_desc)
	{
		const auto bytes = a_device->GetResourceAllocationInfo(0, 1, &a_desc).SizeInBytes;
		if (bytes == UINT64_MAX)
			throw std::runtime_error("NativePBR material resource allocation size unavailable");
		return bytes;
	}
}

bool NativePBRSurfacePass::Initialize(ID3D12Device* a_device)
{
	if (root && pipeline && device == a_device)
		return true;
	status = "NativePBRSurfaceEval.dxil missing from Data/Shaders/NativeComposite";
	if (!a_device)
		return false;
	try {
		std::ifstream file("Data/Shaders/NativeComposite/NativePBRSurfaceEval.dxil", std::ios::binary | std::ios::ate);
		if (!file)
			return false;
		const auto bytes = file.tellg();
		if (bytes <= 0 || bytes > 16 * 1024 * 1024)
			throw std::runtime_error("NativePBRSurfaceEval.dxil size unsupported");
		std::vector<char> shader(static_cast<size_t>(bytes));
		file.seekg(0);
		if (!file.read(shader.data(), bytes))
			throw std::runtime_error("NativePBRSurfaceEval.dxil read incomplete");
		CD3DX12_DESCRIPTOR_RANGE ranges[3];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kFrameDescriptors, 0, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaterialPages, 0, 1);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kSurfaceOutputs, 0, 0);
		CD3DX12_ROOT_PARAMETER parameters[4];
		for (uint32_t index = 0; index < 3; ++index)
			parameters[index].InitAsDescriptorTable(1, &ranges[index]);
		parameters[3].InitAsConstants(5, 0);
		std::array<D3D12_STATIC_SAMPLER_DESC, 4> samplers{};
		for (uint32_t index = 0; index < samplers.size(); ++index) {
			auto& sampler = samplers[index];
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = index < 2 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressV = index % 2 == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxAnisotropy = 1;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = index;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		}
		const CD3DX12_ROOT_SIGNATURE_DESC signature(4, parameters, uint32_t(samplers.size()), samplers.data());
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
			serialized.put(), errors.put()));
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		DX::ThrowIfFailed(a_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = newRoot.get();
		desc.CS = { shader.data(), shader.size() };
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(a_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(newPipeline.put())));
		root = std::move(newRoot);
		pipeline = std::move(newPipeline);
		device = a_device;
		status = "NativePBR material PSO ready; awaiting synchronized surface inputs";
		return true;
	} catch (const std::exception& error) {
		status = error.what();
		return false;
	}
}

bool NativePBRSurfacePass::Record(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_commands, Slot& a_slot,
	std::shared_ptr<const InputFrame> a_input, std::shared_ptr<const NativePBR::GpuMaterialResources> a_materials,
	std::shared_ptr<const NativePBR::MaterialPublication> a_publication,
	uint32_t a_frame, uint32_t a_width, uint32_t a_height, uint64_t a_additionalBudget)
{
	CS_GPU_PASS("NativePBR::MaterialEvaluation");
	status = "Waiting for synchronized NativePBR screen inputs";
	lastDraws = 0;
	lastEligibleDraws = 0;
	lastEligibility = {};
	if (!a_input || !a_materials || !a_publication || !a_publication->recipes || !a_commands || !a_device)
		return false;
	if (a_input->abiVersion != kProvisionalSurfaceAbiVersion || !a_input->nativeOwnedDraws ||
		a_input->frame != a_frame || a_input->width < a_width || a_input->height < a_height ||
		!a_input->readyFence || !a_input->readyValue || a_input->readyValue == UINT64_MAX || !a_input->retainThrough ||
		!a_input->draws || !NativePBR::ValidSurfaceDrawTable(*a_input->draws)) {
		status = "NativePBR opaque surface ABI, draw ownership, or producer retention unconfirmed";
		return false;
	}
	if (a_materials->device.get() != a_device || !a_materials->records || !a_materials->textureDescriptors ||
		!a_materials->residency || !a_materials->generation || !a_materials->generation->batchesComplete) {
		status = "NativePBR GPU material generation or descriptors unavailable";
		return false;
	}
	winrt::com_ptr<ID3D12Device> producerDevice;
	if (FAILED(a_input->readyFence->GetDevice(IID_PPV_ARGS(producerDevice.put()))) || producerDevice.get() != a_device) {
		status = "NativePBR screen producer fence belongs to another device";
		return false;
	}
	if (a_input->readyFence->GetCompletedValue() == UINT64_MAX) {
		status = "NativePBR screen producer fence reports device removal";
		return false;
	}
	for (uint32_t index = 0; index < kSurfaceInputs; ++index) {
		if (index >= NativePBR::kSurfaceTransportBaseAttachments && !a_input->projectedAvailable && !a_input->textures[index])
			continue;
		const auto& resource = a_input->textures[index];
		if (!resource || resource->GetDesc().Format != kInputFormats[index] ||
			resource->GetDesc().Width < a_width || resource->GetDesc().Height < a_height) {
			status = "NativePBR surface attachment format or dimensions mismatch";
			return false;
		}
		winrt::com_ptr<ID3D12Device> owner;
		if (FAILED(resource->GetDevice(IID_PPV_ARGS(owner.put()))) || owner.get() != a_device) {
			status = "NativePBR surface attachment belongs to another device";
			return false;
		}
	}
	for (const auto& page : a_materials->residency->pools)
		if (page.readyFence && page.readyFence->GetCompletedValue() == UINT64_MAX) {
			status = "NativePBR DDS upload fence reports device removal";
			return false;
		}
	if (!Initialize(a_device))
		return false;
	try {
		if (a_input->draws->size() > NativePBR::kMaximumFrameDraws)
			throw std::runtime_error("NativePBR frame-local draw table exceeds supported capacity");
		const uint32_t drawCount = uint32_t(a_input->draws->size());
		lastDraws = drawCount;
		const uint32_t materialCount = uint32_t(a_materials->entries.size());
		if (uint64_t(materialCount) * sizeof(NativePBR::GpuMaterialRecord) > a_materials->records->GetDesc().Width) {
			status = "NativePBR material record allocation is smaller than its immutable generation";
			return false;
		}
		std::vector<NativePBR::GpuDrawBinding> drawBindings(drawCount);
		uint32_t eligibleDraws = 0;
		for (uint32_t index = 0; index < drawCount; ++index) {
			const auto& draw = (*a_input->draws)[index];
			if (draw.projectedUv)
				++lastEligibility.projected;
			else
				++lastEligibility.opaque;
			const auto identity = a_materials->Find(draw.materialIdentity);
			const auto candidate = a_materials->Find(draw.materialIdentity, draw.materialRevision);
			const auto exact = candidate && candidate->revision == draw.materialRevision && draw.materialRevision ? candidate : std::nullopt;
			const auto instance = a_publication->FindInstance(draw.referenceKey, draw.shapeName);
			const bool matchedInstance = instance && instance.recipe && instance.recipe->identity == draw.materialIdentity &&
				instance.recipe->revision == draw.materialRevision && instance.instanceIssues == 0;
			if (!draw.projectedUv) {
				if (!identity)
					++lastEligibility.missingMaterial;
				else if (!exact)
					++lastEligibility.revisionMismatch;
				if (instance.status == NativePBR::MaterialLookupStatus::MissingInstance)
					++lastEligibility.missingInstance;
				else if (instance.status == NativePBR::MaterialLookupStatus::AmbiguousInstance)
					++lastEligibility.ambiguousInstance;
				else if (!instance || !instance.recipe || instance.recipe->identity != draw.materialIdentity ||
					instance.recipe->revision != draw.materialRevision)
					++lastEligibility.mismatchedInstance;
				if (instance && instance.instanceIssues)
					++lastEligibility.instanceIssues;
			}
			if (!identity)
				++missingMaterials;
			else if (!exact)
				++missingRevisions;
			const bool resident = exact && exact->availability == NativePBR::GpuMaterialAvailability::Resident;
			const auto readiness = NativePBR::ResolveCapturedOpaqueUv(matchedInstance ? instance.Readiness() :
				exact ? NativePBR::ClassifyRecipeReadiness(exact->recipeIssues) : NativePBR::RecipeReadiness{},
				matchedInstance && !draw.projectedUv);
			const bool compatible = matchedInstance && readiness.IsOpaqueFeatureCompatible() && readiness.unresolvedInstance == 0;
			if (!draw.projectedUv) {
				lastEligibility.sourceIssues += readiness.sourceIssues != 0;
				lastEligibility.featureIssues += readiness.unsupportedFeatures != 0;
				lastEligibility.unresolvedInstance += readiness.unresolvedInstance != 0;
				lastEligibility.missingDds += exact && !resident;
			}
			if (exact) {
				if (!resident)
					++missingTextures;
				if (!readiness.IsOpaqueFeatureCompatible())
					++unsupportedRecipes;
			}
			if (draw.projectedUv || !matchedInstance || readiness.unresolvedInstance != 0)
				++unsupportedInputs;
			if (draw.ordinal != index + 1)
				++ordinalMismatches;
			const bool eligible = resident && compatible && draw.ordinal == index + 1 && !draw.projectedUv;
			if (eligible)
				++eligibleDraws;
			drawBindings[index] = NativePBR::PackGpuDrawBinding(draw.materialIdentity, draw.materialRevision,
				eligible ? exact->index : UINT32_MAX,
				NativePBR::kGpuDrawUsesVertexColor | (draw.projectedUv ? NativePBR::kGpuDrawProjected : 0u));
		}
		lastEligibleDraws = eligibleDraws;
		if (!eligibleDraws) {
			++noEligibleFrames;
			status = "NativePBR surface frame has no eligible opaque draw bindings";
			return false;
		}
		const bool recreate = !a_slot.descriptors || a_slot.width != a_width || a_slot.height != a_height || a_slot.drawCapacity < drawCount;
		if (recreate) {
			uint64_t bytes = kDescriptorReserveBytes;
			for (const auto format : kOutputFormats) {
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, a_width, a_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				bytes += ResourceBytes(a_device, desc);
			}
			const auto drawDesc = CD3DX12_RESOURCE_DESC::Buffer(uint64_t(drawCount) * sizeof(NativePBR::GpuDrawBinding));
			bytes += ResourceBytes(a_device, drawDesc);
			if (bytes > a_slot.bytes && bytes - a_slot.bytes > a_additionalBudget)
				throw std::runtime_error("NativePBR surface outputs deferred by DXGI memory budget");
			Slot replacement;
			replacement.bytes = bytes;
			replacement.width = a_width;
			replacement.height = a_height;
			replacement.drawCapacity = drawCount;
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				kTotalDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			DX::ThrowIfFailed(a_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(replacement.descriptors.put())));
			replacement.descriptors->SetName(L"NativePBR::SurfaceDescriptors");
			const auto gpuHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			for (size_t index = 0; index < replacement.outputs.size(); ++index) {
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(kOutputFormats[index], a_width, a_height, 1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				DX::ThrowIfFailed(a_device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(replacement.outputs[index].put())));
			}
			const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			DX::ThrowIfFailed(a_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &drawDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(replacement.drawBindings.put())));
			a_slot = std::move(replacement);
		}
		void* mapped = nullptr;
		const D3D12_RANGE noRead{};
		DX::ThrowIfFailed(a_slot.drawBindings->Map(0, &noRead, &mapped));
		std::memcpy(mapped, drawBindings.data(), drawBindings.size() * sizeof(drawBindings.front()));
		a_slot.drawBindings->Unmap(0, nullptr);
		const uint32_t step = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpu = a_slot.descriptors->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		for (uint32_t index = 0; index < kSurfaceInputs; ++index) {
			srv.Format = kInputFormats[index];
			a_device->CreateShaderResourceView(a_input->textures[index].get(), &srv, cpu);
			cpu = Offset(cpu, 1, step);
		}
		srv = {};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = drawCount;
		srv.Buffer.StructureByteStride = sizeof(NativePBR::GpuDrawBinding);
		a_device->CreateShaderResourceView(a_slot.drawBindings.get(), &srv, cpu);
		cpu = Offset(cpu, 1, step);
		srv.Buffer.NumElements = std::max(1u, materialCount);
		srv.Buffer.StructureByteStride = sizeof(NativePBR::GpuMaterialRecord);
		a_device->CreateShaderResourceView(a_materials->records.get(), &srv, cpu);
		cpu = Offset(cpu, 1, step);
		a_device->CopyDescriptorsSimple(kMaterialPages, cpu,
			a_materials->textureDescriptors->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		cpu = Offset(cpu, kMaterialPages, step);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		for (size_t index = 0; index < a_slot.outputs.size(); ++index) {
			uav.Format = kOutputFormats[index];
			a_device->CreateUnorderedAccessView(a_slot.outputs[index].get(), nullptr, &uav, cpu);
			cpu = Offset(cpu, 1, step);
		}
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(kSurfaceInputs + kSurfaceOutputs);
		for (uint32_t index = 0; index < kSurfaceInputs; ++index) {
			const auto& source = a_input->textures[index];
			if (source && (a_input->states[index] & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
				barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(source.get(), a_input->states[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
		for (const auto& output : a_slot.outputs)
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(output.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		a_commands->ResourceBarrier(uint32_t(barriers.size()), barriers.data());
		ID3D12DescriptorHeap* heap[]{ a_slot.descriptors.get() };
		a_commands->SetDescriptorHeaps(1, heap);
		a_slot.root = root;
		a_slot.pipeline = pipeline;
		a_commands->SetComputeRootSignature(a_slot.root.get());
		a_commands->SetPipelineState(a_slot.pipeline.get());
		auto gpu = a_slot.descriptors->GetGPUDescriptorHandleForHeapStart();
		a_commands->SetComputeRootDescriptorTable(0, gpu);
		a_commands->SetComputeRootDescriptorTable(1, Offset(gpu, kFrameDescriptors, step));
		a_commands->SetComputeRootDescriptorTable(2, Offset(gpu, kFrameDescriptors + kMaterialPages, step));
		const std::array<uint32_t, 5> constants{ a_width, a_height, drawCount, materialCount, kMaterialPages };
		a_commands->SetComputeRoot32BitConstants(3, 5, constants.data(), 0);
		a_commands->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		a_commands->ResourceBarrier(uint32_t(barriers.size()), barriers.data());
		a_slot.input = std::move(a_input);
		a_slot.materials = std::move(a_materials);
		status = "NativePBR SM6.5 material dispatch recorded; awaiting queue submission";
		return true;
	} catch (const std::exception& error) {
		status = error.what();
		return false;
	}
}


bool NativePBRSurfacePass::InitializeFinal(ID3D12Device* a_device)
{
	if (finalRoot && finalPipeline && finalDevice == a_device)
		return true;
	finalStatus = "NativePBRFinalComposite.dxil unavailable";
	if (!a_device)
		return false;
	try {
		std::ifstream file("Data/Shaders/NativeComposite/NativePBRFinalComposite.dxil", std::ios::binary | std::ios::ate);
		if (!file)
			return false;
		const auto bytes = file.tellg();
		if (bytes <= 0 || bytes > 16 * 1024 * 1024)
			throw std::runtime_error("NativePBR final shader size invalid");
		std::vector<char> shader(static_cast<size_t>(bytes));
		file.seekg(0);
		if (!file.read(shader.data(), bytes))
			throw std::runtime_error("NativePBR final shader read incomplete");
		CD3DX12_DESCRIPTOR_RANGE ranges[2];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER parameters[5];
		parameters[0].InitAsDescriptorTable(1, &ranges[0]);
		parameters[1].InitAsDescriptorTable(1, &ranges[1]);
		parameters[2].InitAsShaderResourceView(7);
		parameters[3].InitAsShaderResourceView(8);
		parameters[4].InitAsConstants(52, 0);
		const CD3DX12_ROOT_SIGNATURE_DESC signature(5, parameters);
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
			serialized.put(), errors.put()));
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		DX::ThrowIfFailed(a_device->CreateRootSignature(0, serialized->GetBufferPointer(),
			serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = newRoot.get();
		desc.CS = { shader.data(), shader.size() };
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(a_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(newPipeline.put())));
		finalRoot = std::move(newRoot);
		finalPipeline = std::move(newPipeline);
		finalDevice = a_device;
		finalStatus = "NativePBR material-aware final compositor ready";
		return true;
	} catch (const std::exception& error) {
		finalStatus = error.what();
		return false;
	}
}

bool NativePBRSurfacePass::RecordFinalColor(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_commands,
	Slot& a_slot, ID3D12Resource* a_sceneColor, DXGI_FORMAT a_sceneFormat,
	const NativeLightingFeature::Contribution& a_lighting, uint32_t a_frame,
	uint32_t a_width, uint32_t a_height, float a_compositeStrength, bool a_legacyTransfer)
{
	CS_GPU_PASS("NativePBR::FinalMaterialComposite");
	finalStatus = "NativePBR final color awaits exact material and light frames";
	if (!a_device || !a_commands || !a_sceneColor || !a_slot.input || !a_slot.materials) {
		finalStatus = "NativePBR final output or evaluated material slot missing";
		return false;
	}
	if (a_slot.input->frame != a_frame || a_slot.width != a_width || a_slot.height != a_height ||
		a_lighting.frame != a_frame || a_lighting.width != a_width || a_lighting.height != a_height) {
		finalStatus = "NativePBR surface and native-light frames or dimensions differ";
		return false;
	}
	if (!a_lighting.readyFence || !a_lighting.readyValue || a_lighting.readyValue == UINT64_MAX ||
		!a_lighting.lights || !a_lighting.nativePointLightCount ||
		a_lighting.nativePointLightCount > DX12Native::kMaximumNativePointLights || !a_lighting.textures[7]) {
		finalStatus = "NativePBR raw DX12 lights or native validity texture unavailable";
		return false;
	}
	if (!a_lighting.scene.prepared || !a_lighting.scene.prepared->acceleration ||
		!a_lighting.scene.prepared->acceleration->result) {
		finalStatus = "NativePBR final direct lighting awaits native scene acceleration";
		return false;
	}
	if (!a_lighting.scene.raster || !a_lighting.scene.raster->cameraValid ||
		!a_slot.input->textures[3] || !std::isfinite(a_compositeStrength) || a_compositeStrength < 0.0f) {
		finalStatus = "NativePBR current camera, depth, or composite strength invalid";
		return false;
	}
	if (a_sceneColor->GetDesc().Format != a_sceneFormat ||
		a_sceneColor->GetDesc().Width < a_width || a_sceneColor->GetDesc().Height < a_height) {
		finalStatus = "Final scene color dimensions or format mismatch";
		return false;
	}
	if (!InitializeFinal(a_device))
		return false;
	try {
		if (!a_slot.finalDescriptors) {
			const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			DX::ThrowIfFailed(a_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(a_slot.finalDescriptors.put())));
		}
		const std::array<ID3D12Resource*, 7> sources{
			a_slot.outputs[0].get(), a_slot.outputs[1].get(),
			a_slot.outputs[2].get(), a_slot.outputs[3].get(),
			a_slot.outputs[4].get(), a_slot.input->textures[3].get(),
			a_lighting.textures[7].get()
		};
		const std::array<DXGI_FORMAT, 7> formats{
			DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_UINT
		};
		for (size_t index = 0; index < sources.size(); ++index)
			if (!sources[index] || sources[index]->GetDesc().Format != formats[index]) {
				finalStatus = "NativePBR final source format invalid";
				return false;
			}
		const uint32_t step = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpu = a_slot.finalDescriptors->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		for (size_t index = 0; index < sources.size(); ++index) {
			srv.Format = formats[index];
			a_device->CreateShaderResourceView(sources[index], &srv, cpu);
			cpu = Offset(cpu, 1, step);
		}
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = a_sceneFormat;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		a_device->CreateUnorderedAccessView(a_sceneColor, nullptr, &uav, cpu);
		const auto& camera = a_lighting.scene.raster->camera;
		struct Constants
		{
			uint32_t width, height, lightCount, lobes;
			float3 origin;
			float minimumDistance;
			DirectX::SimpleMath::Matrix inverseProjection, inverseView;
			float4 lightTuning, shadowTuning;
			float diffuseGain, specularGain, compositeStrength;
			uint32_t legacyTransfer;
		};
		static_assert(sizeof(Constants) == 52 * sizeof(uint32_t));
		const Constants constants{
			a_width, a_height, a_lighting.nativePointLightCount, a_lighting.enabledLobes,
			{ camera.CameraPosAdjust.x + camera.CameraViewInverse._14,
				camera.CameraPosAdjust.y + camera.CameraViewInverse._24,
				camera.CameraPosAdjust.z + camera.CameraViewInverse._34 },
			a_lighting.nativeMinimumDistance,
			camera.CameraProjInverse, camera.CameraViewInverse,
			a_lighting.nativeLightTuning, a_lighting.nativeShadowTuning,
			a_lighting.diffuseIntensity, a_lighting.specularIntensity,
			a_compositeStrength, a_legacyTransfer ? 1u : 0u
		};
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(9);
		for (size_t index = 0; index < sources.size(); ++index) {
			const auto before = index == 5 ? a_slot.input->states[3] : D3D12_RESOURCE_STATE_COMMON;
			if ((before & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
				barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(sources[index], before,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_sceneColor,
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		a_commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		ID3D12DescriptorHeap* heaps[]{ a_slot.finalDescriptors.get() };
		a_commands->SetDescriptorHeaps(1, heaps);
		a_slot.finalRoot = finalRoot;
		a_slot.finalPipeline = finalPipeline;
		a_commands->SetComputeRootSignature(a_slot.finalRoot.get());
		a_commands->SetPipelineState(a_slot.finalPipeline.get());
		auto gpu = a_slot.finalDescriptors->GetGPUDescriptorHandleForHeapStart();
		a_commands->SetComputeRootDescriptorTable(0, gpu);
		a_commands->SetComputeRootDescriptorTable(1, Offset(gpu, 7, step));
		a_commands->SetComputeRootShaderResourceView(2, a_lighting.lights->GetGPUVirtualAddress());
		a_commands->SetComputeRootShaderResourceView(3,
			a_lighting.scene.prepared->acceleration->result->GetGPUVirtualAddress());
		a_commands->SetComputeRoot32BitConstants(4, 52, &constants, 0);
		a_commands->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		a_commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		finalStatus = "NativePBR opaque BRDF replacement recorded; awaiting GPU submission";
		return true;
	} catch (const std::exception& error) {
		finalStatus = error.what();
		return false;
	}
}

void NativePBRSurfacePass::MarkSubmitted()
{
	++submissions;
	status = "NativePBR SM6.5 dispatch submitted; final composition unchanged";
}
