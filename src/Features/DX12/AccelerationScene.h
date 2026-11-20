#pragma once

#include "DX12Resources.h"

#include <atomic>
#include <condition_variable>
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <winrt/base.h>

namespace DX12
{
	/** @brief Completed scene only: no consumer ever observes a TLAS still being built. */
	struct AccelerationSceneReady
	{
		struct Bottom
		{
			std::shared_ptr<const Shape> shape;
			winrt::com_ptr<ID3D12Resource> acceleration;
		};
		winrt::com_ptr<ID3D12Resource> acceleration;
		std::vector<std::shared_ptr<const Bottom>> bottoms;
		std::unordered_set<uint64_t> shapeIDs;
		uint64_t geometryRevision = 0;
		uint32_t scope = 0;
		bool complete = false;
	};

	/** @brief Computes incremental BLAS and replacement TLAS on a private GPU queue and CPU worker. */
	class AccelerationScene
	{
	public:
		~AccelerationScene();
		bool Start(ID3D12Device* device);
		void Request(std::shared_ptr<const Grid> grid);
		void Clear();
		std::shared_ptr<const AccelerationSceneReady> Ready() const { return ready.load(std::memory_order_acquire); }

	private:
		using Bottom = AccelerationSceneReady::Bottom;
		winrt::com_ptr<ID3D12Resource> MakeBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap,
			D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) const;
		bool SubmitAndWait(ID3D12GraphicsCommandList4* list, std::stop_token stop);
		std::shared_ptr<const Bottom> BuildBottom(const std::shared_ptr<const Shape>& shape, std::stop_token stop);
		std::shared_ptr<AccelerationSceneReady> BuildTop(const std::vector<std::shared_ptr<const Bottom>>& bottoms,
			const Grid& grid, bool complete, std::stop_token stop);
		void Run(std::stop_token stop);

		winrt::com_ptr<ID3D12Device5> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t fenceValue = 0;
		std::mutex mutex;
		std::condition_variable_any wake;
		std::shared_ptr<const Grid> requested;
		uint64_t requestedRevision = 0;
		uint64_t generation = 0;
		std::atomic<std::shared_ptr<const AccelerationSceneReady>> ready;
		std::jthread worker;
	};
}
