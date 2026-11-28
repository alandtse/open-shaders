#pragma once

#include "DX12Resources.h"
#include "ScenePrepMath.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace DX12
{
	struct CapturedMeshSlice
	{
		std::vector<uint8_t> vertices;
		std::vector<uint16_t> indices;
		uint32_t vertexCount = 0;
		uint32_t stride = 0;
	};

	struct CapturedAttachedShape
	{
		uint64_t identity = 0;
		std::shared_ptr<const CapturedMeshSlice> mesh;
		std::array<float, 12> transform{};
		CasterSource casterSource;
	};

	struct InteriorPreparationBatch
	{
		struct BoundedShape
		{
			std::shared_ptr<const Shape> shape;
			PreparationBounds bounds;
			uint64_t bytes = 0;
		};
		SubcellKey center;
		uint32_t size = 0;
		bool loading = false;
		int64_t expiresQpc = 0;
		std::vector<BoundedShape> shapes;
		std::vector<PreparedLight> lights;
		std::vector<std::pair<std::string, uint32_t>> rejectionReasons;
		uint32_t scopeRejections = 0;
	};

	struct ScenePrepRequest
	{
		SubcellKey key{};
		uint32_t size = kMinSubcellSize;
		bool worldspace = false;
		bool predictive = false;
		bool corner = false;
		bool consolidate = false;
		bool loading = false;
		bool enumerationComplete = false;
		bool hasUnattachedCells = false;
		bool lightCapacityExceeded = false;
		bool candidateOverflow = false;
		bool captureBudgetExceeded = false;
		bool spatialBoundsKnown = true;
		PreparationBounds spatialBounds;
		std::shared_ptr<const InteriorPreparationBatch> interiorBatch;
		uint32_t unlocalizedRejections = 0;
		double captureMs = 0.0;
		std::vector<CapturedAttachedShape> attachedShapes;
		std::vector<PreparedLight> lights;
		std::vector<NifSourceDescriptor> nifSources;
		std::vector<MissingNifShapeDescriptor> missingShapes;
		std::vector<std::pair<std::string, uint32_t>> rejectionReasons;
		uint32_t failedShapes = 0;
		uint32_t rejectedLights = 0;
	};

	struct ScenePrepResult
	{
		TilePublication publication;
		std::string failure;
		bool predictive = false;
		bool corner = false;
		bool consolidationAttempt = false;
		bool reusedInterior = false;
		std::shared_ptr<const InteriorPreparationBatch> interiorBatch;
		double preparationMs = 0.0;
		double captureMs = 0.0;
	};

	/** @brief Captures live references, terrain, and lights on the game thread. */
	[[nodiscard]] std::optional<ScenePrepRequest> CaptureScenePrepRequest(
		SubcellKey a_key, uint32_t a_size, const RE::TESObjectCELL* a_cell,
		const RE::TESWorldSpace* a_world, bool a_predictive, bool a_corner, bool a_consolidate, bool a_loading);

	/** @brief Single bounded producer preparing immutable subcell geometry and lights off the render thread. */
	/** @brief True when the Papyrus VM has scripts bound to this reference. */
	[[nodiscard]] bool HasAttachedScripts(const RE::TESObjectREFR& a_ref);

	/** @brief True once an attached cell reports its reference set fully loaded. */
	[[nodiscard]] bool IsCellFullyLoaded(const RE::TESObjectCELL* a_cell);

	class ScenePrepWorker
	{
	public:
		ScenePrepWorker();
		~ScenePrepWorker();

		ScenePrepWorker(const ScenePrepWorker&) = delete;
		ScenePrepWorker& operator=(const ScenePrepWorker&) = delete;

		bool Idle() const;
		/** @brief A terminal worker failure is distinct from an idle worker. */
		bool Failed() const { return failed.load(std::memory_order_acquire); }
		bool Enqueue(ScenePrepRequest a_request);
		std::optional<ScenePrepResult> Take();
		void Cancel();

	private:
		void Run(std::stop_token a_stop);
		ScenePrepResult Prepare(
			ScenePrepRequest a_request, uint64_t a_epoch, std::stop_token a_stop);

		mutable std::mutex mutex;
		std::condition_variable_any wake;
		std::optional<ScenePrepRequest> queued;
		std::optional<ScenePrepResult> completed;
		bool running = false;
		std::atomic<bool> failed{ false };
		bool errorLogged = false;
		std::atomic<uint64_t> generation = 1;
		std::jthread worker;
	};
}
