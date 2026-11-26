#pragma once

#include "DX12Resources.h"

#include <condition_variable>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace DX12
{
	struct CompanionRequest
	{
		SubcellKey key;
		uint32_t size = 4096;
		uint32_t localScopeID = 0;
		std::string scopePlugin;
		bool worldspace = false;
		bool warmup = false;
		bool preloadLastSave = false;
		bool consolidate = false;
		bool predictive = false;
		bool corner = false;  // Predictive ring corners have separate generation timing.
		bool loading = false;
	};

	struct CompanionLight
	{
		std::string referencePlugin, basePlugin;
		uint32_t referenceLocal = 0, baseLocal = 0;
		PreparedLight value;
	};

	struct CompanionResult
	{
		CompanionPublication publication;
		std::vector<CompanionLight> lights;
		std::string failure;
		std::string residencyReason;
		bool startup = false, preload = false;
		std::string scopePlugin;
		uint32_t scopeLocal = 0, size = 0;
		bool predictive = false;
		bool corner = false;         // Predictive ring corners have separate generation timing.
		double preparationMs = 0.0;  // Complete worker-side preparation, including companion IPC and parsing.
	};

	struct CompanionPriority;

	/** @brief Single bounded producer; engine form resolution is performed by the caller on the game thread. */
	class CompanionWorker
	{
	public:
		CompanionWorker();
		~CompanionWorker();
		bool Idle() const;
		bool Enqueue(CompanionRequest request);
		std::optional<CompanionResult> Take();
		void Cancel();
		void CancelStartup();
		/** @brief Preempts ordinary work and prepares the destination grid until loading ends. */
		bool BeginLoading(CompanionRequest request, double x, double y, std::shared_ptr<const Grid> existing);
		/** @brief Revokes loading priority and cancels unfinished destination work. */
		void EndLoading();
		/** @brief Transfers completed destination results without resolving engine forms. */
		std::vector<std::unique_ptr<CompanionResult>> TakeLoadingResults();
		void Stop();

	private:
		struct LoadingBatch
		{
			CompanionRequest request;
			double x = 0, y = 0;
			std::shared_ptr<const Grid> existing;
		};
		void RunLoading(const LoadingBatch& batch, uint64_t epoch, std::stop_token stop);
		void Run(std::stop_token stop);
		CompanionResult Prepare(const CompanionRequest& request, uint64_t requestGeneration, std::stop_token stop);

		mutable std::mutex mutex;
		std::condition_variable_any wake;
		std::optional<CompanionRequest> queued;
		std::optional<CompanionResult> completed;
		bool running = false, runningStartup = false, runningLoading = false;
		std::optional<LoadingBatch> loadingQueued;
		std::optional<CompanionRequest> loadingTarget;
		std::vector<std::unique_ptr<CompanionResult>> loadingResults;
		std::atomic<uint64_t> generation = 1;
		std::unique_ptr<CompanionPriority> priorityState;
		std::jthread worker;
	};
}
