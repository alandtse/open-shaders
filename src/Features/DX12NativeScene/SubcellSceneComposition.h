#pragma once

#include "PreparedScene.h"
#include <algorithm>
#include <span>
#include <string>
#include <unordered_set>

namespace DX12Native
{
    struct SubcellComposition
    {
        std::shared_ptr<const PreparedCell> cell;
        uint32_t sources = 0, admittedInstances = 0, duplicateInstances = 0;
        uint32_t admissionFailures = 0, deduplicatedLights = 0, uniqueGeometries = 0;
    };

    /** Build one immutable CPU snapshot for an incomplete or complete active
     *  3x3 from already-loaded independently owned subcell packages.
     *  GPU geometry buffers and material attributes remain reference-counted.
     *  No GPU work or engine pointer traversal happens here.
     */
    inline SubcellComposition ComposeActiveSubcells(
        std::span<const std::shared_ptr<const PreparedCell>> sources,
        uint64_t sceneEpoch, uint64_t batch)
    {
        SubcellComposition stats{};
        if (!sceneEpoch || !batch) return stats;
        auto result = std::make_shared<PreparedCell>();
        auto merged = std::make_shared<Snapshot>();
        merged->generation = merged->sweep = batch;
        merged->scope.scene = sceneEpoch;
        merged->scope.batch = batch;
        merged->scope.policy = 3; // native player-centered subcell composition
        merged->truncated = true; // missing input chunks remain valid as a partial RT scene
        merged->offline = true;

        json textures = json::array();
        json rooms = json::array();
        std::unordered_set<std::string> texturePaths, instanceKeys, lightKeys;
        std::unordered_set<const Geometry*> residentGeometry;
        std::shared_ptr<MaterialSource> materialSource;
        for (const auto& source : sources) {
            if (!source || !source->snapshot ||
                !source->snapshot->offline || !source->snapshot->materialSource)
                continue;
            ++stats.sources;
            if (!materialSource)
                materialSource = std::make_shared<MaterialSource>(*source->snapshot->materialSource);
            const uint32_t roomBase = uint32_t(rooms.size());
            if (rooms.size() + source->roomDefinitions.size() > 128) {
                ++stats.admissionFailures;
                continue;
            }
            for (const auto& room : source->roomDefinitions) rooms.push_back(room);
            if (const auto refs = source->snapshot->materialSource->textureReferences;
                refs && refs->is_array()) {
                for (const auto& ref : *refs) {
                    if (!ref.is_object() || !ref.contains("path") || !ref.at("path").is_string()) continue;
                    const auto path = ref.at("path").get<std::string>();
                    if (texturePaths.insert(path).second) textures.push_back(ref);
                }
            }
            // Dedup by stable placed-reference identity, never by proximity.
            // Two meshes at similar coordinates are not necessarily the same
            // object and must not be clustered like lights.
            for (const auto& input : source->snapshot->instances) {
                if (!input.mesh || !source->geometries.contains(input.mesh.get()) ||
                    input.preparedIdentity.referenceKey.empty() ||
                    input.preparedIdentity.shapeName.empty()) {
                    ++stats.admissionFailures;
                    continue;
                }
                const auto& id = input.preparedIdentity;
                const auto key = id.referenceKey + std::string(1, char(0)) + id.shapeName + std::string(1, char(0)) +
                    std::to_string(id.blockIndex);
                if (!instanceKeys.insert(key).second) {
                    ++stats.duplicateInstances;
                    continue;
                }
                if (merged->instances.size() >= 8192) {
                    ++stats.admissionFailures;
                    continue;
                }
                Instance instance = input;
                instance.identity = merged->instances.size() + 1;
                if (instance.material.roomIndex != UINT32_MAX) {
                    if (instance.material.roomIndex >= source->roomDefinitions.size())
                        instance.material.roomIndex = UINT32_MAX;
                    else
                        instance.material.roomIndex += roomBase;
                }
                const auto& geometry = source->geometries.at(input.mesh.get());
                if (residentGeometry.insert(geometry.get()).second) {
                    result->geometries.emplace(input.mesh.get(), geometry);
                    result->bytes += geometry->allocationBytes;
                }
                merged->instances.push_back(std::move(instance));
                ++stats.admittedInstances;
                if (!input.material.opaqueCandidate) ++result->maskedInstances;
                if (input.material.attributes) {
                    ++result->shadedInstances;
                    if (input.material.attributes->hasTangentFrame) ++result->tangentInstances;
                }
            }
            for (size_t index = 0; index < source->nativeLights.size(); ++index) {
                std::string key;
                if (index < source->lightDefinitions.size()) {
                    const auto& definition = source->lightDefinitions[index];
                    if (definition.is_object() && definition.contains("referenceKey") &&
                        definition.at("referenceKey").is_string())
                        key = definition.at("referenceKey").get<std::string>();
                }
                if (!key.empty() && !lightKeys.insert(key).second) {
                    ++stats.deduplicatedLights;
                    continue;
                }
                if (result->nativeLights.size() >= 256) { ++stats.admissionFailures; continue; }
                result->nativeLights.push_back(source->nativeLights[index]);
                if (index < source->lightDefinitions.size())
                    result->lightDefinitions.push_back(source->lightDefinitions[index]);
                if (index < source->lightEligibility.size())
                    result->lightEligibility.push_back(source->lightEligibility[index]);
            }
        }
        if (!materialSource || merged->instances.empty() || textures.size() > 4096) return stats;
        materialSource->rooms = std::make_shared<const json>(std::move(rooms));
        materialSource->textureReferences = std::make_shared<const json>(std::move(textures));
        materialSource->usePreparedTextureManifest = true;
        merged->materialSource = std::move(materialSource);
        result->roomDefinitions = *merged->materialSource->rooms;
        result->digest = "composed-active-subcells";
        result->snapshot = std::move(merged);
        stats.uniqueGeometries = uint32_t(result->geometries.size());
        stats.cell = std::move(result);
        return stats;
    }
}
