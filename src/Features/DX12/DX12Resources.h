#pragma once

#include "Feature.h"
#include "RE/B/BSTEvent.h"
#include "RE/P/PositionPlayerEvent.h"
#include "SpatialGrid.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace DX12
{
	// Shared upper bound for prepared light records and the native shadow consumer.
	inline constexpr uint32_t kMaxPreparedLights = 2048;

	class ScenePrepWorker;
	struct ScenePrepRequest;
	/** @brief Immutable light record prepared for native scene shadow tracing. */
	struct PreparedLight
	{
		uint32_t referenceID = 0, flags = 0;
		float3 position{}, color{};
		float radius = 0.0f, fade = 1.0f, emitterRadius = 0.0f, falloffExponent = 1.0f;
		uint64_t runtimeIdentity = 0;  // Supplemental Full RT emitter observed at runtime.
		uint32_t unifiedCount = 1;
	};

	/** @brief Original installed NIF reference, not an embedded NIF binary. */
	struct NifSourceDescriptor
	{
		std::string referencePlugin, nifPath;
		uint32_t referenceLocal = 0;
	};

	/** @brief A shape or model unresolved during subcell preparation; runtime rigid geometry may supplement it. */
	struct MissingNifShapeDescriptor
	{
		std::string referencePlugin, nifPath, reason;
		uint32_t referenceLocal = 0;
		int32_t blockIndex = -1;  // -1: entire NIF/model unresolved.
	};

	/** @brief Triangle geometry without textures or materials. */
	struct Shape
	{
		uint64_t identity = 0;
		uint64_t runtimeObjectKey = 0;  // Stable across transform changes; zero for subcell-prepared shapes.
		std::vector<float3> positions;
		std::vector<uint32_t> indices;
		std::array<float, 12> transform{};
	};

	struct Tile
	{
		enum class State : uint8_t
		{
			Requested,
			Preparing,
			Ready,         // Geometry prepared and authoritative coverage verified.
			ReadyPartial,  // Valid geometry/lights, but shadow-offload coverage not certified.
			Empty,
			EmptyPartial,  // Enumeration finished, but some caster bounds or geometry are unresolved.
			RetryWaiting,
			Incomplete
		};
		SubcellKey key;
		std::vector<std::shared_ptr<const Shape>> shapes;
		std::vector<PreparedLight> lights;
		uint64_t bytes = 0;
		uint8_t preparationAttempts = 0;
		int64_t retryAfterQpc = 0;
		State state = State::Requested;
		bool preparationValidated = false;
		bool hasUnattachedCells = false;
		std::string failureReason;                                       // Last bounded preparation failure, retained for Dev and tile hover.
		std::vector<std::pair<std::string, uint32_t>> rejectionReasons;  // Bounded structured rejection diagnoses.
		std::vector<NifSourceDescriptor> nifSources;
		std::vector<MissingNifShapeDescriptor> missingShapes;
	};

	/** @brief Subcell preparation output indicating whether every eligible shape was enumerated. */
	struct TilePublication
	{
		SubcellKey key;
		std::vector<std::shared_ptr<const Shape>> shapes;  // Validated and frozen off the render thread.
		std::vector<PreparedLight> lights;
		uint64_t geometryBytes = 0;
		bool validated = false;
		uint32_t eligibleShapes = 0;
		uint32_t failedShapes = 0;
		uint32_t rejectedLights = 0;
		bool enumerationComplete = false;
		bool wholeCell = false;
		bool hasUnattachedCells = false;
		std::string failureReason;
		std::vector<std::pair<std::string, uint32_t>> rejectionReasons;
		std::vector<NifSourceDescriptor> nifSources;
		std::vector<MissingNifShapeDescriptor> missingShapes;
	};

	/** @brief Renderer-observed static geometry, never proof of full shadow coverage. */
	struct RuntimeShape
	{
		SubcellKey tile;
		std::shared_ptr<const Shape> shape;
		uint64_t bytes = 0;
	};

	/** @brief Immutable view of the nine resident runtime cells. */
	struct Grid
	{
		uint64_t geometryRevision = 0;
		uint32_t size = kSubcellSizes.front();
		bool wholeCell = false;
		std::array<SubcellKey, kGridCapacity> requested{};  // Active 3x3, available to future native consumers.
		std::array<std::shared_ptr<const Tile>, kGridCapacity> resident{};
		std::array<SubcellKey, kPredictiveCapacity> predictive{};  // Predictive outer 5x5 ring.
		std::array<std::shared_ptr<const Tile>, kPredictiveCapacity> predictiveResident{};
		// Bounded, transient observations from existing engine render passes.
		// No persistent cache, materials, or raster-shadow coverage authority.
		std::vector<RuntimeShape> runtimeShapes;
		uint64_t runtimeBytes = 0;
	};

	struct PreparationTiming
	{
		uint64_t completed = 0;  // Accepted, completed subcell publications.
		double lastMs = 0.0, totalMs = 0.0, worstMs = 0.0;
		double AverageMs() const { return completed ? totalMs / double(completed) : 0.0; }
	};

	struct PreparationStats
	{
		PreparationTiming active, predictive, corners;
	};

	class ResourceGrid
	{
	public:
		/** @brief Recenters the live grid and retires unneeded tiles without saving geometry. */
		void Advance(uint32_t scope, uint32_t size, double x, double y);
		/** @brief Returns an unprepared tile request to the scene preparation producer. */
		std::optional<SubcellKey> NextRequest();
		/** @brief Publishes one prepared subcell, otherwise marks it incomplete. */
		bool Publish(TilePublication publication);
		/** @brief Merges only compatible immutable prepared tiles, never runtime observations. */
		void MergePrepared(const Grid& source);
		/** @brief Marks tiles overlapping unattached exterior cells for re-preparation after grid shifts. */
		void RefreshUnattachedTiles();
		/** @brief Accept an observed static shape within the active 3x3 without caching it on disk. */
		bool PublishRuntimeShape(std::shared_ptr<const Shape> shape, float centerX, float centerY, uint32_t expectedScope = 0);
		/** @brief Releases every in-memory tile at scene transition. */
		void Clear();
		void FlushRetiredCache();  // DRAM only; caller decides transition policy.
		/** @brief Cancel a superseded predictive request and allow active work to preempt it. */
		void CancelPending();
		/** @brief Retires runtime observations at loading boundaries while retaining prepared geometry. */
		void DiscardRuntimeShapes();
		/** @brief Retains an immutable render-consumer snapshot. */
		std::shared_ptr<const Grid> Snapshot() const { return snapshot.load(std::memory_order_acquire); }

	private:
		std::mutex mutex;
		std::atomic<std::shared_ptr<const Grid>> snapshot{ std::make_shared<const Grid>() };
		std::optional<SubcellKey> pending;
		std::deque<std::shared_ptr<const Tile>> recentTiles;
		double previousX = 0, previousY = 0, movementX = 0, movementY = 0;
		uint32_t previousScope = 0, previousSize = 0;
	};
}

/** @brief Memory-resident geometry and lights prepared in-process. */
struct DX12Resources final : Feature, private RE::BSTEventSink<RE::PositionPlayerEvent>
{
	// Keep construction and destruction out of this header: the unique_ptr
	// pointee is forward-declared until ScenePrepWorker.h is included in the TU.
	DX12Resources();
	~DX12Resources();
	struct Settings
	{
		uint32_t SubcellSize = DX12::kSubcellSizes.front();
		bool ConsolidateInterior = true;
	};

	std::string GetName() override { return "DX12 Resources"; }
	std::string GetShortName() override { return "DX12Resources"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	// Resources follows the DX12 master and must not be separately disabled.
	bool IsAlwaysEnabled() const override { return true; }
	bool IsInMenu() const override { return false; }
	bool IsInFeatureList() const override { return false; }
	bool SupportsVR() override { return false; }
	void DrawSettings() override;
	/** @brief Read-only preparation publication and residency display. */
	void DrawResourceStatus();
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	bool WantsPreShadowMapsHook() const override;
	void PreShadowMaps() override;
	void Reset() override;
	void DataLoaded() override;
	void OnLoadingMenuEvent(bool opening) override;
	void OnRuntimeDisabled() override;
	/** @brief Returns current geometry residency without traversing engine objects. */
	std::shared_ptr<const DX12::Grid> GetGrid() const { return grid.Snapshot(); }
	/** @brief Accepts a bounded, static renderer observation for native geometry (never for raster suppression). */
	bool PublishRuntimeShape(std::shared_ptr<const DX12::Shape> shape, float centerX, float centerY, uint32_t expectedScope = 0)
	{
		return runtimePublicationEnabled.load(std::memory_order_acquire) &&
		       grid.PublishRuntimeShape(std::move(shape), centerX, centerY, expectedScope);
	}
	bool IsActive() const;
	bool HasMenuPreload() const { return menuPreloaded; }
	/** @brief True while the main-thread loading menu notification is active. */
	bool IsLoading() const { return loadingActive.load(std::memory_order_acquire); }
	/** @brief Returns the most recent prepared-light publication. */
	std::vector<DX12::PreparedLight> GetPreparedLights() const;
	/** @brief Collect an immutable grid's ordered light selection without engine access. */
	static std::vector<DX12::PreparedLight> CollectPreparedLights(const DX12::Grid& grid);
	/** @brief In-memory generation timings for active 3x3, predictive sides and corners. */
	std::shared_ptr<const DX12::PreparationStats> GetPreparationStats() const
	{
		return generationStats.load(std::memory_order_acquire);
	}

	Settings settings;

private:
	struct LoadingDestination
	{
		uint32_t scope = 0;
		bool worldspace = false;
		std::array<float, 3> position{}, rotation{};
	};
	/** @brief Destination requests captured on the main thread once the target cell has loaded. */
	struct LoadingCapture
	{
		uint64_t serial = 0;
		uint32_t scope = 0, size = 0;
		std::array<float, 2> position{};
		std::vector<DX12::ScenePrepRequest> requests;
	};
	static std::optional<LoadingDestination> CaptureLoadingDestination();
	void CaptureLoadingRequests(const LoadingDestination& destination);
	RE::BSEventNotifyControl ProcessEvent(const RE::PositionPlayerEvent* event,
		RE::BSTEventSource<RE::PositionPlayerEvent>*) override;
	std::optional<LoadingDestination> queuedDestination;  // Main-thread positioning events only.
	std::atomic<std::shared_ptr<const LoadingDestination>> loadingDestination;
	std::atomic<std::shared_ptr<LoadingCapture>> loadingCapture;
	std::atomic<bool> loadingActive{ false };
	struct LoadingSettings
	{
		uint32_t size;
		bool consolidate;
	};
	std::atomic<LoadingSettings> loadingSettings{ LoadingSettings{ DX12::kSubcellSizes.front(), true } };
	std::atomic<uint64_t> loadingSerial{ 0 };
	uint64_t handledLoadingSerial = 0;
	std::unique_ptr<DX12::ResourceGrid> loadingGrid;
	uint64_t loadingGridSerial = 0;
	std::vector<DX12::ScenePrepRequest> loadingRequests;
	std::optional<DX12::SubcellKey> loadingInFlight;
	void AdvanceLoadingPreparation();
	void EnsureWorker();
	enum class PublicationState
	{
		Valid,
		Invalid,
		Unavailable
	};
	struct PublicationStatus
	{
		PublicationState state;
		std::string detail;
	};
	std::atomic<std::shared_ptr<const PublicationStatus>> latestPublication;
	void RecordPublication(PublicationState state, std::string detail);
	bool menuPreloaded = false;
	uint32_t consolidationAttemptScope = 0;
	void ResetPreparation();
	/** @brief Cancel and clear native preparation once when the selected mode or cell is ineligible. */
	void SuspendPreparation();
	bool preparationSuspended = false;  // Render-thread transition latch.
	bool preparationActive = false;
	std::atomic<bool> runtimePublicationEnabled{ false };
	DX12::ResourceGrid grid;
	std::unique_ptr<DX12::ScenePrepWorker> worker;
	uint32_t lastVisitedCell = 0, previousVisitedCell = 0, distinctTransitions = 0;
	int32_t lastExteriorGridX = std::numeric_limits<int32_t>::min();
	int32_t lastExteriorGridY = std::numeric_limits<int32_t>::min();
	std::optional<DX12::SubcellKey> inFlight;
	std::atomic<std::shared_ptr<const DX12::PreparationStats>> generationStats{
		std::make_shared<const DX12::PreparationStats>()
	};
};
