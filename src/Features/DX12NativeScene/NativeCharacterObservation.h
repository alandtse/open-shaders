#pragma once

#include "Utils/ActorUtils.h"
#include "Features/DX12NativeScene/NativeCharacterMeshCapture.h"
#include "Features/DX12NativeScene/NativeCharacterGeometryPreparation.h"
#include "Features/DX12NativeScene/NativeCharacterDeformation.h"
#include "Features/DX12NativeScene/NativeCharacterBLASPreflight.h"
#include "Features/DX12NativeScene/NativeCharacterIsolatedBLAS.h"

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <limits>
#include <format>
#include <memory>
#include <mutex>
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
        uint64_t sourceToken = 0; // Runtime-only identity, never persisted.
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
        std::optional<CharacterDeformValidation> validation;
        std::optional<CharacterBLASPreflight> blasPreflight;
        std::shared_ptr<const CharacterIsolatedBLAS> isolatedBLAS;
        std::string deformationError, blasError;
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
            std::scoped_lock lock(stateMutex);
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
            bool stageGeometry, bool runSkinning, bool validateSkinning,
            bool buildIsolatedBLAS, ID3D12Device* device, ID3D12CommandQueue* queue)
        {
            // Runtime toggles can reset the observer while the engine is
            // supplying input. Serialize only this bounded CPU acquisition;
            // independent DX12 workers never acquire the state mutex.
            std::scoped_lock lock(stateMutex);
            if (pending && pending->done.load(std::memory_order_acquire)) {
                auto done = std::move(pending);
                workerBusy.store(false, std::memory_order_release);
                if (done->result && done->sceneEpoch == sceneEpoch) {
                    // Rebuild admission keys from actually resident meshes.
                    // Evicted, replaced and failed uploads must be eligible
                    // when their actor or mesh reappears in a later sample.
                    attemptedMeshKeys.clear();
                    if (const auto staged = done->result->stagedGeometry) {
                        for (const auto& part : staged->meshes) {
                            attemptedMeshKeys.emplace(std::format("{:08X}:{}:{:016X}",
                                part->actorFormID, part->partOrdinal, part->sourceToken));
                        }
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
                    part.sourceToken = CharacterMeshSourceToken(*geometry);
                    part.shapeName = geometry->name.empty() ?
                        std::string("unnamed") : std::string(geometry->name.c_str());
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
                    const bool partSkinned = part.skinned;
                    const uint64_t sourceToken = part.sourceToken;
                    captured->parts.emplace_back(std::move(part));
                    if (stageGeometry && device && partSkinned && !capturedPose) {
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
                    nativeQueue, runSkinning, validateSkinning,
                    buildIsolatedBLAS] {
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
                        // Retain only observed mesh identities from the
                        // current third-person snapshot. Old actor/equipment
                        // resources remain safe if an in-flight GPU frame still
                        // retains a previous immutable generation.
                        std::shared_ptr<const CharacterMeshGeneration> retained =
                            previous ? previous->stagedGeometry : nullptr;
                        if (retained && retained->sceneEpoch == work->sceneEpoch) {
                            auto trimmed = std::make_shared<CharacterMeshGeneration>(*retained);
                            std::erase_if(trimmed->meshes, [&](const auto& gpuMesh) {
                                const bool present = std::ranges::any_of(
                                    output->parts, [&](const auto& observed) {
                                        return observed.actorFormID == gpuMesh->actorFormID &&
                                            observed.ordinal == gpuMesh->partOrdinal &&
                                            observed.sourceToken == gpuMesh->sourceToken &&
                                            observed.shapeName == gpuMesh->shapeName;
                                    });
                                if (!present) {
                                    trimmed->residentBytes -= std::min(trimmed->residentBytes,
                                        gpuMesh->allocationBytes);
                                    ++trimmed->evictedMeshes;
                                }
                                return !present;
                            });
                            if (trimmed->pose) {
                                const bool hasMatchingSkin = std::ranges::any_of(
                                    trimmed->meshes, [&](const auto& gpuMesh) {
                                        return gpuMesh->skinned &&
                                            gpuMesh->actorFormID == trimmed->pose->actorFormID &&
                                            gpuMesh->partOrdinal == trimmed->pose->partOrdinal &&
                                            gpuMesh->sourceToken == trimmed->pose->sourceToken;
                                    });
                                if (!hasMatchingSkin) {
                                    trimmed->residentBytes -= std::min(trimmed->residentBytes,
                                        trimmed->pose->allocationBytes);
                                    trimmed->pose.reset();
                                }
                            }
                            retained = std::move(trimmed);
                        }
                        // DX12 upload and DXGI residency checks are performed
                        // on this native CPU worker. The renderer publishes
                        // the complete immutable generation only afterwards.
                        auto staged = PrepareCharacterMeshes(
                            nativeDevice.get(), mesh ? &*mesh : nullptr,
                            retained, work->sceneEpoch);
                        output->stagedGeometry = StageCharacterPose(
                            nativeDevice.get(), pose ? &*pose : nullptr,
                            staged, work->sceneEpoch);
                        if (runSkinning && previous && previous->deformed &&
                            output->stagedGeometry &&
                            std::ranges::any_of(output->stagedGeometry->meshes,
                                [&](const auto& gpuMesh) {
                                    return gpuMesh->skinned &&
                                        gpuMesh->actorFormID == previous->deformed->actorFormID &&
                                        gpuMesh->partOrdinal == previous->deformed->partOrdinal &&
                                        previous->deformed->input->pose &&
                                        gpuMesh->sourceToken ==
                                            previous->deformed->input->pose->sourceToken;
                                })) {
                            output->deformed = previous->deformed;
                            output->validation = previous->validation;
                            // Never wait for the GPU; consume completed debug
                            // readbacks only on this native CPU worker.
                            if (previous->deformed && previous->deformed->Ready()) {
                                if (!output->validation ||
                                    output->validation->poseFrame !=
                                        previous->deformed->poseFrame) {
                                    if (auto checked =
                                        InspectCharacterDeformation(previous->deformed))
                                        output->validation = std::move(checked);
                                }
                                output->blasPreflight = previous->blasPreflight;
                                if (!output->blasPreflight ||
                                    !output->blasPreflight->budgetAvailable ||
                                    output->blasPreflight->poseFrame !=
                                        previous->deformed->poseFrame) {
                                    if (auto estimated = PreflightCharacterDynamicBLAS(
                                        nativeDevice.get(), previous->deformed))
                                        output->blasPreflight = std::move(estimated);
                                }
                            }
                        }
                        // Standalone BLAS test: only mesh identities still
                        // resident in this generation may retain a completed
                        // acceleration result. Never publish it to a TLAS.
                        if (buildIsolatedBLAS && runSkinning && nativeQueue && previous) {
                            if (previous->isolatedBLAS && output->stagedGeometry &&
                                std::ranges::any_of(output->stagedGeometry->meshes,
                                    [&](const auto& gpuMesh) {
                                        return gpuMesh->skinned &&
                                            gpuMesh->actorFormID ==
                                                previous->isolatedBLAS->actorFormID &&
                                            gpuMesh->partOrdinal ==
                                                previous->isolatedBLAS->partOrdinal &&
                                            gpuMesh->sourceToken ==
                                                previous->isolatedBLAS->sourceToken;
                                    }))
                                output->isolatedBLAS = previous->isolatedBLAS;
                            const bool validPose =
                                output->deformed && output->deformed->Ready() &&
                                output->validation && output->validation->sane &&
                                output->validation->poseFrame ==
                                    output->deformed->poseFrame &&
                                output->blasPreflight &&
                                output->blasPreflight->poseFrame ==
                                    output->deformed->poseFrame &&
                                output->blasPreflight->budgetAvailable;
                            if (validPose && (!output->isolatedBLAS ||
                                (output->isolatedBLAS->Ready() &&
                                    output->isolatedBLAS->poseFrame !=
                                        output->deformed->poseFrame))) {
                                try {
                                    if (auto candidate = BuildCharacterIsolatedBLAS(
                                        nativeDevice.get(), nativeQueue.get(),
                                        output->deformed, *output->blasPreflight,
                                        output->isolatedBLAS))
                                        output->isolatedBLAS = std::move(candidate);
                                } catch (const std::exception& e) {
                                    output->blasError = e.what();
                                } catch (...) {
                                    output->blasError = "Native isolated BLAS build failed";
                                }
                            }
                        }
                        if (runSkinning && pose && output->stagedGeometry &&
                            output->stagedGeometry->pose && nativeQueue &&
                            output->stagedGeometry->pose->captureFrame == pose->capturedFrame) {
                            try {
                                output->deformed = DispatchCharacterSkinning(
                                    nativeDevice.get(), nativeQueue.get(),
                                    output->stagedGeometry, validateSkinning);
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
                { "nativeEvictedMeshes", current && current->stagedGeometry ?
                    current->stagedGeometry->evictedMeshes : 0u },
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
                { "posePaletteRejections", current && current->stagedGeometry ?
                    current->stagedGeometry->posePaletteRejections : 0u },
                { "poseRequiredBones", current && current->stagedGeometry ?
                    current->stagedGeometry->lastPoseRequiredBones : 0u },
                { "poseAvailableBones", current && current->stagedGeometry ?
                    current->stagedGeometry->lastPoseAvailableBones : 0u },
                { "deformSubmitted", current && bool(current->deformed) },
                { "deformGpuReady", current && current->deformed && current->deformed->Ready() },
                { "deformFrame", current && current->deformed ?
                    current->deformed->poseFrame : 0u },
                { "deformOutputVertices", current && current->deformed ?
                    current->deformed->vertices : 0u },
                { "deformGpuBytes", current && current->deformed ?
                    current->deformed->allocationBytes : 0ull },
                { "deformInFlight", CharacterDeformationRetirement::InFlight() },
                { "validationReady", current && current->validation.has_value() },
                { "validationFrame", current && current->validation ?
                    current->validation->poseFrame : 0u },
                { "validationInspected", current && current->validation ?
                    current->validation->inspected : 0u },
                { "validationNonFinite", current && current->validation ?
                    current->validation->nonFinite : 0u },
                { "validationSane", current && current->validation &&
                    current->validation->sane },
                { "validationMaximumBindDisplacement", current && current->validation ?
                    current->validation->maximumBindDisplacement : 0.0f },
                { "validationMinimum", current && current->validation ?
                    current->validation->minimum : std::array<float,3>{} },
                { "validationMaximum", current && current->validation ?
                    current->validation->maximum : std::array<float,3>{} },
                { "blasPreflightReady", current && current->blasPreflight.has_value() },
                { "blasPreflightPoseFrame", current && current->blasPreflight ?
                    current->blasPreflight->poseFrame : 0u },
                { "blasPreflightDXR", current && current->blasPreflight &&
                    current->blasPreflight->dxrSupported },
                { "blasPreflightBudget", current && current->blasPreflight &&
                    current->blasPreflight->budgetAvailable },
                { "blasPreflightResultBytes", current && current->blasPreflight ?
                    current->blasPreflight->resultBytes : 0ull },
                { "blasPreflightScratchBytes", current && current->blasPreflight ?
                    current->blasPreflight->scratchBytes : 0ull },
                { "blasPreflightUpdateScratchBytes", current && current->blasPreflight ?
                    current->blasPreflight->updateScratchBytes : 0ull },
                { "blasPreflightAvailableBytes", current && current->blasPreflight ?
                    current->blasPreflight->availableBytes : 0ull },
                { "blasPreflightStatus", current && current->blasPreflight ?
                    current->blasPreflight->status : std::string("No completed character pose") },
                { "isolatedBLASSubmitted", current && bool(current->isolatedBLAS) },
                { "isolatedBLASReady", current && current->isolatedBLAS &&
                    current->isolatedBLAS->Ready() },
                { "isolatedBLASFrame", current && current->isolatedBLAS ?
                    current->isolatedBLAS->poseFrame : 0u },
                { "isolatedBLASRefit", current && current->isolatedBLAS &&
                    current->isolatedBLAS->refit },
                { "isolatedBLASBytes", current && current->isolatedBLAS ?
                    current->isolatedBLAS->allocationBytes : 0ull },
                { "isolatedBLASScratchBytes", current && current->isolatedBLAS ?
                    current->isolatedBLAS->scratchBytes : 0ull },
                { "isolatedBLASPending", CharacterBLASRetirement::Pending() },
                { "isolatedBLASError", current ? current->blasError : std::string{} },
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

        std::mutex stateMutex;
        uint64_t sceneEpoch = 1;
        uint32_t lastObservedFrame = UINT32_MAX;
        std::shared_ptr<Work> pending;
        std::unordered_set<std::string> attemptedMeshKeys;
        std::atomic<std::shared_ptr<const CharacterObservationGeneration>> published;
        std::atomic<bool> workerBusy{ false };
        std::atomic<uint64_t> submitted{ 0 }, completed{ 0 };
    };
}
