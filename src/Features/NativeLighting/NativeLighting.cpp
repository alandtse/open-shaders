#include "NativeLighting.h"
#include "NativeLightingRoot.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Features/ReverseZ.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include <directx/d3dx12.h>
#include <fstream>
#include <cmath>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeLightingFeature::Settings, Enabled, Diffuse, Specular, EarlyShadowCull, Debug, CompareCulling, DiffuseIntensity, SpecularIntensity, BudgetMB)

void NativeLightingFeature::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	ImGui::Checkbox("Enable native lighting", &settings.Enabled);
	ImGui::Checkbox("Diffuse", &settings.Diffuse);
	ImGui::Checkbox("Specular", &settings.Specular);
	ImGui::SliderFloat("Diffuse intensity", &settings.DiffuseIntensity, 0.0f, 4.0f);
	ImGui::SliderFloat("Specular intensity", &settings.SpecularIntensity, 0.0f, 4.0f);
	ImGui::Checkbox("Early shadow-mask culling", &settings.EarlyShadowCull);
	ImGui::Checkbox("Publish validity debug texture", &settings.Debug);
	ImGui::Checkbox("Compare culling on and off", &settings.CompareCulling);
}

void NativeLightingFeature::LoadSettings(json& a_json)
{
	auto selected = a_json.get<Settings>();
	selected.DiffuseIntensity = std::isfinite(selected.DiffuseIntensity) ? std::clamp(selected.DiffuseIntensity, 0.0f, 4.0f) : 1.0f;
	selected.SpecularIntensity = std::isfinite(selected.SpecularIntensity) ? std::clamp(selected.SpecularIntensity, 0.0f, 4.0f) : 1.0f;
	selected.BudgetMB = std::clamp(selected.BudgetMB, 64u, 4096u);
	std::scoped_lock lock(settingsMutex);
	settings = selected;
}
void NativeLightingFeature::SaveSettings(json& a_json) { std::scoped_lock lock(settingsMutex); a_json = settings; }
void NativeLightingFeature::RestoreDefaultSettings() { std::scoped_lock lock(settingsMutex); settings = {}; }
void NativeLightingFeature::OnSceneTransitionReset(bool a_open) { transitionOpen = a_open; resetRequested = true; }

NativeLightingFeature::~NativeLightingFeature()
{
	current.reset();
	const auto done = completion ? completion->GetCompletedValue() : 0;
	for (auto& frame : frames) {
		if (!frame) continue;
		const bool consumerPending = std::ranges::any_of(frame->consumers, [](const auto& item) {
			const auto value = item.first->GetCompletedValue();
			return value == UINT64_MAX || value < item.second;
		});
		if (done == UINT64_MAX || frame->readyValue == UINT64_MAX || frame->readyValue > done || consumerPending)
			(void)new std::shared_ptr<Contribution>(std::move(frame));
	}
}

void NativeLightingFeature::Reset()
{
	current.reset();
	active = false;
	status.clear();
	failed = false;
}

void NativeLightingFeature::Retire()
{
	if (!completion) return;
	const auto done = completion->GetCompletedValue();
	if (done == UINT64_MAX) { status = "Native lighting device removed"; failed = true; return; }
	for (auto& frame : frames) {
		if (!frame || frame->countersRead || !frame->readyValue || frame->readyValue == UINT64_MAX || frame->readyValue > done) continue;
		void* mapped = nullptr;
		D3D12_RANGE read{ 0, sizeof(uint32_t) * totals.size() };
		if (FAILED(frame->counterReadback->Map(0, &read, &mapped))) { ++counterReadFailures; status = "Native lighting counter readback unavailable"; continue; }
		const auto* counts = static_cast<const uint32_t*>(mapped);
		for (size_t i = 0; i < totals.size(); ++i) totals[i] += counts[i];
		D3D12_RANGE noWrite{ 0, 0 };
		frame->counterReadback->Unmap(0, &noWrite);
		frame->countersRead = true;
		frame->scene = {};
		++completions;
	}
}

std::shared_ptr<const NativeLightingFeature::Contribution> NativeLightingFeature::GetContribution(uint32_t a_frame) const
{
	return loaded && active && !transitionOpen && current && current->frame == a_frame ? current : nullptr;
}
bool NativeLightingFeature::RetainContribution(const std::shared_ptr<const Contribution>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
{
	if (!a_frame || !a_fence || !a_value || a_value == UINT64_MAX) return false;
	for (const auto& frame : frames) {
		if (frame.get() != a_frame.get()) continue;
		winrt::com_ptr<ID3D12Fence> retained;
		retained.copy_from(a_fence);
		frame->consumers.emplace_back(std::move(retained), a_value);
		return true;
	}
	return false;
}

void NativeLightingFeature::PublishDiagnostics()
{
	json snapshot{ { "status", status }, { "submissions", submissions }, { "gpuCompletions", completions }, { "counterReadFailures", counterReadFailures },
		{ "candidatePixels", totals[0] }, { "maskCulled", totals[1] }, { "maskUnavailable", totals[2] },
		{ "primaryQueries", totals[3] }, { "primaryHits", totals[4] }, { "primaryMisses", totals[5] },
		{ "materials", totals[6] }, { "shadowQueries", totals[7] }, { "lights", totals[8] },
		{ "validDiffuse", totals[9] }, { "unsupported", totals[10] }, { "validSpecular", totals[11] },
		{ "currentMaskAuthoritative", false }, { "comparisonFrames", comparisonFrames }, { "visualConsumptionVerified", false } };
	std::scoped_lock lock(diagnosticsMutex);
	diagnostics = std::move(snapshot);
}
json NativeLightingFeature::GetDiagnostics() { std::scoped_lock lock(diagnosticsMutex); return diagnostics; }

void NativeLightingFeature::RegisterUxActions()
{
	FEATURE_COMMAND("configure", "Set native direct lighting; optional enabled, diffuse, specular, earlyShadowCull, debug, compareCulling, diffuseIntensity, specularIntensity, budgetMB.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeLightingFeature*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		if (args.contains("enabled")) feature.settings.Enabled = args.at("enabled").get<bool>();
		if (args.contains("diffuse")) feature.settings.Diffuse = args.at("diffuse").get<bool>();
		if (args.contains("specular")) feature.settings.Specular = args.at("specular").get<bool>();
		if (args.contains("earlyShadowCull")) feature.settings.EarlyShadowCull = args.at("earlyShadowCull").get<bool>();
		if (args.contains("debug")) feature.settings.Debug = args.at("debug").get<bool>();
		if (args.contains("compareCulling")) feature.settings.CompareCulling = args.at("compareCulling").get<bool>();
		if (args.contains("diffuseIntensity")) { auto v = args.at("diffuseIntensity").get<float>(); feature.settings.DiffuseIntensity = std::isfinite(v) ? std::clamp(v, 0.0f, 4.0f) : 1.0f; }
		if (args.contains("specularIntensity")) { auto v = args.at("specularIntensity").get<float>(); feature.settings.SpecularIntensity = std::isfinite(v) ? std::clamp(v, 0.0f, 4.0f) : 1.0f; }
		if (args.contains("budgetMB")) feature.settings.BudgetMB = std::clamp(args.at("budgetMB").get<uint32_t>(), 64u, 4096u);
	});
}

bool NativeLightingFeature::Initialize(const GraphicsBackend& a_backend)
{
	if (failed) return false;
	if (completion && (backend.d3d12Device.get() != a_backend.d3d12Device.get() || backend.commandQueue.get() != a_backend.commandQueue.get())) {
		status = "Native lighting graphics backend changed"; failed = true; return false;
	}
	if (pipeline) return true;
	try {
		backend = a_backend;
		if (!backend.d3d12Device || !backend.commandQueue) throw std::runtime_error("Native lighting backend unavailable");
		const auto* shaderPath = globals::features::reverseZ.IsActive() ? "Data/Shaders/NativeLighting/DirectReverseZ.dxil" : "Data/Shaders/NativeLighting/Direct.dxil";
		std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
		if (!file) throw std::runtime_error("Native lighting shader unavailable");
		const auto size = file.tellg();
		if (size <= 0 || size > 16 * 1024 * 1024) throw std::runtime_error("Native lighting shader size invalid");
		std::vector<char> shader(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(shader.data(), size)) throw std::runtime_error("Native lighting shader read failed");
		static_assert(NativeLighting::kMaximumInstances == DX12Native::ShadowBindings::kMaximumInstances && NativeLighting::kMaximumTexturePools == DX12Native::kMaximumTexturePools);
		DX::ThrowIfFailed(NativeLighting::CreateRootSignature(backend.d3d12Device.get(), root.put()));
		root->SetName(L"NativeLighting::Root");
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root.get(); pso.CS = { shader.data(), shader.size() };
		DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline.put())));
		pipeline->SetName(L"NativeLighting::DirectPipeline");
		if (!completion) {
			DX::ThrowIfFailed(backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
			completion->SetName(L"NativeLighting::Completion");
		}
		return true;
	} catch (const std::exception& error) { status = error.what(); failed = true; return false; }
}

void NativeLightingFeature::OnOpaqueRasterReady(const OpaqueRasterInputs&)
{
	CS_GPU_PASS("NativeLighting::Direct");
	const SKSE::stl::scope_exit publish([this]() noexcept { try { PublishDiagnostics(); } catch (...) {} });
	Retire();
	if (reloadRequested.exchange(false)) { Reset(); pipeline = nullptr; root = nullptr; }
	if (resetRequested.exchange(false)) Reset();
	Settings selected;
	{ std::scoped_lock lock(settingsMutex); selected = settings; }
	if (!selected.Enabled || (!selected.Diffuse && !selected.Specular) || transitionOpen || globals::game::isVR) {
		current.reset(); active = false;
		status = "Native direct lighting disabled or unavailable";
		return;
	}
	if (!globals::state || !globals::state->inWorld || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) return;
	if (!globals::game::graphicsState) { current.reset(); active = false; status = "Current frame unavailable"; return; }
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (current && current->frame == frame) return;
	current.reset(); active = false;
	auto& owner = globals::features::dx12NativeScene;
	auto scene = owner.GetSceneFrame(frame);
	auto graphics = owner.GetGraphicsBackend();
	if (!scene || !graphics || !scene->raster || !scene->raster->cameraValid || !scene->prepared || !scene->prepared->materials) {
		status = "Waiting for current scene, raster depth, camera or material bindings";
		return;
	}
	if (scene->prepared->cell->nativeLights.empty() || scene->prepared->cell->nativeLights.size() > 32) {
		status = "Prepared point-light count outside 1..32";
		return;
	}
	if (!Initialize(*graphics)) return;
	scene->shadow.reset();
	Dispatch(std::move(*scene), selected);
}

void NativeLightingFeature::Dispatch(DX12NativeScene::SceneFrame a_scene, const Settings& a_settings)
{
	const auto completed = completion->GetCompletedValue();
	if (completed == UINT64_MAX) { status = "Native lighting device removed"; failed = true; return; }
	auto available = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) {
		if (!frame) return true;
		if (frame.use_count() != 1 || frame->readyValue == UINT64_MAX || frame->readyValue > completed || (frame->readyValue && !frame->countersRead)) return false;
		return std::ranges::all_of(frame->consumers, [](const auto& consumer) {
			const auto value = consumer.first->GetCompletedValue();
			return value != UINT64_MAX && value >= consumer.second;
		});
	});
	if (available == frames.end()) { status = "Native lighting outputs are still in use"; return; }
	try {
		const auto size = Util::ConvertToDynamic(globals::state->screenSize);
		const uint32_t width = uint32_t(size.x), height = uint32_t(size.y);
		if (!width || !height || width > a_scene.raster->width || height > a_scene.raster->height || width > 16384 || height > 16384)
			throw std::runtime_error("Native lighting dimensions unavailable");
		constexpr std::array formats{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT,
			DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
		const uint64_t budget = uint64_t(a_settings.BudgetMB) * 1024 * 1024;
		uint64_t bytes = 2 * 1024 * 1024;
		const size_t textureCount = a_settings.CompareCulling ? formats.size() : 5;
		for (size_t i = 0; i < textureCount; ++i) {
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[i], width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			const auto allocation = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			if (allocation == UINT64_MAX || allocation > budget || bytes > budget - allocation) throw std::runtime_error("Native lighting output budget exceeded");
			bytes += allocation;
		}
		for (auto it = frames.begin(); it != frames.end(); ++it)
			if (it != available && *it) {
				if ((*it)->bytes > budget || bytes > budget - (*it)->bytes) throw std::runtime_error("Native lighting retained frames exceed budget");
				bytes += (*it)->bytes;
			}
		auto& slot = *available;
		if (!slot || slot->width != width || slot->height != height || slot->comparisonEnabled != a_settings.CompareCulling) {
			slot.reset();
			auto created = std::make_shared<Contribution>();
			created->width = width; created->height = height; created->comparisonEnabled = a_settings.CompareCulling;
			created->bytes = bytes;
			for (auto it = frames.begin(); it != frames.end(); ++it) if (it != available && *it) created->bytes -= (*it)->bytes;
			const auto make = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, const wchar_t* name) {
				winrt::com_ptr<ID3D12Resource> resource;
				const auto heap = CD3DX12_HEAP_PROPERTIES(type);
				DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource.put())));
				resource->SetName(name);
				return resource;
			};
			constexpr std::array names{ L"NativeLighting::RawDiffuse", L"NativeLighting::RawSpecular", L"NativeLighting::Validity", L"NativeLighting::ReceiverGuide", L"NativeLighting::Debug", L"NativeLighting::ComparisonDiffuse", L"NativeLighting::ComparisonSpecular" };
			for (size_t i = 0; i < textureCount; ++i) {
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formats[i], width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				created->textures[i] = make(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, names[i]);
			}
			const auto counters = CD3DX12_RESOURCE_DESC::Buffer(12 * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			const auto plain = CD3DX12_RESOURCE_DESC::Buffer(12 * sizeof(uint32_t));
			created->counters = make(counters, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"NativeLighting::Counters");
			created->counterReset = make(plain, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::CounterReset");
			created->counterReadback = make(plain, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, L"NativeLighting::CounterReadback");
			created->lights = make(CD3DX12_RESOURCE_DESC::Buffer(32 * 48), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::Lights");
			created->constants = make(CD3DX12_RESOURCE_DESC::Buffer(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"NativeLighting::Constants");
			void* zeros = nullptr;
			const D3D12_RANGE noRead{ 0, 0 };
			DX::ThrowIfFailed(created->counterReset->Map(0, &noRead, &zeros));
			std::memset(zeros, 0, 12 * sizeof(uint32_t));
			created->counterReset->Unmap(0, nullptr);
			constexpr uint32_t kDescriptors = 10 + 2 * DX12Native::ShadowBindings::kMaximumInstances + DX12Native::kMaximumTexturePools;
			const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			DX::ThrowIfFailed(backend.d3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(created->heap.put())));
			created->heap->SetName(L"NativeLighting::Descriptors");
			DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(created->allocator.put())));
			slot = std::move(created);
		}
		slot->scene = std::move(a_scene);
		slot->sceneRevision = slot->scene.prepared->revision;
		slot->frame = slot->scene.frame;
		slot->enabledLobes = (a_settings.Diffuse ? 1u : 0u) | (a_settings.Specular ? 2u : 0u);
		slot->diffuseIntensity = a_settings.DiffuseIntensity; slot->specularIntensity = a_settings.SpecularIntensity;
		slot->root = root; slot->pipeline = pipeline; slot->backend = backend; slot->readyFence = completion;
		slot->consumers.clear(); slot->readyValue = 0; slot->countersRead = true;
		const auto& scene = *slot->scene.prepared;
		const auto& camera = slot->scene.raster->camera;
		struct Light { float4 positionRadius, radianceSourceRadius; std::array<uint32_t, 4> flags; };
		static_assert(sizeof(Light) == 48);
		std::array<Light, 32> lights{};
		for (size_t i = 0; i < scene.cell->nativeLights.size(); ++i) {
			const auto& light = scene.cell->nativeLights[i];
			lights[i] = { { light.position.x, light.position.y, light.position.z, light.radius },
				{ light.radiance.x, light.radiance.y, light.radiance.z, light.sourceRadius }, { light.supported ? 1u : 0u, 0, 0, 0 } };
		}
		struct Constants
		{
			uint32_t width, height, instances, lights;
			float3 origin; float minimum;
			float diffuse, specular; uint32_t lobes, maskAuthoritative;
			uint32_t maskRequested, debug, comparison, reserved;
			DirectX::SimpleMath::Matrix inverseProjection, inverseView;
		};
		static_assert(sizeof(Constants) == 192);
		const Constants constants{ width, height, uint32_t(scene.acceleration->instances.size()), uint32_t(scene.cell->nativeLights.size()),
			{ camera.CameraPosAdjust.x + camera.CameraViewInverse._14, camera.CameraPosAdjust.y + camera.CameraViewInverse._24, camera.CameraPosAdjust.z + camera.CameraViewInverse._34 },
			0.5f, a_settings.DiffuseIntensity, a_settings.SpecularIntensity, (a_settings.Diffuse ? 1u : 0u) | (a_settings.Specular ? 2u : 0u),
			0u, a_settings.EarlyShadowCull ? 1u : 0u, a_settings.Debug ? 1u : 0u, a_settings.CompareCulling ? 1u : 0u, 0u, camera.CameraProjInverse, camera.CameraViewInverse };
		const D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		DX::ThrowIfFailed(slot->lights->Map(0, &noRead, &mapped)); std::memcpy(mapped, lights.data(), sizeof(lights)); slot->lights->Unmap(0, nullptr);
		DX::ThrowIfFailed(slot->constants->Map(0, &noRead, &mapped)); std::memcpy(mapped, &constants, sizeof(constants)); slot->constants->Unmap(0, nullptr);
		const auto step = backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		constexpr uint32_t kGeometry = 10;
		constexpr uint32_t kAttributes = kGeometry + DX12Native::ShadowBindings::kMaximumInstances;
		constexpr uint32_t kTextures = kAttributes + DX12Native::ShadowBindings::kMaximumInstances;
		auto cpu = slot->heap->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{};
		textureSrv.Format = DXGI_FORMAT_R32_FLOAT;
		textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		textureSrv.Texture2D.MipLevels = 1;
		backend.d3d12Device->CreateShaderResourceView(slot->scene.raster->depth->resource.get(), &textureSrv, cpu);
		cpu.ptr += step;
		textureSrv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		backend.d3d12Device->CreateShaderResourceView(nullptr, &textureSrv, cpu);
		cpu.ptr += step;
		for (size_t i = 0; i < 8; ++i) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			if (i == 4) {
				uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
				uav.Format = DXGI_FORMAT_UNKNOWN;
				uav.Buffer.NumElements = uint32_t(totals.size());
				uav.Buffer.StructureByteStride = sizeof(uint32_t);
				backend.d3d12Device->CreateUnorderedAccessView(slot->counters.get(), nullptr, &uav, cpu);
			} else {
				const size_t textureIndex = i < 4 ? i : i - 1;
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uav.Format = formats[textureIndex];
				backend.d3d12Device->CreateUnorderedAccessView(slot->textures[textureIndex].get(), nullptr, &uav, cpu);
			}
			cpu.ptr += step;
		}
		if (slot->descriptorBindings.lock() != scene.materials) {
			cpu = slot->heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += uint64_t(kGeometry) * step;
			for (const auto& bottom : scene.acceleration->bottoms) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_R32_TYPELESS;
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Buffer.NumElements = uint32_t(bottom->geometry->buffer->GetDesc().Width / 4);
				srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				backend.d3d12Device->CreateShaderResourceView(bottom->geometry->buffer.get(), &srv, cpu);
				cpu.ptr += step;
			}
			cpu = slot->heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += uint64_t(kAttributes) * step;
			for (const auto& instance : scene.acceleration->instances) {
				const auto attributes = instance.material.attributes;
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_R32_TYPELESS;
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Buffer.NumElements = attributes ? uint32_t(attributes->buffer->GetDesc().Width / 4) : 1;
				srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				backend.d3d12Device->CreateShaderResourceView(attributes ? attributes->buffer.get() : nullptr, &srv, cpu);
				cpu.ptr += step;
			}
			cpu = slot->heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += uint64_t(kTextures) * step;
			for (uint32_t i = 0; i < DX12Native::kMaximumTexturePools; ++i) {
				const auto* pool = i < scene.materials->materials->pools.size() ? &scene.materials->materials->pools[i] : nullptr;
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = pool ? pool->format : DXGI_FORMAT_R8G8B8A8_UNORM;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2DArray.MipLevels = pool ? pool->mips : 1;
				srv.Texture2DArray.ArraySize = pool ? pool->slices : 1;
				backend.d3d12Device->CreateShaderResourceView(pool ? pool->resource.get() : nullptr, &srv, cpu);
				cpu.ptr += step;
			}
			slot->descriptorBindings = scene.materials;
		}
		DX::ThrowIfFailed(slot->allocator->Reset());
		if (slot->commands) DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
		else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator.get(), pipeline.get(), IID_PPV_ARGS(slot->commands.put())));
		auto* commands = slot->commands.get();
		commands->CopyBufferRegion(slot->counters.get(), 0, slot->counterReset.get(), 0, totals.size() * sizeof(uint32_t));
		std::vector<D3D12_RESOURCE_BARRIER> begin;
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->scene.raster->depth->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		for (const auto& texture : slot->textures) if (texture) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
		ID3D12DescriptorHeap* heaps[]{ slot->heap.get() };
		commands->SetDescriptorHeaps(1, heaps);
		commands->SetComputeRootSignature(root.get());
		commands->SetComputeRootShaderResourceView(0, scene.acceleration->result->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(2, scene.materials->offsets->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(3, slot->lights->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(5, scene.materials->materials->surfaces->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(6, scene.materials->materials->materials->GetGPUVirtualAddress());
		commands->SetComputeRootConstantBufferView(11, slot->constants->GetGPUVirtualAddress());
		auto gpu = slot->heap->GetGPUDescriptorHandleForHeapStart();
		commands->SetComputeRootDescriptorTable(1, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(9, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(10, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += uint64_t(kGeometry) * step;
		commands->SetComputeRootDescriptorTable(4, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += uint64_t(kAttributes) * step;
		commands->SetComputeRootDescriptorTable(7, gpu);
		gpu = slot->heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += uint64_t(kTextures) * step;
		commands->SetComputeRootDescriptorTable(8, gpu);
		commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		std::vector<D3D12_RESOURCE_BARRIER> end;
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot->scene.raster->depth->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		for (const auto& texture : slot->textures) if (texture) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		commands->ResourceBarrier(uint32_t(end.size()), end.data());
		commands->CopyBufferRegion(slot->counterReadback.get(), 0, slot->counters.get(), 0, totals.size() * sizeof(uint32_t));
		const auto reset = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		commands->ResourceBarrier(1, &reset);
		DX::ThrowIfFailed(commands->Close());
		DX::ThrowIfFailed(backend.commandQueue->Wait(slot->scene.raster->readyFence.get(), slot->scene.raster->readyValue));
		ID3D12CommandList* lists[]{ commands };
		slot->readyValue = UINT64_MAX;
		slot->countersRead = false;
		backend.commandQueue->ExecuteCommandLists(1, lists);
		const auto ready = ++nextFence;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), ready));
		slot->readyValue = ready;
		globals::features::dx12NativeScene.RetainRasterFrame(slot->scene.raster, completion.get(), ready);
		current = slot; active = true; ++submissions; if (a_settings.CompareCulling) ++comparisonFrames; status.clear();
	} catch (const std::exception& error) { status = error.what(); }
}
