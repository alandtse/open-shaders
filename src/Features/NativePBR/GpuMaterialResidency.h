#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NativePBR
{
	/** @brief Maps currently referenced DDS pages into a compact generation-local descriptor range. */
	inline std::vector<uint32_t> PlanGpuMaterialPageRemap(
		const std::unordered_set<std::string>& a_requiredPaths,
		const std::unordered_map<std::string, uint64_t>& a_locations,
		const std::unordered_map<uint64_t, std::array<uint32_t, 2>>& a_logicalLocations,
		size_t a_poolCount)
	{
		std::vector<uint32_t> remap(a_poolCount, UINT32_MAX);
		for (const auto& path : a_requiredPaths) {
			const auto resident = a_locations.find(path);
			if (resident == a_locations.end())
				continue;
			const auto logical = a_logicalLocations.find(resident->second);
			if (logical != a_logicalLocations.end() && logical->second[0] < remap.size())
				remap[logical->second[0]] = 0;
		}
		uint32_t selected = 0;
		for (auto& index : remap)
			if (index != UINT32_MAX)
				index = selected++;
		return remap;
	}
}
