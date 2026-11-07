#include "ShadowBindings.h"
#include <directx/d3dx12.h>
#include <BS_thread_pool.hpp>
#include <atomic>
#include <chrono>

namespace DX12Native
{
	static uint64_t MaterialClockMs()
	{
		return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	std::shared_ptr<const ShadowBindings> ShadowBindings::Create(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene,
        uint64_t a_budget, uint64_t a_uploadBatchBudget, std::shared_ptr<const NativePBR::LiveBindingSource> a_livePbr,
        bool a_importLoose, bool a_importArchive, std::shared_ptr<const ShadowBindings> a_previous)
    {
		auto result = std::make_shared<ShadowBindings>();
		result->uploadBatchBudget = a_uploadBatchBudget;
		result->scene = std::move(a_scene);
		if (!a_backend.d3d12Device || !result->scene || result->scene->bottoms.empty() || result->scene->bottoms.size() > kMaximumInstances || result->scene->bottoms.size() != result->scene->instances.size())
			throw std::runtime_error("Invalid shadow scene bindings");
		if (!a_budget) throw std::runtime_error("Shadow binding budget is zero");
		VideoMemoryBudget budget;
		result->materialBudget = budget.Query(a_backend.d3d12Device.get(), 0, a_budget);
		a_budget = result->materialBudget.effectiveBytes;
		const auto offsetsDesc = CD3DX12_RESOURCE_DESC::Buffer(result->scene->bottoms.size() * sizeof(uint32_t));
		const auto materialDesc = CD3DX12_RESOURCE_DESC::Buffer(result->scene->instances.size() * sizeof(MaterialRecord));
		const auto surfaceDesc = CD3DX12_RESOURCE_DESC::Buffer(result->scene->instances.size() * sizeof(SurfaceRecord));
		const auto offsetsBytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &offsetsDesc).SizeInBytes;
		const auto materialBytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &materialDesc).SizeInBytes;
		const auto surfaceBytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &surfaceDesc).SizeInBytes;
		if (offsetsBytes == UINT64_MAX || materialBytes == UINT64_MAX || surfaceBytes == UINT64_MAX ||
			offsetsBytes > a_budget || materialBytes > a_budget - offsetsBytes || surfaceBytes > a_budget - offsetsBytes - materialBytes)
			throw std::runtime_error("Shadow binding tables exceed budget");
		constexpr uint64_t kDescriptorAndRoomReserve = 1024 * 1024;
		const auto tables = offsetsBytes + materialBytes + surfaceBytes;
		if (kDescriptorAndRoomReserve > a_budget - tables) throw std::runtime_error("Shadow binding overhead exceeds budget");
		auto boundedScene = *result->scene;
        const bool canInherit = a_previous && a_previous->scene == result->scene &&
            a_previous->materials && a_importLoose && a_livePbr;
        if (boundedScene.materialSource) {
			auto source = std::make_shared<MaterialSource>(*boundedScene.materialSource);
			source->budget = std::min(source->budget, a_budget - tables - kDescriptorAndRoomReserve);
			source->uploadBatchBytes = a_uploadBatchBudget;
            source->importLoosePbr = a_importLoose && a_livePbr != nullptr;
            source->importArchivePbr = source->importLoosePbr && a_importArchive;
            // Skip the legacy manifest on later batches; previously published
            // atlas and runtime pages are inherited through a_previous.
            if (canInherit) source->usePreparedTextureManifest = false;
			boundedScene.materialSource = std::move(source);
		}
		result->bytes = offsetsBytes + kDescriptorAndRoomReserve;
		result->materials = MaterialResources::Load(a_backend, boundedScene, a_livePbr.get(),
            canInherit ? a_previous->materials.get() : nullptr);
		if (result->materials->bytes > a_budget - result->bytes || result->materials->tableBytes > a_budget - result->bytes - result->materials->bytes)
			throw std::runtime_error("Shadow materials exceed budget");
		result->bytes += result->materials->bytes + result->materials->tableBytes;
		const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTextureDescriptorBase + kMaximumTexturePools, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		DX::ThrowIfFailed(a_backend.d3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(result->heap.put())));
		result->heap->SetName(L"DX12NativeScene::ShadowBindings");
		const auto step = a_backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpu = result->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(kGeometryDescriptorBase) * step;
		std::vector<uint32_t> offsets;
		offsets.reserve(result->scene->bottoms.size());
		for (const auto& bottom : result->scene->bottoms) {
			const auto& geometry = *bottom->geometry;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = uint32_t(geometry.buffer->GetDesc().Width / sizeof(uint32_t));
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			a_backend.d3d12Device->CreateShaderResourceView(geometry.buffer.get(), &srv, cpu);
			cpu.ptr += step;
			offsets.push_back(uint32_t(geometry.validationOffset));
		}
		cpu = result->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(kAttributeDescriptorBase) * step;
		for (const auto& instance : result->scene->instances) {
			const auto attributes = instance.material.attributes;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = attributes ? uint32_t(attributes->buffer->GetDesc().Width / 4) : 1;
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			a_backend.d3d12Device->CreateShaderResourceView(attributes ? attributes->buffer.get() : nullptr, &srv, cpu);
			cpu.ptr += step;
		}
		cpu = result->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(kTextureDescriptorBase) * step;
		for (uint32_t i = 0; i < kMaximumTexturePools; ++i) {
			const auto* pool = i < result->materials->pools.size() ? &result->materials->pools[i] : nullptr;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = pool ? pool->format : DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2DArray.MipLevels = pool ? pool->mips : 1;
			srv.Texture2DArray.ArraySize = pool ? pool->slices : 1;
			a_backend.d3d12Device->CreateShaderResourceView(pool ? pool->resource.get() : nullptr, &srv, cpu);
			cpu.ptr += step;
		}
		const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		const auto upload = CD3DX12_RESOURCE_DESC::Buffer(offsets.size() * sizeof(uint32_t));
		DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(result->offsets.put())));
		result->offsets->SetName(L"DX12NativeScene::ValidationOffsets");
		const D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		DX::ThrowIfFailed(result->offsets->Map(0, &noRead, &mapped));
		std::memcpy(mapped, offsets.data(), offsets.size() * sizeof(uint32_t));
		result->offsets->Unmap(0, nullptr);
		return result;
	}

	struct ShadowBindingWorker::State
	{
		std::atomic<bool> done{ false };
		uint64_t revision = 0;
		std::shared_ptr<const ShadowBindings> bindings;
		std::string failure;
		bool deferred = false;
	};

	void ShadowBindingWorker::Request(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene,
        uint64_t a_budget, uint64_t a_uploadBatchBudget, std::shared_ptr<const NativePBR::LiveBindingSource> a_livePbr,
        bool a_importLoose, bool a_importArchive)
    {
        const uint64_t incomingRevision = a_livePbr && a_livePbr->generation ? a_livePbr->generation->revision : 0;
        if (desired == a_scene && budget == a_budget && uploadBatchBudget == a_uploadBatchBudget &&
            liveRevision == incomingRevision && importLoose == a_importLoose && importArchive == a_importArchive && backend.d3d12Device.get() == a_backend.d3d12Device.get() &&
            backend.commandQueue.get() == a_backend.commandQueue.get()) return;
        const bool sameScene = desired == a_scene && backend.d3d12Device.get() == a_backend.d3d12Device.get();
        ++revision;
        desired = std::move(a_scene);
        livePbr = std::move(a_livePbr);
        liveRevision = incomingRevision;
        importLoose = a_importLoose;
        importArchive = a_importArchive;
        backend = a_backend;
        budget = a_budget;
        uploadBatchBudget = a_uploadBatchBudget;
        if (!sameScene) published.reset();
		attempted = false;
		uploadRetry.Reset();
		failure.clear();
	}

	void ShadowBindingWorker::Advance()
	{
		RetireMaterialUploads();
		if (pending) {
			if (!pending->done.load(std::memory_order_acquire)) return;
			if (pending->bindings && pending->bindings->materials->pendingUpload) {
				const auto status = pending->bindings->materials->pendingUpload->Poll();
				if (status == MaterialUploadStatus::Pending) return;
				if (status == MaterialUploadStatus::Failed) {
					pending->bindings.reset();
					pending->failure = "Final material upload did not complete";
				}
			}
			if (pending->revision == revision) {
                if (pending->bindings) published = pending->bindings;
                failure = pending->failure;
				if (pending->deferred) uploadRetry.Start(MaterialClockMs());
			}
			pending.reset();
		}
		if (uploadRetry.Waiting()) {
			const auto status = uploadRetry.Poll(MaterialUploadsReady(), MaterialClockMs());
			if (status == MaterialUploadStatus::Pending) return;
			if (status == MaterialUploadStatus::Failed) { failure = "Previous material upload did not retire"; return; }
			attempted = false;
			failure.clear();
		}
		if (!desired || attempted) return;
		auto task = std::make_shared<State>();
		task->revision = revision;
		try {
			static BS::thread_pool<> pool{ 1 };
			pool.detach_task([task, backend = backend, scene = desired, budget = budget, uploadBudget = uploadBatchBudget, live = livePbr, import = importLoose, archives = importArchive] {
                try { task->bindings = ShadowBindings::Create(backend, scene, budget, uploadBudget, live, import, archives); }
				catch (const MaterialUploadBusy& error) { task->failure = error.what(); task->deferred = true; }
				catch (const std::exception& error) { task->failure = error.what(); }
				catch (...) { task->failure = "Unknown shadow binding failure"; }
				task->done.store(true, std::memory_order_release);
			});
			pending = std::move(task);
			attempted = true;
		} catch (const std::exception& error) {
			failure = error.what();
			attempted = true;
		}
	}

	void ShadowBindingWorker::Reset()
	{
		++revision;
        desired.reset();
        livePbr.reset(); liveRevision = 0; importLoose = false; importArchive = false;
		published.reset();
		failure.clear();
		attempted = false;
		uploadRetry.Reset();
	}

	bool ShadowBindingWorker::UploadPending() const
	{
		return uploadRetry.Waiting() || (pending && pending->done.load(std::memory_order_acquire) && pending->bindings && pending->bindings->materials->pendingUpload);
	}

}
