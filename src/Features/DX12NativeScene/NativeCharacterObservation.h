#pragma once

#include "Utils/ActorUtils.h"
#include "Features/DX12NativeScene/NativeCharacterMeshCapture.h"
#include "Features/DX12NativeScene/NativeCharacterGeometryPreparation.h"
#include "Features/DX12NativeScene/NativeCharacterDeformation.h"

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <limits>
#include <format>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Character capture is an input-only sidecar. No engine object or renderer
// pointer crosses the native-CPU worker boundary, and original LLF/SCM and
// raster draw paths remain the authorities until native coverage is proven.
namespace DX12Native
{
    struct CharacterPartObservation
    {
        uint32_t actorFormID = 0, ordinal = 0;
        std::string shapeName;
        std::array<float, 4> worldBound{};
        uint32_t vertexCount = 0, triangleCount = 0;
        bool skinned = false, dynamic = false, alpha = false, gpuOnly = false;
    };

    struct CharacterActorObservation
    {
        uint32_t formID = 0;
        bool player = false, ragdoll = false;
        std::array<float, 4> worldBound{};
    };

    struct CharacterObservationGeneration
    {
        uint32_t frame = 0;
        uint64_t sceneEpoch = 0;
        bool playerOnly = false;
        std::vector<CharacterActorObservation> actors;
        std::vector<CharacterPartObservation> parts;
        std::shared_ptr<const CharacterMeshGeneration> stagedGeometry;
        std::shared_ptr<const CharacterDeformedResult> deformed;
        std::string deformationError;
        uint32_t capturedMeshes = 0;
        uint64_t capturedBytes = 0;
        uint32_t droppedActors = 0, droppedParts = 0, invalidParts = 0;
        uint32_t skinnedParts = 0, dynamicParts = 0, rigidParts = 0;
        uint32_t alphaParts = 0, gpuOnlyParts = 0;
    };

    /** @brief Off-thread inventory of third-person character input.
     * This is an observation stage only: no BLAS, native ownership or GPU
     * submission. Dynamic and skinned geometry remains raster-owned.
     */
    class NativeCharacterObservation
    {
    public:
        void Reset()
        {
            ++sceneEpoch;
            pending.reset(); // Detached worker retains only owned values.
            published.store({}, std::memory_order_release);
            attemptedMeshKeys.clear();
            lastObservedFrame = UINT32_MAX;
            workerBusy.store(false, std::memory_order_release);
            submitted.store(0, std::memory_order_relaxed);
            completed.store(0, std::memory_order_relaxed);
        }

        /** @brief Called from the existing opaque-ready render callback.
         * Bounded engine observation is synchronous. Validation, counting and
         * deterministic publication are handled on an independent CPU worker.
         */
        void Advance(uint32_t frame, uint32_t maxActors, uint32_t periodFrames, bool playerOnly,
            bool stageGeometry, bool runSkinning,
            ID3D12Device* device, ID3D12CommandQueue* queue)
        {
            if (pending && pending->done.load(std::memory_order_acquire)) {
                auto done = std::move(pending);
                workerBusy.store(false, std::memory_order_release);
                if (done->result && done->sceneEpoch == sceneEpoch) {
                    // A failed allocation must remain eligible for a later
                    // bounded retry rather than being silently marked cached.
                    if (!done->attemptedKey.empty()) {
                        bool resident = false;
                        if (const auto staged = done->result->stagedGeometry) {
                            for (const auto& part : staged->meshes)
                                resident |= part->actorFormID == done->actorFormID &&
                                    part->partOrdinal == done->partOrdinal &&
                                    part->sourceToken == done->sourceToken;
                        }
                        if (!resident) attemptedMeshKeys.erase(done->attemptedKey);
                    }
                    published.store(std::move(done->result), std::memory_order_release);
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (pending || !frame) return;
            periodFrames = std::clamp(periodFrames, 1u, 120u);
            maxActors = std::clamp(maxActors, 1u, 32u);
            if (lastObservedFrame != UINT32_MAX &&
                uint32_t(frame - lastObservedFrame) < periodFrames) return;
            lastObservedFrame = frame;

            auto captured = std::make_shared<CharacterObservationGeneration>();
            captured->sceneEpoch = sceneEpoch;
            captured->frame = frame;
            captured->playerOnly = playerOnly;
            constexpr size_t kMaximumParts = 512;
            constexpr uint32_t kMaximumPartsPerActor = 96;
            std::optional<CharacterMeshCapture> capturedMesh;
            std::optional<CharacterPoseCapture> capturedPose;
            std::string attemptedKey;

            // Only Get3D(false): never inspect Skyrim's first-person root.
            // In player-only mode, skip the high-process actor enumeration
            // entirely so other actors add zero scenegraph traversal cost.
            const auto captureActor = [&](RE::Actor* actor) {
                if (!actor || !actor->Is3DLoaded()) return;
                if (captured->actors.size() >= maxActors) {
                    ++captured->droppedActors;
                    return;
                }
                auto* root = actor->Get3D(false);
                if (!root) return;
                CharacterActorObservation info;
                info.formID = actor->GetFormID();
                info.player = actor == RE::PlayerCharacter::GetSingleton();
                info.ragdoll = actor->IsDead() || actor->IsInRagdollState();
                const auto& bound = root->worldBound;
                info.worldBound = { bound.center.x, bound.center.y, bound.center.z, bound.radius };
                captured->actors.push_back(std::move(info));
                uint32_t ordinal = 0;

                Util::ForEachGeometry(root, [&](RE::BSGeometry* geometry) {
                    if (!geometry) return;
                    if (ordinal >= kMaximumPartsPerActor || captured->parts.size() >= kMaximumParts) {
                        ++captured->droppedParts;
                        return;
                    }
                    CharacterPartObservation part;
                    part.actorFormID = actor->GetFormID();
                    part.ordinal = ordinal++;
                    if (!geometry->name.empty()) part.shapeName = geometry->name.c_str();
                    const auto& bounds = geometry->worldBound;
                    part.worldBound = { bounds.center.x, bounds.center.y, bounds.center.z, bounds.radius };
                    const auto& runtime = geometry->GetGeometryRuntimeData();
                    part.skinned = bool(runtime.skinInstance);
                    part.dynamic = geometry->AsDynamicTriShape() != nullptr;
                    part.alpha = runtime.alphaProperty != nullptr;
                    if (const auto* tri = geometry->AsTriShape()) {
                        const auto& counts = tri->GetTrishapeRuntimeData();
                        part.vertexCount = counts.vertexCount;
                        part.triangleCount = counts.triangleCount;
                    }
                    part.gpuOnly = !runtime.rendererData ||
                        !runtime.rendererData->rawVertexData ||
                        !runtime.rendererData->rawIndexData;
                    captured->parts.emplace_back(std::move(part));
                    if (stageGeometry && device && part.skinned && !capturedPose) {
                        const uint64_t sourceToken = CharacterMeshSourceToken(*geometry);
                        bool hasBind = capturedMesh && capturedMesh->skinned &&
                            capturedMesh->actorFormID == actor->GetFormID() &&
                            capturedMesh->partOrdinal == ordinal - 1 &&
                            capturedMesh->sourceToken == sourceToken;
                        if (const auto previous = GetPublished();
                            previous && previous->stagedGeometry) {
                            for (const auto& staged : previous->stagedGeometry->meshes)
                                hasBind |= staged->skinned &&
                                    staged->actorFormID == actor->GetFormID() &&
                                    staged->partOrdinal == ordinal - 1 &&
                                    staged->sourceToken == sourceToken;
                        }
                        if (hasBind) capturedPose = CaptureCharacterShaderPose(
                            *geometry, actor->GetFormID(), ordinal - 1, sourceToken, frame);
                    }
                    if (stageGeometry && device && !capturedMesh) {
                        const uint64_t sourceToken = CharacterMeshSourceToken(*geometry);
                        const std::string key = std::format("{:08X}:{}:{:016X}",
                            actor->GetFormID(), ordinal - 1, sourceToken);
                        if (!attemptedMeshKeys.contains(key)) {
                            auto owned = CaptureCharacterBindMesh(*geometry,
                                actor->GetFormID(), ordinal - 1, frame);
                            if (owned) {
                                captured->capturedMeshes = 1;
                                captured->capturedBytes = owned->Bytes();
                                capturedMesh = std::move(*owned);
                                attemptedKey = key;
                                attemptedMeshKeys.insert(std::move(key));
                            }
                        }
                    }
                });
            };
            if (playerOnly)
                captureActor(RE::PlayerCharacter::GetSingleton());
            else
                Util::ForEachLoadedActor(captureActor);

            try {
                auto work = std::make_shared<Work>();
                work->sceneEpoch = sceneEpoch;
                if (capturedMesh) {
                    work->attemptedKey = attemptedKey;
                    work->actorFormID = capturedMesh->actorFormID;
                    work->partOrdinal = capturedMesh->partOrdinal;
                    work->sourceToken = capturedMesh->sourceToken;
                }
                auto previous = GetPublished();
                winrt::com_ptr<ID3D12Device> nativeDevice;
                winrt::com_ptr<ID3D12CommandQueue> nativeQueue;
                if (stageGeometry && device) nativeDevice.copy_from(device);
                if (runSkinning && queue) nativeQueue.copy_from(queue);
                static BS::thread_pool<> pool{ 1 };
                pool.detach_task([work, captured, previous,
                    mesh = std::move(capturedMesh),
                    pose = std::move(capturedPose), nativeDevice,
                    nativeQueue, runSkinning] {
                    try {
                        // All aggregation uses owned records, never engine pointers.
                        auto output = std::make_shared<CharacterObservationGeneration>(*captured);
                        std::stable_sort(output->parts.begin(), output->parts.end(),
                            [](const auto& a, const auto& b) {
                                return a.actorFormID != b.actorFormID ?
                                    a.actorFormID < b.actorFormID : a.ordinal < b.ordinal;
                            });
                        for (const auto& part : output->parts) {
                            if (part.skinned) ++output->skinnedParts;
                            else if (part.dynamic) ++output->dynamicParts;
                            else ++output->rigidParts;
                            if (part.alpha) ++output->alphaParts;
                            if (part.gpuOnly) ++output->gpuOnlyParts;
                            if (!std::isfinite(part.worldBound[3]) ||
                                part.worldBound[3] <= 0 || !part.vertexCount || !part.triangleCount)
                                ++output->invalidParts;
                        }
                        // DX12 upload and DXGI residency checks are performed
                        // on this native CPU worker. The renderer publishes
                        // the complete immutable generation only afterwards.
                        auto staged = PrepareCharacterMeshes(
                            nativeDevice.get(), mesh ? &*mesh : nullptr,
                            previous ? previous->stagedGeometry : nullptr, work->sceneEpoch);
                        output->stagedGeometry = StageCharacterPose(
                            nativeDevice.get(), pose ? &*pose : nullptr,
                            staged, work->sceneEpoch);
                        if (runSkinning && previous)
                            output->deformed = previous->deformed;
                        if (runSkinning && pose && output->stagedGeometry &&
                            output->stagedGeometry->pose && nativeQueue &&
                            output->stagedGeometry->pose->captureFrame == pose->capturedFrame) {
                            try {
                                output->deformed = DispatchCharacterSkinning(
                                    nativeDevice.get(), nativeQueue.get(),
                                    output->stagedGeometry);
                            } catch (const std::exception& e) {
                                output->deformationError = e.what();
                            } catch (...) {
                                output->deformationError = "Native DX12 skinning dispatch failed";
                            }
                        }
                        work->result = std::move(output);
                    } catch (...) {
                        // A failed optional inventory cannot affect raster or TLAS.
                    }
                    work->done.store(true, std::memory_order_release);
                });
                pending = std::move(work);
                workerBusy.store(true, std::memory_order_release);
                submitted.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                workerBusy.store(false, std::memory_order_release);
            }
        }

        std::shared_ptr<const CharacterObservationGeneration> GetPublished() const
        {
            return published.load(std::memory_order_acquire);
        }

        json GetDiagnostics() const
        {
            auto current = GetPublished();
            return {
                { "mode", current && current->playerOnly ?
                    "third-person-player-only" : "third-person-observe-only" },
                { "playerOnly", current ? current->playerOnly : false },
                { "firstPersonIncluded", false },
                { "nativeShadowOwnership", false },
                { "dx12GeometryStage", "owned-bind-pose-buffers" },
                { "capturedMeshes", current ? current->capturedMeshes : 0u },
                { "capturedBytes", current ? current->capturedBytes : 0ull },
                { "nativeStagedMeshes", current && current->stagedGeometry ?
                    current->stagedGeometry->meshes.size() : size_t{ 0 } },
                { "nativeStagedBytes", current && current->stagedGeometry ?
                    current->stagedGeometry->residentBytes : 0ull },
                { "poseFrame", current && current->stagedGeometry && current->stagedGeometry->pose ?
                    current->stagedGeometry->pose->captureFrame : 0u },
                { "poseSkinFrame", current && current->stagedGeometry && current->stagedGeometry->pose ?
                    current->stagedGeometry->pose->skinFrameID : 0u },
                { "poseRegisters", current && current->stagedGeometry && current->stagedGeometry->pose ?
                    current->stagedGeometry->pose->registerCount : 0u },
                { "poseSamplesStaged", current && current->stagedGeometry ?
                    current->stagedGeometry->posedFrames : 0u },
                { "poseDeferrals", current && current->stagedGeometry ?
                    current->stagedGeometry->poseDeferrals : 0u },
                { "deformSubmitted", current && bool(current->deformed) },
                { "deformGpuReady", current && current->deformed && current->deformed->Ready() },
                { "deformFrame", current && current->deformed ?
                    current->deformed->poseFrame : 0u },
                { "deformOutputVertices", current && current->deformed ?
                    current->deformed->vertices : 0u },
                { "deformGpuBytes", current && current->deformed ?
                    current->deformed->allocationBytes : 0ull },
                { "deformInFlight", CharacterDeformationRetirement::InFlight() },
                { "deformError", current ? current->deformationError : std::string{} },
                { "dxgiCharacterBudget", current && current->stagedGeometry ?
                    current->stagedGeometry->budgetBytes : 0ull },
                { "nativeUploadRejections", current && current->stagedGeometry ?
                    current->stagedGeometry->rejectedMeshes : 0u },
                { "nativeDxgiDeferrals", current && current->stagedGeometry ?
                    current->stagedGeometry->dxgiDeferrals : 0u },
                { "workerBusy", workerBusy.load(std::memory_order_acquire) },
                { "submitted", submitted.load(std::memory_order_relaxed) },
                { "completed", completed.load(std::memory_order_relaxed) },
                { "generationFrame", current ? current->frame : 0u },
                { "sceneEpoch", current ? current->sceneEpoch : 0ull },
                { "actors", current ? current->actors.size() : size_t{ 0 } },
                { "playerActors", current ? std::count_if(current->actors.begin(), current->actors.end(),
                    [](const auto& actor) { return actor.player; }) : std::ptrdiff_t{ 0 } },
                { "parts", current ? current->parts.size() : size_t{ 0 } },
                { "skinned", current ? current->skinnedParts : 0u },
                { "dynamic", current ? current->dynamicParts : 0u },
                { "rigid", current ? current->rigidParts : 0u },
                { "alpha", current ? current->alphaParts : 0u },
                { "gpuOnly", current ? current->gpuOnlyParts : 0u },
                { "invalid", current ? current->invalidParts : 0u },
                { "droppedActors", current ? current->droppedActors : 0u },
                { "droppedParts", current ? current->droppedParts : 0u }
            };
        }

    private:
        struct Work
        {
            std::atomic<bool> done{ false };
            uint64_t sceneEpoch = 0;
            std::string attemptedKey;
            uint32_t actorFormID = 0, partOrdinal = 0;
            uint64_t sourceToken = 0;
            std::shared_ptr<const CharacterObservationGeneration> result;
        };

        uint64_t sceneEpoch = 1;
        uint32_t lastObservedFrame = UINT32_MAX;
        std::shared_ptr<Work> pending;
        std::unordered_set<std::string> attemptedMeshKeys;
        std::atomic<std::shared_ptr<const CharacterObservationGeneration>> published;
        std::atomic<bool> workerBusy{ false };
        std::atomic<uint64_t> submitted{ 0 }, completed{ 0 };
    };
}
