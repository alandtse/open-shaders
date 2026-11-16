#pragma once

#include "GpuMaterialAbi.h"
#include "Features/DX12NativeScene/MaterialResources.h"
#include "Features/DX12NativeScene/RuntimeTextureProgress.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace NativePBR
{
	/** @brief A record index refers only to this immutable material generation. */
	struct GpuMaterialLookup
	{
		uint32_t index = 0, residentRoles = 0, requestedRoles = 0, recipeIssues = 0;
		uint64_t revision = 0;
		GpuMaterialAvailability availability = GpuMaterialAvailability::Unavailable;
	};

	struct GpuMaterialResources;
	/** @brief A separately retained screen or ray view of one immutable GPU generation. */
	struct GpuMaterialView
	{
		std::shared_ptr<const GpuMaterialResources> generation;
		D3D12_GPU_VIRTUAL_ADDRESS recordAddress = 0;
		/** @brief Bind this heap with SetDescriptorHeaps before using textureTable. Owned by generation. */
		ID3D12DescriptorHeap* descriptorHeap = nullptr;
		D3D12_GPU_DESCRIPTOR_HANDLE textureTable{};
		uint32_t recordCount = 0;
		uint64_t publicationRevision = 0;
		static constexpr uint32_t kRecordStride = sizeof(GpuMaterialRecord);
		static constexpr uint32_t kDescriptorCount = kGpuTextureDescriptorCapacity;
	};

	/** @brief Reusable screen/ray material records and DDS residency, without scene geometry. */
	struct GpuMaterialResources : std::enable_shared_from_this<GpuMaterialResources>
	{
		static constexpr uint32_t kTextureDescriptorCount = kGpuTextureDescriptorCapacity;
		static_assert(kTextureDescriptorCount == 512);
		static_assert(DX12Native::kMaximumTexturePools == kGpuTexturePoolCapacity);
		std::shared_ptr<const RecipeGeneration> generation;
		std::shared_ptr<const DX12Native::MaterialResources> residency;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12Resource> records;
		winrt::com_ptr<ID3D12DescriptorHeap> textureDescriptors;
		std::vector<GpuMaterialLookup> entries;
		std::unordered_map<uint64_t, uint32_t> indices;
		uint64_t tableBytes = 0;
		uint32_t fullyResident = 0, partiallyResident = 0, unavailable = 0, featureLimited = 0;
		uint32_t reusedPoolCount = 0, prunedPoolCount = 0, unindexableRecipes = 0;
		uint32_t deferredTextureSources = 0, rejectedTextureSources = 0;
		std::string firstTextureSourceFailure;

		/** @brief Resolve an identity for either a visible draw or an independently bound TLAS hit. */
		std::optional<GpuMaterialLookup> Find(uint64_t a_identity, uint64_t a_revision = 0) const;
		/** @brief Bind one published screen/ray view while retaining its generation during recording. */
		GpuMaterialView MakeView() const;
		/** @brief Retain material pages, records and descriptors until a submitted consumer fence completes. */
		void RetainUntil(winrt::com_ptr<ID3D12Fence> a_fence, uint64_t a_value) const;
		/** @brief Prepare immutable material records and shared texture pages on a worker. */
		static std::shared_ptr<const GpuMaterialResources> Create(const GraphicsBackend& a_backend,
			std::shared_ptr<const RecipeGeneration> a_generation, uint64_t a_budgetLimit,
			uint64_t a_batchLimit,
			std::shared_ptr<const GpuMaterialResources> a_previous = {},
			std::shared_ptr<const DX12Native::MaterialResources> a_sharedResidency = {},
			std::unordered_set<std::string> a_attemptedPaths = {});
	};

	/** @brief Owns background generation preparation and publishes after upload completion. */
	class GpuMaterialWorker
	{
	public:
		void Request(const GraphicsBackend& a_backend, std::shared_ptr<const RecipeGeneration> a_generation,
			uint64_t a_budgetLimit = 0, uint64_t a_batchLimit = 16ull * 1024 * 1024,
			std::shared_ptr<const DX12Native::MaterialResources> a_sharedResidency = {});
		void Advance();
		void Reset();
		std::shared_ptr<const GpuMaterialResources> Get() const { return published; }
		const std::string& Failure() const { return failure; }
		uint32_t StreamBatches() const { return textureProgress.Batches(); }
		uint32_t StreamSuccessful() const { return textureProgress.Successful(); }
		uint32_t StreamFailed() const { return textureProgress.Failed(); }
		bool UploadPending() const;

	private:
		struct State;
		std::shared_ptr<State> pending;
		std::shared_ptr<const RecipeGeneration> desired;
		std::shared_ptr<const DX12Native::MaterialResources> initialResidency;
		std::shared_ptr<const GpuMaterialResources> published;
		DX12Native::RuntimeTextureProgress textureProgress;
		GraphicsBackend backend;
		uint64_t requestRevision = 0, budgetLimit = 0, batchLimit = 0, nextBatchAtMs = 0;
		bool attempted = false;
		DX12Native::MaterialUploadRetry retry;
		std::string failure;
	};

	/** @brief Keeps generations alive until screen/ray consumer fences finish. */
	class GpuMaterialRetirement
	{
	public:
		static GpuMaterialRetirement& Global();
		static bool Completed(uint64_t a_completed, uint64_t a_target)
		{
			return a_completed == UINT64_MAX || a_completed >= a_target;
		}
		void RetainUntil(std::shared_ptr<const GpuMaterialResources> a_generation,
			winrt::com_ptr<ID3D12Fence> a_consumerFence, uint64_t a_value);
		void Collect();

	private:
		struct Pending
		{
			std::shared_ptr<const GpuMaterialResources> generation;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value;
		};
		std::mutex mutex;
		std::vector<Pending> pending;
	};
}
