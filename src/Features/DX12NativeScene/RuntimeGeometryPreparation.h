#pragma once

#include "PreparedScene.h"
#include "RuntimeGeometryEncoding.h"
#include <BS_thread_pool.hpp>
#include <atomic>
#include <directx/d3dx12.h>
#include <exception>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
    /** @brief Upload heap is owned by the published geometry and never mapped by the draw hook. */
    inline winrt::com_ptr<ID3D12Resource> UploadRuntimeGeometryBuffer(
        const GraphicsBackend& backend, std::span<const uint8_t> bytes,
        uint64_t budget, uint64_t& allocation, const wchar_t* name)
    {
        if (!backend.d3d12Device || bytes.empty() || bytes.size() > budget)
            throw std::runtime_error("Runtime geometry upload unavailable or outside budget");
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes.size() + 4);
        const auto info = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
        if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes > budget)
            throw std::runtime_error("Runtime geometry GPU allocation exceeds budget");
        winrt::com_ptr<ID3D12Resource> resource;
        DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(resource.put())));
        resource->SetName(name);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        DX::ThrowIfFailed(resource->Map(0, &noRead, &mapped));
        std::memcpy(mapped, bytes.data(), bytes.size());
        std::memset(static_cast<uint8_t*>(mapped) + bytes.size(), 0, 4);
        resource->Unmap(0, nullptr);
        allocation = info.SizeInBytes;
        return resource;
    }

    /** @brief Builds another immutable runtime-only native geometry generation. */
    inline std::shared_ptr<const PreparedCell> PrepareRuntimeGeometryBatch(
        const GraphicsBackend& backend,
        const std::vector<std::shared_ptr<const RuntimeGeometryPacket>>& packets,
        const std::shared_ptr<const PreparedCell>& previous, uint64_t epoch, uint64_t budget)
    {
        auto result = previous ? std::make_shared<PreparedCell>(*previous) : std::make_shared<PreparedCell>();
        auto snapshot = previous && previous->snapshot ?
            std::make_shared<Snapshot>(*previous->snapshot) : std::make_shared<Snapshot>();
        snapshot->offline = false;
        snapshot->generation = previous && previous->snapshot ? previous->snapshot->generation + 1 : 1;
        snapshot->sweep = snapshot->generation;
        snapshot->scope.scene = epoch;
        snapshot->scope.batch = snapshot->generation;
        snapshot->scope.policy = 1; // runtime-owned geometry, not offline package
        snapshot->materialSource.reset(); // diagnostic AS preview does not publish material tables
        result->digest = "runtime-captured";
        for (const auto& packet : packets) {
            if (!packet || snapshot->instances.size() >= 8192) break;
            const auto encoded = EncodeRuntimeGeometry(*packet);
            if (!encoded) { ++snapshot->omissions["invalid-runtime-packet"]; continue; }
            try {
                if (budget <= result->bytes) { ++snapshot->omissions["geometry-budget"]; continue; }
                auto geometry = std::make_shared<Geometry>();
                auto mesh = std::make_shared<Mesh>();
                mesh->stride = 12;
                mesh->vertexCount = packet->vertexCount;
                mesh->bytes = packet->Bytes();
                geometry->source = mesh;
                geometry->prepared = true;
                geometry->indexOffset = uint64_t(mesh->vertexCount) * 12;
                geometry->validationOffset = geometry->indexOffset + uint64_t(packet->indexCount) * 4;
                geometry->buffer = UploadRuntimeGeometryBuffer(backend,
                    std::span(encoded->geometry).subspan(16), budget - result->bytes,
                    geometry->allocationBytes, L"DX12NativeScene::RuntimeGeometry");
                result->bytes += geometry->allocationBytes;
                Instance instance;
                instance.identity = snapshot->instances.size() + 1;
                instance.preparedIdentity.referenceKey = packet->referenceKey;
                instance.preparedIdentity.shapeName = packet->shapeName;
                instance.mesh = mesh;
                instance.indexCount = packet->indexCount;
                instance.material.opaqueCandidate = true;
                instance.material.shadingStatus = "runtime-captured";
                instance.observationFrame = packet->captureFrame;
                std::memcpy(instance.world.m, packet->transform.data(), sizeof(instance.world.m));
                std::copy(packet->bound.begin(), packet->bound.end(), &instance.bound.x);
                if (!encoded->attributes.empty() && budget > result->bytes) {
                    try {
                        auto attrs = std::make_shared<VertexAttributes>();
                        attrs->vertexCount = mesh->vertexCount;
                        attrs->stride = encoded->attributeStride;
                        attrs->hasTangentFrame = encoded->tangentFrame;
                        attrs->buffer = UploadRuntimeGeometryBuffer(backend,
                            std::span(encoded->attributes).subspan(16), budget - result->bytes,
                            attrs->allocationBytes, L"DX12NativeScene::RuntimeAttributes");
                        result->bytes += attrs->allocationBytes;
                        instance.material.attributes = std::move(attrs);
                        ++result->shadedInstances;
                        if (encoded->tangentFrame) ++result->tangentInstances;
                    } catch (const std::exception&) {
                        ++snapshot->omissions["runtime-attributes-budget"];
                    }
                }
                result->geometries.emplace(mesh.get(), std::move(geometry));
                snapshot->instances.push_back(std::move(instance));
            } catch (const std::exception&) { ++snapshot->omissions["runtime-geometry-upload"]; }
        }
        result->snapshot = std::move(snapshot);
        return result;
    }

    /** @brief One owned batch at a time; publication never waits on the render thread. */
    class RuntimeGeometryPreparation
    {
        struct Work
        {
            std::atomic<bool> done{ false };
            uint64_t epoch = 0;
            std::vector<std::string> keys;
            std::shared_ptr<const PreparedCell> result;
            std::string failure;
        };
    public:
        void Reset()
        {
            pending.reset(); // Detached worker owns all its input data.
            prepared.reset();
            attempted.clear();
            epoch = 0;
            batches = 0;
            failures = 0;
        }
        void Advance(const GraphicsBackend& backend, const RuntimeGeometryMailbox& mailbox,
            uint64_t sceneEpoch, uint64_t budget)
        {
            if (epoch != sceneEpoch) { Reset(); epoch = sceneEpoch; }
            if (pending && pending->done.load(std::memory_order_acquire)) {
                for (const auto& key : pending->keys) attempted.insert(key);
                if (pending->result) { prepared = pending->result; ++batches; }
                else ++failures;
                pending.reset();
            }
            if (pending || !backend.d3d12Device || !budget) return;
            const auto items = mailbox.Export(0, mailbox.Count());
            std::vector<std::shared_ptr<const RuntimeGeometryPacket>> next;
            auto work = std::make_shared<Work>();
            work->epoch = sceneEpoch;
            next.reserve(4);
            for (const auto& item : items) {
                const auto key = item->referenceKey + '\0' + item->shapeName;
                if (attempted.contains(key)) continue;
                next.push_back(item);
                work->keys.push_back(key);
                if (next.size() == 4) break;
            }
            if (next.empty()) return;
            try {
                static BS::thread_pool<> pool{ 1 };
                pool.detach_task([work, source = std::move(next), backend,
                    previous = prepared, budget, sceneEpoch] {
                    try { work->result = PrepareRuntimeGeometryBatch(
                        backend, source, previous, sceneEpoch, budget); }
                    catch (const std::exception& error) { work->failure = error.what(); }
                    work->done.store(true, std::memory_order_release);
                });
                pending = std::move(work);
            } catch (const std::exception&) { ++failures; }
        }

        std::shared_ptr<const PreparedCell> GetCell() const { return prepared; }
        bool Busy() const { return pending != nullptr; }
        uint32_t Batches() const { return batches; }
        uint32_t Failures() const { return failures; }
        size_t Attempted() const { return attempted.size(); }

    private:
        uint64_t epoch = 0;
        uint32_t batches = 0, failures = 0;
        std::unordered_set<std::string> attempted;
        std::shared_ptr<const PreparedCell> prepared;
        std::shared_ptr<Work> pending;
    };
}
