#pragma once

#include "Features/DX12NativeScene/NativeCharacterMeshCapture.h"
#include "Features/DX12NativeScene/NativeCharacterPoseCapture.h"
#include "Features/DX12NativeScene/RuntimeGeometryEncoding.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"

#include <directx/d3dx12.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
    // Only native-owned DX12 buffers are published here. No DX11 staging
    // resources, bone-node pointers, mesh pointers or raster overrides.
    struct CharacterStagedMesh
    {
        uint32_t actorFormID = 0, partOrdinal = 0, vertices = 0, indices = 0;
        uint64_t sourceToken = 0, allocationBytes = 0;
        uint32_t vertexStride = 0;
        uint64_t indexOffset = 0;
        bool skinned = false;
        winrt::com_ptr<ID3D12Resource> source;
        std::string shapeName;
    };

    struct CharacterStagedPose
    {
        uint32_t actorFormID = 0, partOrdinal = 0, captureFrame = 0;
        uint32_t skinFrameID = 0, boneCount = 0, registerCount = 0;
        uint64_t sourceToken = 0, allocationBytes = 0;
        winrt::com_ptr<ID3D12Resource> registers; // Raw float4 VS inputs.
    };

    struct CharacterMeshGeneration
    {
        uint64_t sceneEpoch = 0, residentBytes = 0, budgetBytes = 0;
        uint32_t uploadedMeshes = 0, rejectedMeshes = 0, dxgiDeferrals = 0;
        uint32_t evictedMeshes = 0;
        uint32_t posedFrames = 0, poseDeferrals = 0;
        std::shared_ptr<const CharacterStagedPose> pose;
        std::vector<std::shared_ptr<const CharacterStagedMesh>> meshes;
    };

    inline std::vector<uint8_t> EncodeCharacterBindVertices(
        const std::vector<Util::SkinnedVertex>& source)
    {
        // 44-byte independent ABI: xyz(float3), bone indices(uint4),
        // weights(float4). Shader-side interpretation will be introduced
        // together with a validated pose-palette contract.
        constexpr size_t stride = 12 + 16 + 16;
        std::vector<uint8_t> bytes;
        if (source.empty() || source.size() > 65535) return bytes;
        bytes.reserve(source.size() * stride);
        for (const auto& v : source) {
            const float pos[3]{ v.position.x, v.position.y, v.position.z };
            const uint32_t bones[4]{
                v.bone[0] == Util::InvalidSkinBone ? UINT32_MAX : v.bone[0],
                v.bone[1] == Util::InvalidSkinBone ? UINT32_MAX : v.bone[1],
                v.bone[2] == Util::InvalidSkinBone ? UINT32_MAX : v.bone[2],
                v.bone[3] == Util::InvalidSkinBone ? UINT32_MAX : v.bone[3] };
            float sum = 0.0f;
            for (size_t j = 0; j < 4; ++j) {
                if (!std::isfinite(v.weight[j]) || v.weight[j] < 0.0f)
                    return {};
                if (bones[j] != UINT32_MAX) sum += v.weight[j];
            }
            if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) ||
                !std::isfinite(pos[2]) || !std::isfinite(sum) || sum <= 0.0f)
                return {};
            float weights[4]{};
            for (size_t j = 0; j < 4; ++j)
                if (bones[j] != UINT32_MAX) weights[j] = v.weight[j] / sum;
            AppendGeometryBytes(bytes, pos, sizeof(pos));
            AppendGeometryBytes(bytes, bones, sizeof(bones));
            AppendGeometryBytes(bytes, weights, sizeof(weights));
        }
        return bytes;
    }

    inline winrt::com_ptr<ID3D12Resource> UploadCharacterSourceBuffer(
        ID3D12Device* device, std::span<const uint8_t> bytes, uint64_t budget,
        uint64_t& allocationBytes)
    {
        if (!device || bytes.empty() || bytes.size() >= UINT64_MAX - 4)
            throw std::runtime_error("Character source data unavailable");
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(uint64_t(bytes.size()) + 4ull);
        const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
        if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes ||
            info.SizeInBytes > budget)
            throw std::runtime_error("DXGI character buffer budget exhausted");
        winrt::com_ptr<ID3D12Resource> result;
        const HRESULT created = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(result.put()));
        if (FAILED(created)) throw std::runtime_error("Character DX12 source allocation failed");
        result->SetName(L"DX12NativeCharacter::BindSource");
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        if (FAILED(result->Map(0, &noRead, &mapped)))
            throw std::runtime_error("Character DX12 upload map failed");
        std::memcpy(mapped, bytes.data(), bytes.size());
        std::memset(static_cast<uint8_t*>(mapped) + bytes.size(), 0, 4);
        result->Unmap(0, nullptr);
        allocationBytes = info.SizeInBytes;
        return result;
    }

    /** @brief Independent CPU worker routine; DX12 upload is the only GPU API.
     * This is bind-pose staging, not GPU deformation or BLAS admission.
     */
    inline std::shared_ptr<const CharacterMeshGeneration> PrepareCharacterMeshes(
        ID3D12Device* device, const CharacterMeshCapture* next,
        std::shared_ptr<const CharacterMeshGeneration> previous,
        uint64_t sceneEpoch, uint64_t maxResidentBytes = 16ull * 1024 * 1024)
    {
        auto out = std::make_shared<CharacterMeshGeneration>();
        out->sceneEpoch = sceneEpoch;
        if (previous && previous->sceneEpoch == sceneEpoch) {
            out->meshes = previous->meshes;
            out->residentBytes = previous->residentBytes;
            out->uploadedMeshes = previous->uploadedMeshes;
            out->rejectedMeshes = previous->rejectedMeshes;
            out->dxgiDeferrals = previous->dxgiDeferrals;
            out->evictedMeshes = previous->evictedMeshes;
            out->posedFrames = previous->posedFrames;
            out->poseDeferrals = previous->poseDeferrals;
            out->pose = previous->pose;
        }
        if (!device || !next) return out;
        // Reuse immutable GPU bytes for unchanged mesh identities. Identical
        // actor/ordinal with a different renderer token is a new mesh version.
        for (auto& existing : out->meshes) {
            if (existing->actorFormID != next->actorFormID ||
                existing->partOrdinal != next->partOrdinal) continue;
            if (existing->sourceToken == next->sourceToken) return out;
            out->residentBytes -= existing->allocationBytes;
            if (out->pose && out->pose->actorFormID == existing->actorFormID &&
                out->pose->partOrdinal == existing->partOrdinal) {
                out->residentBytes -= out->pose->allocationBytes;
                out->pose.reset();
            }
            existing.reset();
            break;
        }
        std::erase(out->meshes, nullptr);
        std::vector<uint8_t> bytes;
        uint32_t vertexCount = 0, indexCount = 0, stride = 0;
        uint64_t indexOffset = 0;
        if (next->skinned) {
            bytes = EncodeCharacterBindVertices(next->bindVertices);
            vertexCount = uint32_t(next->bindVertices.size());
            stride = 44;
            if (!bytes.empty() && !next->skinIndices.empty() &&
                next->skinIndices.size() % 3 == 0) {
                indexOffset = bytes.size();
                for (uint32_t index : next->skinIndices) {
                    if (index >= vertexCount) { bytes.clear(); break; }
                    AppendGeometryU32(bytes, index);
                }
                indexCount = bytes.empty() ? 0 : uint32_t(next->skinIndices.size());
            } else {
                bytes.clear();
            }
        } else if (next->rigid) {
            auto encoded = EncodeRuntimeGeometry(*next->rigid);
            if (encoded && encoded->geometry.size() > 16) {
                bytes.assign(encoded->geometry.begin() + 16, encoded->geometry.end());
                vertexCount = next->rigid->vertexCount;
                indexCount = next->rigid->indexCount;
                stride = 12;
                indexOffset = uint64_t(vertexCount) * stride;
            }
        }
        if (bytes.empty() || !vertexCount) { ++out->rejectedMeshes; return out; }
        VideoMemoryBudget policy;
        const auto dxgi = policy.Query(device, out->residentBytes, 0);
        out->budgetBytes = std::min(dxgi.effectiveBytes, maxResidentBytes);
        if (!dxgi.dxgiAvailable || out->residentBytes >= out->budgetBytes) {
            ++out->dxgiDeferrals;
            return out;
        }
        try {
            auto mesh = std::make_shared<CharacterStagedMesh>();
            mesh->actorFormID = next->actorFormID;
            mesh->partOrdinal = next->partOrdinal;
            mesh->sourceToken = next->sourceToken;
            mesh->shapeName = next->shapeName;
            mesh->vertices = vertexCount;
            mesh->indices = indexCount;
            mesh->vertexStride = stride;
            mesh->indexOffset = indexOffset;
            mesh->skinned = next->skinned;
            mesh->source = UploadCharacterSourceBuffer(device, bytes,
                out->budgetBytes - out->residentBytes, mesh->allocationBytes);
            out->residentBytes += mesh->allocationBytes;
            out->meshes.push_back(std::move(mesh));
            ++out->uploadedMeshes;
        } catch (...) {
            ++out->dxgiDeferrals; // Never touch raster or static TLAS on failure.
        }
        return out;
    }

    /** @brief Publish a new immutable DX12 palette buffer, if a matching
     * skinned bind resource is already resident. No native deformation,
     * shader dispatch or character shadow ownership is implied.
     */
    inline std::shared_ptr<const CharacterMeshGeneration> StageCharacterPose(
        ID3D12Device* device, const CharacterPoseCapture* incoming,
        const std::shared_ptr<const CharacterMeshGeneration>& prior,
        uint64_t sceneEpoch, uint64_t maxResidentBytes = 16ull * 1024 * 1024)
    {
        // Never attach a pose to geometry retained from an earlier scene.
        if (!prior || prior->sceneEpoch != sceneEpoch) return {};
        if (!device || !incoming ||
            incoming->registers.size() != incoming->registerCount ||
            incoming->registerCount != incoming->boneCount * 3u ||
            incoming->registerCount > 240u) return prior;
        bool hasBind = false;
        for (const auto& mesh : prior->meshes)
            hasBind |= mesh->skinned &&
                mesh->actorFormID == incoming->actorFormID &&
                mesh->partOrdinal == incoming->partOrdinal &&
                mesh->sourceToken == incoming->sourceToken;
        if (!hasBind) return prior;
        auto updated = std::make_shared<CharacterMeshGeneration>(*prior);
        if (updated->pose) {
            updated->residentBytes -= updated->pose->allocationBytes;
            updated->pose.reset();
        }
        const auto* first = reinterpret_cast<const uint8_t*>(incoming->registers.data());
        const std::span<const uint8_t> bytes(first,
            incoming->registers.size() * sizeof(incoming->registers[0]));
        VideoMemoryBudget policy;
        const auto snapshot = policy.Query(device, updated->residentBytes, 0);
        updated->budgetBytes = std::min(snapshot.effectiveBytes, maxResidentBytes);
        if (!snapshot.dxgiAvailable || updated->residentBytes >= updated->budgetBytes) {
            ++updated->poseDeferrals;
            return updated;
        }
        try {
            auto pose = std::make_shared<CharacterStagedPose>();
            pose->actorFormID = incoming->actorFormID;
            pose->partOrdinal = incoming->partOrdinal;
            pose->sourceToken = incoming->sourceToken;
            pose->captureFrame = incoming->capturedFrame;
            pose->skinFrameID = incoming->skinFrameID;
            pose->boneCount = incoming->boneCount;
            pose->registerCount = incoming->registerCount;
            pose->registers = UploadCharacterSourceBuffer(device, bytes,
                updated->budgetBytes - updated->residentBytes, pose->allocationBytes);
            updated->residentBytes += pose->allocationBytes;
            updated->pose = std::move(pose);
            ++updated->posedFrames;
        } catch (...) { ++updated->poseDeferrals; }
        return updated;
    }
}
