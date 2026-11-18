#pragma once

#include "Features/NativePBR/VerifiedBridge.h"
#include <array>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
	/** @brief Worker-only plan. The existing 8-file cap and upload budgets remain authoritative. */
	struct LiveLooseTexturePlan
	{
		std::vector<std::string> paths;
		size_t alreadyPresent = 0, alreadyAttempted = 0, deferred = 0, invalid = 0;
		size_t supportedRecipes = 0, featureLimitedRecipes = 0, skippedRecipes = 0;
	};

	/** @brief Source acquisition should not be vetoed by unsupported *shading* features.
     *
     * Fully shadable recipes are serviced first. Feature-limited opaque mesh recipes
     * are considered afterward, within the same cap; this allows source provenance
     * and eventual progressive shader support to advance independently. Invalid,
     * landscape and incomplete identities remain excluded.
     */
	inline LiveLooseTexturePlan PlanLiveLooseTextures(const NativePBR::LiveMatchResult& a_matches,
		const std::unordered_set<std::string>& a_present, size_t a_maxTextures = 8,
		const std::unordered_set<std::string>& a_attempted = {}, bool a_extendedRoles = false)
	{
		LiveLooseTexturePlan plan;
		std::array<std::vector<const NativePBR::Recipe*>, 2> priority;
		std::unordered_set<uint64_t> seenIdentities;
		for (const auto& match : a_matches.matches) {
			if (!match || !match->recipe)
				continue;
			const auto& recipe = *match->recipe;
			if (!seenIdentities.insert(recipe.identity).second)
				continue;
			constexpr uint32_t invalid = NativePBR::InvalidSnapshot | NativePBR::InvalidTexturePath |
			                             NativePBR::MissingRequiredTextures | NativePBR::UnsupportedLandscape;
			if (!recipe.identity || !recipe.revision || recipe.parameters.kind != NativePBR::MaterialKind::Mesh ||
				!NativePBR::IsFinite(recipe.parameters) || !recipe.parameters.texturePathsComplete ||
				(recipe.issues & invalid) != 0) {
				++plan.skippedRecipes;
				continue;
			}
			const bool supported = NativePBR::NativeMeshRecipeSupported(recipe);
			priority[supported ? 0 : 1].push_back(&recipe);
			if (supported)
				++plan.supportedRecipes;
			else
				++plan.featureLimitedRecipes;
		}
		constexpr std::array roles{ NativePBR::TextureRole::BaseColor, NativePBR::TextureRole::Normal,
			NativePBR::TextureRole::Rmaos, NativePBR::TextureRole::Emission, NativePBR::TextureRole::Displacement,
			NativePBR::TextureRole::Features0, NativePBR::TextureRole::Features1 };
		std::unordered_set<std::string> visited;
		for (const auto& group : priority) {
			for (const auto* recipe : group) {
				for (const auto role : roles) {
					if (!a_extendedRoles && role > NativePBR::TextureRole::Emission)
						break;
					const auto& request = recipe->textures[static_cast<size_t>(role)];
					if (request.canonicalPath.empty()) {
						if (request.required || !request.assetPath.empty())
							++plan.invalid;
						continue;
					}
					const auto valid = NativePBR::CanonicalTexturePath(request.canonicalPath);
					if (!valid || *valid != request.canonicalPath || !request.canonicalPath.ends_with(".dds")) {
						++plan.invalid;
						continue;
					}
					if (!visited.insert(request.canonicalPath).second)
						continue;
					if (a_present.contains(request.canonicalPath)) {
						++plan.alreadyPresent;
						continue;
					}
					if (a_attempted.contains(request.canonicalPath)) {
						++plan.alreadyAttempted;
						continue;
					}
					if (plan.paths.size() >= a_maxTextures) {
						++plan.deferred;
						continue;
					}
					plan.paths.push_back(request.canonicalPath);
				}
			}
		}
		return plan;
	}
}
