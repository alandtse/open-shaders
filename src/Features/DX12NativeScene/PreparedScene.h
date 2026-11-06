#pragma once

#include "AccelerationScene.h"
#include <atomic>
#include <filesystem>

namespace DX12Native
{
	/** @brief Validated static source data. Runtime/animated light handling is deferred. */
	struct PreparedPointLight
	{
		float3 position{};
		float3 radiance{};
		float radius = 0;
		float sourceRadius = 0;
		bool supported = false;
	};
	/** @brief Owns offline geometry and baseline lights independently of engine pointers. */
	struct PreparedCell
	{
		std::shared_ptr<const Snapshot> snapshot;
		AccelerationScene::GeometryMap geometries;
		std::vector<float3> lights;
		std::vector<json> lightDefinitions;
		std::vector<PreparedPointLight> nativeLights;
		// Per-light baseline admission result, kept in the same order as nativeLights.
		std::vector<std::string> lightEligibility;
		std::vector<json> roomDefinitions;
		std::string digest;
		uint64_t bytes = 0;
		uint32_t maskedInstances = 0;
		uint32_t shadedInstances = 0;
		uint32_t tangentInstances = 0;
	};

	/** @brief Loads one bounded package on an owned worker without D3D11 capture. */
	class PreparedScene
	{
	public:
		/** @brief Starts one file/decode/upload task using copied identity and backend data. */
		void Request(const GraphicsBackend& a_backend, std::filesystem::path a_directory, std::string a_cell, uint64_t a_epoch, uint64_t a_budget, uint64_t a_materialBudget = 0);
		/** @brief Cancels publication without joining the loader. */
		void Reset();
		/** @brief Reports whether this revision already attempted a package load. */
		bool Requested() const { return requested; }
		/** @brief Retains only a current-revision fully loaded package. */
		std::shared_ptr<const PreparedCell> GetCell() const;
		/** @brief Reports loader state independently of traversal and runtime validity. */
		json GetDiagnostics() const;
	private:
		struct State;
		std::shared_ptr<State> state;
		uint64_t revision = 1;
		bool requested = false;
	};
}
