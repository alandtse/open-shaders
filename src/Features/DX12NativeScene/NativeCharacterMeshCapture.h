#pragma once

#include "Features/DX12NativeScene/RuntimeGeometryCapture.h"
#include "Utils/SkinnedMesh.h"

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace DX12Native
{
    // An owned, bounded snapshot: engine objects and D3D11 resources are
    // read only in the caller's render-thread scope and never retained.
    struct CharacterMeshCapture
    {
        uint32_t actorFormID = 0, partOrdinal = 0;
        uint64_t sourceToken = 0; // Opaque identity token, never dereferenced.
        uint32_t captureFrame = 0;
        std::string shapeName;
        std::vector<Util::SkinnedVertex> bindVertices;
        // Validated partition-local triangle indices, rebased into one
        // concatenated bind-vertex stream. Never invent skin topology.
        std::vector<uint32_t> skinIndices;
        std::shared_ptr<const RuntimeGeometryPacket> rigid;
        bool skinned = false;

        uint64_t Bytes() const
        {
            return rigid ? rigid->Bytes() :
                uint64_t(bindVertices.size()) * sizeof(Util::SkinnedVertex) +
                uint64_t(skinIndices.size()) * sizeof(uint32_t);
        }
    };

    inline uint64_t CharacterMeshSourceToken(RE::BSGeometry& geometry)
    {
        // Identity comparison only. No pointer is sent to a worker, and this
        // is never a persistent disk-cache key.
        const auto* renderer = geometry.GetGeometryRuntimeData().rendererData;
        auto a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&geometry));
        auto b = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(renderer));
        return (a >> 4) ^ (b << 1) ^ (b >> 17);
    }

    /** @brief Copy at most one eligible character part per sampled frame.
     * No skin pose is calculated here: the existing Skyrim renderer remains
     * authoritative while native bind geometry is staged for future skinning.
     */
    inline std::optional<CharacterMeshCapture> CaptureCharacterBindMesh(
        RE::BSGeometry& geometry, uint32_t actorFormID, uint32_t ordinal,
        uint32_t frame, uint64_t maxBytes = 256ull * 1024)
    {
        if (!maxBytes || !actorFormID || geometry.GetGeometryRuntimeData().alphaProperty)
            return std::nullopt;

        CharacterMeshCapture output;
        output.actorFormID = actorFormID;
        output.partOrdinal = ordinal;
        output.sourceToken = CharacterMeshSourceToken(geometry);
        output.captureFrame = frame;
        output.shapeName = geometry.name.empty() ? std::string("unnamed") :
            std::string(geometry.name.c_str());
        const auto& runtime = geometry.GetGeometryRuntimeData();

        if (runtime.skinInstance) {
            // This utility resolves partition-local bone slots into the
            // skin-instance palette. Never read arbitrary-size partitions.
            const auto* skin = runtime.skinInstance.get();
            const auto* partition = skin ? skin->skinPartition.get() : nullptr;
            if (!partition || !partition->numPartitions ||
                partition->numPartitions > 64 ||
                partition->partitions.size() < partition->numPartitions ||
                !skin->skinData || !skin->bones ||
                !skin->numMatrices || skin->numMatrices > 256) return std::nullopt;
            uint64_t totalVertices = 0, totalIndices = 0;
            for (uint32_t i = 0; i < partition->numPartitions; ++i) {
                const auto& p = partition->partitions[i];
                const auto desc = p.vertexDesc;
                if (!p.vertices || !p.triangles || p.strips ||
                    !p.bones || !p.buffData ||
                    !p.buffData->rawVertexData || !p.buffData->rawIndexData ||
                    !desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) ||
                    !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) ||
                    totalVertices + p.vertices > 65535) return std::nullopt;
                totalVertices += p.vertices;
                totalIndices += uint64_t(p.triangles) * 3u;
                if (totalIndices > UINT32_MAX ||
                    totalVertices * sizeof(Util::SkinnedVertex) +
                    totalIndices * sizeof(uint32_t) > maxBytes)
                    return std::nullopt;
            }
            if (!totalVertices || !totalIndices ||
                !Util::ReadSkinnedVertices(&geometry, output.bindVertices) ||
                output.bindVertices.size() != totalVertices) return std::nullopt;
            output.skinIndices.reserve(size_t(totalIndices));
            uint32_t vertexBase = 0;
            for (uint32_t i = 0; i < partition->numPartitions; ++i) {
                const auto& p = partition->partitions[i];
                for (uint32_t j = 0; j < uint32_t(p.triangles) * 3u; ++j) {
                    const uint32_t index = p.buffData->rawIndexData[j];
                    if (index >= p.vertices) return std::nullopt;
                    output.skinIndices.push_back(vertexBase + index);
                }
                vertexBase += p.vertices;
            }
            if (output.Bytes() > maxBytes) return std::nullopt;
            output.skinned = true;
        } else {
            // First phase admits opaque, CPU-backed rigid attachments only.
            // Skinned topology and alpha coverage need a separate renderer
            // contract, not an unsafe reinterpretation as static geometry.
            auto* tri = geometry.AsTriShape();
            if (!tri || geometry.AsDynamicTriShape()) return std::nullopt;
            const auto* renderer = runtime.rendererData;
            const auto desc = runtime.vertexDesc;
            if (!renderer || !renderer->rawVertexData || !renderer->rawIndexData ||
                !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) return std::nullopt;
            const auto& counts = tri->GetTrishapeRuntimeData();
            const uint32_t stride = desc.GetSize();
            if (!counts.vertexCount || counts.vertexCount > 65535 ||
                !counts.triangleCount || counts.triangleCount > 65535 ||
                stride < 16 || stride > 256) return std::nullopt;
            const uint64_t bytes = uint64_t(counts.vertexCount) * stride +
                uint64_t(counts.triangleCount) * 3 * sizeof(uint16_t);
            if (bytes > maxBytes) return std::nullopt;
            RuntimeGeometryInput input;
            input.referenceKey = std::format("actor:{:08X}", actorFormID);
            input.shapeName = std::format("{}:{}", output.shapeName, ordinal);
            input.vertexCount = counts.vertexCount;
            input.vertexStride = stride;
            input.triangleCount = counts.triangleCount;
            input.hasNormal = desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
            input.hasUv = desc.HasFlag(RE::BSGraphics::Vertex::VF_UV);
            input.hasTangent = desc.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT);
            if (input.hasNormal) input.normalOffset =
                desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
            if (input.hasUv) input.uvOffset =
                desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
            if (input.hasTangent) input.tangentOffset =
                desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL);
            for (uint32_t row = 0; row < 3; ++row)
                for (uint32_t col = 0; col < 3; ++col)
                    input.transform[4 * row + col] =
                        geometry.world.rotate.entry[row][col] * geometry.world.scale;
            input.transform[3] = geometry.world.translate.x;
            input.transform[7] = geometry.world.translate.y;
            input.transform[11] = geometry.world.translate.z;
            input.bound = { geometry.worldBound.center.x,
                geometry.worldBound.center.y, geometry.worldBound.center.z,
                geometry.worldBound.radius };
            input.frame = frame;
            input.vertices = { renderer->rawVertexData, size_t(counts.vertexCount) * stride };
            input.indices = { renderer->rawIndexData, size_t(counts.triangleCount) * 3 };
            output.rigid = CopyRuntimeGeometry(input, maxBytes);
            if (!output.rigid) return std::nullopt;
        }
        return output;
    }
}
