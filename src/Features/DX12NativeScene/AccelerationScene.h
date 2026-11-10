#pragma once

#include "GeometryBridge.h"

namespace DX12Native
{
	/** @brief Retains a range-specific BLAS and its immutable native input geometry. */
	struct BottomLevel
	{
		std::shared_ptr<const Geometry> geometry;
		winrt::com_ptr<ID3D12Resource> result;
		uint32_t firstIndex = 0, indexCount = 0;
		uint64_t bytes = 0;
	};

	/** @brief Owns a completed TLAS with matching instance records and BLAS lifetimes. */
	struct TopLevel
	{
		winrt::com_ptr<ID3D12Resource> result;
		std::vector<std::shared_ptr<const BottomLevel>> bottoms;
		std::vector<Instance> instances;
		uint64_t generation = 0, sweep = 0, bytes = 0, blasRevision = 0;
		bool updateCapable = false;
		uint32_t omittedInstances = 0;
		SceneScope scope;
		std::shared_ptr<const MaterialSource> materialSource;
	};

	/** @brief Builds bounded acceleration structures independently of engine object traversal. */
	class AccelerationScene
	{
	public:
		/** @brief Uses the existing device and queue for DXR 1.1 builds. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Preserves pending build resources on teardown without a CPU fence wait. */
		~AccelerationScene();
		/** @brief Invalidates publication and retires pending builds by generation. */
		void Reset();
		/** @brief Replaces the observed light scope without discarding completed immutable BLASes. */
		void ReplaceScope() { ++generation; published.reset(); }
		using GeometryMap = std::unordered_map<const Mesh*, std::shared_ptr<const Geometry>>;
		/** @brief Advances one missing BLAS and one ready TLAS per worker step, retaining two GPU slots. */
		void Advance(const Snapshot* a_snapshot, const GeometryMap& a_geometries, uint64_t a_budgetBytes);
		/** @brief Retains the native completion fence for nonblocking progress polling. */
		winrt::com_ptr<ID3D12Fence> CompletionFence() const { return completion; }
		/** @brief Returns the last submitted GPU retirement point. */
		uint64_t SubmittedFence() const { return nextFence; }
		/** @brief Reports whether submitted builds still own GPU work. */
		bool HasPendingBuilds() const;
		/** @brief Account for completed/pending GPU resources when checking DXGI. */
        uint64_t TrackedBytes() const { return ResidentBytes(); }
		/** @brief Reports allocation deferrals without rebuilding diagnostics. */
		uint64_t BudgetDeferrals() const { return budgetDeferrals; }
		/** @brief Returns the latched device/build failure. */
		const std::string& Failure() const { return failure; }
		/** @brief Retains the latest completed TLAS for a separately fenced ray consumer. */
		std::shared_ptr<const TopLevel> GetScene() const { return published; }
		/** @brief Reports build submission/completion separately from traversal and coverage. */
		json GetDiagnostics() const;

	private:
		static constexpr uint32_t kMaximumInstances = 8192;
		struct RangeKey
		{
			const Mesh* mesh = nullptr;
			uint32_t first = 0, count = 0;
			bool operator==(const RangeKey&) const = default;
		};
		struct RangeHash
		{
			size_t operator()(const RangeKey& a_key) const
			{
				return std::hash<const Mesh*>{}(a_key.mesh) ^ std::rotl(size_t(a_key.first), 1) ^ std::rotl(size_t(a_key.count), 2);
			}
		};
		struct Build
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
			winrt::com_ptr<ID3D12Resource> scratch, instanceUpload;
			std::shared_ptr<BottomLevel> bottom;
			std::shared_ptr<TopLevel> top;
			// Keep the source acceleration structure alive until a TLAS update fence retires.
			std::shared_ptr<const TopLevel> updateSource;
			uint64_t fence = 0, generation = 0, transientBytes = 0;
			bool submitted = false;
		};
		winrt::com_ptr<ID3D12Resource> Buffer(uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, const wchar_t* a_name);
		uint64_t AllocationBytes(uint64_t a_bytes, D3D12_HEAP_TYPE a_heap) const;
		uint64_t ResidentBytes() const;
		void Begin(Build& a_build);
		void Submit(Build& a_build);
		bool BuildBottom(Build& a_build, const Instance& a_instance, std::shared_ptr<const Geometry> a_geometry, uint64_t a_budgetBytes);
		bool BuildTop(Build& a_build, const Snapshot& a_snapshot, uint64_t a_budgetBytes);
		void Retire();
		GraphicsBackend backend;
		winrt::com_ptr<ID3D12Device5> device;
		winrt::com_ptr<ID3D12Fence> completion;
		std::array<Build, 2> builds;
		std::unordered_map<RangeKey, std::shared_ptr<const BottomLevel>, RangeHash> bottoms;
		std::shared_ptr<const TopLevel> published;
		uint64_t generation = 0, nextFence = 0, bottomSubmissions = 0, topSubmissions = 0, bottomCompletions = 0, topCompletions = 0, topUpdates = 0;
		uint64_t budgetDeferrals = 0, busyFrames = 0;
		size_t buildCursor = 0;
		std::string failure;
	};

	/** @brief Hands owned batches to one AS worker without render-thread waits. */
	class AccelerationWorker
	{
	public:
		/** @brief Starts one owned batch; never queues another worker task while busy. */
		void Start(const GraphicsBackend& a_backend, std::shared_ptr<const Snapshot> a_snapshot,
			AccelerationScene::GeometryMap a_geometries, uint64_t a_manualBudgetBytes); // 0 = DXGI auto
		/** @brief Polls publication and GPU completion, then schedules one progress step. */
		void Advance();
		/** @brief Cancels eligibility and requests asynchronous resource retirement. */
		void Reset();
		/** @brief True once the active batch completed or could not progress. */
		bool Finished() const { return finished; }
		/** @brief Retains the completed worker scene for a separately fenced diagnostic consumer. */
		std::shared_ptr<const TopLevel> GetScene() const;
		/** @brief Returns cached worker evidence without accessing worker-owned caches. */
		json GetDiagnostics() const;

	private:
		struct State;
		struct Input;
		struct Output;
		std::shared_ptr<State> state;
		std::shared_ptr<const Input> input;
		std::shared_ptr<const Output> output;
		uint64_t revision = 0;
		bool finished = true, retiring = false;
		std::string dispatchFailure;
		json lastDiagnostics;
		SceneScope lastDiagnosticScope, lastCompletedScope;
		std::string lastBatchFailure;
		uint64_t lastFailedBatch = 0;
	};
}
