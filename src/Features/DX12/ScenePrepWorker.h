#pragma once

#include "DX12Resources.h"
#include "ScenePrepMath.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace DX12
{
	/** @brief Model-space triangles decoded from engine buffers; shared by instances of one mesh. */
	struct DecodedMesh
	{
		std::vector<float3> positions;
		std::vector<uint32_t> indices;
		uint64_t bytes = 0;
	};

	struct CapturedAttachedShape
	{
		uint64_t identity = 0;
		std::shared_ptr<const DecodedMesh> mesh;
		std::array<float, 12> transform{};
		uintptr_t sourceGeometry = 0;
		uintptr_t sourceVertices = 0;
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
		uint32_t rejectedLights = 0;
		uint64_t capturedBytes = 0;
	};

	struct ScenePrepResult
	{
		TilePublication publication;
		std::string failure;
		bool predictive = false;
		bool corner = false;
		double preparationMs = 0.0;
	};

	/** @brief Captures live references, terrain, lights, and unattached model descriptors on the game thread. */
	[[nodiscard]] std::optional<ScenePrepRequest> CaptureScenePrepRequest(
		SubcellKey a_key, uint32_t a_size, const RE::TESObjectCELL* a_cell,
		const RE::TESWorldSpace* a_world, bool a_predictive, bool a_corner);

	/** @brief True when the Papyrus VM has scripts bound to this reference. */
	[[nodiscard]] bool HasAttachedScripts(const RE::TESObjectREFR& a_ref);

	/** @brief True once an attached cell reports its reference set fully loaded. */
	[[nodiscard]] bool IsCellFullyLoaded(const RE::TESObjectCELL* a_cell);

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
		struct ModelCache;
		std::unique_ptr<ModelCache> modelCache;  // Worker thread only.
		std::jthread worker;
	};
}
