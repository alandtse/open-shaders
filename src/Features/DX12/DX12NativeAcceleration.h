#pragma once

#include "DX12NativeSceneInputs.h"
#include "GraphicsBackend.h"

#include <array>
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
	public:
		~AccelerationScene();
		/** @brief Retains the shared device and the native scene's direct queue. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Schedules one missing BLAS or a complete TLAS without waiting on the GPU. */
		void Advance(std::shared_ptr<const SceneInputs> a_inputs);
		/** @brief Invalidates the visible scene while letting submitted GPU work retire. */
		void Reset();
		/** @brief Returns only a GPU-completed TLAS, with all rigid casters retained. */
		std::shared_ptr<const TopLevel> GetScene() const { return published; }
		/** @brief Keeps a published scene alive until a downstream GPU consumer finishes. */
		bool RetainThrough(const std::shared_ptr<const TopLevel>& a_scene, ID3D12Fence* a_fence, uint64_t a_value);
		/** @brief Reports an actual DX12 creation or submission error. */
		const std::string& Failure() const { return failure; }

	private:
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
}
