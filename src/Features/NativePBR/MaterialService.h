#pragma once

#include "MaterialCatalog.h"
#include "MaterialInterpretation.h"
#include "MaterialReadiness.h"
#include "NativePBRCompiler.h"
#include "Utils/StringUtils.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>

namespace NativePBR
{
	enum class MaterialLookupStatus : uint8_t
	{
		Available,
		NoPublication,
		MissingMaterial,
		PendingMaterial,
		RevisionMismatch,
		MissingInstance,
		AmbiguousInstance
	};

	inline constexpr std::array<std::string_view, 7> kMaterialLookupStatusNames{
		"available", "no-publication", "missing-material", "pending-material", "revision-mismatch",
		"missing-instance", "ambiguous-instance"
	};

	enum InstanceMaterialIssue : uint32_t
	{
		MissingMatoConfiguration = 1u << 0,
		UnsupportedMatoGlint = 1u << 1,
		NonOpaqueInstance = 1u << 2,
		UnsupportedModelSpaceNormal = 1u << 3,
		UnsupportedAnimatedUv = 1u << 4,
		UnsupportedSkinnedInstance = 1u << 5,
		UnsupportedProjectedInstance = 1u << 6
	};

	inline constexpr std::array<std::string_view, 7> kInstanceMaterialIssueNames{
		"missing-mato-configuration", "unsupported-mato-glint", "non-opaque-instance",
		"unsupported-model-space-normal", "unsupported-animated-uv", "unsupported-skinned-instance",
		"unsupported-projected-instance"
	};

	/** @brief Self-contained lookup result that retains source generations across retirement. */
	struct MaterialLookup
	{
		MaterialLookupStatus status = MaterialLookupStatus::NoPublication;
		std::shared_ptr<const Recipe> recipe;
		std::shared_ptr<const InstanceMaterialOverride> instanceOverride;
		InstanceProperties instanceProperties;
		uint32_t recipeIssues = 0, instanceIssues = 0;
		std::string missingMatoEditorId;
		explicit operator bool() const { return status == MaterialLookupStatus::Available && recipe != nullptr; }
		/** @brief Inspect source, shading and unresolved properties without changing lookup availability. */
		RecipeReadiness Readiness() const
		{
			if (!recipe)
				return {};
			auto effective = EffectiveNativeIssues(*recipe);
			effective &= ~(recipe->issues & ~recipeIssues);
			effective |= recipeIssues & ~recipe->issues;
			return ClassifyRecipeReadiness(effective);
		}
	};

	/** @brief An immutable material generation usable without geometry or renderer state. */
	struct MaterialPublication
	{
		struct InstanceBinding
		{
			uint64_t materialIdentity = 0;
			InstanceProperties properties;
			std::shared_ptr<const InstanceMaterialOverride> materialOverride;
			uint32_t issues = 0;
			bool ambiguous = false;
		};

		uint64_t serial = 0, instanceEpoch = 0;
		std::shared_ptr<const RecipeGeneration> recipes;
		std::shared_ptr<const MaterialCatalogSnapshot> catalog;
		std::vector<InstanceHint> sourceHints;
		std::unordered_map<std::string, InstanceBinding> instances;

		/** @brief Lookup by stable identity, with optional revision pinning. */
		MaterialLookup FindMaterial(uint64_t a_identity, uint64_t a_revision = 0) const
		{
			if (!recipes)
				return { MaterialLookupStatus::NoPublication };
			const auto found = recipes->recipes.find(a_identity);
			if (found == recipes->recipes.end())
				return { recipes->pendingIdentities.contains(a_identity) ? MaterialLookupStatus::PendingMaterial : MaterialLookupStatus::MissingMaterial };
			if (a_revision && found->second->revision != a_revision)
				return { MaterialLookupStatus::RevisionMismatch };
			MaterialLookup result;
			result.status = MaterialLookupStatus::Available;
			result.recipe = found->second;
			result.recipeIssues = result.recipe->issues;
			return result;
		}

		/** @brief Lookup a unique observed reference and shape, independent of scene streaming. */
		MaterialLookup FindInstance(std::string_view a_reference, std::string_view a_shape) const
		{
			if (a_reference.empty() || a_shape.empty())
				return { MaterialLookupStatus::MissingInstance };
			const auto found = instances.find(Key(a_reference, a_shape));
			if (found == instances.end())
				return { MaterialLookupStatus::MissingInstance };
			const auto& binding = found->second;
			if (binding.ambiguous)
				return { MaterialLookupStatus::AmbiguousInstance };
			auto result = FindMaterial(binding.materialIdentity);
			result.instanceProperties = binding.properties;
			result.instanceOverride = binding.materialOverride;
			result.instanceIssues = binding.issues;
			if (binding.issues & MissingMatoConfiguration)
				result.missingMatoEditorId = binding.properties.materialObjectEditorId;
			if (result.recipe) {
				result.recipeIssues &= ~UnmatchedSceneInstance;
				if (binding.properties.opacity != InstanceProperties::Opacity::Unknown)
					result.recipeIssues &= ~UnknownOpacity;
				if (binding.properties.normalMode != InstanceProperties::NormalMode::Unknown)
					result.recipeIssues &= ~UnknownNormalMode;
				if (binding.properties.uvAnimation != InstanceProperties::UvAnimation::Unknown)
					result.recipeIssues &= ~UnknownUvAnimation;
				if (binding.materialOverride)
					result.recipeIssues |= UnsupportedProjection;
			}
			if (binding.properties.opacity == InstanceProperties::Opacity::NonOpaque)
				result.instanceIssues |= NonOpaqueInstance;
			if (binding.properties.normalMode == InstanceProperties::NormalMode::ModelSpace)
				result.instanceIssues |= UnsupportedModelSpaceNormal;
			if (binding.properties.uvAnimation == InstanceProperties::UvAnimation::Animated)
				result.instanceIssues |= UnsupportedAnimatedUv;
			if (binding.properties.skinned)
				result.instanceIssues |= UnsupportedSkinnedInstance;
			if (binding.properties.projectedUv)
				result.instanceIssues |= UnsupportedProjectedInstance;
			return result;
		}

		/** @brief Produces the same key for visible-pixel and ray-hit consumers. */
		static std::string Key(std::string_view a_reference, std::string_view a_shape)
		{
			auto key = Util::ToLowerAscii(a_reference);
			key.push_back('\0');
			key += Util::ToLowerAscii(a_shape);
			return key;
		}
	};

	/** @brief Atomic publisher for NativePBR-owned recipes and per-instance overlays. */
	class MaterialService
	{
	public:
		/** @brief Incrementally compiles independent registry sources and publishes their instance views. */
		bool Refresh(const Registry& a_registry, const MaterialCatalog& a_catalog, size_t a_batchLimit = 32)
		{
			std::scoped_lock lock(refreshMutex);
			const auto catalog = a_catalog.GetSnapshot();
			const auto changes = a_registry.ExportChanges(recipeCache.Revision(), recipeCache.KnownRevisions(), a_batchLimit);
			if (!changes) {
				const auto previous = GetSnapshot();
				if (!previous)
					return Publish(std::make_shared<RecipeGeneration>(), catalog, {});
				return previous && previous->catalog != catalog &&
					Publish(previous->recipes, catalog, previous->sourceHints, previous->instanceEpoch);
			}
			recipeCache.Update(*changes);
			const auto previous = GetSnapshot();
			return Publish(PublishRecipes(recipeCache, *changes, previous ? previous->recipes : nullptr),
				catalog, changes->instanceHints, changes->instanceEpoch);
		}

		/** @brief Publishes a consistent read-only view, preserving older readers' handles. */
		bool Publish(std::shared_ptr<const RecipeGeneration> a_recipes,
			std::shared_ptr<const MaterialCatalogSnapshot> a_catalog, const std::vector<InstanceHint>& a_hints,
			uint64_t a_instanceEpoch = 0)
		{
			std::scoped_lock lock(publicationMutex);
			const auto previous = published.load(std::memory_order_acquire);
			if (previous && previous->recipes && a_recipes && a_recipes->revision < previous->recipes->revision)
				return false;
			if (previous && a_instanceEpoch < previous->instanceEpoch)
				return false;
			auto next = std::make_shared<MaterialPublication>();
			next->serial = ++nextSerial;
			next->instanceEpoch = a_instanceEpoch;
			next->recipes = std::move(a_recipes);
			next->catalog = std::move(a_catalog);
			next->sourceHints = a_hints;
			next->instances.reserve(a_hints.size());
			for (const auto& hint : a_hints) {
				if (!hint.materialIdentity || hint.referenceKey.empty() || hint.shapeName.empty())
					continue;
				const auto key = MaterialPublication::Key(hint.referenceKey, hint.shapeName);
				auto [it, inserted] = next->instances.try_emplace(key);
				auto& binding = it->second;
				if (!inserted) {
					if (binding.materialIdentity != hint.materialIdentity || binding.properties != hint.properties || hint.propertiesConflict)
						binding.ambiguous = true;
					continue;
				}
				binding.materialIdentity = hint.materialIdentity;
				binding.properties = hint.properties;
				binding.ambiguous = hint.propertiesConflict;
				const auto& editorId = hint.properties.materialObjectEditorId;
				if (editorId.empty())
					continue;
				const auto canonical = Util::ToLowerAscii(editorId);
				if (!next->catalog || !next->catalog->materialObjects.contains(canonical)) {
					binding.issues |= MissingMatoConfiguration;
					continue;
				}
				binding.materialOverride = std::make_shared<const InstanceMaterialOverride>(
					InterpretMaterialObject(editorId, next->catalog->materialObjects.at(canonical)));
				if (binding.materialOverride->glint.enabled)
					binding.issues |= UnsupportedMatoGlint;
			}
			published.store(std::move(next), std::memory_order_release);
			return true;
		}

		/** @brief Retain a snapshot for any number of visible or ray-traced lookups. */
		std::shared_ptr<const MaterialPublication> GetSnapshot() const
		{
			return published.load(std::memory_order_acquire);
		}

		/** @brief Exposes only a publication matching the requested registry and instance epoch. */
		std::shared_ptr<const MaterialPublication> GetSnapshotAt(uint64_t a_revision, uint64_t a_instanceEpoch) const
		{
			auto snapshot = GetSnapshot();
			return snapshot && snapshot->recipes && snapshot->recipes->revision == a_revision &&
				snapshot->instanceEpoch == a_instanceEpoch ? snapshot : nullptr;
		}

		/** @brief Reject scene transitions racing a lookup, without filtering material capabilities. */
		std::shared_ptr<const MaterialPublication> GetSnapshotForRegistry(const Registry& a_registry) const
		{
			const auto revision = a_registry.Revision();
			const auto epoch = a_registry.InstanceEpoch();
			auto result = GetSnapshotAt(revision, epoch);
			return a_registry.Revision() == revision && a_registry.InstanceEpoch() == epoch ? result : nullptr;
		}

	private:
		std::mutex refreshMutex, publicationMutex;
		RecipeCache recipeCache;
		uint64_t nextSerial = 0;
		std::atomic<std::shared_ptr<const MaterialPublication>> published;
	};
}
