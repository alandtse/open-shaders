#pragma once

#include "NativePBRRegistry.h"
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
        UnsupportedProjection = 1u << 8
    };

    enum class TextureInterpretation : uint8_t { ColorSrgb, DataLinear };
    enum class TextureChannels : uint8_t { Unspecified, RoughnessMetallicAoSpecular };

    /** @brief Describes a texture requirement without assuming source resolution or GPU residency. */
    struct TextureRequest
    {
        TextureRole role = TextureRole::BaseColor;
        TextureInterpretation interpretation = TextureInterpretation::DataLinear;
        TextureChannels channels = TextureChannels::Unspecified;
        std::string assetPath, observedResourceName;
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

    /** @brief Compiles an owned snapshot without accessing game objects or GPU resources. */
    inline Recipe CompileRecipe(const Snapshot& a_snapshot)
    {
        Recipe result{ a_snapshot.identity, a_snapshot.revision, a_snapshot.material, UnknownOpacity };
        const auto& material = result.parameters;
        for (size_t index = 0; index < result.textures.size(); ++index) {
            auto& texture = result.textures[index];
            texture.role = static_cast<TextureRole>(index);
            texture.interpretation = texture.role == TextureRole::BaseColor || texture.role == TextureRole::Emission ?
                TextureInterpretation::ColorSrgb : TextureInterpretation::DataLinear;
            if (texture.role == TextureRole::Rmaos) texture.channels = TextureChannels::RoughnessMetallicAoSpecular;
            texture.assetPath = material.texturePaths[index];
            texture.observedResourceName = material.textureResourceNames[index];
            texture.required = index <= static_cast<size_t>(TextureRole::Rmaos);
        }
        if (!result.identity || !result.revision || !IsFinite(material) || material.clampMode > 3 ||
            (material.kind != MaterialKind::Mesh && material.kind != MaterialKind::Landscape)) result.issues |= InvalidSnapshot;
        if (material.kind == MaterialKind::Landscape) result.issues |= UnsupportedLandscape;
        else if (!material.texturePathsComplete || material.texturePaths[0].empty() ||
            material.texturePaths[1].empty() || material.texturePaths[2].empty()) result.issues |= MissingRequiredTextures;
        if (material.pbrFlags) result.issues |= UnsupportedPbrFeatures;
        if (material.glint.enabled || material.projectedGlint.enabled) result.issues |= UnsupportedGlint;
        if (!material.texturePaths[static_cast<size_t>(TextureRole::Displacement)].empty()) result.issues |= UnsupportedDisplacement;
        if (!material.texturePaths[static_cast<size_t>(TextureRole::Features0)].empty() ||
            !material.texturePaths[static_cast<size_t>(TextureRole::Features1)].empty()) result.issues |= UnsupportedFeatureTextures;
        if (material.projectedColor != std::array<float, 3>{ 1, 1, 1 } || material.projectedRoughness != 1.0f ||
            material.projectedSpecularLevel != 0.04f || material.projectedGlint.enabled) result.issues |= UnsupportedProjection;
        return result;
    }
    /** @brief Maintains compiled recipes across consistent registry generations. */
    class RecipeCache
    {
    public:
        /** @brief Recompiles changed snapshots and retains pending identities until removal. */
        void Update(const Registry::ExportedGeneration& a_generation)
        {
            if (a_generation.revision <= revision || a_generation.revision < observedRevision) return;
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
            if (a_changes.revision < observedRevision || (a_changes.revision == revision && !a_changes.hasMore)) return;
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
            if (!a_changes.hasMore) revision = a_changes.revision;
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

}
