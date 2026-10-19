#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

enum class WindSettingsPage
{
	WindField,
	WindEffects,
	Trees,
	TreeMeshes,
	GrassMeshes,
	Grass
};

/** Transient menu and tree-editor state for the Wind settings UI. */
struct WindUIState
{
	WindSettingsPage activeSettingsPage = WindSettingsPage::WindField;
	std::size_t activeWindEffectIndex = 0;
	std::array<char, 256> treeMeshSearch{};
	std::vector<std::size_t> filteredTreeRuleIndices;
	std::string appliedTreeMeshSearch;
	std::string treeWindSaveStatus;
	std::size_t filteredTreeRuleCount = static_cast<std::size_t>(-1);
	int debugWindEffectSpawnCount = 8;
	bool debugWindEffectWorstCase = false;
	bool treeWindSaveSucceeded = false;
	std::array<char, 256> grassMeshSearch{};
	std::vector<std::size_t> filteredGrassRuleIndices;
	std::string appliedGrassMeshSearch;
	std::string grassWindSaveStatus;
	std::size_t filteredGrassRuleCount = static_cast<std::size_t>(-1);
	std::size_t selectedUnconfiguredGrassMesh = 0;
	bool grassWindSaveSucceeded = false;
};
