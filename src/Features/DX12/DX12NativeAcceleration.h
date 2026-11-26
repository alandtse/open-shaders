#pragma once

#include "DX12NativeSceneInputs.h"
#include "GraphicsBackend.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Native
{
	struct BottomLevel
	{
		std::shared_ptr<const DX12::Shape> shape;
		winrt::com_ptr<ID3D12Resource> acceleration;
	};

	struct TopLevel
	{
		std::shared_ptr<const SceneInputs> inputs;
		std::vector<std::shared_ptr<const BottomLevel>> bottoms;
		std::unordered_multimap<uint64_t, const DX12::Shape*> casterCoverage;
		winrt::com_ptr<ID3D12Resource> acceleration;
		winrt::com_ptr<ID3D12Fence> producerFence;
		uint64_t producerValue = 0;
		uint64_t geometryRevision = 0;
		uint32_t scope = 0;
	};

	/** @brief Asynchronously advances rigid BLAS/TLAS builds on the native DX12 queue. */
	class AccelerationScene
	{
		friend class AccelerationWorker;

	public:
		~AccelerationScene();
		/** @brief Retains the shared device and the native scene's direct queue. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Invalidates the visible scene while letting submitted GPU work retire. */
		void Reset();
		/** @brief Returns only a GPU-completed TLAS, with all rigid casters retained. */
		std::shared_ptr<const TopLevel> GetScene() const { return published; }
		/** @brief Submitted builds still need GPU retirement even after scene reset. */
		bool HasPendingBuilds() const
		{
			for (const auto& build : builds)
				if (build.allocator)
					return true;
			return false;
		}
		/** @brief Reports an actual DX12 creation or submission error. */
		const std::string& Failure() const { return failure; }
		bool BudgetDeferred() const { return budgetDeferred; }

	private:
		/** @brief Only the serialized acceleration task may allocate or submit native AS work. */
		void Advance(std::shared_ptr<const SceneInputs> a_inputs);
		struct Build
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
			winrt::com_ptr<ID3D12Resource> sourceUpload;
			winrt::com_ptr<ID3D12Resource> scratch;
			std::shared_ptr<BottomLevel> bottom;
			std::shared_ptr<TopLevel> top;
			uint64_t fence = 0, generation = 0;
		};

		winrt::com_ptr<ID3D12Resource> Buffer(uint64_t a_size, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, const wchar_t* a_name) const;
		void Begin(Build& a_build);
		void Submit(Build& a_build);
		void Retire();
		bool BuildBottom(Build& a_build, const std::shared_ptr<const DX12::Shape>& a_shape);
		bool BuildTop(Build& a_build);
		bool HasPendingBottom(uint64_t a_identity, const DX12::Shape* a_shape) const;

		GraphicsBackend backend;
		mutable VideoMemoryBudget videoBudget;
		winrt::com_ptr<ID3D12Device5> device;
		winrt::com_ptr<ID3D12Fence> completion;
		std::array<Build, 2> builds;
		std::unordered_map<uint64_t, std::shared_ptr<const BottomLevel>> bottoms;
		std::shared_ptr<const SceneInputs> target;
		std::shared_ptr<const TopLevel> published;
		uint64_t generation = 0, nextFence = 0;
		uint64_t targetRevision = 0;
		uint32_t targetScope = 0;
		std::string failure;
		bool budgetDeferred = false;
	};

	/** @brief Task coordinator using the shared CPU pool. Snapshot delivery and completed-TLAS consumption never build on the caller. */
	class AccelerationWorker
	{
	public:
		AccelerationWorker();
		~AccelerationWorker();
		AccelerationWorker(const AccelerationWorker&) = delete;
		AccelerationWorker& operator=(const AccelerationWorker&) = delete;

		/** @brief Captures device references lazily; initialization runs on the shared pool. */
		bool Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
		/** @brief Replaces the pending grid snapshot; only the latest unpublished request is built. */
		void Request(std::shared_ptr<const DX12::Grid> a_grid, bool menuPriority = false);
		/** @brief Cancels eligibility without interrupting previously submitted GPU work. */
		void Reset();
		/** @brief Ends the session without waiting for queued CPU or GPU work. */
		void Stop();
		/** @brief Acquires the most recent completed immutable scene, never waiting for a build. */
		std::shared_ptr<const TopLevel> GetScene() const;
		/** @brief True when the matching whole-cell build exceeded available DXGI budget. */
		bool WholeCellBudgetDeferred(uint64_t revision) const;

	private:
		struct State;
		static void TrySchedule(const std::shared_ptr<State>& a_state);
		static void RunStep(std::shared_ptr<State> a_state);
		std::atomic<std::shared_ptr<State>> state{};
	};
}
