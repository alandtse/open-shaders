#pragma once

#include "Feature.h"
#include "SpatialGrid.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace DX12
{
	// Shared upper bound for companion light records and the native shadow consumer.
	inline constexpr uint32_t kMaxPreparedLights = 2048;

	class CompanionWorker;
	struct CompanionResult;
	/** @brief Immutable light record discovered alongside companion geometry. */
	struct PreparedLight
	{
		uint32_t referenceID = 0, flags = 0;
		float3 position{}, color{};
		float radius = 0.0f, fade = 1.0f, emitterRadius = 0.0f, falloffExponent = 1.0f;
	};

	/** @brief Original installed NIF reference, not an embedded NIF binary. */
	struct NifSourceDescriptor
	{
		std::string referencePlugin, nifPath;
		uint32_t referenceLocal = 0;
	};

	/** @brief A shape or model unresolved by the companion; runtime rigid geometry may supplement it. */
	struct MissingNifShapeDescriptor
	{
		std::string referencePlugin, nifPath, reason;
		uint32_t referenceLocal = 0;
		int32_t blockIndex = -1;  // -1: entire NIF/model unresolved.
	};

	/** @brief The companion publishes triangle geometry without textures or materials. */
	struct Shape
	{
		uint64_t identity = 0;
		uint64_t runtimeObjectKey = 0;  // Stable across transform changes; zero for companion shapes.
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
		std::string failureReason;                                       // Last bounded companion failure, retained for Dev and tile hover.
		std::vector<std::pair<std::string, uint32_t>> rejectionReasons;  // Bounded structured companion rejection diagnoses.
		std::vector<NifSourceDescriptor> nifSources;
		std::vector<MissingNifShapeDescriptor> missingShapes;
	};

	/** @brief A companion attests that every eligible shape in this subcell was enumerated. */
	struct CompanionPublication
	{
		SubcellKey key;
		std::vector<std::shared_ptr<const Shape>> shapes;  // Fully validated and frozen on the companion worker.
		std::vector<PreparedLight> lights;
		uint64_t geometryBytes = 0;
		bool validated = false;
		uint32_t eligibleShapes = 0;
		uint32_t failedShapes = 0;
		uint32_t rejectedLights = 0;  // Invalid light records are skipped without losing valid geometry.
		bool enumerationComplete = false;
		bool wholeCell = false;
		bool spatialBoundsComplete = false;
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
		std::array<SubcellKey, kPredictiveCapacity> predictive{};  // Companion-only outer 5x5 ring.
		std::array<std::shared_ptr<const Tile>, kPredictiveCapacity> predictiveResident{};
		// Bounded, transient observations from existing engine render passes.
		// No persistent cache, materials, or raster-shadow coverage authority.
		std::vector<RuntimeShape> runtimeShapes;
		uint64_t runtimeBytes = 0;
	};

	struct PreparationTiming
	{
		uint64_t completed = 0;  // Accepted, completed companion publications.
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
		/** @brief Returns an unprepared tile request to a single companion producer. */
		std::optional<SubcellKey> NextRequest();
		/** @brief Publishes one fully enumerated companion subcell, otherwise marks it incomplete. */
		bool Publish(CompanionPublication publication);
		/** @brief Accept an observed static shape within the active 3x3 without caching it on disk. */
		bool PublishRuntimeShape(std::shared_ptr<const Shape> shape, float centerX, float centerY, uint32_t expectedScope = 0);
		/** @brief Releases every in-memory tile at scene transition. */
		void Clear();
		void FlushRetiredCache();  // DRAM only; caller decides transition policy.
		/** @brief Cancel a superseded predictive request and allow active work to preempt it. */
		void CancelPending();
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

/** @brief Memory-resident geometry and lights with an ephemeral companion scratch directory. */
struct DX12Resources final : Feature
{
	// Keep construction and destruction out of this header: the two unique_ptr
	// pointees are forward-declared until CompanionWorker.h is included in the TU.
	DX12Resources();
	~DX12Resources();
	struct Settings
	{
		uint32_t SubcellSize = DX12::kSubcellSizes.front();
		bool PreloadLastSave = true;
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
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	bool WantsPreShadowMapsHook() const override;
	void PreShadowMaps() override;
	void Reset() override;
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
	/** @brief Returns the most recent prepared-light publication. */
	std::vector<DX12::PreparedLight> GetPreparedLights() const;
	/** @brief In-memory generation timings for active 3x3, predictive sides and corners. */
	std::shared_ptr<const DX12::PreparationStats> GetPreparationStats() const
	{
		return generationStats.load(std::memory_order_acquire);
	}

private:
	void EnsureCompanion(bool menuStartup);
	void PollStartup(uint32_t gameplayScope = 0);
	bool startupPending = false;
	bool menuPreloaded = false;
	uint32_t consolidationAttemptScope = 0;
	void ResetPreparation();
	std::once_flag companionStart;
	std::atomic<bool> companionStarted{ false };
	bool preparationActive = false;
	std::atomic<bool> runtimePublicationEnabled{ false };
	DX12::ResourceGrid grid;
	std::unique_ptr<DX12::CompanionWorker> companion;
	std::unique_ptr<DX12::CompanionResult> adopting;
	size_t lightAdoptionOffset = 0;
	uint32_t lastVisitedCell = 0, previousVisitedCell = 0, distinctTransitions = 0;
	std::optional<DX12::SubcellKey> inFlight;
	std::atomic<std::shared_ptr<const DX12::PreparationStats>> generationStats{
		std::make_shared<const DX12::PreparationStats>()
	};
	Settings settings;
};
