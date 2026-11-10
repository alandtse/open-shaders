#pragma once

#include "Features/DX12NativeScene/NativeCharacterDeformation.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace DX12Native
{
    /** @brief DXR 1.1 dynamic-BLAS prebuild estimate for one GPU-complete
     * skinned character pose. This does not allocate or publish a BLAS.
     * The final source geometry and renderer skinning parity are still
     * experimental; original raster/SCM ownership is unchanged.
     */
    struct CharacterBLASPreflight
    {
        uint32_t poseFrame = 0, actorFormID = 0, partOrdinal = 0;
        uint32_t vertices = 0, triangles = 0;
        uint64_t resultBytes = 0, scratchBytes = 0, updateScratchBytes = 0;
        uint64_t trackedBytes = 0, availableBytes = 0;
        bool dxrSupported = false, budgetAvailable = false;
        std::string status;
    };

    /** No GPU work and no waits; called on an independent native CPU worker.
     * An UPDATE-capable BLAS needs persistent topology and a completed
     * deformation buffer. Prebuild sizes are advisory: fresh DXGI telemetry
     * must be acquired again for the eventual actual allocation.
     */
    inline std::optional<CharacterBLASPreflight> PreflightCharacterDynamicBLAS(
        ID3D12Device* device,
        const std::shared_ptr<const CharacterDeformedResult>& deformation)
    {
        if (!device || !deformation || !deformation->Ready() ||
            !deformation->positions || !deformation->input ||
            !deformation->input->pose)
            return std::nullopt;
        CharacterBLASPreflight report;
        report.poseFrame = deformation->poseFrame;
        report.actorFormID = deformation->actorFormID;
        report.partOrdinal = deformation->partOrdinal;

        std::shared_ptr<const CharacterStagedMesh> mesh;
        for (const auto& part : deformation->input->meshes)
            if (part->skinned &&
                part->actorFormID == deformation->actorFormID &&
                part->partOrdinal == deformation->partOrdinal &&
                part->sourceToken == deformation->input->pose->sourceToken &&
                part->vertices == deformation->vertices &&
                part->source && part->indices >= 3 &&
                part->indices % 3 == 0 && part->indexOffset % 4 == 0 &&
                part->vertexStride == 44)
                { mesh = part; break; }
        if (!mesh) {
            report.status = "No matching DX12 skinned topology";
            return report;
        }
        report.vertices = mesh->vertices;
        report.triangles = mesh->indices / 3;
        const uint64_t sourceBytes = mesh->source->GetDesc().Width;
        const uint64_t indicesBytes = uint64_t(mesh->indices) * 4ull;
        if (mesh->indexOffset > sourceBytes ||
            indicesBytes > sourceBytes - mesh->indexOffset) {
            report.status = "Character index range exceeds the native source buffer";
            return report;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
            &options, sizeof(options))) ||
            options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
            report.status = "DXR 1.1 not available";
            return report;
        }
        winrt::com_ptr<ID3D12Device5> dxr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxr.put())))) {
            report.status = "DXR device interface unavailable";
            return report;
        }
        report.dxrSupported = true;

        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = mesh->vertices;
        geometry.Triangles.VertexBuffer.StartAddress =
            deformation->positions->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes = 12;
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometry.Triangles.IndexCount = mesh->indices;
        geometry.Triangles.IndexBuffer =
            mesh->source->GetGPUVirtualAddress() + mesh->indexOffset;
        if (!geometry.Triangles.VertexBuffer.StartAddress ||
            !geometry.Triangles.IndexBuffer) {
            report.status = "Native character GPU addresses unavailable";
            return report;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dxr->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (!info.ResultDataMaxSizeInBytes ||
            info.ResultDataMaxSizeInBytes == UINT64_MAX ||
            !info.ScratchDataSizeInBytes ||
            info.ScratchDataSizeInBytes == UINT64_MAX ||
            !info.UpdateScratchDataSizeInBytes ||
            info.UpdateScratchDataSizeInBytes == UINT64_MAX) {
            report.status = "Invalid DXR character prebuild sizes";
            return report;
        }
        report.resultBytes = info.ResultDataMaxSizeInBytes;
        report.scratchBytes = info.ScratchDataSizeInBytes;
        report.updateScratchBytes = info.UpdateScratchDataSizeInBytes;

        // The planned temporary peak includes a result and the larger of
        // build/update scratch. No per-feature fallback overrides DXGI.
        const uint64_t scratch = std::max(report.scratchBytes,
            report.updateScratchBytes);
        if (scratch > UINT64_MAX - report.resultBytes) {
            report.status = "DXR character size overflow";
            return report;
        }
        const uint64_t required = report.resultBytes + scratch;
        report.trackedBytes = deformation->input->residentBytes;
        if (deformation->allocationBytes > UINT64_MAX - report.trackedBytes) {
            report.status = "Character tracked allocation overflow";
            return report;
        }
        report.trackedBytes += deformation->allocationBytes;

        VideoMemoryBudget policy;
        const auto budget = policy.Query(device, report.trackedBytes, 0);
        if (budget.dxgiAvailable &&
            budget.effectiveBytes >= report.trackedBytes)
            report.availableBytes =
                budget.effectiveBytes - report.trackedBytes;
        report.budgetAvailable = budget.dxgiAvailable &&
            required <= report.availableBytes;
        report.status = !budget.dxgiAvailable ?
            "DXGI local-memory telemetry unavailable" :
            report.budgetAvailable ?
                "Dynamic BLAS prebuild estimated; no BLAS allocated" :
                "Insufficient current DXGI headroom for a dynamic BLAS";
        return report;
    }
}
