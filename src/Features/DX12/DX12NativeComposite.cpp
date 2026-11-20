#include "DX12NativeComposite.h"

#include "DX12Control.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <directx/d3dx12.h>
#include <fstream>
#include <vector>

namespace
{
	struct alignas(16) MaskCamera
	{
		Matrix projectionInverse, viewInverse;
		float4 adjust;
		uint32_t width, height, count;
		float padding = 0;
	};
	struct GpuLight
	{
		float4 positionRadius;
		float4 colorFade;
	};
	static_assert(sizeof(MaskCamera) == 160);
	static_assert(sizeof(GpuLight) == 32);

	std::vector<char> ReadShader(const char* path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
			return {};
		const auto size = file.tellg();
		if (size <= 0 || size > 16 * 1024 * 1024)
			return {};
		std::vector<char> bytes(size_t(size), '\0');
		file.seekg(0);
		return file.read(bytes.data(), std::streamsize(bytes.size())) ? bytes : std::vector<char>{};
	}

	bool CreateSignature(ID3D12Device* device, std::span<const D3D12_ROOT_PARAMETER> parameters,
		winrt::com_ptr<ID3D12RootSignature>& output)
	{
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = UINT(parameters.size());
		desc.pParameters = parameters.data();
		winrt::com_ptr<ID3DBlob> serialized, errors;
		return SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put())) &&
		       SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(output.put())));
	}

	bool CreatePipeline(ID3D12Device* device, ID3D12RootSignature* root, const char* filename,
		winrt::com_ptr<ID3D12PipelineState>& output)
	{
		const auto bytes = ReadShader(filename);
		if (bytes.empty())
			return false;
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
		pipeline.pRootSignature = root;
		pipeline.CS = { bytes.data(), bytes.size() };
		return SUCCEEDED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(output.put())));
	}
}

DX12NativeComposite::~DX12NativeComposite()
{
	Collect();
	for (auto& frame : pending)
		if (frame.fence && frame.fence->GetCompletedValue() < frame.value)
			(void)new Pending(std::move(frame));
}

bool DX12NativeComposite::IsActive() const
{
	return loaded && settings.Enabled && !globals::game::isVR &&
	       globals::features::dx12Control.IsActive() && globals::features::dx12NativeScene.IsActive();
}

void DX12NativeComposite::DrawSettings()
{
	if (ImGui::Checkbox("Enable native shadow composite", &settings.Enabled) && !settings.Enabled)
		OnRuntimeDisabled();
	ImGui::SliderFloat("Shadow-mask strength", &settings.ShadowStrength, 0.0f, 1.0f, "%.2f");
}

void DX12NativeComposite::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", true);
	const float strength = input.value("ShadowStrength", 1.0f);
	settings.ShadowStrength = std::clamp(std::isfinite(strength) ? strength : 1.0f, 0.0f, 1.0f);
}

void DX12NativeComposite::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "ShadowStrength", settings.ShadowStrength } };
}

void DX12NativeComposite::RestoreDefaultSettings()
{
	settings = {};
	OnRuntimeDisabled();
}

void DX12NativeComposite::EarlyPrepass()
{
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0;
	const auto scene = globals::features::dx12NativeScene.GetScene();
	const auto grid = globals::features::dx12Resources.GetGrid();
	const bool active = IsActive() && settings.ShadowStrength > 0 && frame != 0 && lastSubmitFrame + 1 == frame && scene && grid &&
	                    scene->complete && scene->scope == grid->requested.front().scope &&
	                    scene->geometryRevision == submittedGeometry && grid->geometryRevision == submittedGeometry &&
	                    grid->lightRevision == submittedLights &&
	                    globals::features::upscaling.fidelityFX.ShouldUseRuntimeUpscalerForFSR();
	ShadowCasterManager::SetNativeStaticMaskActive(active);
}

void DX12NativeComposite::OnRuntimeDisabled()
{
	ShadowCasterManager::SetNativeStaticMaskActive(false);
	lastSubmitFrame = 0;
}

void DX12NativeComposite::OnSceneTransitionReset(bool opening)
{
	if (opening)
		OnRuntimeDisabled();
}

bool DX12NativeComposite::EnsurePipelines(ID3D12Device* candidate)
{
	if (!candidate || unavailable || (device && device.get() != candidate))
		return false;
	if (shadowPipeline && compositePipeline)
		return true;
	device.copy_from(candidate);
	D3D12_DESCRIPTOR_RANGE depthRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
	D3D12_DESCRIPTOR_RANGE maskRange{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
	std::array<D3D12_ROOT_PARAMETER, 5> shadow{};
	shadow[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	shadow[0].Descriptor.ShaderRegister = 0;
	shadow[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	shadow[1].DescriptorTable = { 1, &depthRange };
	shadow[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	shadow[2].Descriptor.ShaderRegister = 2;
	shadow[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	shadow[3].DescriptorTable = { 1, &maskRange };
	shadow[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	shadow[4].Descriptor.ShaderRegister = 0;
	D3D12_DESCRIPTOR_RANGE colorRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	D3D12_DESCRIPTOR_RANGE compositeMaskRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
	D3D12_DESCRIPTOR_RANGE outputRange{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
	std::array<D3D12_ROOT_PARAMETER, 4> composite{};
	for (uint32_t i = 0; i < 3; ++i)
		composite[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	composite[0].DescriptorTable = { 1, &colorRange };
	composite[1].DescriptorTable = { 1, &compositeMaskRange };
	composite[2].DescriptorTable = { 1, &outputRange };
	composite[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	composite[3].Constants = { 0, 0, 4 };
	if (!CreateSignature(device.get(), shadow, shadowRoot) || !CreateSignature(device.get(), composite, compositeRoot) ||
		!CreatePipeline(device.get(), shadowRoot.get(), "Data/Shaders/DX12NativeComposite/PreparedShadowMask.dxil", shadowPipeline) ||
		!CreatePipeline(device.get(), compositeRoot.get(), "Data/Shaders/DX12NativeComposite/RasterShadowComposite.dxil", compositePipeline)) {
		unavailable = true;
		return false;
	}
	descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return descriptorSize != 0;
}

std::shared_ptr<DX12NativeComposite::Surfaces> DX12NativeComposite::MakeSurfaces(
	ID3D12Resource* rasterColor, ID3D12Resource* depth, uint32_t width, uint32_t height)
{
	const auto raster = rasterColor->GetDesc();
	const auto z = depth->GetDesc();
	const DXGI_FORMAT depthSrv = z.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : z.Format;
	if (!width || !height || raster.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		z.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || width > raster.Width || height > raster.Height ||
		width > z.Width || height > z.Height ||
		depthSrv != DXGI_FORMAT_R32_FLOAT || raster.Format == DXGI_FORMAT_UNKNOWN)
		return nullptr;
	if (surfaces && surfaces->rasterSource.get() == rasterColor && surfaces->depthSource.get() == depth &&
		surfaces->width == width && surfaces->height == height &&
		surfaces->rasterFormat == raster.Format && surfaces->depthFormat == z.Format)
		return surfaces;
	const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	const auto maskDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, width, height, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	const auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	std::shared_ptr<Surfaces> next = std::make_shared<Surfaces>();
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &maskDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(next->shadowMask.put()))) ||
		FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &colorDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(next->color.put()))))
		return nullptr;
	next->rasterSource.copy_from(rasterColor);
	next->depthSource.copy_from(depth);
	next->shadowMask->SetName(L"DX12NativeComposite::ShadowMask");
	next->color->SetName(L"DX12NativeComposite::RasterComposite");
	D3D12_DESCRIPTOR_HEAP_DESC descriptorDesc{};
	descriptorDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	descriptorDesc.NumDescriptors = 5;
	descriptorDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&descriptorDesc, IID_PPV_ARGS(next->descriptors.put()))))
		return {};
	const auto base = next->descriptors->GetCPUDescriptorHandleForHeapStart();
	const auto cpu = [&](uint32_t index) { return CD3DX12_CPU_DESCRIPTOR_HANDLE(base, int(index), descriptorSize); };
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	srv.Format = depthSrv;
	device->CreateShaderResourceView(depth, &srv, cpu(0));
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uav.Format = DXGI_FORMAT_R16_FLOAT;
	device->CreateUnorderedAccessView(next->shadowMask.get(), nullptr, &uav, cpu(1));
	srv.Format = raster.Format;
	device->CreateShaderResourceView(rasterColor, &srv, cpu(2));
	srv.Format = DXGI_FORMAT_R16_FLOAT;
	device->CreateShaderResourceView(next->shadowMask.get(), &srv, cpu(3));
	uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	device->CreateUnorderedAccessView(next->color.get(), nullptr, &uav, cpu(4));
	next->width = width;
	next->height = height;
	next->rasterFormat = raster.Format;
	next->depthFormat = z.Format;
	surfaces = next;
	return next;
}

winrt::com_ptr<ID3D12Resource> DX12NativeComposite::Upload(const void* source, size_t bytes)
{
	winrt::com_ptr<ID3D12Resource> buffer;
	if (!source || !bytes || bytes > 64 * 1024)
		return buffer;
	const auto desc = CD3DX12_RESOURCE_DESC::Buffer((bytes + 255) & ~size_t(255));
	const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.put()))))
		return {};
	void* mapped = nullptr;
	if (FAILED(buffer->Map(0, nullptr, &mapped)) || !mapped)
		return {};
	std::memcpy(mapped, source, bytes);
	buffer->Unmap(0, nullptr);
	return buffer;
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12NativeComposite::Descriptor(const Surfaces& target, uint32_t index) const
{
	return CD3DX12_GPU_DESCRIPTOR_HANDLE(target.descriptors->GetGPUDescriptorHandleForHeapStart(), int(index), descriptorSize);
}

void DX12NativeComposite::Collect()
{
	while (!pending.empty() && pending.front().fence &&
		   pending.front().fence->GetCompletedValue() >= pending.front().value)
		pending.pop_front();
}

std::optional<DX12NativeComposite::Submission> DX12NativeComposite::Dispatch(
	ID3D12GraphicsCommandList4* commands, ID3D12Resource* rasterColor, ID3D12Resource* depth,
	uint32_t width, uint32_t height)
{
	CS_GPU_PASS("DX12NativeComposite::Dispatch");
	if (!IsActive() || !commands || !rasterColor || !depth || !settings.ShadowStrength ||
		!EnsurePipelines(globals::features::upscaling.dx12SwapChain.d3d12Device.get()))
		return std::nullopt;
	const auto scene = globals::features::dx12NativeScene.GetScene();
	const auto lightSnapshot = globals::features::dx12NativeScene.GetLights();
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!scene || !scene->complete || !scene->acceleration || !lightSnapshot || !grid ||
		scene->geometryRevision != grid->geometryRevision || scene->scope != grid->requested.front().scope)
		return std::nullopt;
	std::array<GpuLight, DX12::kMaximumShadowLights> lightUpload{};
	uint32_t count = 0;
	for (uint32_t index = 0; index < lightSnapshot->count; ++index) {
		const auto& resolved = lightSnapshot->values[index];
		if (!resolved.matched || resolved.runtimeShadowSlot == UINT32_MAX || resolved.light.fade <= 0 ||
			(resolved.light.color.x <= 0 && resolved.light.color.y <= 0 && resolved.light.color.z <= 0))
			continue;
		const auto& light = resolved.light;
		lightUpload[count++] = { { light.position.x, light.position.y, light.position.z, light.radius },
			{ light.color.x, light.color.y, light.color.z, light.fade } };
	}
	if (!count)
		return std::nullopt;
	Collect();
	const auto target = MakeSurfaces(rasterColor, depth, width, height);
	if (!target)
		return std::nullopt;
	MaskCamera camera{ globals::game::frameBufferCached.GetCameraProjInverse(),
		globals::game::frameBufferCached.GetCameraViewInverse(),
		globals::game::frameBufferCached.GetCameraPosAdjust(), width, height, count };
	Submission submission;
	submission.scene = scene;
	submission.surfaces = target;
	submission.geometryRevision = scene->geometryRevision;
	submission.lightRevision = grid->lightRevision;
	submission.camera = Upload(&camera, sizeof(camera));
	submission.lights = Upload(lightUpload.data(), count * sizeof(GpuLight));
	if (!submission.camera || !submission.lights)
		return std::nullopt;
	ID3D12DescriptorHeap* heaps[]{ target->descriptors.get() };
	commands->SetDescriptorHeaps(1, heaps);
	std::array<D3D12_RESOURCE_BARRIER, 2> begin{
		CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		CD3DX12_RESOURCE_BARRIER::Transition(target->color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	};
	commands->ResourceBarrier(UINT(begin.size()), begin.data());
	commands->SetComputeRootSignature(shadowRoot.get());
	commands->SetPipelineState(shadowPipeline.get());
	commands->SetComputeRootShaderResourceView(0, scene->acceleration->GetGPUVirtualAddress());
	commands->SetComputeRootDescriptorTable(1, Descriptor(*target, 0));
	commands->SetComputeRootShaderResourceView(2, submission.lights->GetGPUVirtualAddress());
	commands->SetComputeRootDescriptorTable(3, Descriptor(*target, 1));
	commands->SetComputeRootConstantBufferView(4, submission.camera->GetGPUVirtualAddress());
	commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	const auto shadowDone = CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	commands->ResourceBarrier(1, &shadowDone);
	commands->SetComputeRootSignature(compositeRoot.get());
	commands->SetPipelineState(compositePipeline.get());
	commands->SetComputeRootDescriptorTable(0, Descriptor(*target, 2));
	commands->SetComputeRootDescriptorTable(1, Descriptor(*target, 3));
	commands->SetComputeRootDescriptorTable(2, Descriptor(*target, 4));
	const std::array<uint32_t, 4> compositeConstants{ width, height, std::bit_cast<uint32_t>(settings.ShadowStrength), 0 };
	commands->SetComputeRoot32BitConstants(3, UINT(compositeConstants.size()), compositeConstants.data(), 0);
	commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	std::array<D3D12_RESOURCE_BARRIER, 2> compositeDone{
		CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		CD3DX12_RESOURCE_BARRIER::Transition(target->color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
	};
	commands->ResourceBarrier(UINT(compositeDone.size()), compositeDone.data());
	return submission;
}

void DX12NativeComposite::OnSubmitted(ID3D12Fence* fence, uint64_t value, Submission submission, bool applied)
{
	if (!fence || !value)
		return;
	if (applied) {
		lastSubmitFrame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0;
		submittedGeometry = submission.geometryRevision;
		submittedLights = submission.lightRevision;
	}
	Pending frame;
	frame.fence.copy_from(fence);
	frame.value = value;
	frame.submission = std::move(submission);
	pending.push_back(std::move(frame));
	Collect();
}
