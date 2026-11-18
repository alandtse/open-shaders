#pragma once

#include "Features/DX12NativeScene/NativeCharacterBLASPreflight.h"
#include "Features/DX12NativeScene/NativeCharacterDeformation.h"
#include "Features/DX12NativeScene/NativeCharacterGeometryPreparation.h"
#include "Features/DX12NativeScene/NativeCharacterIsolatedBLAS.h"
#include "Features/DX12NativeScene/NativeCharacterIsolatedTLAS.h"
#include "Features/DX12NativeScene/NativeCharacterMeshCapture.h"
#include "Features/DX12NativeScene/NativeCharacterRayProbe.h"
#include "Utils/ActorUtils.h"

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
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
		uint64_t sourceToken = 0;  // Runtime-only identity, never persisted.
		std::string shapeName;
		std::array<float, 4> worldBound{};
		std::array<float, 12> worldTransform{};
		uint32_t vertexCount = 0, triangleCount = 0;
		bool skinned = false, dynamic = false, alpha = false, gpuOnly = false;
	};

	struct CharacterActorObservation
	{
		uint32_t formID = 0;
		bool player = false, ragdoll = false;
		std::array<float, 4> worldBound{};
	};

	// Check current rigid-local bounds against the observed world-space part.
	// A failed match must not admit an attachment to the character TLAS.
	inline bool CharacterRigidBoundsMatch(
		const CharacterStagedMesh& staged, const CharacterPartObservation& observed)
	{
		if (staged.skinned || !staged.rigidLocalBoundsValid ||
			observed.skinned || observed.alpha || observed.dynamic || observed.gpuOnly)
			return false;
		const auto& bounds = observed.worldBound;
		const auto& transform = observed.worldTransform;
		const float radius = bounds[3];
		if (!std::isfinite(radius) || radius <= 0.0f || radius > 50000.0f)
			return false;
		for (size_t axis = 0; axis < 3; ++axis) {
			if (!std::isfinite(bounds[axis]) ||
				!std::isfinite(staged.rigidLocalMin[axis]) ||
				!std::isfinite(staged.rigidLocalMax[axis]) ||
				staged.rigidLocalMin[axis] > staged.rigidLocalMax[axis])
				return false;
		}
		for (const float element : transform)
			if (!std::isfinite(element))
				return false;

		const float slack = std::max(2.0f, radius * 0.25f);
		for (size_t axis = 0; axis < 3; ++axis) {
			float worldMin = transform[axis * 4 + 3];
			float worldMax = worldMin;
			for (size_t component = 0; component < 3; ++component) {
				const float a = transform[axis * 4 + component] *
				                staged.rigidLocalMin[component];
				const float b = transform[axis * 4 + component] *
				                staged.rigidLocalMax[component];
				worldMin += std::min(a, b);
				worldMax += std::max(a, b);
			}
			if (!std::isfinite(worldMin) || !std::isfinite(worldMax) ||
				worldMin < bounds[axis] - radius - slack ||
				worldMax > bounds[axis] + radius + slack)
				return false;
		}
		return true;
	}

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
		// Completed/updateable BLAS generations keyed by observed mesh identity.
		// Multi-part acceleration is independently gated before any
		// opt-in native secondary-visibility ownership.
		std::vector<std::shared_ptr<const CharacterIsolatedBLAS>> blasParts;
		std::vector<std::shared_ptr<const CharacterIsolatedBLAS>> rigidBLASParts;
		std::shared_ptr<const CharacterIsolatedTLAS> isolatedTLAS;
		std::shared_ptr<const CharacterRayResult> rayProbe;
		std::optional<CharacterRayOutcome> rayOutcome;
		std::vector<CharacterRayOutcome> rayOutcomes;
		std::string deformationError, blasError, tlasError, rayError;
		uint32_t capturedMeshes = 0;
		uint64_t capturedBytes = 0;
		uint32_t droppedActors = 0, droppedParts = 0, invalidParts = 0;
		uint32_t skinnedParts = 0, dynamicParts = 0, rigidParts = 0;
		uint32_t alphaParts = 0, gpuOnlyParts = 0;
	};

	struct CharacterCasterReadiness
	{
		std::shared_ptr<const CharacterIsolatedTLAS> tlas;
		const char* reason = "Character observation not available";
		bool worldBoundsPlausible = false;
		bool meshBoundsPlausible = false;
	};

	/** @brief Require matching GPU self-hits and spatially plausible,
     * immutable skinned-part generations before any production occlusion.
     * This is a safety gate, not proof of exact Skyrim skinning parity.
     */
	inline CharacterCasterReadiness EvaluateCharacterCaster(
		const std::shared_ptr<const CharacterObservationGeneration>& generation,
		uint32_t renderFrame, uint32_t maxPoseAgeFrames = 12u,
		bool allowMultiPart = false)
	{
		CharacterCasterReadiness readiness;
		if (!generation)
			return readiness;
		const auto& scene = generation->isolatedTLAS;
		if (!scene || !scene->Ready() || !scene->acceleration) {
			readiness.reason = "Isolated character TLAS not GPU-ready";
			return readiness;
		}
		const auto& parts = scene->bottoms;
		if (parts.empty() || parts.size() > 8u || (parts.size() > 1u && !allowMultiPart)) {
			readiness.reason = "Multi-part caster requires separate production opt-in";
			return readiness;
		}
		if (!generation->rayProbe || !generation->rayProbe->Ready() ||
			generation->rayProbe->scene != scene ||
			generation->rayProbe->partCount != parts.size() ||
			generation->rayOutcomes.size() != parts.size()) {
			readiness.reason = "No completed matching per-part RayQuery diagnostics";
			return readiness;
		}
		maxPoseAgeFrames = std::clamp(maxPoseAgeFrames, 1u, 30u);
		uint32_t actorIdentity = 0;
		for (size_t i = 0; i < parts.size(); ++i) {
			const auto& part = parts[i];
			if (!part || !part->Ready() || !part->acceleration ||
				!part->deformation || !part->deformation->Ready() ||
				!part->validation || !part->validation->sane ||
				part->validation->poseFrame != part->poseFrame) {
				readiness.reason = "Character BLAS or pose validation incomplete";
				return readiness;
			}
			const auto& outcome = generation->rayOutcomes[i];
			if (outcome.status != 3u || outcome.instanceID != i) {
				readiness.reason = "A character part did not pass its own RayQuery self-hit";
				return readiness;
			}
			if (renderFrame < part->poseFrame ||
				renderFrame - part->poseFrame > maxPoseAgeFrames) {
				readiness.reason = "At least one character-part pose is stale";
				return readiness;
			}
			if (actorIdentity && actorIdentity != part->actorFormID) {
				readiness.reason = "Caster TLAS contains mixed actors";
				return readiness;
			}
			actorIdentity = part->actorFormID;
			const auto actor = std::ranges::find_if(generation->actors,
				[&](const auto& candidate) {
					return candidate.formID == part->actorFormID;
				});
			if (actor == generation->actors.end()) {
				readiness.reason = "Character actor left third-person observation";
				return readiness;
			}
			const auto observed = std::ranges::find_if(generation->parts,
				[&](const auto& mesh) {
					return mesh.actorFormID == part->actorFormID &&
					       mesh.ordinal == part->partOrdinal &&
					       mesh.sourceToken == part->sourceToken;
				});
			if (observed == generation->parts.end() ||
				!observed->skinned || observed->alpha || observed->dynamic ||
				observed->gpuOnly) {
				readiness.reason = "Skinned part material or identity is unsupported";
				return readiness;
			}
			// A finite result in the actor's root sphere alone can still be
			// local-space data. Also require this exact mesh-part sphere.
			const auto& minimum = part->validation->minimum;
			const auto& maximum = part->validation->maximum;
			for (const auto* bounds : { &actor->worldBound, &observed->worldBound }) {
				const float r = (*bounds)[3];
				if (!std::isfinite(r) || r <= 0.0f || r > 50000.0f) {
					readiness.reason = "Actor or mesh world bound is invalid";
					return readiness;
				}
				const float slack = bounds == &actor->worldBound ?
				                        r * 0.5f + 4.0f :
				                        r * 0.25f + 2.0f;
				for (size_t axis = 0; axis < 3; ++axis) {
					if (!std::isfinite((*bounds)[axis]) ||
						!std::isfinite(minimum[axis]) ||
						!std::isfinite(maximum[axis]) ||
						minimum[axis] > maximum[axis] ||
						minimum[axis] < (*bounds)[axis] - r - slack ||
						maximum[axis] > (*bounds)[axis] + r + slack) {
						readiness.reason = "Deformed part outside observed world bounds";
						return readiness;
					}
				}
			}
		}
		readiness.worldBoundsPlausible = true;
		readiness.meshBoundsPlausible = true;
		readiness.tlas = scene;
		readiness.reason = parts.size() == 1 ?
		                       "Single-part caster eligible; pose parity still experimental" :
		                       "Multi-part caster eligible; pose/material parity still experimental";
		return readiness;
	}

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
			pending.reset();  // Detached worker retains only owned values.
			published.store({}, std::memory_order_release);
			attemptedMeshKeys.clear();
			nextPoseSearchIndex = 0;
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
			bool buildIsolatedBLAS, bool buildIsolatedTLAS,
			bool runIsolatedRayProbe, uint32_t blasIntervalFrames,
			uint32_t meshBatchLimit, bool accumulatePartBLAS,
			bool includeRigidAttachments,
			ID3D12Device* device, ID3D12CommandQueue* queue)
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
			if (pending || !frame)
				return;
			periodFrames = std::clamp(periodFrames, 1u, 120u);
			blasIntervalFrames = std::clamp(blasIntervalFrames, 1u, 120u);
			meshBatchLimit = std::clamp(meshBatchLimit, 1u, 4u);
			maxActors = std::clamp(maxActors, 1u, 32u);
			if (lastObservedFrame != UINT32_MAX &&
				uint32_t(frame - lastObservedFrame) < periodFrames)
				return;
			lastObservedFrame = frame;

			auto captured = std::make_shared<CharacterObservationGeneration>();
			captured->sceneEpoch = sceneEpoch;
			captured->frame = frame;
			captured->playerOnly = playerOnly;
			constexpr size_t kMaximumParts = 512;
			constexpr uint32_t kMaximumPartsPerActor = 96;
			std::vector<CharacterMeshCapture> capturedMeshes;
			capturedMeshes.reserve(meshBatchLimit);
			std::optional<CharacterPoseCapture> capturedPose;
			const size_t poseStart = accumulatePartBLAS ? nextPoseSearchIndex : 0u;
			size_t capturedPoseIndex = SIZE_MAX;

			// Only Get3D(false): never inspect Skyrim's first-person root.
			// In player-only mode, skip the high-process actor enumeration
			// entirely so other actors add zero scenegraph traversal cost.
			const auto captureActor = [&](RE::Actor* actor) {
				if (!actor || !actor->Is3DLoaded())
					return;
				if (captured->actors.size() >= maxActors) {
					++captured->droppedActors;
					return;
				}
				auto* root = actor->Get3D(false);
				if (!root)
					return;
				CharacterActorObservation info;
				info.formID = actor->GetFormID();
				info.player = actor == RE::PlayerCharacter::GetSingleton();
				info.ragdoll = actor->IsDead() || actor->IsInRagdollState();
				const auto& bound = root->worldBound;
				info.worldBound = { bound.center.x, bound.center.y, bound.center.z, bound.radius };
				captured->actors.push_back(std::move(info));
				uint32_t ordinal = 0;

				Util::ForEachGeometry(root, [&](RE::BSGeometry* geometry) {
					if (!geometry)
						return;
					if (ordinal >= kMaximumPartsPerActor || captured->parts.size() >= kMaximumParts) {
						++captured->droppedParts;
						return;
					}
					CharacterPartObservation part;
					part.actorFormID = actor->GetFormID();
					part.ordinal = ordinal++;
					part.sourceToken = CharacterMeshSourceToken(*geometry);
					part.shapeName = geometry->name.empty() ?
					                     std::string("unnamed") :
					                     std::string(geometry->name.c_str());
					const auto& bounds = geometry->worldBound;
					part.worldBound = { bounds.center.x, bounds.center.y, bounds.center.z, bounds.radius };
					for (size_t row = 0; row < 3; ++row)
						for (size_t col = 0; col < 3; ++col)
							part.worldTransform[4 * row + col] =
								geometry->world.rotate.entry[row][col] *
								geometry->world.scale;
					part.worldTransform[3] = geometry->world.translate.x;
					part.worldTransform[7] = geometry->world.translate.y;
					part.worldTransform[11] = geometry->world.translate.z;
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
					if (stageGeometry && device && partSkinned && !capturedPose &&
						captured->parts.size() - 1 >= poseStart) {
						bool hasBind = std::ranges::any_of(capturedMeshes,
							[&](const auto& candidate) {
								return candidate.skinned &&
								       candidate.actorFormID == actor->GetFormID() &&
								       candidate.partOrdinal == ordinal - 1 &&
								       candidate.sourceToken == sourceToken;
							});
						if (const auto previous = GetPublished();
							previous && previous->stagedGeometry) {
							for (const auto& staged : previous->stagedGeometry->meshes)
								hasBind |= staged->skinned &&
								           staged->actorFormID == actor->GetFormID() &&
								           staged->partOrdinal == ordinal - 1 &&
								           staged->sourceToken == sourceToken;
						}
						if (hasBind) {
							capturedPose = CaptureCharacterShaderPose(
								*geometry, actor->GetFormID(), ordinal - 1, sourceToken, frame);
							if (capturedPose)
								capturedPoseIndex = captured->parts.size() - 1;
						}
					}
					if (stageGeometry && device &&
						capturedMeshes.size() < meshBatchLimit) {
						const std::string key = std::format("{:08X}:{}:{:016X}",
							actor->GetFormID(), ordinal - 1, sourceToken);
						if (!attemptedMeshKeys.contains(key)) {
							auto owned = CaptureCharacterBindMesh(*geometry,
								actor->GetFormID(), ordinal - 1, frame);
							if (owned) {
								++captured->capturedMeshes;
								captured->capturedBytes += owned->Bytes();
								capturedMeshes.emplace_back(std::move(*owned));
								attemptedMeshKeys.insert(key);
							}
						}
					}
				});
			};
			if (playerOnly)
				captureActor(RE::PlayerCharacter::GetSingleton());
			else
				Util::ForEachLoadedActor(captureActor);
			if (accumulatePartBLAS)
				nextPoseSearchIndex = capturedPoseIndex != SIZE_MAX ?
				                          capturedPoseIndex + 1u :
				                          0u;

			try {
				auto work = std::make_shared<Work>();
				work->sceneEpoch = sceneEpoch;
				auto previous = GetPublished();
				winrt::com_ptr<ID3D12Device> nativeDevice;
				winrt::com_ptr<ID3D12CommandQueue> nativeQueue;
				if (stageGeometry && device)
					nativeDevice.copy_from(device);
				if (runSkinning && queue)
					nativeQueue.copy_from(queue);
				static BS::thread_pool<> pool{ 1 };
				pool.detach_task([work, captured, previous,
									 meshes = std::move(capturedMeshes),
									 pose = std::move(capturedPose), nativeDevice,
									 nativeQueue, runSkinning, validateSkinning,
									 buildIsolatedBLAS, buildIsolatedTLAS,
									 runIsolatedRayProbe, blasIntervalFrames, accumulatePartBLAS,
									 includeRigidAttachments] {
					try {
						// All aggregation uses owned records, never engine pointers.
						auto output = std::make_shared<CharacterObservationGeneration>(*captured);
						std::stable_sort(output->parts.begin(), output->parts.end(),
							[](const auto& a, const auto& b) {
								return a.actorFormID != b.actorFormID ?
								           a.actorFormID < b.actorFormID :
								           a.ordinal < b.ordinal;
							});
						for (const auto& part : output->parts) {
							if (part.skinned)
								++output->skinnedParts;
							else if (part.dynamic)
								++output->dynamicParts;
							else
								++output->rigidParts;
							if (part.alpha)
								++output->alphaParts;
							if (part.gpuOnly)
								++output->gpuOnlyParts;
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
						std::shared_ptr<const CharacterMeshGeneration> staged =
							PrepareCharacterMeshes(nativeDevice.get(), nullptr,
								retained, work->sceneEpoch);
						for (const auto& mesh : meshes)
							staged = PrepareCharacterMeshes(nativeDevice.get(),
								&mesh, std::move(staged), work->sceneEpoch);
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
									       previous->deformed->input &&
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
						// Keep per-part acceleration generations across
						// observations, but prune absent/equipment-swapped parts.
						if (accumulatePartBLAS && previous && output->stagedGeometry) {
							output->blasParts = previous->blasParts;
							std::erase_if(output->blasParts, [&](const auto& item) {
								return !item || !std::ranges::any_of(
													output->stagedGeometry->meshes,
													[&](const auto& mesh) {
														return mesh->skinned &&
														       mesh->actorFormID == item->actorFormID &&
														       mesh->partOrdinal == item->partOrdinal &&
														       mesh->sourceToken == item->sourceToken;
													});
							});
						}
						// Standalone BLAS test: only mesh identities still
						// resident in this generation may retain a completed
						// acceleration result. Never publish it to the world TLAS.
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
								output->deformed->input && output->deformed->input->pose &&
								output->validation && output->validation->sane &&
								output->validation->poseFrame ==
									output->deformed->poseFrame &&
								output->blasPreflight &&
								output->blasPreflight->poseFrame ==
									output->deformed->poseFrame &&
								output->blasPreflight->budgetAvailable;
							auto previousPart = output->isolatedBLAS;
							if (accumulatePartBLAS) {
								previousPart.reset();
								if (validPose) {
									const auto found = std::ranges::find_if(output->blasParts,
										[&](const auto& item) {
											return item &&
											       item->actorFormID == output->deformed->actorFormID &&
											       item->partOrdinal == output->deformed->partOrdinal &&
											       item->sourceToken ==
											           output->deformed->input->pose->sourceToken;
										});
									if (found != output->blasParts.end())
										previousPart = *found;
								}
							}
							if (validPose && (!previousPart ||
												 (previousPart->Ready() &&
													 previousPart->poseFrame != output->deformed->poseFrame &&
													 uint32_t(output->deformed->poseFrame -
															  previousPart->poseFrame) >=
														 blasIntervalFrames))) {
								try {
									if (auto candidate = BuildCharacterIsolatedBLAS(
											nativeDevice.get(), nativeQueue.get(),
											output->deformed, *output->blasPreflight,
											*output->validation, previousPart)) {
										output->isolatedBLAS = candidate;
										if (accumulatePartBLAS) {
											const auto at = std::ranges::find_if(
												output->blasParts, [&](const auto& item) {
													return item &&
													       item->actorFormID == candidate->actorFormID &&
													       item->partOrdinal == candidate->partOrdinal &&
													       item->sourceToken == candidate->sourceToken;
												});
											if (at == output->blasParts.end())
												output->blasParts.push_back(candidate);
											else
												*at = candidate;
										}
									}
								} catch (const std::exception& e) {
									output->blasError = e.what();
								} catch (...) {
									output->blasError = "Native isolated BLAS build failed";
								}
							}
						}
						if (accumulatePartBLAS && includeRigidAttachments &&
							buildIsolatedBLAS && nativeDevice && nativeQueue &&
							output->stagedGeometry) {
							if (previous)
								output->rigidBLASParts = previous->rigidBLASParts;
							std::erase_if(output->rigidBLASParts, [&](const auto& candidate) {
								return !candidate || !candidate->rigidSource ||
								       !std::ranges::any_of(
										   output->stagedGeometry->meshes,
										   [&](const auto& residentMesh) {
											   return !residentMesh->skinned &&
											          residentMesh->actorFormID == candidate->actorFormID &&
											          residentMesh->partOrdinal == candidate->partOrdinal &&
											          residentMesh->sourceToken == candidate->sourceToken;
										   });
							});
							// One additional rigid BLAS at most per worker
							// generation. Once staged, a rigid mesh reuses its
							// BLAS; only its TLAS transform changes.
							for (const auto& rigidMesh : output->stagedGeometry->meshes) {
								if (!rigidMesh || rigidMesh->skinned ||
									!rigidMesh->rigidLocalBoundsValid)
									continue;
								const auto observed = std::ranges::find_if(
									output->parts, [&](const auto& part) {
										return !part.skinned && !part.alpha &&
										       !part.dynamic && !part.gpuOnly &&
										       part.actorFormID == rigidMesh->actorFormID &&
										       part.ordinal == rigidMesh->partOrdinal &&
										       part.sourceToken == rigidMesh->sourceToken;
									});
								if (observed == output->parts.end() ||
									!CharacterRigidBoundsMatch(*rigidMesh, *observed))
									continue;
								if (std::ranges::any_of(output->rigidBLASParts,
										[&](const auto& candidate) {
											return candidate->actorFormID == rigidMesh->actorFormID &&
											       candidate->partOrdinal == rigidMesh->partOrdinal &&
											       candidate->sourceToken == rigidMesh->sourceToken;
										}))
									continue;
								try {
									if (auto candidate = BuildCharacterRigidBLAS(
											nativeDevice.get(), nativeQueue.get(), rigidMesh))
										output->rigidBLASParts.push_back(std::move(candidate));
								} catch (const std::exception& e) {
									output->blasError = e.what();
								} catch (...) {
									output->blasError = "Rigid character BLAS build failed";
								}
								break;
							}
						}
						// The optional one-instance TLAS is intentionally
						// separate from the world scene and every native
						// lighting/shadow consumer. It references ONLY a
						// completed isolated character BLAS.
						if (buildIsolatedTLAS && buildIsolatedBLAS &&
							nativeDevice && nativeQueue) {
							if (accumulatePartBLAS) {
								// Production visibility requires all per-part
								// GPU proofs, valid pose age and world bounds.
								// Each skinned part needs its own finite pose,
								// current identity and plausible world bounds.
								std::vector<std::shared_ptr<const CharacterIsolatedBLAS>> parts;
								std::vector<std::array<float, 12>> transforms;
								const std::array<float, 12> identity{
									1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
									0.f, 0.f, 1.f, 0.f
								};
								uint32_t actorIdentity = 0;
								for (const auto& part : output->blasParts) {
									if (!part || !part->Ready() || !part->validation ||
										!part->validation->sane ||
										part->validation->poseFrame != part->poseFrame ||
										output->frame < part->poseFrame ||
										output->frame - part->poseFrame > 30u)
										continue;
									const auto observed = std::ranges::find_if(
										output->parts, [&](const auto& mesh) {
											return mesh.actorFormID == part->actorFormID &&
											       mesh.ordinal == part->partOrdinal &&
											       mesh.sourceToken == part->sourceToken &&
											       mesh.skinned && !mesh.alpha && !mesh.dynamic;
										});
									if (observed == output->parts.end())
										continue;
									if (actorIdentity && actorIdentity != part->actorFormID)
										continue;
									const auto& bounds = observed->worldBound;
									if (!std::isfinite(bounds[3]) || bounds[3] <= 0.0f)
										continue;
									const float tolerance = std::max(2.0f, bounds[3] * 0.25f);
									bool plausible = true;
									for (size_t axis = 0; axis < 3; ++axis)
										plausible &= std::isfinite(bounds[axis]) &&
										             std::isfinite(part->validation->minimum[axis]) &&
										             std::isfinite(part->validation->maximum[axis]) &&
										             part->validation->minimum[axis] >=
										                 bounds[axis] - bounds[3] - tolerance &&
										             part->validation->maximum[axis] <=
										                 bounds[axis] + bounds[3] + tolerance;
									if (!plausible)
										continue;
									actorIdentity = part->actorFormID;
									parts.push_back(part);
									transforms.push_back(identity);
									if (parts.size() >= 8)
										break;
								}
								if (includeRigidAttachments && parts.size() < 8u) {
									for (const auto& rigid : output->rigidBLASParts) {
										if (!rigid || !rigid->Ready() || !rigid->rigidSource)
											continue;
										const auto observed = std::ranges::find_if(
											output->parts, [&](const auto& mesh) {
												return mesh.actorFormID == rigid->actorFormID &&
												       mesh.ordinal == rigid->partOrdinal &&
												       mesh.sourceToken == rigid->sourceToken &&
												       !mesh.skinned && !mesh.alpha &&
												       !mesh.dynamic && !mesh.gpuOnly;
											});
										if (observed == output->parts.end() ||
											(actorIdentity &&
												actorIdentity != rigid->actorFormID) ||
											!CharacterRigidBoundsMatch(
												*rigid->rigidSource, *observed))
											continue;
										actorIdentity = rigid->actorFormID;
										parts.push_back(rigid);
										transforms.push_back(observed->worldTransform);
										if (parts.size() >= 8u)
											break;
									}
								}
								if (!parts.empty()) {
									const auto old = previous ?
									                     previous->isolatedTLAS :
									                     nullptr;
									const bool sameMembership = old &&
									                            old->bottoms == parts &&
									                            old->instanceTransforms == transforms;
									const bool oldStillPresent = sameMembership;
									const bool awaitingQuery = oldStillPresent &&
									                           runIsolatedRayProbe &&
									                           (!previous->rayProbe ||
																   previous->rayProbe->scene != old ||
																   !previous->rayProbe->Ready() ||
																   previous->rayOutcomes.size() !=
																	   previous->rayProbe->partCount);
									if (sameMembership || awaitingQuery)
										output->isolatedTLAS = old;
									else {
										try {
											output->isolatedTLAS = BuildCharacterMultiPartTLAS(
												nativeDevice.get(), nativeQueue.get(),
												std::move(parts),
												std::move(transforms), output->frame);
										} catch (const std::exception& e) {
											output->tlasError = e.what();
										} catch (...) {
											output->tlasError =
												"Isolated multi-part character TLAS failed";
										}
									}
								}
							} else if (output->isolatedBLAS &&
									   output->isolatedBLAS->Ready()) {
								if (previous && previous->isolatedTLAS &&
									output->stagedGeometry &&
									std::ranges::any_of(output->stagedGeometry->meshes,
										[&](const auto& mesh) {
											const auto& old = previous->isolatedTLAS;
											return mesh->skinned &&
											       mesh->actorFormID == old->actorFormID &&
											       mesh->partOrdinal == old->partOrdinal &&
											       mesh->sourceToken == old->bottom->sourceToken;
										})) {
									const bool awaitingQuery = runIsolatedRayProbe &&
									                           (!previous->rayProbe ||
																   previous->rayProbe->scene != previous->isolatedTLAS ||
																   !previous->rayProbe->Ready() ||
																   previous->rayOutcomes.size() !=
																	   previous->rayProbe->partCount);
									// Never replace a TLAS mid-probe. Its old
									// BLAS, pose validation and ray outcome must
									// remain one immutable evidence generation.
									if (previous->isolatedTLAS->bottom ==
											output->isolatedBLAS ||
										awaitingQuery)
										output->isolatedTLAS = previous->isolatedTLAS;
								}
								if (!output->isolatedTLAS) {
									try {
										output->isolatedTLAS = BuildCharacterIsolatedTLAS(
											nativeDevice.get(), nativeQueue.get(),
											output->isolatedBLAS);
									} catch (const std::exception& e) {
										output->tlasError = e.what();
									} catch (...) {
										output->tlasError = "Isolated character TLAS build failed";
									}
								}
							}
						}
						// Only the isolated character TLAS is bound. The
						// query reports one triangle-intersection diagnostic
						// through a fenced readback, never a production mask.
						if (runIsolatedRayProbe && buildIsolatedTLAS &&
							output->isolatedTLAS && nativeDevice && nativeQueue) {
							if (previous && previous->rayProbe &&
								previous->rayProbe->scene == output->isolatedTLAS) {
								output->rayProbe = previous->rayProbe;
								output->rayOutcome = previous->rayOutcome;
								output->rayOutcomes = previous->rayOutcomes;
							}
							if (output->rayProbe && output->rayProbe->Ready() &&
								output->rayOutcomes.size() != output->rayProbe->partCount) {
								if (auto all = ReadCharacterRayOutcomes(output->rayProbe)) {
									output->rayOutcomes = std::move(*all);
									if (!output->rayOutcomes.empty())
										output->rayOutcome = output->rayOutcomes.front();
								}
							}
							if (!output->rayProbe && output->isolatedTLAS->Ready()) {
								try {
									output->rayProbe = DispatchCharacterRayProbe(
										nativeDevice.get(), nativeQueue.get(),
										output->isolatedTLAS);
								} catch (const std::exception& e) {
									output->rayError = e.what();
								} catch (...) {
									output->rayError = "Isolated character RayQuery failed";
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

		CharacterCasterReadiness GetCasterReadiness(
			uint32_t renderFrame, uint32_t maxPoseAgeFrames = 12u,
			bool allowMultiPart = false) const
		{
			return EvaluateCharacterCaster(GetPublished(), renderFrame,
				maxPoseAgeFrames, allowMultiPart);
		}

		json GetDiagnostics() const
		{
			auto current = GetPublished();
			const auto eligibility = EvaluateCharacterCaster(current,
				current ? current->frame : 0u);
			return {
				{ "casterBoundsPlausible", eligibility.worldBoundsPlausible },
				{ "casterMeshBoundsPlausible", eligibility.meshBoundsPlausible },
				{ "casterEligibleAtObservation", bool(eligibility.tlas) },
				{ "casterEligibilityReason", eligibility.reason },
				{ "mode", current && current->playerOnly ?
							  "third-person-player-only" :
							  "third-person-observe-only" },
				{ "playerOnly", current ? current->playerOnly : false },
				{ "firstPersonIncluded", false },
				{ "nativeShadowOwnership", false },
				{ "dx12GeometryStage", "owned-bind-pose-buffers" },
				{ "capturedMeshes", current ? current->capturedMeshes : 0u },
				{ "capturedBytes", current ? current->capturedBytes : 0ull },
				{ "nativeStagedMeshes", current && current->stagedGeometry ?
											current->stagedGeometry->meshes.size() :
											size_t{ 0 } },
				{ "nativeRigidStagedMeshes", current && current->stagedGeometry ?
												 std::count_if(current->stagedGeometry->meshes.begin(),
													 current->stagedGeometry->meshes.end(),
													 [](const auto& mesh) { return !mesh->skinned; }) :
												 std::ptrdiff_t{ 0 } },
				{ "nativeReceiverAttributeReady", current && current->stagedGeometry ?
													  std::count_if(current->stagedGeometry->meshes.begin(),
														  current->stagedGeometry->meshes.end(),
														  [](const auto& mesh) {
															  return mesh->normalAvailable && mesh->uvAvailable &&
															         mesh->tangentAvailable;
														  }) :
													  std::ptrdiff_t{ 0 } },
				{ "nativeEvictedMeshes", current && current->stagedGeometry ?
											 current->stagedGeometry->evictedMeshes :
											 0u },
				{ "nativeStagedBytes", current && current->stagedGeometry ?
										   current->stagedGeometry->residentBytes :
										   0ull },
				{ "poseFrame", current && current->stagedGeometry && current->stagedGeometry->pose ?
								   current->stagedGeometry->pose->captureFrame :
								   0u },
				{ "poseSkinFrame", current && current->stagedGeometry && current->stagedGeometry->pose ?
									   current->stagedGeometry->pose->skinFrameID :
									   0u },
				{ "poseRegisters", current && current->stagedGeometry && current->stagedGeometry->pose ?
									   current->stagedGeometry->pose->registerCount :
									   0u },
				{ "poseSamplesStaged", current && current->stagedGeometry ?
										   current->stagedGeometry->posedFrames :
										   0u },
				{ "poseDeferrals", current && current->stagedGeometry ?
									   current->stagedGeometry->poseDeferrals :
									   0u },
				{ "posePaletteRejections", current && current->stagedGeometry ?
											   current->stagedGeometry->posePaletteRejections :
											   0u },
				{ "poseRequiredBones", current && current->stagedGeometry ?
										   current->stagedGeometry->lastPoseRequiredBones :
										   0u },
				{ "poseAvailableBones", current && current->stagedGeometry ?
											current->stagedGeometry->lastPoseAvailableBones :
											0u },
				{ "deformSubmitted", current && bool(current->deformed) },
				{ "deformGpuReady", current && current->deformed && current->deformed->Ready() },
				{ "deformFrame", current && current->deformed ?
									 current->deformed->poseFrame :
									 0u },
				{ "deformOutputVertices", current && current->deformed ?
											  current->deformed->vertices :
											  0u },
				{ "deformGpuBytes", current && current->deformed ?
										current->deformed->allocationBytes :
										0ull },
				{ "deformInFlight", CharacterDeformationRetirement::InFlight() },
				{ "validationReady", current && current->validation.has_value() },
				{ "validationFrame", current && current->validation ?
										 current->validation->poseFrame :
										 0u },
				{ "validationInspected", current && current->validation ?
											 current->validation->inspected :
											 0u },
				{ "validationNonFinite", current && current->validation ?
											 current->validation->nonFinite :
											 0u },
				{ "validationSane", current && current->validation &&
										current->validation->sane },
				{ "validationMaximumBindDisplacement", current && current->validation ?
														   current->validation->maximumBindDisplacement :
														   0.0f },
				{ "validationMinimum", current && current->validation ?
										   current->validation->minimum :
										   std::array<float, 3>{} },
				{ "validationMaximum", current && current->validation ?
										   current->validation->maximum :
										   std::array<float, 3>{} },
				{ "blasPreflightReady", current && current->blasPreflight.has_value() },
				{ "blasPreflightPoseFrame", current && current->blasPreflight ?
												current->blasPreflight->poseFrame :
												0u },
				{ "blasPreflightDXR", current && current->blasPreflight &&
										  current->blasPreflight->dxrSupported },
				{ "blasPreflightBudget", current && current->blasPreflight &&
											 current->blasPreflight->budgetAvailable },
				{ "blasPreflightResultBytes", current && current->blasPreflight ?
												  current->blasPreflight->resultBytes :
												  0ull },
				{ "blasPreflightScratchBytes", current && current->blasPreflight ?
												   current->blasPreflight->scratchBytes :
												   0ull },
				{ "blasPreflightUpdateScratchBytes", current && current->blasPreflight ?
														 current->blasPreflight->updateScratchBytes :
														 0ull },
				{ "blasPreflightAvailableBytes", current && current->blasPreflight ?
													 current->blasPreflight->availableBytes :
													 0ull },
				{ "blasPreflightStatus", current && current->blasPreflight ?
											 current->blasPreflight->status :
											 std::string("No completed character pose") },
				{ "isolatedBLASSubmitted", current && bool(current->isolatedBLAS) },
				{ "isolatedBLASReady", current && current->isolatedBLAS &&
										   current->isolatedBLAS->Ready() },
				{ "isolatedBLASFrame", current && current->isolatedBLAS ?
										   current->isolatedBLAS->poseFrame :
										   0u },
				{ "isolatedBLASRefit", current && current->isolatedBLAS &&
										   current->isolatedBLAS->refit },
				{ "accumulatedBLASParts", current ? current->blasParts.size() : size_t{ 0 } },
				{ "rigidBLASParts", current ? current->rigidBLASParts.size() : size_t{ 0 } },
				{ "isolatedBLASBytes", current && current->isolatedBLAS ?
										   current->isolatedBLAS->allocationBytes :
										   0ull },
				{ "isolatedBLASScratchBytes", current && current->isolatedBLAS ?
												  current->isolatedBLAS->scratchBytes :
												  0ull },
				{ "isolatedBLASPending", CharacterBLASRetirement::Pending() },
				{ "isolatedBLASError", current ? current->blasError : std::string{} },
				{ "isolatedTLASSubmitted", current && bool(current->isolatedTLAS) },
				{ "isolatedTLASReady", current && current->isolatedTLAS &&
										   current->isolatedTLAS->Ready() },
				{ "isolatedTLASFrame", current && current->isolatedTLAS ?
										   current->isolatedTLAS->poseFrame :
										   0u },
				{ "isolatedTLASInstances", current && current->isolatedTLAS ?
											   current->isolatedTLAS->bottoms.size() :
											   size_t{ 0 } },
				{ "isolatedTLASBytes", current && current->isolatedTLAS ?
										   current->isolatedTLAS->allocationBytes :
										   0ull },
				{ "isolatedTLASPending", CharacterTLASRetirement::Pending() },
				{ "isolatedTLASError", current ? current->tlasError : std::string{} },
				{ "isolatedRaySubmitted", current && bool(current->rayProbe) },
				{ "isolatedRayReady", current && current->rayProbe &&
										  current->rayProbe->Ready() },
				{ "isolatedRayFrame", current && current->rayProbe ?
										  current->rayProbe->poseFrame :
										  0u },
				{ "isolatedRayOutcomeReady", current && current->rayOutcome.has_value() },
				{ "isolatedRayStatus", current && current->rayOutcome ?
										   current->rayOutcome->status :
										   0u },
				{ "isolatedRayPartsTested", current ? current->rayOutcomes.size() : size_t{ 0 } },
				{ "isolatedRayPartsHit", current ?
											 std::count_if(current->rayOutcomes.begin(),
												 current->rayOutcomes.end(),
												 [](const auto& result) { return result.status == 3u; }) :
											 std::ptrdiff_t{ 0 } },
				{ "isolatedRayDistance", current && current->rayOutcome ?
											 current->rayOutcome->hitDistance :
											 0.0f },
				{ "isolatedRayPrimitive", current && current->rayOutcome ?
											  current->rayOutcome->primitiveIndex :
											  UINT32_MAX },
				{ "isolatedRayInstance", current && current->rayOutcome ?
											 current->rayOutcome->instanceID :
											 UINT32_MAX },
				{ "isolatedRayPending", CharacterRayRetirement::Pending() },
				{ "isolatedRayError", current ? current->rayError : std::string{} },
				{ "deformError", current ? current->deformationError : std::string{} },
				{ "dxgiCharacterBudget", current && current->stagedGeometry ?
											 current->stagedGeometry->budgetBytes :
											 0ull },
				{ "nativeUploadRejections", current && current->stagedGeometry ?
												current->stagedGeometry->rejectedMeshes :
												0u },
				{ "nativeDxgiDeferrals", current && current->stagedGeometry ?
											 current->stagedGeometry->dxgiDeferrals :
											 0u },
				{ "workerBusy", workerBusy.load(std::memory_order_acquire) },
				{ "submitted", submitted.load(std::memory_order_relaxed) },
				{ "completed", completed.load(std::memory_order_relaxed) },
				{ "generationFrame", current ? current->frame : 0u },
				{ "sceneEpoch", current ? current->sceneEpoch : 0ull },
				{ "actors", current ? current->actors.size() : size_t{ 0 } },
				{ "playerActors", current ? std::count_if(current->actors.begin(), current->actors.end(),
												[](const auto& actor) { return actor.player; }) :
											std::ptrdiff_t{ 0 } },
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
			std::shared_ptr<const CharacterObservationGeneration> result;
		};

		std::mutex stateMutex;
		uint64_t sceneEpoch = 1;
		uint32_t lastObservedFrame = UINT32_MAX;
		size_t nextPoseSearchIndex = 0;
		std::shared_ptr<Work> pending;
		std::unordered_set<std::string> attemptedMeshKeys;
		std::atomic<std::shared_ptr<const CharacterObservationGeneration>> published;
		std::atomic<bool> workerBusy{ false };
		std::atomic<uint64_t> submitted{ 0 }, completed{ 0 };
	};
}
