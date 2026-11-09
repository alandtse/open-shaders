#pragma once

#include "RuntimeGeometryPreparation.h"
#include "Utils/SkinnedMesh.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Native
{
    struct PlayerSkinTopology
    {
        std::vector<Util::SkinnedVertex> vertices;
        std::vector<uint16_t> indices;
        uint32_t partitions = 0;
        uint32_t bones = 0;
        std::uintptr_t partitionIdentity = 0;
        uint32_t sourceVertices = 0;
    };
    struct PlayerSkinPose
    {
        std::shared_ptr<const PlayerSkinTopology> topology;
        std::vector<RE::NiTransform> matrices;
        std::string key, shape;
        std::array<float, 4> bound{};
        uint32_t frame = 0;
    };

    /** @brief Captures the player model's current skeleton without keeping
     * engine pointers or GPU work on the draw hook. Not a disk cache.
     */
    class PlayerSkinObserver
    {
    public:
        void Reset()
        {
            std::scoped_lock lock(mutex);
            ++epoch;
            topologies.clear();
            poses.clear();
            observed = rejected = 0;
        }
        uint64_t Epoch() const { std::scoped_lock lock(mutex); return epoch; }
        uint64_t Observed() const { std::scoped_lock lock(mutex); return observed; }
        uint64_t Rejected() const { std::scoped_lock lock(mutex); return rejected; }
        size_t Shapes() const { std::scoped_lock lock(mutex); return poses.size(); }

        /** @brief Uses the game render hook's borrowed pointers only here. */
        void Observe(RE::BSGeometry* geometry, uint32_t frame)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!geometry || !player) return;
            // Many skinned draw shapes have no userData of their own. Resolve
            // ownership through the attached scenegraph, without accepting
            // NPCs or objects merely because they share a body mesh asset.
            bool isPlayer = false;
            const RE::NiAVObject* node = geometry;
            for (uint32_t level = 0; node && level < 32; ++level, node = node->parent) {
                if (node->GetUserData() == player) { isPlayer = true; break; }
            }
            if (!isPlayer) return;
            auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
            // Until alpha/material parity is available, restrict the DXR
            // character path to surfaces that can safely be treated as opaque.
            if (geometry->GetGeometryRuntimeData().alphaProperty) return;
            if (!skin || !skin->skinData || !skin->skinData->boneData ||
                !skin->bones || !skin->skinPartition || !skin->numMatrices ||
                skin->numMatrices > 256 || skin->skinData->bones < skin->numMatrices)
                return;
            const auto id = reinterpret_cast<std::uintptr_t>(geometry);
            const auto* partitions = skin->skinPartition.get();
            if (!partitions->numPartitions || partitions->numPartitions > 64) return;
            uint32_t sourceVertices = 0;
            for (uint32_t p = 0; p < partitions->numPartitions; ++p) {
                sourceVertices += partitions->partitions[p].vertices;
                if (sourceVertices > UINT16_MAX) return;
            }
            std::shared_ptr<const PlayerSkinTopology> topology;
            uint64_t captureEpoch = 0;
            {
                std::scoped_lock lock(mutex);
                captureEpoch = epoch;
                const auto prior = poses.find(id);
                if (prior != poses.end() && prior->second->frame == frame)
                    return;
                const auto found = topologies.find(id);
                if (found != topologies.end() &&
                    found->second->partitions == partitions->numPartitions &&
                    found->second->bones == skin->numMatrices &&
                    found->second->partitionIdentity == reinterpret_cast<std::uintptr_t>(partitions) &&
                    found->second->sourceVertices == sourceVertices)
                    topology = found->second;
            }
            if (!topology) {
                auto created = std::make_shared<PlayerSkinTopology>();
                created->partitions = partitions->numPartitions;
                created->bones = skin->numMatrices;
                created->partitionIdentity = reinterpret_cast<std::uintptr_t>(partitions);
                created->sourceVertices = sourceVertices;
                if (!Util::ReadSkinnedVertices(geometry, created->vertices) ||
                    created->vertices.empty() || created->vertices.size() > UINT16_MAX)
                    return;
                uint32_t vertexOffset = 0;
                bool valid = true;
                for (uint32_t p = 0; p < partitions->numPartitions; ++p) {
                    const auto& part = partitions->partitions[p];
                    if (!part.buffData || !part.buffData->rawVertexData ||
                        !part.vertices || !part.triangles || !part.triList) {
                        valid = false; break;
                    }
                    // Partition-local indices must align with the vertices
                    // decoded from that same partition; rawIndexData may be a
                    // different, full-mesh indexing space.
                    const auto* indices = part.triList;
                    for (uint32_t i = 0; i < uint32_t(part.triangles) * 3u; ++i) {
                        if (indices[i] >= part.vertices ||
                            vertexOffset + indices[i] > UINT16_MAX) {
                            valid = false; break;
                        }
                        created->indices.push_back(uint16_t(vertexOffset + indices[i]));
                    }
                    if (!valid) break;
                    vertexOffset += part.vertices;
                }
                if (!valid || vertexOffset != created->vertices.size() ||
                    created->indices.empty() || created->indices.size() > 400000)
                    return;
                topology = std::move(created);
            }
            auto sample = std::make_shared<PlayerSkinPose>();
            sample->topology = topology;
            sample->matrices.reserve(skin->numMatrices);
            for (uint32_t i = 0; i < skin->numMatrices; ++i) {
                if (!skin->bones[i]) return;
                const RE::NiTransform* boneWorld = skin->boneWorldTransforms &&
                    skin->boneWorldTransforms[i] ? skin->boneWorldTransforms[i] :
                    &skin->bones[i]->world;
                // Output is in world space: bone world * bind-pose skin-to-bone.
                const auto matrix = (*boneWorld) * skin->skinData->boneData[i].skinToBone;
                if (!std::isfinite(matrix.translate.x) ||
                    !std::isfinite(matrix.translate.y) ||
                    !std::isfinite(matrix.translate.z) || !std::isfinite(matrix.scale))
                    return;
                sample->matrices.push_back(matrix);
            }
            sample->key = std::format("player:{:X}", id);
            sample->shape = geometry->name.empty() ? "player-skinned" : geometry->name.c_str();
            sample->bound = { geometry->worldBound.center.x,
                geometry->worldBound.center.y, geometry->worldBound.center.z,
                geometry->worldBound.radius };
            if (!std::isfinite(sample->bound[3]) || sample->bound[3] <= 0) return;
            sample->frame = frame;
            std::scoped_lock lock(mutex);
            if (epoch != captureEpoch) return;
            topologies[id] = topology;
            poses[id] = std::move(sample);
            ++observed;
        }

        std::vector<std::shared_ptr<const PlayerSkinPose>> Export(uint32_t frame)
        {
            std::scoped_lock lock(mutex);
            std::vector<std::shared_ptr<const PlayerSkinPose>> output;
            for (auto iter = poses.begin(); iter != poses.end();) {
                if (uint32_t(frame - iter->second->frame) > 12) {
                    topologies.erase(iter->first);
                    iter = poses.erase(iter);
                    continue;
                }
                output.push_back(iter->second);
                ++iter;
            }
            return output;
        }

    private:
        mutable std::mutex mutex;
        std::unordered_map<std::uintptr_t, std::shared_ptr<const PlayerSkinTopology>> topologies;
        std::unordered_map<std::uintptr_t, std::shared_ptr<const PlayerSkinPose>> poses;
        uint64_t epoch = 1, observed = 0, rejected = 0;
    };

    /** @brief Deforms copied bind-pose vertices off-thread.
     * Missing bone influences are rejected rather than generating false hits.
     * Position-only geometry is valid for DXR intersections, but not yet for
     * full character PBR shading.
     */
    inline std::shared_ptr<const RuntimeGeometryPacket> DeformPlayerSkin(
        const PlayerSkinPose& pose)
    {
        if (!pose.topology || pose.topology->vertices.empty() ||
            pose.topology->indices.empty() || pose.matrices.empty())
            return {};
        auto packet = std::make_shared<RuntimeGeometryPacket>();
        packet->referenceKey = pose.key;
        packet->shapeName = pose.shape;
        packet->vertexCount = uint32_t(pose.topology->vertices.size());
        packet->vertexStride = 16;
        packet->indexCount = uint32_t(pose.topology->indices.size());
        packet->indices = pose.topology->indices;
        packet->captureFrame = pose.frame;
        packet->bound = pose.bound;
        packet->transform = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        packet->vertices.resize(size_t(packet->vertexCount) * 16);
        if (packet->Bytes() > 8ull * 1024 * 1024) return {};
        for (uint32_t i = 0; i < packet->vertexCount; ++i) {
            const auto& source = pose.topology->vertices[i];
            std::array<float, 4> output{};
            float total = 0;
            for (size_t slot = 0; slot < 4; ++slot) {
                const auto bone = source.bone[slot];
                const auto weight = source.weight[slot];
                if (weight <= 0) continue;
                if (bone == Util::InvalidSkinBone || bone >= pose.matrices.size() ||
                    !std::isfinite(weight)) return {};
                const auto point = pose.matrices[bone] * source.position;
                if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                    !std::isfinite(point.z)) return {};
                output[0] += point.x * weight;
                output[1] += point.y * weight;
                output[2] += point.z * weight;
                total += weight;
            }
            if (!std::isfinite(total) || total < 0.5f || total > 1.5f) return {};
            output[0] /= total;
            output[1] /= total;
            output[2] /= total;
            output[3] = 1.0f;
            std::memcpy(packet->vertices.data() + size_t(i) * 16,
                output.data(), sizeof(output));
        }
        return packet;
    }

    /** @brief Experimental worker: current-frame player mesh, never written to
     * the static geometry disk cache. Completed results are immutable.
     */
    class PlayerSkinPreparation
    {
        struct Work
        {
            std::atomic<bool> done{ false };
            std::shared_ptr<const PreparedCell> combined;
            uint32_t frame = 0, shapes = 0;
            bool hasPlayer = false;
            std::string error;
        };
    public:
        void Reset()
        {
            observer.Reset();
            pending.reset();
            completed.reset();
            lastQueuedFrame = lastCompletedFrame = 0;
            successful = failed = 0;
            completedHasPlayer = false;
            lastError.clear();
        }
        void Observe(RE::BSGeometry* geometry, uint32_t frame)
        {
            observer.Observe(geometry, frame);
        }
        void Advance(const GraphicsBackend& backend,
            const std::shared_ptr<const PreparedCell>& baseline,
            uint64_t sceneEpoch, uint64_t gpuAllowance, uint32_t currentFrame,
            uint32_t minFrameInterval)
        {
            if (pending && pending->done.load(std::memory_order_acquire)) {
                if (pending->combined && pending->frame >= lastCompletedFrame) {
                    completed = pending->combined;
                    lastCompletedFrame = pending->frame;
                    completedHasPlayer = pending->hasPlayer;
                    ++successful;
                } else ++failed;
                lastError = pending->error;
                pending.reset();
            }
            if (pending || !backend.d3d12Device || !baseline ||
                !baseline->snapshot || !gpuAllowance || !currentFrame ||
                (lastQueuedFrame && uint32_t(currentFrame - lastQueuedFrame) < minFrameInterval))
                return;
            const auto poses = observer.Export(currentFrame);
            // Drop stale character geometry when the player has been
            // invisible for long enough (first person, out of view, etc.).
            // A fresh CELL generation then contains only the baseline.
            if (poses.empty() && !completedHasPlayer) return;
            if (!poses.empty() && std::ranges::none_of(poses, [&](const auto& pose) {
                return pose && pose->frame > lastQueuedFrame;
            })) return;
            auto next = std::make_shared<Work>();
            next->frame = currentFrame;
            lastQueuedFrame = currentFrame;
            try {
                static BS::thread_pool<> pool{ 1 };
                pool.detach_task([next, poses, backend, baseline, sceneEpoch, gpuAllowance] {
                    try {
                        std::vector<std::shared_ptr<const RuntimeGeometryPacket>> packets;
                        packets.reserve(poses.size());
                        for (const auto& pose : poses) {
                            if (auto packet = DeformPlayerSkin(*pose))
                                packets.push_back(std::move(packet));
                        }
                        next->shapes = uint32_t(packets.size());
                        if (!poses.empty() && packets.size() != poses.size()) {
                            next->error = "One or more player shapes could not be deformed";
                        } else if (!packets.empty()) {
                            auto runtime = PrepareRuntimeGeometryBatch(
                                backend, packets, {}, sceneEpoch, gpuAllowance);
                            if (runtime && runtime->snapshot &&
                                runtime->snapshot->instances.size() == packets.size()) {
                                // Make every successful pose generation distinguishable
                                // from previous GPU builds, even with identical topology.
                                auto mutableRuntime = std::make_shared<PreparedCell>(*runtime);
                                auto snapshot = std::make_shared<Snapshot>(*runtime->snapshot);
                                snapshot->generation = next->frame;
                                snapshot->scope.batch = next->frame;
                                mutableRuntime->snapshot = snapshot;
                                next->combined = ComposeCurrentSceneGeometry(
                                    baseline, mutableRuntime, sceneEpoch);
                                next->hasPlayer = bool(next->combined);
                            }
                        } else if (poses.empty()) {
                            auto clear = std::make_shared<PreparedCell>(*baseline);
                            auto snapshot = std::make_shared<Snapshot>(*baseline->snapshot);
                            snapshot->generation += next->frame;
                            snapshot->sweep = snapshot->generation;
                            snapshot->scope.batch = snapshot->generation;
                            snapshot->scope.policy = 2;
                            snapshot->offline = false;
                            clear->snapshot = snapshot;
                            next->combined = std::move(clear);
                        }
                    } catch (const std::exception& e) { next->error = e.what(); }
                    next->done.store(true, std::memory_order_release);
                });
                pending = std::move(next);
            } catch (const std::exception& e) {
                lastError = e.what();
                ++failed;
            }
        }
        std::shared_ptr<const PreparedCell> GetCombined() const { return completed; }
        size_t ObservedShapes() const { return observer.Shapes(); }
        uint64_t Observations() const { return observer.Observed(); }
        uint32_t LastCompletedFrame() const { return lastCompletedFrame; }
        uint32_t Successful() const { return successful; }
        uint32_t Failed() const { return failed; }
        bool Busy() const { return bool(pending); }
        const std::string& Failure() const { return lastError; }

    private:
        PlayerSkinObserver observer;
        std::shared_ptr<Work> pending;
        std::shared_ptr<const PreparedCell> completed;
        uint32_t lastQueuedFrame = 0, lastCompletedFrame = 0;
        uint32_t successful = 0, failed = 0;
        bool completedHasPlayer = false;
        std::string lastError;
    };
}
