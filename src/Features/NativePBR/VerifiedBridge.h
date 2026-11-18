#pragma once

#include "NativePBRCompiler.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NativePBR
{
	/** @brief Owned, revision-stamped render-thread observations for a native worker. */
	struct LiveBindingSource
	{
		std::shared_ptr<const RecipeGeneration> generation;
		std::vector<InstanceHint> hints;
	};

	/** @brief Offline source properties, copied before any live recipe is considered. */
	struct PreparedMatchInput
	{
		std::string referenceKey, shapeName;
		std::array<std::string, 4> texturePaths{};  // diffuse, normal, RMAOS, emission
		std::array<float, 4> uvOffsetScale{};
		uint32_t alphaFlags = 0, clampMode = 0;
		float alpha = 1.0f;
		bool pbr = false, opaque = false, valid = false;
	};

	struct VerifiedLiveMatch
	{
		std::shared_ptr<const Recipe> recipe;
		InstanceProperties properties;
	};

	struct LiveMatchReport
	{
		size_t matched = 0, missing = 0, ambiguous = 0, incompatible = 0;
	};

	struct LiveMatchResult
	{
		std::vector<std::optional<VerifiedLiveMatch>> matches;
		LiveMatchReport report;
	};

	/** @brief Only apply observed per-instance emission when each component is finite and physically nonnegative. */
	inline bool ValidLiveEmission(const InstanceProperties& a_properties)
	{
		return a_properties.emissionKnown && std::isfinite(a_properties.emissiveMultiplier) &&
		       a_properties.emissiveMultiplier >= 0 &&
		       std::all_of(a_properties.emissiveColor.begin(), a_properties.emissiveColor.end(),
				   [](float value) { return std::isfinite(value) && value >= 0; });
	}

	/** @brief Rejects advanced PBR that the current native BRDF cannot represent. */
	inline bool NativeMeshRecipeSupported(const Recipe& a_recipe)
	{
		constexpr uint32_t kResolvedByPreparedOpaqueSource =
			UnknownOpacity | UnknownNormalMode | UnknownUvAnimation | UnmatchedSceneInstance;
		return a_recipe.identity != 0 && a_recipe.revision != 0 &&
		       a_recipe.parameters.kind == MaterialKind::Mesh &&
		       (EffectiveNativeIssues(a_recipe) & ~kResolvedByPreparedOpaqueSource) == 0 &&
		       IsFinite(a_recipe.parameters) && a_recipe.parameters.texturePathsComplete;
	}

	/** @brief Proves path and static-surface equivalence before consuming native PBR scalars.
     *
     * The reference/shape observation is merely a candidate. No instance is accepted
     * without a unique candidate, matching shader flags, opaque state, original UVs,
     * alpha and sampler, and four exact normalized texture paths. Neither DX11 objects
     * nor a new texture upload participate in this bridge.
     */
	inline bool NativeMatchCompatible(const PreparedMatchInput& a_prepared,
		const InstanceHint& a_hint, const Recipe& a_recipe)
	{
		if (!a_prepared.valid || !a_prepared.pbr || !a_prepared.opaque ||
			a_hint.propertiesConflict || a_hint.properties.skinned || a_hint.properties.alphaProperty ||
			a_hint.properties.alphaFlags != a_prepared.alphaFlags ||
			!NativeMeshRecipeSupported(a_recipe) || a_hint.materialIdentity != a_recipe.identity ||
			a_prepared.clampMode != a_recipe.parameters.clampMode ||
			(!std::isfinite(a_prepared.alpha) || std::abs(a_prepared.alpha - a_recipe.parameters.materialAlpha) > 0.0001f))
			return false;
		for (size_t i = 0; i < a_prepared.uvOffsetScale.size(); ++i)
			if (!std::isfinite(a_prepared.uvOffsetScale[i]) || std::abs(a_prepared.uvOffsetScale[i] - a_recipe.parameters.uvOffsetScale[0][i]) > 0.0001f)
				return false;
		constexpr std::array<TextureRole, 4> roles{
			TextureRole::BaseColor, TextureRole::Normal, TextureRole::Rmaos, TextureRole::Emission
		};
		for (size_t i = 0; i < roles.size(); ++i) {
			const auto& request = a_recipe.textures[static_cast<size_t>(roles[i])];
			if (a_prepared.texturePaths[i].empty() && request.canonicalPath.empty())
				continue;
			const auto prepared = CanonicalTexturePath(a_prepared.texturePaths[i]);
			if (!prepared || request.canonicalPath.empty() || *prepared != request.canonicalPath)
				return false;
		}
		return true;
	}

	/** @brief Runtime-authoritative identity binding: offline textures/UVs/scalars are not evidence.
     *
     * A unique reference + shape observation is the only association currently available
     * from the render hook. Both prepared and runtime sides must be unique; ambiguous
     * geometry always fails closed. Native capability and residency are evaluated later.
     */
	inline LiveMatchResult ResolveRuntimeMatches(const std::vector<PreparedMatchInput>& a_prepared,
		const LiveBindingSource& a_source)
	{
		LiveMatchResult output;
		output.matches.resize(a_prepared.size());
		if (!a_source.generation || !a_source.generation->batchesComplete) {
			output.report.missing = a_prepared.size();
			return output;
		}
		auto keyFor = [](const std::string& reference, const std::string& shape) {
			std::string key = Util::ToLowerAscii(reference);
			key.push_back('\0');
			key += Util::ToLowerAscii(shape);
			return key;
		};
		std::unordered_map<std::string, size_t> preparedCounts;
		for (const auto& candidate : a_prepared)
			if (!candidate.referenceKey.empty() && !candidate.shapeName.empty())
				++preparedCounts[keyFor(candidate.referenceKey, candidate.shapeName)];
		std::unordered_map<std::string, std::vector<const InstanceHint*>> observed;
		for (const auto& hint : a_source.hints)
			if (hint.materialIdentity && !hint.referenceKey.empty() && !hint.shapeName.empty())
				observed[keyFor(hint.referenceKey, hint.shapeName)].push_back(&hint);
		for (size_t i = 0; i < a_prepared.size(); ++i) {
			const auto& candidate = a_prepared[i];
			if (candidate.referenceKey.empty() || candidate.shapeName.empty()) {
				++output.report.missing;
				continue;
			}
			const auto key = keyFor(candidate.referenceKey, candidate.shapeName);
			auto it = observed.find(key);
			if (it == observed.end()) {
				++output.report.missing;
				continue;
			}
			if (preparedCounts[key] != 1 || it->second.size() != 1 || it->second.front()->propertiesConflict) {
				++output.report.ambiguous;
				continue;
			}
			const auto& hint = *it->second.front();
			// Trust prepared geometry only for traversal eligibility, not material identity.
			// Skinned, alpha-property and non-opaque instances require a different native path.
			if (!candidate.opaque || hint.properties.alphaProperty || hint.properties.skinned ||
				hint.properties.alphaFlags != 0) {
				++output.report.incompatible;
				continue;
			}
			const auto recipeIt = a_source.generation->recipes.find(hint.materialIdentity);
			if (recipeIt == a_source.generation->recipes.end() || !recipeIt->second) {
				++output.report.missing;
				continue;
			}
			const auto& recipe = *recipeIt->second;
			if (recipe.identity != hint.materialIdentity || !recipe.revision ||
				recipe.parameters.kind != MaterialKind::Mesh || !IsFinite(recipe.parameters) ||
				(recipe.issues & InvalidSnapshot) != 0) {
				++output.report.incompatible;
				continue;
			}
			output.matches[i] = VerifiedLiveMatch{ recipeIt->second, hint.properties };
			++output.report.matched;
		}
		return output;
	}

	/** @brief Builds a read-only, per-instance binding plan with fail-closed ambiguity handling. */
	inline LiveMatchResult ResolveLiveMatches(const std::vector<PreparedMatchInput>& a_prepared,
		const LiveBindingSource& a_source)
	{
		LiveMatchResult output;
		output.matches.resize(a_prepared.size());
		if (!a_source.generation || !a_source.generation->batchesComplete) {
			output.report.missing = a_prepared.size();
			return output;
		}
		auto makeKey = [](const std::string& a_reference, const std::string& a_shape) {
			auto key = Util::ToLowerAscii(a_reference);
			key.push_back('\0');
			key += Util::ToLowerAscii(a_shape);
			return key;
		};
		std::unordered_map<std::string, size_t> occurrences;
		for (const auto& candidate : a_prepared)
			if (!candidate.referenceKey.empty() && !candidate.shapeName.empty())
				++occurrences[makeKey(candidate.referenceKey, candidate.shapeName)];
		std::unordered_map<std::string, std::vector<const InstanceHint*>> observations;
		for (const auto& hint : a_source.hints)
			if (hint.materialIdentity && !hint.referenceKey.empty() && !hint.shapeName.empty())
				observations[makeKey(hint.referenceKey, hint.shapeName)].push_back(&hint);
		for (size_t i = 0; i < a_prepared.size(); ++i) {
			const auto& candidate = a_prepared[i];
			if (candidate.referenceKey.empty() || candidate.shapeName.empty()) {
				++output.report.missing;
				continue;
			}
			const auto key = makeKey(candidate.referenceKey, candidate.shapeName);
			const auto found = observations.find(key);
			if (found == observations.end()) {
				++output.report.missing;
				continue;
			}
			if (occurrences[key] != 1 || found->second.size() != 1 ||
				found->second.front()->propertiesConflict) {
				++output.report.ambiguous;
				continue;
			}
			const auto& hint = *found->second.front();
			const auto recipe = a_source.generation->recipes.find(hint.materialIdentity);
			if (recipe == a_source.generation->recipes.end() ||
				!NativeMatchCompatible(candidate, hint, *recipe->second)) {
				++output.report.incompatible;
				continue;
			}
			output.matches[i] = VerifiedLiveMatch{ recipe->second, hint.properties };
			++output.report.matched;
		}
		return output;
	}
}
