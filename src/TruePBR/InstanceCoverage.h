#pragma once

#include "NativePBRCompiler.h"
#include "Utils/StringUtils.h"
#include <unordered_map>

namespace NativePBR
{
	struct PreparedInstanceKey
	{
		std::string referenceKey, shapeName;
	};

	struct InstanceCoverage
	{
		size_t total = 0, missingIdentity = 0, noObservation = 0, uniqueCandidates = 0, ambiguous = 0;
		size_t candidateRecipes = 0, candidateRecipesMissing = 0;
		bool recipeGenerationAvailable = false;
		uint64_t recipeRevision = 0;
	};

	/** @brief Counts unverified candidates; duplicate source keys or material identities remain ambiguous. */
	inline InstanceCoverage AnalyzeInstanceCoverage(const std::vector<PreparedInstanceKey>& a_prepared, const std::vector<InstanceHint>& a_hints,
		const RecipeGeneration* a_recipes = nullptr)
	{
		auto key = [](const std::string& reference, const std::string& shape) {
			auto result = Util::ToLowerAscii(reference);
			result.push_back('\0');
			result += Util::ToLowerAscii(shape);
			return result;
		};
		std::unordered_map<std::string, size_t> preparedCounts;
		std::unordered_map<std::string, std::vector<uint64_t>> materials;
		std::unordered_map<std::string, InstanceProperties> properties;
		std::unordered_set<std::string> conflicts;
		for (const auto& item : a_prepared)
			if (!item.referenceKey.empty() && !item.shapeName.empty())
				++preparedCounts[key(item.referenceKey, item.shapeName)];
		for (const auto& hint : a_hints) {
			const auto hintKey = key(hint.referenceKey, hint.shapeName);
			if (hint.propertiesConflict)
				conflicts.insert(hintKey);
			const auto [prior, inserted] = properties.try_emplace(hintKey, hint.properties);
			if (!inserted && prior->second != hint.properties)
				conflicts.insert(hintKey);
			auto& values = materials[hintKey];
			if (std::find(values.begin(), values.end(), hint.materialIdentity) == values.end())
				values.push_back(hint.materialIdentity);
		}
		InstanceCoverage result;
		result.total = a_prepared.size();
		result.recipeGenerationAvailable = a_recipes != nullptr;
		if (a_recipes)
			result.recipeRevision = a_recipes->revision;
		for (const auto& item : a_prepared) {
			if (item.referenceKey.empty() || item.shapeName.empty()) {
				++result.missingIdentity;
				continue;
			}
			const auto identity = key(item.referenceKey, item.shapeName);
			const auto found = materials.find(identity);
			if (found == materials.end())
				++result.noObservation;
			else if (preparedCounts[identity] != 1 || found->second.size() != 1 || conflicts.contains(identity))
				++result.ambiguous;
			else {
				++result.uniqueCandidates;
				if (a_recipes) {
					if (a_recipes->recipes.contains(found->second.front()))
						++result.candidateRecipes;
					else
						++result.candidateRecipesMissing;
				}
			}
		}
		return result;
	}
}
