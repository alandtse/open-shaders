#include "AccelerationScene.h"

#include <BS_thread_pool.hpp>
#include <atomic>
#include <chrono>
#include "../../../extern/FidelityFX-SDK/framework/cauldron/framework/libs/pix/pix3.h"

namespace DX12Native
{
	namespace
	{
		template <size_t Size>
		void BeginBuildEvent(ID3D12GraphicsCommandList* a_commands, const wchar_t (&a_name)[Size])
		{
			a_commands->BeginEvent(WINPIX_EVENT_UNICODE_VERSION, a_name, static_cast<UINT>(Size * sizeof(wchar_t)));
		}
	}

	bool AccelerationScene::Initialize(const GraphicsBackend& a_backend)
	{
		if (completion) {
			if (backend.d3d12Device.get() != a_backend.d3d12Device.get() || backend.commandQueue.get() != a_backend.commandQueue.get())
				failure = "Acceleration backend changed; restart required";
			return failure.empty();
		}
		backend = a_backend;
		if (!backend.d3d12Device || !backend.commandQueue) return false;
		try {
			DX::ThrowIfFailed(backend.d3d12Device->QueryInterface(IID_PPV_ARGS(device.put())));
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
			DX::ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
			if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) throw std::runtime_error("Inline shadows require DXR 1.1");
			DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
			completion->SetName(L"DX12NativeScene::AccelerationCompletion");
			return true;
		} catch (const std::exception& error) { failure = error.what(); return false; }
	}

	AccelerationScene::~AccelerationScene()
	{
		const uint64_t done = completion ? completion->GetCompletedValue() : 0;
		bool inFlight = false;
		for (auto& build : builds) {
			if (!build.submitted || (build.fence && build.fence <= done)) continue;
			inFlight = true;
			(void)new Build(std::move(build));
		}
		if (inFlight) (void)completion.detach();
	}

	void AccelerationScene::Reset()
	{
		++generation;
		bottoms.clear();
		published.reset();
	}

	uint64_t AccelerationScene::AllocationBytes(uint64_t a_bytes, D3D12_HEAP_TYPE a_heap) const
	{
		const auto flags = a_heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_bytes, flags);
		const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
		if (!a_bytes || info.SizeInBytes == UINT64_MAX) throw std::runtime_error("Invalid acceleration allocation size");
		return info.SizeInBytes;
	}

	winrt::com_ptr<ID3D12Resource> AccelerationScene::Buffer(uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, const wchar_t* a_name)
	{
		const auto heap = CD3DX12_HEAP_PROPERTIES(a_heap);
		const auto flags = a_heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_bytes, flags);
		winrt::com_ptr<ID3D12Resource> buffer;
		DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr, IID_PPV_ARGS(buffer.put())));
		buffer->SetName(a_name);
		return buffer;
	}

	uint64_t AccelerationScene::ResidentBytes() const
	{
		uint64_t bytes = 0;
		std::unordered_set<const BottomLevel*> seenBottom;
		std::unordered_set<const TopLevel*> seenTop;
		auto countBottom = [&](const std::shared_ptr<const BottomLevel>& bottom) {
			if (bottom && seenBottom.insert(bottom.get()).second)
				bytes += bottom->bytes + bottom->geometry->allocationBytes + bottom->geometry->source->bytes;
		};
		auto countTop = [&](const std::shared_ptr<const TopLevel>& top) {
			if (!top || !seenTop.insert(top.get()).second) return;
			bytes += top->bytes;
			for (const auto& bottom : top->bottoms) countBottom(bottom);
		};
		for (const auto& [key, bottom] : bottoms) countBottom(bottom);
		countTop(published);
		for (const auto& build : builds) {
			countBottom(build.bottom);
			countTop(build.top);
			bytes += build.transientBytes;
		}
		return bytes;
	}

	void AccelerationScene::Begin(Build& a_build)
	{
		a_build.generation = generation;
		DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_build.allocator.put())));
		DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_build.allocator.get(), nullptr, IID_PPV_ARGS(a_build.commands.put())));
	}

	void AccelerationScene::Submit(Build& a_build)
	{
		DX::ThrowIfFailed(a_build.commands->Close());
		ID3D12CommandList* lists[]{ a_build.commands.get() };
		a_build.submitted = true;
		backend.commandQueue->ExecuteCommandLists(1, lists);
		a_build.fence = ++nextFence;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), a_build.fence));
	}

	bool AccelerationScene::BuildBottom(Build& a_build, const Instance& a_instance, std::shared_ptr<const Geometry> a_geometry, uint64_t a_budgetBytes)
	{
		const auto& mesh = *a_geometry->source;
		if (mesh.stride < sizeof(float) * 3 || mesh.stride % sizeof(float) || !mesh.vertexCount || !a_instance.indexCount || a_instance.indexCount % 3 ||
			a_geometry->indexOffset < uint64_t(mesh.vertexCount) * mesh.stride ||
			a_geometry->validationOffset < a_geometry->indexOffset + (uint64_t(a_instance.firstIndex) + a_instance.indexCount) * sizeof(uint32_t))
			throw std::runtime_error("Invalid BLAS input range");
		D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
		geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometry.Flags = a_geometry->prepared ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geometry.Triangles.VertexCount = mesh.vertexCount;
		geometry.Triangles.VertexBuffer = { a_geometry->buffer->GetGPUVirtualAddress(), mesh.stride };
		geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		geometry.Triangles.IndexCount = a_instance.indexCount;
		geometry.Triangles.IndexBuffer = a_geometry->buffer->GetGPUVirtualAddress() + a_geometry->indexOffset + uint64_t(a_instance.firstIndex) * sizeof(uint32_t);
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = 1;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.pGeometryDescs = &geometry;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		const auto resultBytes = AllocationBytes(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT);
		const auto scratchBytes = AllocationBytes(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT);
		const auto required = resultBytes + scratchBytes + a_geometry->allocationBytes + mesh.bytes;
		if (required > a_budgetBytes || ResidentBytes() > a_budgetBytes - required) { ++budgetDeferrals; return false; }
		Begin(a_build);
		BeginBuildEvent(a_build.commands.get(), L"DX12NativeScene::BuildBLAS");
		a_build.bottom = std::make_shared<BottomLevel>();
		auto& bottom = *a_build.bottom;
		bottom.geometry = std::move(a_geometry);
		bottom.firstIndex = a_instance.firstIndex;
		bottom.indexCount = a_instance.indexCount;
		bottom.bytes = resultBytes;
		bottom.result = Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::BLAS");
		a_build.scratch = Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::BLAS Scratch");
		a_build.transientBytes = scratchBytes;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = bottom.result->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = a_build.scratch->GetGPUVirtualAddress();
		a_build.commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(bottom.result.get());
		a_build.commands->ResourceBarrier(1, &barrier);
		a_build.commands->EndEvent();
		Submit(a_build);
		++bottomSubmissions;
		return true;
	}

	bool AccelerationScene::BuildTop(Build& a_build, const Snapshot& a_snapshot, uint64_t a_budgetBytes)
	{
		if (published && published->generation == a_snapshot.generation && published->sweep == a_snapshot.sweep && published->blasRevision == bottomCompletions) return false;
		auto top = std::make_shared<TopLevel>();
		top->generation = a_snapshot.generation;
		top->sweep = a_snapshot.sweep;
		top->scope = a_snapshot.scope;
		top->materialSource = a_snapshot.materialSource;
		top->blasRevision = bottomCompletions;
		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
		for (const auto& instance : a_snapshot.instances) {
			const auto found = bottoms.find({ instance.mesh.get(), instance.firstIndex, instance.indexCount });
			if (instance.material.alphaTest || found == bottoms.end() || instances.size() >= kMaximumInstances) return false;
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			std::memcpy(desc.Transform, instance.world.m, sizeof(desc.Transform));
			desc.InstanceID = uint32_t(instances.size());
			desc.InstanceMask = instance.material.opaqueCandidate ? kOpaqueInstanceMask : kMaterialPendingInstanceMask;
			desc.Flags = instance.material.twoSided ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			desc.AccelerationStructure = found->second->result->GetGPUVirtualAddress();
			instances.push_back(desc);
			top->bottoms.push_back(found->second);
			top->instances.push_back(instance);
		}
		if (instances.empty()) return false;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = uint32_t(instances.size());
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		const auto resultBytes = AllocationBytes(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT);
		const auto scratchBytes = AllocationBytes(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT);
		const uint64_t uploadSize = instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
		const auto uploadBytes = AllocationBytes(uploadSize, D3D12_HEAP_TYPE_UPLOAD);
		const auto required = resultBytes + scratchBytes + uploadBytes;
		if (required > a_budgetBytes || ResidentBytes() > a_budgetBytes - required) { ++budgetDeferrals; return false; }
		Begin(a_build);
		BeginBuildEvent(a_build.commands.get(), L"DX12NativeScene::BuildTLAS");
		top->result = Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::TLAS");
		top->bytes = resultBytes;
		a_build.top = std::move(top);
		a_build.scratch = Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::TLAS Scratch");
		a_build.instanceUpload = Buffer(uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::TLAS Instances");
		a_build.transientBytes = scratchBytes + uploadBytes;
		void* mapped = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		DX::ThrowIfFailed(a_build.instanceUpload->Map(0, &noRead, &mapped));
		std::memcpy(mapped, instances.data(), size_t(uploadSize));
		a_build.instanceUpload->Unmap(0, nullptr);
		inputs.InstanceDescs = a_build.instanceUpload->GetGPUVirtualAddress();
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = a_build.top->result->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = a_build.scratch->GetGPUVirtualAddress();
		a_build.commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(a_build.top->result.get());
		a_build.commands->ResourceBarrier(1, &barrier);
		a_build.commands->EndEvent();
		Submit(a_build);
		++topSubmissions;
		return true;
	}

	void AccelerationScene::Retire()
	{
		const uint64_t done = completion->GetCompletedValue();
		if (done == UINT64_MAX) { failure = "Acceleration device removed"; return; }
		for (auto& build : builds) {
			if (!build.submitted || !build.fence || build.fence > done) continue;
			if (build.generation == generation) {
				if (build.bottom) {
					const auto& bottom = build.bottom;
					bottoms.emplace(RangeKey{ bottom->geometry->source.get(), bottom->firstIndex, bottom->indexCount }, bottom);
					++bottomCompletions;
				}
				if (build.top) { published = build.top; ++topCompletions; }
			}
			build = {};
		}
	}

	void AccelerationScene::Advance(const Snapshot* a_snapshot, const GeometryMap& a_geometries, uint64_t a_budgetBytes)
	{
		if (!completion || !failure.empty()) return;
		Retire();
		if (!a_snapshot || !failure.empty()) return;
		std::unordered_set<const Mesh*> current;
		for (const auto& instance : a_snapshot->instances) current.insert(instance.mesh.get());
		std::erase_if(bottoms, [&](const auto& pair) { return !current.contains(pair.first.mesh); });
		Build* available = nullptr;
		for (auto& build : builds) if (!build.allocator) { available = &build; break; }
		if (!available) { ++busyFrames; return; }
		try {
			for (size_t attempt = 0; attempt < a_snapshot->instances.size(); ++attempt) {
				const auto& instance = a_snapshot->instances[buildCursor++ % a_snapshot->instances.size()];
				const RangeKey key{ instance.mesh.get(), instance.firstIndex, instance.indexCount };
				if (instance.material.alphaTest || bottoms.contains(key) || std::ranges::any_of(builds, [&](const Build& build) {
					return build.bottom && build.bottom->geometry->source.get() == key.mesh && build.bottom->firstIndex == key.first && build.bottom->indexCount == key.count;
				})) continue;
				if (const auto found = a_geometries.find(key.mesh); found != a_geometries.end()) BuildBottom(*available, instance, found->second, a_budgetBytes);
				break;
			}
			for (auto& build : builds) {
				if (build.allocator) continue;
				if (std::ranges::any_of(builds, [](const Build& queued) { return bool(queued.top); })) break;
				BuildTop(build, *a_snapshot, a_budgetBytes);
				break;
			}
		} catch (const std::exception& error) {
			failure = error.what();
			for (auto& build : builds) if (!build.submitted) build = {};
		}
	}

	bool AccelerationScene::HasPendingBuilds() const
	{
		return std::ranges::any_of(builds, [](const Build& a_build) { return a_build.submitted; });
	}

	json AccelerationScene::GetDiagnostics() const
	{
		return {
			{ "bottomSubmissions", bottomSubmissions }, { "bottomCompletions", bottomCompletions },
			{ "topSubmissions", topSubmissions }, { "topCompletions", topCompletions }, { "residentBLAS", bottoms.size() },
			{ "residentBytes", completion ? ResidentBytes() : 0 }, { "budgetDeferrals", budgetDeferrals }, { "busyFrames", busyFrames },
			{ "failure", failure }, { "publishedInstances", published ? published->instances.size() : 0 },
			{ "omittedInstances", published ? published->omittedInstances : 0 }, { "coverageValid", false }
		};
	}

	struct AccelerationWorker::Input
	{
		GraphicsBackend backend;
		std::shared_ptr<const Snapshot> snapshot;
		AccelerationScene::GeometryMap geometries;
		uint64_t budget = 0;
	};

	struct AccelerationWorker::Output
	{
		uint64_t revision = 0, fenceValue = 0;
		SceneScope scope;
		winrt::com_ptr<ID3D12Fence> fence;
		std::shared_ptr<const TopLevel> scene;
		json diagnostics;
		std::string failure;
		double cpuMs = 0;
		bool terminal = false;
	};

	struct AccelerationWorker::State
	{
		std::atomic<bool> busy{ false };
		std::atomic<uint64_t> desired{ 0 };
		AccelerationScene acceleration;
		std::shared_ptr<const Output> completed;
		uint64_t appliedRevision = 0, sceneEpoch = 0;
	};

	void AccelerationWorker::Start(const GraphicsBackend& a_backend, std::shared_ptr<const Snapshot> a_snapshot,
		AccelerationScene::GeometryMap a_geometries, uint64_t a_budgetBytes)
	{
		if (!state) state = std::make_shared<State>();
		auto prepared = std::make_shared<Input>();
		prepared->backend = a_backend;
		prepared->snapshot = std::move(a_snapshot);
		prepared->geometries = std::move(a_geometries);
		prepared->budget = a_budgetBytes;
		input = std::move(prepared);
		output.reset();
		dispatchFailure.clear();
		finished = false;
		retiring = false;
		state->desired.store(++revision, std::memory_order_release);
	}

	void AccelerationWorker::Reset()
	{
		++revision;
		input.reset();
		output.reset();
		dispatchFailure.clear();
		finished = true;
		retiring = bool(state);
		if (state) state->desired.store(revision, std::memory_order_release);
	}

	void AccelerationWorker::Advance()
	{
		if (!state || state->busy.load(std::memory_order_acquire)) return;
		if (state->completed && state->completed->revision == revision) {
			output = state->completed;
			if (!output->diagnostics.empty()) {
				lastDiagnostics = output->diagnostics;
				lastDiagnosticScope = output->scope;
			}
			if (output->terminal) {
				if (output->scene && output->scene->scope == output->scope) lastCompletedScope = output->scope;
				if (!output->failure.empty()) { lastBatchFailure = output->failure; lastFailedBatch = output->scope.batch; }
				finished = true;
				retiring = false;
				return;
			}
			if (output->fence && output->fenceValue > output->fence->GetCompletedValue()) return;
		}
		if (!retiring && (finished || !input)) return;
		if (state->busy.exchange(true, std::memory_order_acq_rel)) return;
		try {
			auto result = std::make_shared<Output>();
			result->revision = revision;
			static BS::thread_pool<> pool{ 1 };
			pool.detach_task([owned = state, prepared = input, result] {
				const auto started = std::chrono::steady_clock::now();
				const SKSE::stl::scope_exit complete([&]() noexcept {
					result->cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
					owned->completed = result;
					owned->busy.store(false, std::memory_order_release);
				});
				if (owned->desired.load(std::memory_order_acquire) != result->revision) return;
				try {
					auto& acceleration = owned->acceleration;
					const auto* snapshot = prepared ? prepared->snapshot.get() : nullptr;
					if (owned->appliedRevision != result->revision) {
						if (!snapshot || owned->sceneEpoch != snapshot->scope.scene) acceleration.Reset();
						else acceleration.ReplaceScope();
						owned->sceneEpoch = snapshot ? snapshot->scope.scene : 0;
						owned->appliedRevision = result->revision;
					}
					const auto deferrals = acceleration.BudgetDeferrals();
					if (snapshot) {
						result->scope = snapshot->scope;
						if (acceleration.Initialize(prepared->backend))
							acceleration.Advance(snapshot, prepared->geometries, prepared->budget);
						else if (acceleration.Failure().empty()) result->failure = "Acceleration backend unavailable";
					} else {
						acceleration.Advance(nullptr, {}, 0);
					}
					if (!acceleration.Failure().empty()) result->failure = acceleration.Failure();
					result->fence = acceleration.CompletionFence();
					result->fenceValue = acceleration.SubmittedFence();
					result->scene = acceleration.GetScene();
					result->diagnostics = acceleration.GetDiagnostics();
					const bool pending = acceleration.HasPendingBuilds();
					const bool built = snapshot && result->scene && result->scene->scope == snapshot->scope;
					result->terminal = !result->failure.empty() || (!pending && (!snapshot || snapshot->instances.empty() || built));
					if (!pending && acceleration.BudgetDeferrals() != deferrals) {
						result->failure = "Acceleration budget prevents this batch from completing";
						result->terminal = true;
					}
				} catch (const std::exception& error) {
					result->failure = error.what();
					result->terminal = true;
				}
			});
		} catch (const std::exception& error) {
			state->busy.store(false, std::memory_order_release);
			dispatchFailure = error.what();
			finished = true;
			retiring = false;
		}
	}

	json AccelerationWorker::GetDiagnostics() const
	{
		json status = lastDiagnostics.is_object() ? lastDiagnostics : json::object();
		const auto* snapshot = input ? input->snapshot.get() : nullptr;
		const bool matching = output && snapshot && output->revision == revision && output->scope == snapshot->scope &&
			snapshot->offline;
		status["workerBusy"] = state && state->busy.load(std::memory_order_acquire);
		status["countersAvailable"] = lastDiagnostics.is_object() && !lastDiagnostics.empty();
		status["counterBatch"] = lastDiagnosticScope.batch;
		status["counterScene"] = lastDiagnosticScope.scene;
		status["lastCompletedBatch"] = lastCompletedScope.batch;
		status["lastCompletedScene"] = lastCompletedScope.scene;
		status["lastCompletedLight"] = lastCompletedScope.light;
		status["lastBatchFailure"] = lastBatchFailure;
		status["lastFailedBatch"] = lastFailedBatch;
		status["inputInstances"] = snapshot ? snapshot->instances.size() : 0;
		status["inputMeshes"] = input ? input->geometries.size() : 0;
		status["submittedFence"] = output ? output->fenceValue : 0;
		status["completedFence"] = output && output->fence ? output->fence->GetCompletedValue() : 0;
		status["batchFinished"] = finished;
		status["publicationMatchesBatch"] = matching && bool(output->scene);
		status["sourceFrame"] = snapshot ? snapshot->scope.frame : 0;
		status["sourceIsCurrentFrame"] = false;
		status["sourceScene"] = snapshot ? snapshot->scope.scene : 0;
		status["sourceLight"] = snapshot ? snapshot->scope.light : 0;
		status["sourceSlot"] = snapshot ? snapshot->scope.slot : 0;
		status["sourceBatch"] = snapshot ? snapshot->scope.batch : 0;
		status["sourceIncomplete"] = snapshot && snapshot->truncated;
		status["lastWorkerCPUms"] = output ? output->cpuMs : 0;
		status["failure"] = !dispatchFailure.empty() ? dispatchFailure : output ? output->failure : "";
		status["coverageValid"] = false;
		status["publicationEligible"] = false;
		return status;
	}

	std::shared_ptr<const TopLevel> AccelerationWorker::GetScene() const
	{
		return finished && output && input && output->scene && output->revision == revision && output->scope == input->snapshot->scope &&
			output->scene->scope == input->snapshot->scope ? output->scene : nullptr;
	}
}
