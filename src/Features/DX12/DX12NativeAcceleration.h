#pragma once

#include "DX12NativeSceneInputs.h"
#include "GraphicsBackend.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
#include <memory>
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
		/** @brief Keeps a published scene alive until a downstream GPU consumer finishes. */
		bool RetainThrough(const std::shared_ptr<const TopLevel>& a_scene, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Reports an actual DX12 creation or submission error. */
		const std::string& Failure() const { return failure; }

	private:
		/** @brief Only the dedicated worker is permitted to allocate or submit native AS work. */
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
		struct Consumer
		{
			std::shared_ptr<const TopLevel> scene;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value = 0;
		};

		winrt::com_ptr<ID3D12Resource> Buffer(uint64_t a_size, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, const wchar_t* a_name) const;
		void Begin(Build& a_build);
		void Submit(Build& a_build);
		void Retire();
		bool BuildBottom(Build& a_build, const std::shared_ptr<const DX12::Shape>& a_shape);
		bool BuildTop(Build& a_build);
		bool HasPendingBottom(uint64_t a_identity, const DX12::Shape* a_shape) const;

		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Device5> device;
		winrt::com_ptr<ID3D12Fence> completion;
		std::array<Build, 2> builds;
		std::unordered_map<uint64_t, std::shared_ptr<const BottomLevel>> bottoms;
		std::vector<Consumer> consumers;
		std::shared_ptr<const SceneInputs> target;
		std::shared_ptr<const TopLevel> published;
		uint64_t generation = 0, nextFence = 0;
		uint64_t targetRevision = 0;
		uint32_t targetScope = 0;
		std::string failure;
	};

	/** @brief Dedicated CPU worker. Snapshot delivery and completed-TLAS consumption never build on the caller. */
	class AccelerationWorker
	{
	public:
		AccelerationWorker();
		~AccelerationWorker();
		AccelerationWorker(const AccelerationWorker&) = delete;
		AccelerationWorker& operator=(const AccelerationWorker&) = delete;

		/** @brief Starts a dedicated native builder; device and queue initialization run on that thread. */
		bool Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
		/** @brief Replaces the pending grid snapshot; only the latest unpublished request is built. */
		void Request(std::shared_ptr<const DX12::Grid> a_grid);
		/** @brief Cancels eligibility without interrupting previously submitted GPU work. */
		void Reset();
		/** @brief Acquires the most recent completed immutable scene, never waiting for a build. */
		std::shared_ptr<const TopLevel> GetScene() const;
		/** @brief Retains a borrowed scene until the consumer's GPU fence completes. */
		bool RetainThrough(std::shared_ptr<const TopLevel> a_scene, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Reports initialization or build failures without consulting engine objects. */
		std::string Failure() const;

	private:
		struct State;
		static void Run(std::shared_ptr<State> a_state, std::stop_token a_stop,
			winrt::com_ptr<ID3D11Device> a_device, winrt::com_ptr<ID3D11DeviceContext> a_context);
		std::shared_ptr<State> state;
		std::jthread worker;
	};
}
