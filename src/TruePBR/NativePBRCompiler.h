#pragma once

#include "NativePBRRegistry.h"
#include "Utils/StringUtils.h"
#include <memory>
#include <unordered_set>

namespace NativePBR
{
	enum RecipeIssue : uint32_t
	{
		MissingRequiredTextures = 1u << 0,
		UnsupportedLandscape = 1u << 1,
		UnsupportedPbrFeatures = 1u << 2,
		UnsupportedGlint = 1u << 3,
		UnknownOpacity = 1u << 4,
		InvalidSnapshot = 1u << 5,
		UnsupportedDisplacement = 1u << 6,
		UnsupportedFeatureTextures = 1u << 7,
		UnsupportedProjection = 1u << 8,
		UnknownNormalMode = 1u << 9,
		UnknownUvAnimation = 1u << 10,
		UnmatchedSceneInstance = 1u << 11,
		InvalidTexturePath = 1u << 12
	};

	enum class TextureInterpretation : uint8_t
	{
		ColorSrgb,
		DataLinear
	};
	enum class TextureChannels : uint8_t
	{
		Unspecified,
		RoughnessMetallicAoSpecular
	};

	/** @brief Describes a texture requirement without assuming source resolution or GPU residency. */
	struct TextureRequest
	{
		TextureRole role = TextureRole::BaseColor;
		TextureInterpretation interpretation = TextureInterpretation::DataLinear;
		TextureChannels channels = TextureChannels::Unspecified;
		std::string assetPath, canonicalPath, observedResourceName;
		TextureResourceInfo resource;
		bool required = false;
	};

	/** @brief Retains raw material metadata and explicit native compatibility limitations. */
	struct Recipe
	{
		uint64_t identity = 0, revision = 0;
		MaterialData parameters;
		uint32_t issues = 0;
		std::array<TextureRequest, static_cast<size_t>(TextureRole::Count)> textures{};
	};

	/** @brief Ignores only proven zero-contribution optional lobes.
     *
     * Fuzz specular is a lerp with the captured fuzz weight. A weight of zero
     * makes it identical to the base specular response. Subsurface uses
     * (1 - Thickness) for both transmission and lobe weights, so captured
     * thickness one makes it neutral. No other active PBR feature is
     * discarded. In particular coat, glint, projection, displacement and
     * feature textures remain unsupported.
     */
	inline uint32_t EffectiveNativeIssues(const Recipe& recipe)
	{
		uint32_t effective = recipe.issues;
		constexpr uint32_t kSubsurface = 1u << 0;
		constexpr uint32_t kFuzz = 1u << 5;
		const auto flags = recipe.parameters.pbrFlags;
		if ((flags & ~(kSubsurface | kFuzz)) == 0 &&
			((flags & kSubsurface) == 0 || recipe.parameters.subsurfaceOpacity == 1.0f) &&
			((flags & kFuzz) == 0 || recipe.parameters.fuzzWeight == 0.0f))
			effective &= ~UnsupportedPbrFeatures;
		return effective;
	}

	/** @brief Per-bit nonexclusive reasons for runtime PBR incompatibility.
     * Recipe bits 4, 9, 10 and 11 are baseline unknowns resolved by
     * static instance inspection and are intentionally not tallied here.
     */
	struct RecipeIssueTally
	{
		std::array<uint32_t, 13> issues{};
		std::array<uint32_t, 7> pbrFlags{};
		uint32_t affected = 0;

		void Observe(const Recipe& recipe)
		{
			constexpr uint32_t resolved = UnknownOpacity | UnknownNormalMode |
			                              UnknownUvAnimation | UnmatchedSceneInstance;
			const auto mask = recipe.issues & ~resolved;
			if (mask == 0 && recipe.parameters.pbrFlags == 0)
				return;
			++affected;
			for (size_t i = 0; i < issues.size(); ++i)
				if ((mask & (1u << i)) != 0)
					++issues[i];
			for (size_t i = 0; i < pbrFlags.size(); ++i)
				if ((recipe.parameters.pbrFlags & (1u << i)) != 0)
					++pbrFlags[i];
		}
	};

	/** @brief Normalizes a relative texture asset path without permitting parent or drive traversal. */
	inline std::optional<std::string> CanonicalTexturePath(const std::string& a_path)
	{
		if (a_path.empty() || a_path.size() > 1024 || a_path.find(':') != std::string::npos ||
			std::ranges::any_of(a_path, [](unsigned char c) { return c < 32; }))
			return std::nullopt;
		auto normalized = Util::ToLowerAscii(a_path);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		const std::filesystem::path path(normalized);
		try {
			Util::JoinRelativePath({}, path);
		} catch (const std::invalid_argument&) {
			return std::nullopt;
		}
		normalized = path.lexically_normal().generic_string();
		if (normalized.empty() || normalized == "." || normalized.starts_with('/'))
			return std::nullopt;
		if (!normalized.starts_with("textures/"))
			normalized = "textures/" + normalized;
		return normalized;
	}

	/** @brief Compiles an owned snapshot without accessing game objects or GPU resources. */
	inline Recipe CompileRecipe(const Snapshot& a_snapshot)
	{
		Recipe result{ a_snapshot.identity, a_snapshot.revision, a_snapshot.material,
			UnknownOpacity | UnknownNormalMode | UnknownUvAnimation | UnmatchedSceneInstance };
		const auto& material = result.parameters;
		for (size_t index = 0; index < result.textures.size(); ++index) {
			auto& texture = result.textures[index];
			texture.role = static_cast<TextureRole>(index);
			texture.interpretation = texture.role == TextureRole::BaseColor || texture.role == TextureRole::Emission ?
			                             TextureInterpretation::ColorSrgb :
			                             TextureInterpretation::DataLinear;
			if (texture.role == TextureRole::Rmaos)
				texture.channels = TextureChannels::RoughnessMetallicAoSpecular;
			texture.assetPath = material.texturePaths[index];
			if (!texture.assetPath.empty()) {
				if (auto canonical = CanonicalTexturePath(texture.assetPath))
					texture.canonicalPath = std::move(*canonical);
				else
					result.issues |= InvalidTexturePath;
			}
			texture.observedResourceName = material.textureResourceNames[index];
			texture.resource = material.textureResources[index];
			texture.required = index <= static_cast<size_t>(TextureRole::Rmaos);
		}
		if (!result.identity || !result.revision || !IsFinite(material) || material.clampMode > 3 ||
			(material.kind != MaterialKind::Mesh && material.kind != MaterialKind::Landscape))
			result.issues |= InvalidSnapshot;
		if (material.kind == MaterialKind::Landscape)
			result.issues |= UnsupportedLandscape;
		else if (!material.texturePathsComplete || material.texturePaths[0].empty() ||
				 material.texturePaths[1].empty() || material.texturePaths[2].empty())
			result.issues |= MissingRequiredTextures;
		if (material.pbrFlags)
			result.issues |= UnsupportedPbrFeatures;
		if (material.glint.enabled || material.projectedGlint.enabled)
			result.issues |= UnsupportedGlint;
		if (!material.texturePaths[static_cast<size_t>(TextureRole::Displacement)].empty())
			result.issues |= UnsupportedDisplacement;
		if (!material.texturePaths[static_cast<size_t>(TextureRole::Features0)].empty() ||
			!material.texturePaths[static_cast<size_t>(TextureRole::Features1)].empty())
			result.issues |= UnsupportedFeatureTextures;
		if (material.projectedColor != std::array<float, 3>{ 1, 1, 1 } || material.projectedRoughness != 1.0f ||
			material.projectedSpecularLevel != 0.04f || material.projectedGlint.enabled)
			result.issues |= UnsupportedProjection;
		return result;
	}
	/** @brief Maintains compiled recipes across consistent registry generations. */
	class RecipeCache
	{
	public:
		/** @brief Recompiles changed snapshots and retains pending identities until removal. */
		void Update(const Registry::ExportedGeneration& a_generation)
		{
			if (a_generation.revision <= revision || a_generation.revision < observedRevision)
				return;
			std::unordered_set<uint64_t> live(a_generation.pendingIdentities.begin(), a_generation.pendingIdentities.end());
			for (const auto& snapshot : a_generation.snapshots) {
				live.insert(snapshot.identity);
				const auto found = recipes.find(snapshot.identity);
				if (found == recipes.end() || found->second.revision != snapshot.revision) {
					recipes.insert_or_assign(snapshot.identity, CompileRecipe(snapshot));
					knownRevisions.insert_or_assign(snapshot.identity, snapshot.revision);
					++compilations;
				}
			}
			std::erase_if(recipes, [&](const auto& entry) { return !live.contains(entry.first); });
			std::erase_if(knownRevisions, [&](const auto& entry) { return !live.contains(entry.first); });
			pendingIdentities = std::unordered_set<uint64_t>(a_generation.pendingIdentities.begin(), a_generation.pendingIdentities.end());
			revision = a_generation.revision;
			observedRevision = revision;
		}

		/** @brief Applies only changed recipes and removes identities no longer present. */
		void Update(const Registry::ChangeSet& a_changes)
		{
			if (a_changes.revision < observedRevision || (a_changes.revision == revision && !a_changes.hasMore))
				return;
			observedRevision = a_changes.revision;
			std::unordered_set<uint64_t> live(a_changes.liveIdentities.begin(), a_changes.liveIdentities.end());
			for (const auto& snapshot : a_changes.changed) {
				const auto found = knownRevisions.find(snapshot.identity);
				if (found == knownRevisions.end() || found->second != snapshot.revision) {
					recipes.insert_or_assign(snapshot.identity, CompileRecipe(snapshot));
					knownRevisions.insert_or_assign(snapshot.identity, snapshot.revision);
					++compilations;
				}
			}
			std::erase_if(recipes, [&](const auto& entry) { return !live.contains(entry.first); });
			std::erase_if(knownRevisions, [&](const auto& entry) { return !live.contains(entry.first); });
			pendingIdentities = std::unordered_set<uint64_t>(a_changes.pendingIdentities.begin(), a_changes.pendingIdentities.end());
			if (!a_changes.hasMore)
				revision = a_changes.revision;
		}

		/** @brief Returns revisions already represented in the cache. */
		const std::unordered_map<uint64_t, uint64_t>& KnownRevisions() const { return knownRevisions; }

		/** @brief Returns compiled recipes, including prior generations retained while pending. */
		const std::unordered_map<uint64_t, Recipe>& Recipes() const { return recipes; }
		uint64_t Revision() const { return revision; }
		uint64_t Compilations() const { return compilations; }
		size_t Pending() const { return pendingIdentities.size(); }
		/** @brief Identifies recipes retained from a prior generation while replacement is pending. */
		bool IsPending(uint64_t a_identity) const { return pendingIdentities.contains(a_identity); }

	private:
		std::unordered_map<uint64_t, Recipe> recipes;
		std::unordered_map<uint64_t, uint64_t> knownRevisions;
		std::unordered_set<uint64_t> pendingIdentities;
		uint64_t revision = 0, observedRevision = 0, compilations = 0;
	};

	/** @brief Holds immutable completed recipes for one observed registry generation. */
	struct RecipeGeneration
	{
		uint64_t revision = 0;
		bool batchesComplete = false;
		std::unordered_map<uint64_t, std::shared_ptr<const Recipe>> recipes;
	};

	/** @brief Publishes owned recipes while excluding dirty and not-yet-compiled entries. */
	inline std::shared_ptr<const RecipeGeneration> PublishRecipes(const RecipeCache& a_cache,
		const Registry::ChangeSet& a_changes, const std::shared_ptr<const RecipeGeneration>& a_previous)
	{
		auto result = std::make_shared<RecipeGeneration>();
		result->revision = a_changes.revision;
		result->batchesComplete = !a_changes.hasMore;
		result->recipes.reserve(a_cache.Recipes().size());
		for (const auto& [identity, recipe] : a_cache.Recipes()) {
			if (a_cache.IsPending(identity))
				continue;
			if (a_previous) {
				const auto previous = a_previous->recipes.find(identity);
				if (previous != a_previous->recipes.end() && previous->second->revision == recipe.revision) {
					result->recipes.emplace(identity, previous->second);
					continue;
				}
			}
			result->recipes.emplace(identity, std::make_shared<const Recipe>(recipe));
		}
		return result;
	}

}
