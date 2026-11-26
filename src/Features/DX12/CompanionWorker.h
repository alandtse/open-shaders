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
		bool predictive = false;
		bool corner = false;  // Predictive ring corners have separate generation timing.
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
		bool predictive = false;
		bool corner = false;         // Predictive ring corners have separate generation timing.
		double preparationMs = 0.0;  // Complete worker-side preparation, including companion IPC and parsing.
	};

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

	private:
		void Run(std::stop_token stop);
		CompanionResult Prepare(const CompanionRequest& request, uint64_t requestGeneration, std::stop_token stop);

		mutable std::mutex mutex;
		std::condition_variable_any wake;
		std::optional<CompanionRequest> queued;
		std::optional<CompanionResult> completed;
		bool running = false;
		std::atomic<uint64_t> generation = 1;
		std::jthread worker;
	};
}
