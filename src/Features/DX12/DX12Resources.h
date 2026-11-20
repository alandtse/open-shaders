#pragma once

#include "Feature.h"
#include "SpatialGrid.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

namespace DX12
{
	class CompanionWorker;
	/** @brief Immutable light record discovered alongside companion geometry. */
	struct PreparedLight
	{
		uint32_t referenceID = 0, baseID = 0, flags = 0;
		float3 position{}, color{};
		float radius = 0.0f, fade = 1.0f, emitterRadius = 0.0f, falloffExponent = 1.0f;
	};

	/** @brief The companion publishes triangle geometry without textures or materials. */
	struct Shape
	{
		uint64_t identity = 0;
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
			Ready,
			Incomplete
		};
		SubcellKey key;
		std::vector<std::shared_ptr<const Shape>> shapes;
		std::vector<PreparedLight> lights;
		uint64_t bytes = 0;
		State state = State::Requested;
	};

	/** @brief A companion attests that every eligible shape in this subcell was enumerated. */
	struct CompanionPublication
	{
		SubcellKey key;
		std::vector<Shape> shapes;
		std::vector<PreparedLight> lights;
		uint32_t eligibleShapes = 0;
		uint32_t failedShapes = 0;
		bool enumerationComplete = false;
	};

	/** @brief Immutable view of the nine resident runtime cells. */
	struct Grid
	{
		uint64_t revision = 0;
		uint64_t geometryRevision = 0, lightRevision = 0;
		uint32_t size = kSubcellSizes.front();
		std::array<SubcellKey, kGridCapacity> requested{};
		std::array<std::shared_ptr<const Tile>, kGridCapacity> resident{};
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
		/** @brief Releases every in-memory tile at scene transition. */
		void Clear();
		/** @brief Retains an immutable render-consumer snapshot. */
		std::shared_ptr<const Grid> Snapshot() const { return snapshot.load(std::memory_order_acquire); }

	private:
		std::mutex mutex;
		std::atomic<std::shared_ptr<const Grid>> snapshot{ std::make_shared<const Grid>() };
		std::optional<SubcellKey> pending;
	};
}

/** @brief Memory-resident geometry and lights with an ephemeral companion scratch directory. */
struct DX12Resources final : Feature
{
	~DX12Resources();
	struct Settings
	{
		bool Enabled = true;
		uint32_t SubcellSize = DX12::kSubcellSizes.front();
	};

	std::string GetName() override { return "DX12 Resources"; }
	std::string GetShortName() override { return "DX12Resources"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool SupportsVR() override { return false; }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void EarlyPrepass() override;
	void OnSceneTransitionReset(bool opening) override;
	void OnRuntimeDisabled() override;
	/** @brief Returns current geometry residency without traversing engine objects. */
	std::shared_ptr<const DX12::Grid> GetGrid() const { return grid.Snapshot(); }
	/** @brief Claims the next geometry request, if one is needed. */
	std::optional<DX12::SubcellKey> NextRequest() { return grid.NextRequest(); }
	/** @brief Accepts an authoritative in-memory geometry and light publication from the companion. */
	bool Publish(DX12::CompanionPublication publication) { return grid.Publish(std::move(publication)); }
	bool IsActive() const;
	/** @brief Returns the most recent prepared-light publication. */
	std::vector<DX12::PreparedLight> GetPreparedLights() const;

private:
	DX12::ResourceGrid grid;
	std::unique_ptr<DX12::CompanionWorker> companion;
	std::optional<DX12::SubcellKey> inFlight;
	Settings settings;
};
