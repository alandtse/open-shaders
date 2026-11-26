#include "DX12NativeLightWorker.h"

#include "DX12GpuRetirement.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "Utils/TracyZones.h"
#include "VideoMemoryBudget.h"

#include <cstring>
#include <directx/d3dx12.h>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace DX12Native
{
	struct NativeLightWorker::State
	{
		std::atomic<std::shared_ptr<const NativeLightPublication>> latest;
		std::atomic<bool> busy{ false }, retry{ false };
		std::atomic<uint64_t> tasks{ 0 }, publications{ 0 }, unchanged{ 0 }, deferred{ 0 };
		std::array<std::shared_ptr<const DX12::Tile>, DX12::kGridCapacity> preparedResident{};
		std::array<std::shared_ptr<const DX12::Tile>, DX12::kPredictiveCapacity> preparedPredictive{};
		bool preparedInitialized = false;
		std::vector<DX12::PreparedLight> prepared;
		VideoMemoryBudget budget;
		bool failureLogged = false;
	};

	namespace
	{
		void UploadLights(ID3D12Device* device, ID3D12CommandQueue* queue,
			NativeLightPublication& publication, VideoMemoryBudget& budget)
		{
			ZoneScopedN("DX12::Lights::Upload");
			if (publication.lights.empty())
				return;
			std::vector<PackedNativeLight> packed;
			packed.reserve(publication.lights.size());
			for (const auto& light : publication.lights)
				packed.push_back(PackNativeLight(light));
			const uint64_t bytes = packed.size() * sizeof(PackedNativeLight);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
			const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			if (allocation == UINT64_MAX || allocation > budget.Query(device, 0).effectiveBytes)
				throw std::runtime_error("Native light buffer budget deferred");

			const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			winrt::com_ptr<ID3D12Resource> staging;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			DX::ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
				&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(publication.buffer.put())));
			DX::ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
				&desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(staging.put())));
			publication.buffer->SetName(L"DX12NativeScene::LightInventory");
			staging->SetName(L"DX12NativeScene::LightInventoryUpload");
			const D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			DX::ThrowIfFailed(staging->Map(0, &noRead, &mapped));
			std::memcpy(mapped, packed.data(), size_t(bytes));
			staging->Unmap(0, nullptr);
			DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
			DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(commands.put())));
			allocator->SetName(L"DX12NativeScene::LightUploadAllocator");
			commands->SetName(L"DX12NativeScene::LightUploadCommands");
			commands->CopyBufferRegion(publication.buffer.get(), 0, staging.get(), 0, bytes);
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(publication.buffer.get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			commands->ResourceBarrier(1, &barrier);
			DX::ThrowIfFailed(commands->Close());
			DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(publication.fence.put())));
			publication.fence->SetName(L"DX12NativeScene::LightUploadFence");
			constexpr uint64_t done = 1;
			// Retain both copy resources even if this visit is invalidated before publication.
			auto retained = std::tuple{ publication.buffer, std::move(staging), std::move(allocator), commands };
			winrt::com_ptr<ID3D12Device> owner;
			owner.copy_from(device);
			std::vector<FenceDependency> dependencies{ FencePoint(publication.fence.get(), done) };
			ID3D12CommandList* lists[]{ commands.get() };
			queue->ExecuteCommandLists(1, lists);
			const HRESULT hr = queue->Signal(publication.fence.get(), done);
			if (FAILED(hr))
				dependencies.front().value = UINT64_MAX;
			RetireAfterFences(std::move(retained), std::move(owner), std::move(dependencies));
			DX::ThrowIfFailed(hr);
			publication.fenceValue = done;
		}
	}

	void NativeLightWorker::Reset()
	{
		state.reset();
		ready.reset();
	}

	bool NativeLightWorker::Request(const GraphicsBackend& backend, std::shared_ptr<const DX12::Grid> grid,
		std::shared_ptr<const std::vector<NativeRuntimeEmitter>> runtime, NativeLightSettings settings, uint32_t cell)
	{
		if (!grid || !runtime || !cell || !grid->requested.front().scope ||
			!backend.d3d12Device || !backend.commandQueue || !globals::shaderCache)
			return false;
		const auto gate = globals::shaderCache->GetRuntimeTaskGate();
		if (!gate)
			return false;
		if (!state)
			state = std::make_shared<State>();
		auto current = state;
		if (current->busy.load(std::memory_order_acquire))
			return false;
		const auto latest = current->latest.load(std::memory_order_acquire);
		if (latest && latest->fence && !GpuFenceRetired(latest->fence.get(), latest->fenceValue))
			return false;
		current->busy.store(true, std::memory_order_release);
		const bool accepted = gate->TrySubmit([current, grid = std::move(grid), runtime = std::move(runtime),
			settings, cell, device = backend.d3d12Device, queue = backend.commandQueue] {
			ZoneScopedN("DX12::Lights::Task");
			current->tasks.fetch_add(1, std::memory_order_relaxed);
			try {
				if (!current->preparedInitialized || current->preparedResident != grid->resident ||
					current->preparedPredictive != grid->predictiveResident) {
					ZoneScopedN("DX12::Lights::Prepared");
					current->prepared = DX12Resources::CollectPreparedLights(*grid);
					current->preparedResident = grid->resident;
					current->preparedPredictive = grid->predictiveResident;
					current->preparedInitialized = true;
				}
				std::unordered_set<uint32_t> preparedIDs;
				for (const auto& light : current->prepared)
					if (light.referenceID)
						preparedIDs.insert(light.referenceID);
				std::vector<DX12::PreparedLight> supplemental;
				supplemental.reserve(runtime->size());
				for (const auto& emitter : *runtime)
					if (!emitter.placedReference || !preparedIDs.contains(emitter.placedReference))
						supplemental.push_back(emitter.value);
				std::vector<DX12::PreparedLight> lights;
				{
					ZoneScopedN("DX12::Lights::Consolidate");
					lights = ConsolidateFullShadowLights<DX12::PreparedLight>(current->prepared, supplemental,
						settings.unify, settings.exclude, settings.additive);
				}
				lights.resize(std::min(lights.size(), size_t{ DX12::kMaxPreparedLights }));
				const auto previous = current->latest.load(std::memory_order_acquire);
				const auto preparedCount = uint32_t(current->prepared.size());
				if (previous && previous->preparedCount == preparedCount &&
					SameNativeLightInventory<DX12::PreparedLight>(previous->lights, lights)) {
					current->unchanged.fetch_add(1, std::memory_order_relaxed);
				} else {
					auto publication = std::make_shared<NativeLightPublication>();
					publication->scope = grid->requested.front().scope;
					publication->cell = cell;
					publication->revision = previous ? previous->revision + 1 : 1;
					publication->preparedCount = preparedCount;
					publication->lights = std::move(lights);
					UploadLights(device.get(), queue.get(), *publication, current->budget);
					current->latest.store(std::move(publication), std::memory_order_release);
					current->publications.fetch_add(1, std::memory_order_relaxed);
				}
				current->retry.store(false, std::memory_order_release);
				current->failureLogged = false;
			} catch (const winrt::hresult_error& error) {
				if (!current->failureLogged)
					logger::warn("[DX12NativeScene] Native light task failed (HRESULT 0x{:08X}): {}", static_cast<uint32_t>(error.code()), winrt::to_string(error.message()));
				current->failureLogged = true;
				current->retry.store(true, std::memory_order_release);
			} catch (const std::exception& error) {
				if (!current->failureLogged)
					logger::warn("[DX12NativeScene] Native light task failed: {}", error.what());
				current->failureLogged = true;
				current->retry.store(true, std::memory_order_release);
			} catch (...) {
				if (!current->failureLogged)
					logger::warn("[DX12NativeScene] Native light task failed");
				current->failureLogged = true;
				current->retry.store(true, std::memory_order_release);
			}
			current->busy.store(false, std::memory_order_release);
		});
		if (!accepted) {
			current->busy.store(false, std::memory_order_release);
			current->deferred.fetch_add(1, std::memory_order_relaxed);
		}
		return accepted;
	}

	std::shared_ptr<const NativeLightPublication> NativeLightWorker::GetReady()
	{
		if (!state)
			return {};
		const auto latest = state->latest.load(std::memory_order_acquire);
		if (latest && latest != ready && (!latest->fence || GpuFenceRetired(latest->fence.get(), latest->fenceValue)))
			ready = latest;
		return ready;
	}

	bool NativeLightWorker::NeedsRetry() const
	{
		return state && state->retry.load(std::memory_order_acquire);
	}

	NativeLightWorker::Stats NativeLightWorker::GetStats() const
	{
		if (!state)
			return {};
		return { state->tasks.load(), state->publications.load(), state->unchanged.load(),
			state->deferred.load(), state->busy.load() };
	}
}
