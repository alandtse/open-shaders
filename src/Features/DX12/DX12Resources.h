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
		SubcellKey key;
		std::vector<std::shared_ptr<const Shape>> shapes;
		std::vector<PreparedLight> lights;
		uint64_t bytes = 0;
		bool prepared = false;
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
		/** @brief Publishes a completed shape batch only if still inside the requested grid. */
		bool Publish(const SubcellKey& key, std::vector<Shape> shapes);
		/** @brief Adds a newly discovered rigid shape without invalidating older consumers. */
		bool Append(const SubcellKey& key, Shape shape);
		/** @brief Replaces the live grid's light metadata without rebuilding its geometry. */
		void PublishLights(std::span<const PreparedLight> lights);
		/** @brief Releases every in-memory tile at scene transition. */
		void Clear();
		/** @brief Retains an immutable render-consumer snapshot. */
		std::shared_ptr<const Grid> Snapshot() const { return snapshot.load(std::memory_order_acquire); }

	private:
		std::mutex mutex;
		std::atomic<std::shared_ptr<const Grid>> snapshot{ std::make_shared<const Grid>() };
		std::optional<SubcellKey> pending;
		std::unordered_map<uint64_t, SubcellKey> observed;
	};
}

/** @brief Geometry and minimal prepared-light companion with a diskless 3x3 grid. */
struct DX12Resources final : Feature
{
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
	bool WantsStaticGeometryHook() const override { return true; }
	void ObserveStaticGeometry(RE::BSGeometry& geometry) override;
	void OnSceneTransitionReset(bool opening) override;
	void OnRuntimeDisabled() override;
	/** @brief Returns current geometry residency without traversing engine objects. */
	std::shared_ptr<const DX12::Grid> GetGrid() const { return grid.Snapshot(); }
	/** @brief Claims the next geometry request, if one is needed. */
	std::optional<DX12::SubcellKey> NextRequest() { return grid.NextRequest(); }
	/** @brief Accepts a companion-produced, shape-only subcell. */
	bool Publish(const DX12::SubcellKey& key, std::vector<DX12::Shape> shapes) { return grid.Publish(key, std::move(shapes)); }
	bool IsActive() const;
	/** @brief Returns the most recent prepared-light publication. */
	std::vector<DX12::PreparedLight> GetPreparedLights() const;

private:
	void RefreshPreparedLights(uint32_t frame);
	DX12::ResourceGrid grid;
	Settings settings;
	uint32_t copyFrame = 0, copiedFrameBytes = 0;
	uint32_t lightScanFrame = 0;
	std::array<DX12::SubcellKey, DX12::kGridCapacity> lastLightGrid{};
};
