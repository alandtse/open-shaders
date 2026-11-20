#include "AccelerationScene.h"

#include <algorithm>
#include <cstring>
#include <directx/d3dx12.h>
#include <ranges>
#include <utility>

namespace DX12
{
	namespace
	{
		constexpr size_t kMaximumShapes = 384;
		constexpr uint64_t kMaximumSourceBytes = 96ull * 1024 * 1024;
		constexpr uint64_t kMaximumAccelerationBytes = 24ull * 1024 * 1024;
	}

	AccelerationScene::~AccelerationScene()
	{
		worker.request_stop();
		wake.notify_all();
		if (worker.joinable())
			worker.join();
	}

	bool AccelerationScene::Start(ID3D12Device* candidate)
	{
		if (!candidate || initializationFailed.load(std::memory_order_acquire))
			return false;
		if (worker.joinable())
			return candidate == requestedDevice;
		requestedDevice = candidate;
		winrt::com_ptr<ID3D12Device> retained;
		retained.copy_from(candidate);
		// Driver feature checks and DX12 queue/fence allocation can be expensive.
		// Prewarm them on the acceleration-scene worker, never in PreShadowMaps.
		worker = std::jthread([this, held = std::move(retained)](std::stop_token stop) mutable {
			winrt::com_ptr<ID3D12Device5> next;
			if (FAILED(held->QueryInterface(IID_PPV_ARGS(next.put())))) {
				initializationFailed.store(true, std::memory_order_release);
				return;
			}
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
			if (FAILED(next->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
				options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
				initializationFailed.store(true, std::memory_order_release);
				return;
			}
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
			winrt::com_ptr<ID3D12CommandQueue> nextQueue;
			winrt::com_ptr<ID3D12Fence> nextFence;
			if (FAILED(next->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(nextQueue.put()))) ||
				FAILED(next->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(nextFence.put())))) {
				initializationFailed.store(true, std::memory_order_release);
				return;
			}
			device = std::move(next);
			queue = std::move(nextQueue);
			fence = std::move(nextFence);
			if (!stop.stop_requested())
				Run(stop);
		});
		return true;
	}

	void AccelerationScene::Request(std::shared_ptr<const Grid> grid)
	{
		if (!worker.joinable() || !grid || !grid->requested.front().scope)
			return;
		std::scoped_lock lock(mutex);
		if (requestedRevision == grid->geometryRevision)
			return;
		requestedRevision = grid->geometryRevision;
		requested = std::move(grid);
		++generation;
		wake.notify_one();
	}

	void AccelerationScene::Clear()
	{
		std::scoped_lock lock(mutex);
		++generation;
		requested.reset();
		requestedRevision = 0;
		ready.store(nullptr, std::memory_order_release);
	}

	winrt::com_ptr<ID3D12Resource> AccelerationScene::MakeBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap,
		D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) const
	{
		winrt::com_ptr<ID3D12Resource> resource;
		if (!bytes || bytes > kMaximumAccelerationBytes * 4)
			return resource;
		const auto properties = CD3DX12_HEAP_PROPERTIES(heap);
		const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
		if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
				state, nullptr, IID_PPV_ARGS(resource.put()))))
			return {};
		return resource;
	}

	bool AccelerationScene::SubmitAndWait(ID3D12GraphicsCommandList4* list, std::stop_token stop)
	{
		if (FAILED(list->Close()))
			return false;
		ID3D12CommandList* commands[] = { list };
		queue->ExecuteCommandLists(1, commands);
		const uint64_t value = ++fenceValue;
		if (FAILED(queue->Signal(fence.get(), value)))
			return false;
		// A submitted BLAS/TLAS build owns upload, scratch, allocator and destination
		// memory until its fence retires. Cancellation may stop the *next* build,
		// but must never release in-flight buffers on a stop request.
		(void)stop;
		const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		const bool armed = event && SUCCEEDED(fence->SetEventOnCompletion(value, event));
		bool completed = false;
		for (;;) {
			const uint64_t current = fence->GetCompletedValue();
			if (current >= value && current != UINT64_MAX) {
				completed = true;
				break;
			}
			if (current == UINT64_MAX || FAILED(device->GetDeviceRemovedReason()))
				break;
			// Device waits happen solely on AccelerationScene's worker thread.
			if (armed)
				(void)WaitForSingleObject(event, 20);
			else
				Sleep(20);
		}
		if (event)
			CloseHandle(event);
		return completed;
	}

	std::shared_ptr<const AccelerationScene::Bottom> AccelerationScene::BuildBottom(
		const std::shared_ptr<const Shape>& shape, std::stop_token stop)
	{
		if (!shape || shape->positions.empty() || shape->indices.empty() || shape->indices.size() % 3 ||
			shape->positions.size() > UINT32_MAX || shape->indices.size() > UINT32_MAX)
			return nullptr;
		const uint64_t vertexBytes = uint64_t(shape->positions.size()) * sizeof(float3);
		const uint64_t indexBytes = uint64_t(shape->indices.size()) * sizeof(uint32_t);
		if (vertexBytes + indexBytes > 8ull * 1024 * 1024)
			return nullptr;
		auto upload = MakeBuffer(vertexBytes + indexBytes, D3D12_HEAP_TYPE_UPLOAD,
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
		if (!upload)
			return nullptr;
		void* mapped = nullptr;
		if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped)
			return nullptr;
		std::memcpy(mapped, shape->positions.data(), size_t(vertexBytes));
		std::memcpy(static_cast<std::byte*>(mapped) + vertexBytes, shape->indices.data(), size_t(indexBytes));
		upload->Unmap(0, nullptr);
		D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
		geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geometry.Triangles.VertexCount = UINT(shape->positions.size());
		geometry.Triangles.VertexBuffer = { upload->GetGPUVirtualAddress(), sizeof(float3) };
		geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		geometry.Triangles.IndexCount = UINT(shape->indices.size());
		geometry.Triangles.IndexBuffer = upload->GetGPUVirtualAddress() + vertexBytes;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &geometry;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes ||
			info.ResultDataMaxSizeInBytes > kMaximumAccelerationBytes || info.ScratchDataSizeInBytes > kMaximumAccelerationBytes)
			return nullptr;
		auto bottom = std::make_shared<Bottom>();
		bottom->shape = shape;
		bottom->acceleration = MakeBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		auto scratch = MakeBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		if (!bottom->acceleration || !scratch)
			return nullptr;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(allocator.put()))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.get(), nullptr, IID_PPV_ARGS(commands.put()))))
			return nullptr;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = bottom->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
		commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(bottom->acceleration.get());
		commands->ResourceBarrier(1, &barrier);
		return SubmitAndWait(commands.get(), stop) ? bottom : nullptr;
	}

	std::shared_ptr<AccelerationSceneReady> AccelerationScene::BuildTop(
		const std::vector<std::shared_ptr<const Bottom>>& bottoms, const Grid& grid, bool complete, std::stop_token stop)
	{
		if (bottoms.empty())
			return nullptr;
		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
		instances.reserve(bottoms.size());
		for (const auto& bottom : bottoms) {
			D3D12_RAYTRACING_INSTANCE_DESC instance{};
			std::memcpy(instance.Transform, bottom->shape->transform.data(), sizeof(instance.Transform));
			instance.InstanceID = UINT(instances.size());
			instance.InstanceMask = 1;
			instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
			instance.AccelerationStructure = bottom->acceleration->GetGPUVirtualAddress();
			instances.push_back(instance);
		}
		auto upload = MakeBuffer(uint64_t(instances.size()) * sizeof(instances.front()),
			D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
		if (!upload)
			return nullptr;
		void* mapped = nullptr;
		if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped)
			return nullptr;
		std::memcpy(mapped, instances.data(), instances.size() * sizeof(instances.front()));
		upload->Unmap(0, nullptr);
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = UINT(instances.size());
		inputs.InstanceDescs = upload->GetGPUVirtualAddress();
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes ||
			info.ResultDataMaxSizeInBytes > kMaximumAccelerationBytes || info.ScratchDataSizeInBytes > kMaximumAccelerationBytes)
			return nullptr;
		auto top = std::make_shared<AccelerationSceneReady>();
		top->bottoms = bottoms;
		top->scope = grid.requested.front().scope;
		top->geometryRevision = grid.geometryRevision;
		top->complete = complete;
		for (const auto& bottom : bottoms)
			top->shapeIDs.insert(bottom->shape->identity);
		top->acceleration = MakeBuffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		auto scratch = MakeBuffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		if (!top->acceleration || !scratch)
			return nullptr;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(allocator.put()))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.get(), nullptr, IID_PPV_ARGS(commands.put()))))
			return nullptr;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = top->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
		commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(top->acceleration.get());
		commands->ResourceBarrier(1, &barrier);
		return SubmitAndWait(commands.get(), stop) ? top : nullptr;
	}

	void AccelerationScene::Run(std::stop_token stop)
	{
		std::unordered_map<const Shape*, std::shared_ptr<const Bottom>> cache;
		while (!stop.stop_requested()) {
			std::shared_ptr<const Grid> grid;
			uint64_t buildGeneration = 0;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, stop, [&] { return requested != nullptr; });
				if (stop.stop_requested())
					break;
				grid = std::exchange(requested, nullptr);
				buildGeneration = generation;
			}
			std::vector<std::shared_ptr<const Shape>> shapes;
			std::unordered_set<uint64_t> seen;
			uint64_t sourceBytes = 0;
			bool complete = std::ranges::all_of(grid->resident, [](const auto& tile) {
				return tile && tile->state == Tile::State::Ready;
			});
			for (const auto& tile : grid->resident) {
				if (!tile)
					continue;
				for (const auto& shape : tile->shapes) {
					if (!shape || !seen.insert(shape->identity).second)
						continue;
					const uint64_t bytes = uint64_t(shape->positions.size()) * sizeof(float3) +
					                       uint64_t(shape->indices.size()) * sizeof(uint32_t);
					if (shapes.size() >= kMaximumShapes || bytes > kMaximumSourceBytes - sourceBytes) {
						complete = false;
						continue;
					}
					shapes.push_back(shape);
					sourceBytes += bytes;
				}
			}
			std::vector<std::shared_ptr<const Bottom>> bottoms;
			bottoms.reserve(shapes.size());
			for (const auto& shape : shapes) {
				if (stop.stop_requested())
					break;
				{
					std::scoped_lock lock(mutex);
					if (generation != buildGeneration)
						break;
				}
				auto found = cache.find(shape.get());
				auto bottom = found != cache.end() ? found->second : BuildBottom(shape, stop);
				if (!bottom) {
					complete = false;
					continue;
				}
				cache.insert_or_assign(shape.get(), bottom);
				bottoms.push_back(bottom);
			}
			if (bottoms.empty() || stop.stop_requested())
				continue;
			auto top = BuildTop(bottoms, *grid, complete && bottoms.size() == shapes.size(), stop);
			if (!top)
				continue;
			std::scoped_lock lock(mutex);
			if (generation != buildGeneration)
				continue;
			ready.store(std::move(top), std::memory_order_release);
			std::erase_if(cache, [&](const auto& entry) { return !std::ranges::contains(shapes, entry.second->shape); });
		}
	}
}
