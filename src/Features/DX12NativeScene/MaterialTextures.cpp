#include "MaterialResources.h"
#include "MaterialPageCache.h"
#include "MaterialDiskCache.h"
#include "TextureSourceBatch.h"
#include "LiveLooseTexturePlan.h"
#include "MaterialTextureLookup.h"
#include "ArchiveDdsSource.h"
#include "VideoMemoryBudget.h"

#include "Utils/FileDigest.h"
#include <DirectXTex.h>
#include <fstream>
#include <map>
#include <tuple>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <unordered_set>

namespace DX12Native
{
	namespace
	{
		constexpr uint64_t kMaximumDdsBytes = 128ull * 1024 * 1024;
		constexpr uint64_t kMaximumRetainedPageBytes = 64ull * 1024 * 1024;
		constexpr uint64_t kMaximumCacheWritesPerLoad = 8ull * 1024 * 1024;
		constexpr uint32_t kMaximumCacheEntriesPerLoad = 8;
		constexpr uint32_t kPageSlices = 8;
        constexpr uint64_t kLooseDdsLoadLimit = 32ull * 1024 * 1024;
		struct Texture
		{
			std::string hash;
			std::vector<std::string> paths;
            std::filesystem::path looseSource;
            std::string archiveSource;
			DirectX::TexMetadata metadata{};
			DirectX::ScratchImage image;
		};

		struct CachedPages
		{
			std::mutex mutex;
			winrt::com_ptr<ID3D12Device> device;
			MaterialPageCache<MaterialTexturePool> pages;
		};

		CachedPages& PageCache()
		{
			// The worker can outlive static destruction, so retained pages need process lifetime.
			static auto* cache = new CachedPages;
			return *cache;
		}

		std::string TextureKey(const Texture& a_texture)
		{
			const auto& meta = a_texture.metadata;
			auto key = std::format("{}:{}:{}:{}:", meta.width, meta.height, meta.mipLevels, uint32_t(DirectX::MakeLinear(meta.format)));
			auto hash = a_texture.hash;
			std::ranges::transform(hash, hash.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			key += hash;
			return key;
		}

		std::string PageKey(const std::vector<std::string>& a_slices)
		{
			std::string key;
			for (const auto& slice : a_slices) { key += slice; key.push_back('\n'); }
			return key;
		}

		MaterialDiskCache& DiskCache()
		{
			// A loading worker may still access the cache during static destruction.
			static auto* cache = new MaterialDiskCache{ "Data/SKSE/Plugins/CommunityShaders/NativePBRCache/textures" };
			return *cache;
		}

		std::vector<uint8_t> ReadTexture(const std::filesystem::path& a_directory, const std::string& a_hash, MaterialResources& a_result, bool* a_fromCache = nullptr)
		{
			if (a_fromCache) *a_fromCache = false;
			if (a_hash.size() != 64 || !std::ranges::all_of(a_hash, [](unsigned char c) { return std::isxdigit(c) != 0; }))
				throw std::runtime_error("Invalid material texture digest");
			const auto path = a_directory / "textures" / (a_hash + ".dds");
			const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				if (auto cached = DiskCache().Read(a_hash, kMaximumDdsBytes)) { if (a_fromCache) *a_fromCache = true; ++a_result.diskCacheHits; return std::move(*cached); }
				throw std::runtime_error("Material texture unavailable");
			}
			const SKSE::stl::scope_exit close([&]() noexcept { CloseHandle(file); });
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || size.QuadPart < 128 || uint64_t(size.QuadPart) > kMaximumDdsBytes) {
				if (auto cached = DiskCache().Read(a_hash, kMaximumDdsBytes)) { if (a_fromCache) *a_fromCache = true; ++a_result.diskCacheHits; return std::move(*cached); }
				throw std::runtime_error("Material DDS byte limit exceeded");
			}
			const auto hash = Util::FileDigest::Sha256HandleHex(file);
			if (!hash || _stricmp(hash->c_str(), a_hash.c_str()) != 0) {
				if (auto cached = DiskCache().Read(a_hash, kMaximumDdsBytes)) { if (a_fromCache) *a_fromCache = true; ++a_result.diskCacheHits; return std::move(*cached); }
				throw std::runtime_error("Material DDS digest mismatch");
			}
			if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN)) throw std::runtime_error("Material DDS seek failed");
			std::vector<uint8_t> bytes(size_t(size.QuadPart));
			DWORD read = 0;
			if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size()) {
				if (auto cached = DiskCache().Read(a_hash, kMaximumDdsBytes)) { if (a_fromCache) *a_fromCache = true; ++a_result.diskCacheHits; return std::move(*cached); }
				throw std::runtime_error("Material DDS read failed");
			}
			return bytes;
		}


        /** @brief Reopens loose DDS with a non-writable file handle and verifies every upload read.
         *
         * Paths are pre-normalized relative to Data; this intentionally does not read archives
         * or capture live DX11 objects. Reparse-point files are excluded from the opt-in path.
         */
        std::pair<std::vector<uint8_t>, std::string> ReadLooseDds(
            const std::filesystem::path& a_path, std::string_view a_expected = {})
        {
            const auto file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Loose DDS not available as a regular file");
            const SKSE::stl::scope_exit close([&]() noexcept { CloseHandle(file); });
            BY_HANDLE_FILE_INFORMATION info{};
            LARGE_INTEGER fileSize{};
            if (!GetFileInformationByHandle(file, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 128 ||
                uint64_t(fileSize.QuadPart) > kLooseDdsLoadLimit)
                throw std::runtime_error("Loose DDS invalid or exceeds per-source limit");
            auto digest = Util::FileDigest::Sha256HandleHex(file);
            if (!digest) throw std::runtime_error("Loose DDS digest unavailable");
            std::ranges::transform(*digest, digest->begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (!a_expected.empty() && *digest != a_expected)
                throw std::runtime_error("Loose DDS changed after source verification");
            if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN))
                throw std::runtime_error("Loose DDS seek failed");
            std::vector<uint8_t> bytes(size_t(fileSize.QuadPart));
            DWORD read = 0;
            if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size())
                throw std::runtime_error("Loose DDS read failed");
            return { std::move(bytes), std::move(*digest) };
        }

		struct Upload
		{
			winrt::com_ptr<ID3D12Resource> target, staging;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12Fence> fence;
			winrt::handle event;
			std::vector<Upload> pages;
			uint64_t stagingBytes = 0;
			bool submitted = false;
		};

		struct UploadRetirement
		{
			std::mutex mutex;
			std::optional<Upload> pending;
			winrt::com_ptr<ID3D12Device> device;
		};

		UploadRetirement& RetiredUpload()
		{
			// Process-lifetime ownership prevents shutdown from releasing an in-flight upload.
			static auto* retirement = new UploadRetirement;
			return *retirement;
		}

		void RetireCompletedUpload(UploadRetirement& a_retirement)
		{
			if (!a_retirement.pending) return;
			const auto done = a_retirement.pending->fence->GetCompletedValue();
			if (done < 1 && SUCCEEDED(a_retirement.device->GetDeviceRemovedReason())) return;
			a_retirement.pending.reset();
			a_retirement.device = nullptr;
		}

		struct AsyncUpload final : MaterialUploadCompletion
		{
			winrt::com_ptr<ID3D12Fence> fence;
			std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			explicit AsyncUpload(winrt::com_ptr<ID3D12Fence> a_fence) : fence(std::move(a_fence)) {}
			MaterialUploadStatus Poll() override
			{
				if (!fence) return MaterialUploadStatus::Ready;
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
				const auto status = EvaluateMaterialUpload(fence->GetCompletedValue(), uint64_t(elapsed), kMaterialUploadTimeoutMs);
				if (status == MaterialUploadStatus::Ready) fence = nullptr;
				return status;
			}
		};

		uint64_t ValidateMetadata(const DirectX::TexMetadata& a_meta, uint64_t a_budget)
		{
			if (a_meta.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || a_meta.arraySize != 1 || a_meta.IsCubemap() || !a_meta.width || !a_meta.height ||
				a_meta.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_meta.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !a_meta.mipLevels || a_meta.mipLevels > 15)
				throw std::runtime_error("Unsupported material texture shape");
			uint64_t bytes = 0;
			for (size_t mip = 0; mip < a_meta.mipLevels; ++mip) {
				size_t rowPitch = 0, slicePitch = 0;
				DX::ThrowIfFailed(DirectX::ComputePitch(a_meta.format, std::max(size_t(1), a_meta.width >> mip), std::max(size_t(1), a_meta.height >> mip), rowPitch, slicePitch));
				if (slicePitch > kMaximumDdsBytes || bytes > kMaximumDdsBytes - slicePitch) throw std::runtime_error("Decoded material DDS exceeds limit");
				bytes += slicePitch;
			}
			if (bytes > a_budget) throw std::runtime_error("Decoded material DDS exceeds budget");
			return bytes;
		}

		void PublishPage(const std::vector<std::pair<Texture*, uint32_t>>& a_textures, MaterialResources& a_result, MaterialTexturePool a_page)
		{
			const auto pool = uint32_t(a_result.pools.size());
			static LogicalTextureRegistry identities;
			for (const auto& [texture, slice] : a_textures) {
				const auto& meta = texture->metadata;
				auto digest = texture->hash;
				std::ranges::transform(digest, digest.begin(), [](unsigned char c) { return char(std::tolower(c)); });
				auto ticket = identities.Acquire({ std::move(digest), meta.width, meta.height, uint32_t(a_page.format), uint32_t(meta.mipLevels) });
				if (!ticket) throw std::runtime_error("Material logical texture identity limit reached");
				a_result.logicalLocations.emplace(ticket->handle, std::array<uint32_t, 2>{ pool, slice });
				for (const auto& path : texture->paths) {
                    a_result.locations.emplace(path, ticket->handle);
                    a_result.locations.emplace(MaterialTextureLookupKey(path), ticket->handle);
                    // The digest was validated against DDS bytes before this page was made resident.
                    a_result.verifiedTextureSources.Observe(path, texture->hash,
                        uint32_t(meta.width), uint32_t(meta.height), uint32_t(meta.mipLevels));
                }
				a_result.textureTickets.push_back(std::move(ticket));
			}
			a_result.bytes += a_page.bytes;
			a_result.textures += uint32_t(a_textures.size());
			a_result.pools.push_back(std::move(a_page));
		}

		void UploadPage(const GraphicsBackend& a_backend, const std::vector<Texture*>& a_textures, MaterialResources& a_result, uint64_t a_budget, bool a_async, Upload* a_batch, uint64_t a_batchLimit)
		{
			auto& retirement = RetiredUpload();
			std::scoped_lock retirementLock(retirement.mutex);
			RetireCompletedUpload(retirement);
			if (retirement.pending) throw MaterialUploadBusy("Previous material upload is still in flight");
			const auto& meta = a_textures.front()->metadata;
			const auto format = DirectX::MakeLinear(meta.format);
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DirectX::MakeTypeless(format), meta.width, uint32_t(meta.height), uint16_t(a_textures.size()), uint16_t(meta.mipLevels));
			const auto allocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			const auto subresources = uint32_t(a_textures.size() * meta.mipLevels);
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
			std::vector<uint32_t> rows(subresources);
			std::vector<uint64_t> rowBytes(subresources);
			uint64_t uploadBytes = 0;
			a_backend.d3d12Device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), rowBytes.data(), &uploadBytes);
			const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBytes);
			const auto stagingAllocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &stagingDesc).SizeInBytes;
			if (!a_budget) throw std::runtime_error("Material texture budget is zero");
			VideoMemoryBudget budget;
			const auto batchStaging = a_batch ? a_batch->stagingBytes : 0;
			if (a_batch && !MaterialUploadFitsBatch(batchStaging, stagingAllocation, a_batchLimit))
				throw std::runtime_error("Material upload batch staging limit exceeded");
			if (batchStaging > a_budget || a_result.bytes > a_budget - batchStaging) throw std::runtime_error("Material batch budget exceeded");
			a_budget = budget.Query(a_backend.d3d12Device.get(), a_result.bytes + batchStaging, a_budget).effectiveBytes;
			if (allocation == UINT64_MAX || stagingAllocation == UINT64_MAX || allocation > a_budget || stagingAllocation > a_budget - allocation ||
				batchStaging > a_budget - allocation - stagingAllocation ||
				a_result.bytes > a_budget - allocation - stagingAllocation - batchStaging) throw std::runtime_error("Material texture/staging budget exceeded");
			Upload upload;
			const SKSE::stl::scope_exit retainSubmitted([&]() noexcept {
				if (upload.submitted && upload.fence && upload.fence->GetCompletedValue() < 1) {
					retirement.device = a_backend.d3d12Device;
					retirement.pending.emplace(std::move(upload));
				}
			});
			const auto deviceHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			const auto stagingHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&deviceHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(upload.target.put())));
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&stagingHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.staging.put())));
			upload.target->SetName(L"DX12NativeScene::MaterialTexturePool");
			upload.staging->SetName(L"DX12NativeScene::MaterialTextureStaging");
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			DX::ThrowIfFailed(upload.staging->Map(0, &noRead, &mapped));
			for (uint32_t subresource = 0; subresource < subresources; ++subresource) {
				const auto* image = a_textures[subresource / meta.mipLevels]->image.GetImage(subresource % meta.mipLevels, 0, 0);
				if (!image || image->rowPitch < rowBytes[subresource] || image->slicePitch < image->rowPitch * rows[subresource]) {
					upload.staging->Unmap(0, nullptr);
					throw std::runtime_error("Material DDS subresource layout mismatch");
				}
				for (uint32_t row = 0; row < rows[subresource]; ++row)
					std::memcpy(static_cast<uint8_t*>(mapped) + footprints[subresource].Offset + uint64_t(row) * footprints[subresource].Footprint.RowPitch,
						image->pixels + uint64_t(row) * image->rowPitch, size_t(rowBytes[subresource]));
			}
			upload.staging->Unmap(0, nullptr);
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(upload.allocator.put())));
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload.allocator.get(), nullptr, IID_PPV_ARGS(upload.commands.put())));
			upload.commands->SetName(L"DX12NativeScene::MaterialTextureUpload");
			for (uint32_t subresource = 0; subresource < subresources; ++subresource) {
				const auto destination = CD3DX12_TEXTURE_COPY_LOCATION(upload.target.get(), subresource);
				const auto source = CD3DX12_TEXTURE_COPY_LOCATION(upload.staging.get(), footprints[subresource]);
				upload.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			}
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(upload.target.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			upload.commands->ResourceBarrier(1, &barrier);
			DX::ThrowIfFailed(upload.commands->Close());
			if (a_batch) {
				std::vector<std::pair<Texture*, uint32_t>> slices;
				for (uint32_t slice = 0; slice < a_textures.size(); ++slice) slices.emplace_back(a_textures[slice], slice);
				PublishPage(slices, a_result, { upload.target, format, uint32_t(meta.mipLevels), uint32_t(a_textures.size()), allocation });
				a_batch->stagingBytes += stagingAllocation;
				a_result.peakStagingBytes = std::max(a_result.peakStagingBytes, a_batch->stagingBytes);
				a_result.uploadedBytes += uploadBytes;
				++a_result.uploadedPages;
				a_batch->pages.push_back(std::move(upload));
				return;
			}
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(upload.fence.put())));
			upload.event.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
			if (!upload.event) throw std::runtime_error("Material upload event unavailable");
			DX::ThrowIfFailed(upload.fence->SetEventOnCompletion(1, upload.event.get()));
			ID3D12CommandList* lists[]{ upload.commands.get() };
			a_backend.commandQueue->ExecuteCommandLists(1, lists);
			upload.submitted = true;
			if (FAILED(a_backend.commandQueue->Signal(upload.fence.get(), 1))) throw MaterialUploadError("Material upload queue signal failed");
			if (!a_async) {
				const auto waitStart = std::chrono::steady_clock::now();
				if (WaitForSingleObject(upload.event.get(), kMaterialUploadTimeoutMs) != WAIT_OBJECT_0 || upload.fence->GetCompletedValue() == UINT64_MAX)
					throw MaterialUploadError("Background material upload did not complete");
				const auto waitDuration = std::chrono::steady_clock::now() - waitStart;
				a_result.uploadWaitMicroseconds += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waitDuration).count());
			}
			a_result.peakStagingBytes = std::max(a_result.peakStagingBytes, stagingAllocation);
			a_result.uploadedBytes += uploadBytes;
			++a_result.uploadedPages;
			std::vector<std::pair<Texture*, uint32_t>> slices;
			for (uint32_t slice = 0; slice < a_textures.size(); ++slice) slices.emplace_back(a_textures[slice], slice);
			PublishPage(slices, a_result, { upload.target, format, uint32_t(meta.mipLevels), uint32_t(a_textures.size()), allocation });
			if (a_async) {
				a_result.pools.back().readyFence = upload.fence;
				a_result.pendingUpload = std::make_shared<AsyncUpload>(upload.fence);
				retirement.device = a_backend.d3d12Device;
				retirement.pending.emplace(std::move(upload));
			}
		}

		void SubmitUploadBatch(const GraphicsBackend& a_backend, MaterialResources& a_result, Upload& a_batch, size_t a_firstPool)
		{
			if (a_batch.pages.empty()) return;
			auto& retirement = RetiredUpload();
			std::scoped_lock retirementLock(retirement.mutex);
			RetireCompletedUpload(retirement);
			if (retirement.pending) throw MaterialUploadBusy("Previous material upload is still in flight");
			const SKSE::stl::scope_exit retainSubmitted([&]() noexcept {
				if (a_batch.submitted && a_batch.fence && a_batch.fence->GetCompletedValue() < 1) {
					retirement.device = a_backend.d3d12Device;
					retirement.pending.emplace(std::move(a_batch));
				}
			});
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(a_batch.fence.put())));
			std::vector<ID3D12CommandList*> lists;
			lists.reserve(a_batch.pages.size());
			for (const auto& page : a_batch.pages) lists.push_back(page.commands.get());
			a_backend.commandQueue->ExecuteCommandLists(uint32_t(lists.size()), lists.data());
			a_batch.submitted = true;
			if (FAILED(a_backend.commandQueue->Signal(a_batch.fence.get(), 1))) throw MaterialUploadError("Material upload batch signal failed");
			a_result.asyncBatchPages = uint32_t(a_batch.pages.size());
			a_result.asyncBatchStagingBytes = a_batch.stagingBytes;
			for (size_t index = a_firstPool; index < a_result.pools.size(); ++index) a_result.pools[index].readyFence = a_batch.fence;
			a_result.pendingUpload = std::make_shared<AsyncUpload>(a_batch.fence);
			retirement.device = a_backend.d3d12Device;
			retirement.pending.emplace(std::move(a_batch));
		}
	}

	void RetireMaterialUploads()
	{
		auto& retirement = RetiredUpload();
		std::unique_lock lock(retirement.mutex, std::try_to_lock);
		if (lock.owns_lock()) RetireCompletedUpload(retirement);
		static auto lastPressureCheck = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (now - lastPressureCheck < std::chrono::seconds(2)) return;
		lastPressureCheck = now;
		auto& cache = PageCache();
		std::unique_lock cacheLock(cache.mutex, std::try_to_lock);
		if (!cacheLock || !cache.device || !cache.pages.Bytes()) return;
		VideoMemoryBudget budget;
		const auto pressure = budget.Query(cache.device.get(), 0);
		if (FAILED(cache.device->GetDeviceRemovedReason()) || (pressure.dxgiAvailable && pressure.headroom <= pressure.reserve)) {
			cache.pages.Clear();
			cache.device = nullptr;
		}
	}

	std::optional<uint64_t> RetainedMaterialPageBytes()
	{
		auto& cache = PageCache();
		std::unique_lock lock(cache.mutex, std::try_to_lock);
		if (!lock) return std::nullopt;
		return cache.pages.Bytes();
	}

	bool MaterialUploadsReady()
	{
		auto& retirement = RetiredUpload();
		std::unique_lock lock(retirement.mutex, std::try_to_lock);
		if (!lock) return false;
		RetireCompletedUpload(retirement);
		return !retirement.pending;
	}

	void LoadMaterialTextures(const GraphicsBackend& a_backend, const MaterialSource& a_source, MaterialResources& a_result,
        const NativePBR::LiveMatchResult* a_matches)
	{
		if (!MaterialUploadsReady()) throw MaterialUploadBusy("Previous material upload is still in flight");
        // Material acquisition has two independent inputs:
        // (1) a legacy offline DDS manifest when fallback is still required;
        // (2) bounded runtime True PBR recipe sources (loose or archive).
        // Keeping the first optional permits its eventual retirement without
        // changing the native geometry/BLAS preparation protocol.
        json entries = json::array();
        if (a_source.usePreparedTextureManifest && a_source.textureReferences) {
            entries = *a_source.textureReferences; // External game Data/BSA references; no DDS cache files.
            if (!entries.is_array() || entries.size() > 4096)
                throw std::runtime_error("Prepared texture reference count exceeds limit");
        } else if (a_source.usePreparedTextureManifest) {
            const auto manifestPath = a_source.directory / "cell-textures.json";
            if (std::filesystem::file_size(manifestPath) > 16ull * 1024 * 1024)
                throw std::runtime_error("Texture manifest exceeds limit");
            std::ifstream stream(manifestPath);
            const auto manifest = json::parse(stream);
            if (manifest.at("schemaVersion") != 1 ||
                _stricmp(manifest.at("cell").get_ref<const std::string&>().c_str(), a_source.cell.c_str()) != 0 ||
                manifest.at("layoutDigest") != a_source.layoutDigest)
                throw std::runtime_error("Texture manifest identity mismatch");
            entries = manifest.at("textures");
            if (!entries.is_array() || entries.size() > 4096)
                throw std::runtime_error("Texture source count exceeds limit");
        } else if (!a_source.importLoosePbr || !a_matches) {
            // A runtime-only load requires a runtime-owned material generation.
            // Never silently construct empty tables due to absent provenance.
            throw std::runtime_error("Runtime material sources require live recipe matches and DDS acquisition");
        }
		std::map<std::string, std::unique_ptr<Texture>> textures;
		std::unordered_map<std::string, std::string> rejected;
		uint64_t sourceBytes = 0;
		uint64_t cacheWriteBytes = 0;
		uint32_t cacheWrites = 0;
		ProcessTextureSources(entries, [&](const auto& entry) {
			std::string hash;
			bool rejectHash = false;
			try {
				const auto status = entry.value("status", std::string{});
                const bool externalReference = status == "referenced";
                if (!externalReference && status != "cached")
                    throw std::runtime_error("Unresolved texture source: " + status.substr(0, 128));
                const auto texturePath = entry.at("path").get<std::string>();
                std::filesystem::path loosePath;
                std::string archivePath;
                std::vector<uint8_t> bytes;
                bool fromCache = false;
                if (externalReference) {
                    const auto canonical = NativePBR::CanonicalTexturePath(texturePath);
                    if (!canonical || *canonical != texturePath || !canonical->ends_with(".dds"))
                        throw std::runtime_error("Invalid external DDS reference");
                    loosePath = std::filesystem::path("Data") / std::filesystem::path(*canonical);
                    const auto attributes = GetFileAttributesW(loosePath.c_str());
                    const DWORD lookupError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                    const bool missingLoose = attributes == INVALID_FILE_ATTRIBUTES &&
                        (lookupError == ERROR_FILE_NOT_FOUND || lookupError == ERROR_PATH_NOT_FOUND);
                    if (missingLoose) {
                        auto archived = ReadArchiveDds(*canonical);
                        bytes = std::move(archived.bytes);
                        hash = std::move(archived.digest);
                        archivePath = *canonical;
                    } else {
                        auto loose = ReadLooseDds(loosePath);
                        bytes = std::move(loose.first);
                        hash = std::move(loose.second);
                    }
                } else {
                    hash = entry.at("sha256").get<std::string>();
                    if (entry.at("file") != "textures/" + hash + ".dds")
                        throw std::runtime_error("Texture cache path mismatch");
                    std::ranges::transform(hash, hash.begin(),
                        [](unsigned char c) { return char(std::tolower(c)); });
                    bytes = ReadTexture(a_source.directory, hash, a_result, &fromCache);
                }
                if (const auto prior = rejected.find(hash); prior != rejected.end())
                    throw std::runtime_error(prior->second);
                auto found = textures.find(hash);
                if (found == textures.end()) {
                    rejectHash = true;
                    auto texture = std::make_unique<Texture>();
                    texture->hash = hash;
                    if (externalReference) {
                        if (!archivePath.empty()) texture->archiveSource = archivePath;
                        else texture->looseSource = loosePath;
                    }
                    if (sourceBytes > a_source.budget || bytes.size() > a_source.budget - sourceBytes) throw std::runtime_error("DDS source budget exceeded");
					DX::ThrowIfFailed(DirectX::GetMetadataFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, texture->metadata));
					const auto& meta = texture->metadata;
					ValidateMetadata(meta, a_source.budget);
					D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ DirectX::MakeLinear(meta.format) };
					DX::ThrowIfFailed(a_backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
					constexpr auto required = D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_TEXTURE2D;
					if ((support.Support1 & required) != required) throw std::runtime_error("Material texture sampling unsupported");
					if (!externalReference && !fromCache && cacheWrites < kMaximumCacheEntriesPerLoad && bytes.size() <= kMaximumCacheWritesPerLoad - cacheWriteBytes) {
						try {
							if (!DiskCache().Known(hash)) {
								cacheWriteBytes += bytes.size(); ++cacheWrites;
								if (DiskCache().Store(hash, bytes) == MaterialDiskCache::StoreResult::Stored) ++a_result.diskCacheWrites;
							}
						} catch (const std::bad_alloc&) { throw; }
						catch (const std::exception&) {}
					}
					sourceBytes += bytes.size();
					found = textures.emplace(hash, std::move(texture)).first;
					rejectHash = false;
				}
				found->second->paths.push_back(texturePath);
			} catch (const std::bad_alloc&) { throw; }
			catch (const std::exception& error) {
				if (rejectHash && hash.size() == 64) rejected.try_emplace(std::move(hash), std::string(error.what()).substr(0, 256));
				throw;
			}
		}, [&](const std::exception& error) {
			if (FAILED(a_backend.d3d12Device->GetDeviceRemovedReason())) throw MaterialUploadError("Material texture device removed");
			++a_result.rejectedTextureSources;
			if (a_result.firstTextureSourceFailure.empty()) a_result.firstTextureSourceFailure = std::string(error.what()).substr(0, 256);
		});
        if (a_source.importLoosePbr && a_matches) {
            std::unordered_set<std::string> present;
            for (const auto& [hash, texture] : textures)
                for (const auto& textureAlias : texture->paths)
                    if (const auto canonical = NativePBR::CanonicalTexturePath(textureAlias))
                        present.insert(*canonical);
            // A previously published material generation owns its immutable
            // pages. Those paths must not be reacquired in this batch.
            for (const auto& [residentPath, ticket] : a_result.locations)
                if (const auto canonical = NativePBR::CanonicalTexturePath(residentPath))
                    present.insert(*canonical);
            const auto planned = PlanLiveLooseTextures(*a_matches, present, 8,
                a_source.attemptedRuntimePaths);
            a_result.looseDdsAttemptedPaths = planned.paths;
            a_result.looseDdsPreviouslyAttempted = uint32_t(planned.alreadyAttempted);
            a_result.looseDdsPlanned = uint32_t(planned.paths.size());
            a_result.looseDdsDeferred = uint32_t(planned.deferred);
            a_result.looseDdsReadyRecipes = uint32_t(planned.supportedRecipes);
            a_result.looseDdsFeatureLimitedRecipes = uint32_t(planned.featureLimitedRecipes);
            a_result.looseDdsSkippedRecipes = uint32_t(planned.skippedRecipes);
            a_result.looseDdsPrepared = uint32_t(planned.alreadyPresent);
            a_result.looseDdsInvalid = uint32_t(planned.invalid);
            for (const auto& canonical : planned.paths) {
                bool triedArchive = false;
                try {
                    // Only canonical relative paths from uniquely matched PBR materials enter here.
                    const auto looseDdsPath = std::filesystem::path("Data") / std::filesystem::path(canonical);
                    std::vector<uint8_t> bytes;
                    std::string digest;
                    const auto attributes = GetFileAttributesW(looseDdsPath.c_str());
                    const DWORD lookupError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                    const bool missingLoose = attributes == INVALID_FILE_ATTRIBUTES &&
                        (lookupError == ERROR_FILE_NOT_FOUND || lookupError == ERROR_PATH_NOT_FOUND);
                    if (missingLoose && a_source.importArchivePbr) {
                        triedArchive = true;
                        ++a_result.archiveDdsPlanned;
                        auto archived = ReadArchiveDds(canonical);
                        bytes = std::move(archived.bytes);
                        digest = std::move(archived.digest);
                    } else {
                        auto loose = ReadLooseDds(looseDdsPath);
                        bytes = std::move(loose.first);
                        digest = std::move(loose.second);
                    }
                    if (sourceBytes > a_source.budget || bytes.size() > a_source.budget - sourceBytes ||
                        a_result.looseDdsBytes + a_result.archiveDdsBytes > kLooseDdsLoadLimit ||
                        bytes.size() > kLooseDdsLoadLimit - (a_result.looseDdsBytes + a_result.archiveDdsBytes))
                        throw std::runtime_error("Loose DDS source budget exhausted");
                    auto it = textures.find(digest);
                    if (it == textures.end()) {
                        auto image = std::make_unique<Texture>();
                        image->hash = digest;
                        if (triedArchive) image->archiveSource = canonical;
                        else image->looseSource = looseDdsPath;
                        DX::ThrowIfFailed(DirectX::GetMetadataFromDDSMemory(bytes.data(), bytes.size(),
                            DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, image->metadata));
                        ValidateMetadata(image->metadata, a_source.budget);
                        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ DirectX::MakeLinear(image->metadata.format) };
                        DX::ThrowIfFailed(a_backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                            &support, sizeof(support)));
                        constexpr auto required = D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_TEXTURE2D;
                        if ((support.Support1 & required) != required)
                            throw std::runtime_error("Loose DDS texture format unsupported");
                        it = textures.emplace(digest, std::move(image)).first;
                        sourceBytes += bytes.size();
                        if (triedArchive) a_result.archiveDdsBytes += bytes.size();
                        else a_result.looseDdsBytes += bytes.size();
                    }
                    it->second->paths.push_back(canonical);
                    if (triedArchive) ++a_result.archiveDdsImported;
                    else ++a_result.looseDdsImported;
                } catch (const std::bad_alloc&) { throw; }
                catch (const std::exception& error) {
                    if (triedArchive) ++a_result.archiveDdsRejected;
                    else ++a_result.looseDdsRejected;
                    if (a_result.firstTextureSourceFailure.empty())
                        a_result.firstTextureSourceFailure = std::string(error.what()).substr(0, 256);
                }
            }
        }
		using Key = std::tuple<size_t, size_t, size_t, DXGI_FORMAT>;
		auto groupKey = [](const Texture& texture) -> Key {
			const auto& meta = texture.metadata;
			return { meta.width, meta.height, meta.mipLevels, DirectX::MakeLinear(meta.format) };
		};
		std::map<Key, size_t> remainingGroups;
		std::unordered_map<std::string, Texture*> remaining;
		for (const auto& [hash, texture] : textures) {
			remaining.emplace(TextureKey(*texture), texture.get());
			++remainingGroups[groupKey(*texture)];
		}
		if (remaining.empty()) return;
		auto& cache = PageCache();
		std::scoped_lock cacheLock(cache.mutex);
		if (cache.device.get() != a_backend.d3d12Device.get()) { cache.pages.Clear(); cache.device = a_backend.d3d12Device; }
		struct CompletedPage { std::string key; std::vector<std::string> slices; uint32_t index; };
		std::vector<CompletedPage> completed;
		std::map<std::pair<Key, uint16_t>, std::pair<uint64_t, uint64_t>> plannedAllocations;
		for (const auto& [hash, texture] : textures) {
			const auto textureKey = TextureKey(*texture);
			if (!remaining.contains(textureKey)) continue;
			auto page = cache.pages.TakeContaining(textureKey);
			if (!page || page->page.bytes > a_source.budget || a_result.bytes > a_source.budget - page->page.bytes) continue;
			if (page->page.readyFence) {
				const auto ready = page->page.readyFence->GetCompletedValue();
				if (ready == UINT64_MAX) throw MaterialUploadError("Cached material page device removed");
				if (ready < 1) {
					const auto bytes = page->page.bytes;
					cache.pages.Retain(std::move(page->key), std::move(page->page), bytes,
						kMaximumRetainedPageBytes, std::move(page->slices));
					throw MaterialUploadBusy("Cached material page is still in flight");
				}
				page->page.readyFence = nullptr;
			}
			std::vector<std::pair<Texture*, uint32_t>> selected;
			for (uint32_t slice = 0; slice < page->slices.size(); ++slice) {
				const auto found = remaining.find(page->slices[slice]);
				if (found != remaining.end() && slice < page->page.slices) selected.emplace_back(found->second, slice);
			}
			for (const auto& [item, slice] : selected) --remainingGroups[groupKey(*item)];
			size_t pagesNeeded = a_result.pools.size() + 1;
			uint64_t plannedBytes = a_result.bytes + page->page.bytes, peakStaging = 0;
			bool fits = true;
			for (const auto& [key, count] : remainingGroups) {
				pagesNeeded += (count + kPageSlices - 1) / kPageSlices;
				const auto& [width, height, mips, format] = key;
				for (size_t first = 0; first < count; first += kPageSlices) {
					const auto slices = uint16_t(std::min<size_t>(kPageSlices, count - first));
					const auto [info, inserted] = plannedAllocations.try_emplace({ key, slices });
					if (inserted) {
						const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DirectX::MakeTypeless(format), width, uint32_t(height), slices, uint16_t(mips));
						info->second.first = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
						uint64_t staging = 0;
						a_backend.d3d12Device->GetCopyableFootprints(&desc, 0, uint32_t(slices * mips), 0, nullptr, nullptr, nullptr, &staging);
						const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(staging);
						info->second.second = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &stagingDesc).SizeInBytes;
					}
					const auto [allocation, staging] = info->second;
					if (allocation == UINT64_MAX || staging == UINT64_MAX || allocation > a_source.budget - plannedBytes) { fits = false; break; }
					plannedBytes += allocation;
					peakStaging = std::max(peakStaging, staging);
				}
				if (!fits) break;
			}
			if (peakStaging > a_source.budget - plannedBytes) fits = false;
			if (selected.empty() || !fits || pagesNeeded > kMaximumTexturePools) {
				for (const auto& [item, slice] : selected) ++remainingGroups[groupKey(*item)];
				continue;
			}
			for (const auto& [item, slice] : selected) remaining.erase(TextureKey(*item));
			completed.push_back({ std::move(page->key), std::move(page->slices), uint32_t(a_result.pools.size()) });
			PublishPage(selected, a_result, std::move(page->page));
			++a_result.reusedPages;
			a_result.reusedTextures += uint32_t(selected.size());
		}
		cache.pages.Clear();
		std::map<Key, std::vector<Texture*>> groups;
		for (const auto& [hash, texture] : textures)
			if (remaining.contains(TextureKey(*texture))) groups[groupKey(*texture)].push_back(texture.get());
		bool batchable = !groups.empty();
		uint64_t batchStaging = 0, batchTargets = a_result.bytes;
		for (const auto& [key, group] : groups) {
			const auto& [width, height, mips, format] = key;
			for (size_t first = 0; first < group.size(); first += kPageSlices) {
				const auto slices = uint16_t(std::min<size_t>(kPageSlices, group.size() - first));
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DirectX::MakeTypeless(format), width, uint32_t(height), slices, uint16_t(mips));
				const auto target = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				uint64_t upload = 0;
				a_backend.d3d12Device->GetCopyableFootprints(&desc, 0, uint32_t(slices * mips), 0, nullptr, nullptr, nullptr, &upload);
				const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(upload);
				const auto staging = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &stagingDesc).SizeInBytes;
				if (target == UINT64_MAX || staging == UINT64_MAX || !MaterialUploadFitsBatch(batchStaging, staging, a_source.uploadBatchBytes) || target > a_source.budget ||
					batchTargets > a_source.budget - target) { batchable = false; break; }
				batchStaging += staging;
				batchTargets += target;
			}
			if (!batchable) break;
		}
		if (batchable && (batchTargets > a_source.budget || batchStaging > a_source.budget - batchTargets)) batchable = false;
		Upload batch;
		const auto firstBatchPool = a_result.pools.size();
		for (auto groupIt = groups.begin(); groupIt != groups.end(); ++groupIt) {
			const auto& group = groupIt->second;
			for (size_t first = 0; first < group.size(); first += kPageSlices) {
				if (a_result.pools.size() >= kMaximumTexturePools) throw std::runtime_error("Texture pool count exceeded");
				const std::vector<Texture*> candidates(group.begin() + first, group.begin() + std::min(first + kPageSlices, group.size()));
				std::vector<Texture*> page;
				Texture* current = nullptr;
				uint64_t decodedBytes = 0;
				ProcessTextureSources(candidates, [&](Texture* texture) {
					current = texture;
					const auto predicted = ValidateMetadata(texture->metadata, a_source.budget);
					if (decodedBytes > a_source.budget - predicted) throw std::runtime_error("Decoded material page exceeds budget");
					const auto bytes = !texture->archiveSource.empty() ?
                        ReadArchiveDds(texture->archiveSource, texture->hash).bytes :
                        !texture->looseSource.empty() ? ReadLooseDds(texture->looseSource, texture->hash).first :
                        ReadTexture(a_source.directory, texture->hash, a_result);
					DX::ThrowIfFailed(DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, nullptr, texture->image));
					const auto& decoded = texture->image.GetMetadata();
					ValidateMetadata(decoded, a_source.budget);
					const auto& expected = texture->metadata;
					if (decoded.width != expected.width || decoded.height != expected.height || decoded.mipLevels != expected.mipLevels || decoded.format != expected.format)
						throw std::runtime_error("Material DDS metadata changed during preparation");
					const auto size = texture->image.GetPixelsSize();
					if (size > a_source.budget || decodedBytes > a_source.budget - size) throw std::runtime_error("Decoded material page exceeds budget");
					decodedBytes += size;
					page.push_back(texture);
				}, [&](const std::exception& error) {
					if (FAILED(a_backend.d3d12Device->GetDeviceRemovedReason())) throw MaterialUploadError("Material texture device removed");
					if (current) { a_result.rejectedTextureSources += uint32_t(current->paths.size()); current->image.Release(); }
					if (a_result.firstTextureSourceFailure.empty()) a_result.firstTextureSourceFailure = std::string(error.what()).substr(0, 256);
				});
				if (page.empty()) continue;
				std::vector<std::string> slices;
				for (const auto* texture : page) slices.push_back(TextureKey(*texture));
				completed.push_back({ PageKey(slices), std::move(slices), uint32_t(a_result.pools.size()) });
				const bool lastPage = std::next(groupIt) == groups.end() && first + kPageSlices >= group.size();
				UploadPage(a_backend, page, a_result, a_source.budget, lastPage, batchable ? &batch : nullptr, a_source.uploadBatchBytes);
				for (auto* texture : page) texture->image.Release();
			}
		}
		if (batchable) SubmitUploadBatch(a_backend, a_result, batch, firstBatchPool);
		const auto retainLimit = std::min(kMaximumRetainedPageBytes, a_source.budget / 4);
		for (auto& entry : completed) {
			const auto& page = a_result.pools[entry.index];
			cache.pages.Retain(std::move(entry.key), page, page.bytes, retainLimit, std::move(entry.slices));
		}
	}
}
