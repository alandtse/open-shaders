#pragma once

#include "MaterialResources.h"
#include "VideoMemoryBudget.h"
#include "TruePBR/VerifiedBridge.h"
#include "RuntimeTextureProgress.h"

namespace DX12Native
{
	/** @brief Owns immutable geometry and material descriptor tables for shadow tracing. */
	struct ShadowBindings
	{
		static constexpr uint32_t kMaximumInstances = 8192;
		static constexpr uint32_t kFrameDescriptorCount = 3;
		static constexpr uint32_t kMaximumFrameSlots = 4;
		static constexpr uint32_t kGeometryDescriptorBase = kMaximumFrameSlots * kFrameDescriptorCount;
		static constexpr uint32_t kAttributeDescriptorBase = kGeometryDescriptorBase + kMaximumInstances;
		static constexpr uint32_t kTextureDescriptorBase = kAttributeDescriptorBase + kMaximumInstances;
		std::shared_ptr<const TopLevel> scene;
		std::shared_ptr<const MaterialResources> materials;
		winrt::com_ptr<ID3D12Resource> offsets;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;
		uint64_t bytes = 0, uploadBatchBudget = 0;
		VideoMemoryBudget::Snapshot materialBudget;
		/** @brief Creates binding tables on a worker; material upload completion must be polled before use. */
		static std::shared_ptr<const ShadowBindings> Create(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene,
            uint64_t a_budget, uint64_t a_uploadBatchBudget, std::shared_ptr<const NativePBR::LiveBindingSource> a_livePbr,
            bool a_importLoose, bool a_importArchive,
            std::shared_ptr<const ShadowBindings> a_previous = {},
            std::unordered_set<std::string> a_attemptedPaths = {});
	};
	/** @brief Loads shadow material bindings asynchronously with revision-safe publication. */
	class ShadowBindingWorker
	{
	public:
		/** @brief Selects one scene, deferring a new load until an earlier worker retires. */
		void Request(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene,
            uint64_t a_budget, uint64_t a_uploadBatchBudget, std::shared_ptr<const NativePBR::LiveBindingSource> a_livePbr = {},
            bool a_importLoose = false, bool a_importArchive = false);
		/** @brief Polls an owned worker without waiting and publishes only the selected revision. */
		void Advance();
		/** @brief Invalidates current bindings without waiting for the worker. */
		void Reset();
		/** @brief Retains the completed bindings for the selected scene. */
		std::shared_ptr<const ShadowBindings> Get() const { return published; }
		/** @brief Returns the most recent selected-scene load failure. */
		const std::string& Failure() const { return failure; }
        uint32_t StreamBatches() const { return textureProgress.Batches(); }
        size_t StreamAttempted() const { return textureProgress.AttemptedCount(); }
        uint32_t StreamSuccessful() const { return textureProgress.Successful(); }
        uint32_t StreamFailed() const { return textureProgress.Failed(); }
        bool StreamWaitingForNextBatch() const { return streamNextBatchAtMs != 0; }
		/** @brief Reports a completed worker awaiting its final GPU upload. */
		bool UploadPending() const;
	private:
		struct State;
		std::shared_ptr<State> pending;
		std::shared_ptr<const TopLevel> desired;
        std::shared_ptr<const NativePBR::LiveBindingSource> livePbr;
        uint64_t liveRevision = 0;
        bool importLoose = false, importArchive = false;
        RuntimeTextureProgress textureProgress;
        std::shared_ptr<const ShadowBindings> incrementalBase;
        uint64_t streamNextBatchAtMs = 0;
		std::shared_ptr<const ShadowBindings> published;
		GraphicsBackend backend;
		uint64_t revision = 0, budget = 0, uploadBatchBudget = 0;
		bool attempted = false;
		MaterialUploadRetry uploadRetry;
		std::string failure;
	};

}
