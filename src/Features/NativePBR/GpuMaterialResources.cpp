#include "GpuMaterialResources.h"
#include "GpuMaterialResidency.h"

#include "Features/DX12NativeScene/MaterialTextureLookup.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"

#include <BS_thread_pool.hpp>
#include <DirectXTex.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <directx/d3dx12.h>
#include <stdexcept>

namespace NativePBR
{
	namespace
	{
		constexpr uint64_t kDescriptorReserveBytes = 1024 * 1024;
		constexpr uint64_t kMaximumRecordTableBytes = 16ull * 1024 * 1024;

		uint64_t ClockMs()
		{
			return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		GpuTextureLocation LookupTexture(const DX12Native::MaterialResources& a_residency, const TextureRequest& a_request)
		{
			if (a_request.canonicalPath.empty())
				return { kGpuMissingTexture, 0 };
			const auto location = a_residency.locations.find(DX12Native::MaterialTextureLookupKey(a_request.canonicalPath));
			if (location == a_residency.locations.end())
				return { kGpuMissingTexture, 0 };
			const auto logical = a_residency.logicalLocations.find(location->second);
			if (logical == a_residency.logicalLocations.end())
				return { kGpuMissingTexture, 0 };
			auto result = logical->second;
			result[0] = GpuTextureDescriptorIndex(result[0], a_request.interpretation);
			return result;
		}

		std::shared_ptr<DX12Native::MaterialResources> SelectActiveResidency(
			const DX12Native::MaterialResources& a_previous, const RecipeGeneration& a_generation)
		{
			std::unordered_set<std::string> requested, canonical;
			for (const auto& [identity, recipe] : a_generation.recipes) {
				if (!recipe || recipe->identity != identity)
					continue;
				for (const auto& texture : recipe->textures)
					if (!texture.canonicalPath.empty()) {
						requested.insert(DX12Native::MaterialTextureLookupKey(texture.canonicalPath));
						canonical.insert(texture.canonicalPath);
					}
			}
			const auto remap = PlanGpuMaterialPageRemap(requested, a_previous.locations,
				a_previous.logicalLocations, a_previous.pools.size());
			auto active = std::make_shared<DX12Native::MaterialResources>();
			active->pendingUpload = a_previous.pendingUpload;
			for (size_t pool = 0; pool < remap.size(); ++pool)
				if (remap[pool] != UINT32_MAX) {
					active->pools.push_back(a_previous.pools[pool]);
					active->bytes += a_previous.pools[pool].bytes;
				}
			for (const auto& path : requested) {
				const auto location = a_previous.locations.find(path);
				if (location == a_previous.locations.end())
					continue;
				const auto logical = a_previous.logicalLocations.find(location->second);
				if (logical == a_previous.logicalLocations.end() || logical->second[0] >= remap.size() ||
					remap[logical->second[0]] == UINT32_MAX)
					continue;
				active->locations.emplace(path, location->second);
				active->logicalLocations.insert_or_assign(location->second,
					std::array<uint32_t, 2>{ remap[logical->second[0]], logical->second[1] });
			}
			for (const auto& ticket : a_previous.textureTickets)
				if (ticket && active->logicalLocations.contains(ticket->handle))
					active->textureTickets.push_back(ticket);
			active->textures = uint32_t(active->logicalLocations.size());
			active->verifiedTextureSources = a_previous.verifiedTextureSources.Select(canonical);
			return active;
		}

		void WriteTextureDescriptors(ID3D12Device* a_device, GpuMaterialResources& a_resources)
		{
			const D3D12_DESCRIPTOR_HEAP_DESC desc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				GpuMaterialResources::kTextureDescriptorCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			DX::ThrowIfFailed(a_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(a_resources.textureDescriptors.put())));
			a_resources.textureDescriptors->SetName(L"NativePBR::TextureDescriptors");
			const auto start = a_resources.textureDescriptors->GetCPUDescriptorHandleForHeapStart();
			const auto step = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (uint32_t view = 0; view < 2; ++view) {
				D3D12_CPU_DESCRIPTOR_HANDLE cpu{ start.ptr + uint64_t(view * DX12Native::kMaximumTexturePools) * step };
				for (uint32_t index = 0; index < DX12Native::kMaximumTexturePools; ++index) {
					const auto* pool = index < a_resources.residency->pools.size() ? &a_resources.residency->pools[index] : nullptr;
					D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
					srv.Format = pool ? (view ? DirectX::MakeSRGB(pool->format) : pool->format) : DXGI_FORMAT_R8G8B8A8_UNORM;
					srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
					srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srv.Texture2DArray.MipLevels = pool ? pool->mips : 1;
					srv.Texture2DArray.ArraySize = pool ? pool->slices : 1;
					a_device->CreateShaderResourceView(pool ? pool->resource.get() : nullptr, &srv, cpu);
					cpu.ptr += step;
				}
			}
		}
	}

	std::optional<GpuMaterialLookup> GpuMaterialResources::Find(uint64_t a_identity, uint64_t a_revision) const
	{
		const auto found = indices.find(a_identity);
		if (found == indices.end())
			return std::nullopt;
		const auto& entry = entries[found->second];
		if (a_revision && entry.revision != a_revision)
			return std::nullopt;
		return entry;
	}

	GpuMaterialView GpuMaterialResources::MakeView() const
	{
		GpuMaterialView result;
		result.generation = shared_from_this();
		if (records && textureDescriptors) {
			result.recordAddress = records->GetGPUVirtualAddress();
			result.descriptorHeap = textureDescriptors.get();
			result.textureTable = textureDescriptors->GetGPUDescriptorHandleForHeapStart();
			result.recordCount = uint32_t(entries.size());
			result.publicationRevision = generation ? generation->revision : 0;
		}
		return result;
	}

	void GpuMaterialResources::RetainUntil(winrt::com_ptr<ID3D12Fence> a_fence, uint64_t a_value) const
	{
		GpuMaterialRetirement::Global().RetainUntil(shared_from_this(), std::move(a_fence), a_value);
	}

	std::shared_ptr<const GpuMaterialResources> GpuMaterialResources::Create(const GraphicsBackend& a_backend,
		std::shared_ptr<const RecipeGeneration> a_generation, uint64_t a_budgetLimit, uint64_t a_batchLimit,
		std::shared_ptr<const GpuMaterialResources> a_previous,
		std::shared_ptr<const DX12Native::MaterialResources> a_sharedResidency,
		std::unordered_set<std::string> a_attemptedPaths)
	{
		if (!a_backend.d3d12Device || !a_backend.commandQueue || !a_generation || !a_generation->batchesComplete)
			throw std::runtime_error("NativePBR GPU preparation requires a complete immutable recipe generation and DX12 backend");
		auto result = std::make_shared<GpuMaterialResources>();
		result->device = a_backend.d3d12Device;
		result->generation = std::move(a_generation);
		if (a_previous && a_previous->device.get() != a_backend.d3d12Device.get())
			a_previous.reset();
		const auto* inherited = a_previous ? a_previous->residency.get() : a_sharedResidency.get();
		if (!a_previous && a_sharedResidency) {
			for (const auto& page : a_sharedResidency->pools) {
				winrt::com_ptr<ID3D12Device> owner;
				if (!page.resource || FAILED(page.resource->GetDevice(IID_PPV_ARGS(owner.put()))) ||
					owner.get() != a_backend.d3d12Device.get())
					throw std::runtime_error("NativePBR shared DDS residency belongs to a different DX12 device");
			}
		}
		std::shared_ptr<DX12Native::MaterialResources> activeResidency;
		if (inherited) {
			activeResidency = SelectActiveResidency(*inherited, *result->generation);
			result->reusedPoolCount = uint32_t(activeResidency->pools.size());
			result->prunedPoolCount = uint32_t(inherited->pools.size() - activeResidency->pools.size());
			inherited = activeResidency.get();
		}
		std::vector<uint64_t> identities;
		identities.reserve(result->generation->recipes.size());
		for (const auto& [identity, recipe] : result->generation->recipes)
			if (identity && recipe && recipe->identity == identity && recipe->revision)
				identities.push_back(identity);
		result->unindexableRecipes = uint32_t(result->generation->recipes.size() - identities.size());
		std::ranges::sort(identities);
		const auto tableSize = std::max<size_t>(1, identities.size()) * sizeof(GpuMaterialRecord);
		const auto tableDesc = CD3DX12_RESOURCE_DESC::Buffer(tableSize);
		result->tableBytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &tableDesc).SizeInBytes;
		if (result->tableBytes == UINT64_MAX || result->tableBytes > kMaximumRecordTableBytes)
			throw std::runtime_error("NativePBR material record table exceeds maximum allocation");
		DX12Native::VideoMemoryBudget memoryBudget;
		const auto usable = memoryBudget.Query(a_backend.d3d12Device.get(), inherited ? inherited->bytes : 0, a_budgetLimit);
		if (!usable.dxgiAvailable || result->tableBytes > usable.effectiveBytes ||
			kDescriptorReserveBytes > usable.effectiveBytes - result->tableBytes)
			throw std::runtime_error("NativePBR material residency deferred by DXGI memory pressure");
		DX12Native::MaterialSource source;
		source.budget = usable.effectiveBytes - result->tableBytes - kDescriptorReserveBytes;
		source.uploadBatchBytes = a_batchLimit;
		source.usePreparedTextureManifest = false;
		source.attemptedRuntimePaths = std::move(a_attemptedPaths);
		LiveMatchResult matches;
		matches.matches.reserve(identities.size());
		for (const auto identity : identities)
			matches.matches.emplace_back(VerifiedLiveMatch{ result->generation->recipes.at(identity), {} });
		result->residency = DX12Native::MaterialResources::LoadResidency(a_backend, source, &matches, inherited, true);
		result->deferredTextureSources = result->residency->looseDdsDeferred;
		result->rejectedTextureSources = result->residency->rejectedTextureSources +
			result->residency->looseDdsRejected + result->residency->archiveDdsRejected;
		result->firstTextureSourceFailure = result->residency->firstTextureSourceFailure.empty() ?
			result->residency->failure : result->residency->firstTextureSourceFailure;

		std::vector<GpuMaterialRecord> records;
		records.reserve(std::max<size_t>(1, identities.size()));
		for (const auto identity : identities) {
			const auto& recipe = *result->generation->recipes.at(identity);
			GpuTextureLocations locations{};
			for (size_t role = 0; role < locations.size(); ++role)
				locations[role] = LookupTexture(*result->residency, recipe.textures[role]);
			const auto record = PackGpuMaterialRecord(recipe, locations);
			const auto index = uint32_t(records.size());
			result->indices.emplace(identity, index);
			const auto requestedRoles = GpuRequestedTextureRoles(recipe);
			const auto availability = ClassifyGpuMaterialAvailability(record.controls[0], requestedRoles);
			result->entries.push_back({ index, record.controls[0], requestedRoles, EffectiveNativeIssues(recipe),
				recipe.revision, availability });
			if (availability == GpuMaterialAvailability::Resident)
				++result->fullyResident;
			else if (availability == GpuMaterialAvailability::PartiallyResident)
				++result->partiallyResident;
			else
				++result->unavailable;
			if ((EffectiveNativeIssues(recipe) & ~(UnknownOpacity | UnknownNormalMode |
				UnknownUvAnimation | UnmatchedSceneInstance)) != 0)
				++result->featureLimited;
			records.push_back(record);
		}
		if (records.empty())
			records.emplace_back();
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			&tableDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(result->records.put())));
		result->records->SetName(L"NativePBR::GpuMaterialRecords");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{};
		DX::ThrowIfFailed(result->records->Map(0, &noRead, &mapped));
		std::memcpy(mapped, records.data(), records.size() * sizeof(GpuMaterialRecord));
		result->records->Unmap(0, nullptr);
		WriteTextureDescriptors(a_backend.d3d12Device.get(), *result);
		return result;
	}

	struct GpuMaterialWorker::State
	{
		std::atomic<bool> done{ false };
		uint64_t revision = 0;
		std::shared_ptr<const GpuMaterialResources> resources;
		std::string failure;
		bool deferred = false;
	};

	void GpuMaterialWorker::Request(const GraphicsBackend& a_backend,
		std::shared_ptr<const RecipeGeneration> a_generation, uint64_t a_budgetLimit,
		uint64_t a_batchLimit,
		std::shared_ptr<const DX12Native::MaterialResources> a_sharedResidency)
	{
		if (desired == a_generation && budgetLimit == a_budgetLimit && batchLimit == a_batchLimit &&
			backend.d3d12Device.get() == a_backend.d3d12Device.get() &&
			backend.commandQueue.get() == a_backend.commandQueue.get() && initialResidency == a_sharedResidency)
			return;
		const bool sameDevice = backend.d3d12Device.get() == a_backend.d3d12Device.get();
		++requestRevision;
		desired = std::move(a_generation);
		initialResidency = std::move(a_sharedResidency);
		backend = a_backend;
		budgetLimit = a_budgetLimit;
		batchLimit = a_batchLimit;
		textureProgress.Reset();
		nextBatchAtMs = 0;
		attempted = false;
		retry.Reset();
		failure.clear();
		if (!sameDevice)
			published.reset();
	}

	void GpuMaterialWorker::Advance()
	{
		GpuMaterialRetirement::Global().Collect();
		DX12Native::RetireMaterialUploads();
		if (pending) {
			if (!pending->done.load(std::memory_order_acquire))
				return;
			if (pending->resources && pending->resources->residency->pendingUpload) {
				const auto status = pending->resources->residency->pendingUpload->Poll();
				if (status == DX12Native::MaterialUploadStatus::Pending)
					return;
				if (status == DX12Native::MaterialUploadStatus::Failed) {
					pending->resources.reset();
					pending->failure = "NativePBR texture upload did not complete";
				}
			}
			if (pending->revision == requestRevision) {
				if (pending->resources) {
					const auto& residency = *pending->resources->residency;
					std::unordered_set<std::string> resident;
					for (const auto& path : residency.looseDdsAttemptedPaths)
						if (residency.locations.contains(DX12Native::MaterialTextureLookupKey(path)))
							resident.insert(path);
					textureProgress.Record(residency.looseDdsAttemptedPaths, resident);
					published = pending->resources;
					if (residency.looseDdsDeferred && !residency.looseDdsAttemptedPaths.empty()) {
						attempted = false;
						nextBatchAtMs = ClockMs() + 500;
					} else {
						nextBatchAtMs = 0;
					}
				}
				failure = pending->failure;
				if (pending->deferred)
					retry.Start(ClockMs());
			}
			pending.reset();
		}
		if (retry.Waiting()) {
			const auto status = retry.Poll(DX12Native::MaterialUploadsReady(), ClockMs());
			if (status == DX12Native::MaterialUploadStatus::Pending)
				return;
			if (status == DX12Native::MaterialUploadStatus::Failed) {
				failure = "Previous material upload did not retire";
				return;
			}
			attempted = false;
			failure.clear();
		}
		if (!desired || attempted || (nextBatchAtMs && ClockMs() < nextBatchAtMs))
			return;
		nextBatchAtMs = 0;
		auto task = std::make_shared<State>();
		task->revision = requestRevision;
		try {
			static BS::thread_pool<> pool{ 1 };
			pool.detach_task([task, backend = backend, recipes = desired, limit = budgetLimit,
				batch = batchLimit, previous = published,
				shared = initialResidency, attempts = textureProgress.Attempted()] {
				try {
					task->resources = GpuMaterialResources::Create(backend, recipes, limit, batch,
						previous, shared, attempts);
				} catch (const DX12Native::MaterialUploadBusy& error) {
					task->failure = error.what();
					task->deferred = true;
				} catch (const std::exception& error) {
					task->failure = error.what();
				} catch (...) {
					task->failure = "Unknown NativePBR GPU preparation failure";
				}
				task->done.store(true, std::memory_order_release);
			});
			pending = std::move(task);
			attempted = true;
		} catch (const std::exception& error) {
			failure = error.what();
			attempted = true;
		}
	}

	void GpuMaterialWorker::Reset()
	{
		++requestRevision;
		desired.reset();
		initialResidency.reset();
		published.reset();
		textureProgress.Reset();
		nextBatchAtMs = 0;
		attempted = false;
		retry.Reset();
		failure.clear();
	}

	bool GpuMaterialWorker::UploadPending() const
	{
		return retry.Waiting() || (pending && pending->done.load(std::memory_order_acquire) &&
			pending->resources && pending->resources->residency->pendingUpload);
	}

	GpuMaterialRetirement& GpuMaterialRetirement::Global()
	{
		static auto* retirement = new GpuMaterialRetirement;
		return *retirement;
	}

	void GpuMaterialRetirement::RetainUntil(std::shared_ptr<const GpuMaterialResources> a_generation,
		winrt::com_ptr<ID3D12Fence> a_consumerFence, uint64_t a_value)
	{
		if (!a_generation || !a_consumerFence || !a_value)
			throw std::invalid_argument("NativePBR material retirement requires a generation and signaled consumer fence");
		winrt::com_ptr<ID3D12Device> fenceDevice;
		if (!a_generation->device || FAILED(a_consumerFence->GetDevice(IID_PPV_ARGS(fenceDevice.put()))) ||
			fenceDevice.get() != a_generation->device.get())
			throw std::invalid_argument("NativePBR consumer fence and material residency must use the same DX12 device");
		std::scoped_lock lock(mutex);
		pending.push_back({ std::move(a_generation), std::move(a_consumerFence), a_value });
	}

	void GpuMaterialRetirement::Collect()
	{
		std::scoped_lock lock(mutex);
		std::erase_if(pending, [](const Pending& entry) {
			const auto completed = entry.fence->GetCompletedValue();
			return Completed(completed, entry.value);
		});
	}
}
