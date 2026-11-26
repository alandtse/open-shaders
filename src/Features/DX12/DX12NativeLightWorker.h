#pragma once

#include "DX12Resources.h"
#include "GraphicsBackend.h"
#include "NativeLightPublication.h"
#include "Utils/RuntimeTaskGate.h"

#include <atomic>
#include <memory>

namespace DX12Native
{
	struct NativeRuntimeEmitter
	{
		DX12::PreparedLight value;
		uint32_t placedReference = 0;
	};

	struct NativeLightPublication
	{
		uint32_t scope = 0, cell = 0, preparedCount = 0;
		uint64_t revision = 0;
		std::vector<DX12::PreparedLight> lights;
		winrt::com_ptr<ID3D12Resource> buffer;
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t fenceValue = 0;
	};

	/** @brief Immutable light revisions on the shared DX12 task gate. */
	class NativeLightWorker
	{
	public:
		struct Stats
		{
			uint64_t tasks = 0, publications = 0, unchanged = 0, deferred = 0;
			bool busy = false;
		};

		/** @brief Drop the visit-local state without waiting for pending work. */
		void Reset();
		/** @brief Attempt one bounded asynchronous light revision. */
		bool Request(const GraphicsBackend& backend, std::shared_ptr<const DX12::Grid> grid,
			std::shared_ptr<const std::vector<NativeRuntimeEmitter>> runtime, NativeLightSettings settings, uint32_t cell);
		/** @brief Return the newest completed GPU publication, never wait. */
		std::shared_ptr<const NativeLightPublication> GetReady();
		/** @brief Failure requires a retry of the most recent input. */
		bool NeedsRetry() const;
		Stats GetStats() const;

	private:
		struct State;
		std::shared_ptr<State> state;
		std::shared_ptr<const NativeLightPublication> ready;
	};
}
