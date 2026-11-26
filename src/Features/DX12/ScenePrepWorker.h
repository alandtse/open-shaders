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
	struct CapturedAttachedShape
	{
		uint64_t identity = 0;
		const RE::BSGraphics::TriShape* rendererData = nullptr;
		const uint8_t* rawVertices = nullptr;
		const uint16_t* rawIndices = nullptr;
		uint32_t vertexCount = 0;
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		uint32_t stride = 0;
		std::array<float, 12> transform{};
	};

	struct UnattachedModelPlacement
	{
		uint32_t refFormID = 0;
		uint32_t refLocal = 0;
		std::string refPlugin;
		std::string nifPath;
		AffineTransform transform{};
	};

	struct ScenePrepRequest
	{
		SubcellKey key{};
		uint32_t size = kMinSubcellSize;
		bool worldspace = false;
		bool predictive = false;
		bool corner = false;
		bool wholeCell = false;
		bool enumerationComplete = false;
		bool hasUnattachedCells = false;
		bool lightCapacityExceeded = false;
		bool candidateOverflow = false;
		std::vector<CapturedAttachedShape> attachedShapes;
		std::vector<UnattachedModelPlacement> unattachedPlacements;
		std::vector<PreparedLight> lights;
		std::vector<NifSourceDescriptor> nifSources;
		std::vector<MissingNifShapeDescriptor> missingShapes;
		std::vector<std::pair<std::string, uint32_t>> rejectionReasons;
		uint32_t failedShapes = 0;
		uint64_t loadingGeneration = 0;  // Non-zero for a loading-screen destination request.
		std::string residencyReason;
		std::vector<RE::NiPointer<RE::NiObject>> keepAlive;
	};

	struct ScenePrepResult
	{
		TilePublication publication;
		std::string failure;
		bool predictive = false;
		bool corner = false;
		uint64_t loadingGeneration = 0;
		std::string residencyReason;
		double preparationMs = 0.0;
	};

	/**
	 * @brief Captures live references, terrain, lights, and unattached model descriptors on the game thread.
	 * @param a_wholeCell For an interior, capture the whole cell when its references fit the predictive footprint.
	 */
	[[nodiscard]] std::optional<ScenePrepRequest> CaptureScenePrepRequest(
		SubcellKey a_key, uint32_t a_size, const RE::TESObjectCELL* a_cell,
		const RE::TESWorldSpace* a_world, bool a_predictive, bool a_corner, bool a_wholeCell = false);

	/** @brief Single bounded producer preparing immutable subcell geometry and lights off the render thread. */
	class ScenePrepWorker
	{
	public:
		ScenePrepWorker();
		~ScenePrepWorker();

		ScenePrepWorker(const ScenePrepWorker&) = delete;
		ScenePrepWorker& operator=(const ScenePrepWorker&) = delete;

		bool Idle() const;
		bool Enqueue(ScenePrepRequest a_request);
		std::optional<ScenePrepResult> Take();
		void Cancel();
		/** @brief Releases retained engine NiObjects on the game thread. */
		void DrainRetiredObjects();

	private:
		void Run(std::stop_token a_stop);
		ScenePrepResult Prepare(
			ScenePrepRequest a_request, uint64_t a_epoch, std::stop_token a_stop,
			std::vector<RE::NiPointer<RE::NiObject>>& a_outRetired);
		void RetireObjectsLocked(std::vector<RE::NiPointer<RE::NiObject>>& a_objects);

		mutable std::mutex mutex;
		std::condition_variable_any wake;
		std::optional<ScenePrepRequest> queued;
		std::optional<ScenePrepResult> completed;
		std::vector<RE::NiPointer<RE::NiObject>> retiredObjects;
		bool running = false;
		bool errorLogged = false;
		std::atomic<uint64_t> generation = 1;
		std::jthread worker;
	};
}
