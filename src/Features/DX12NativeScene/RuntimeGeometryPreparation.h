#pragma once

#include "PreparedScene.h"
#include "RuntimeGeometryEncoding.h"
#include "RuntimeGeometryCacheWorker.h"
#include "Utils/D3D.h"
#include <BS_thread_pool.hpp>
#include <atomic>
#include <directx/d3dx12.h>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
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
        const std::shared_ptr<const PreparedCell>& previous, uint64_t epoch, uint64_t budget,
        const std::shared_ptr<const PrewarmedRuntimeGeometry>& prewarmed = {},
        uint32_t* cacheHitCounter = nullptr)
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
            // Prewarm only avoids CPU re-encoding after the renderer has supplied
            // byte-identical geometry. A saved reference/shape alone is insufficient.
            std::shared_ptr<const EncodedRuntimeGeometry> cached;
            if (prewarmed && !prewarmed->empty()) {
                if (const auto digest = RuntimeGeometryContentId(*packet)) {
                    const auto it = prewarmed->find(*digest);
                    if (it != prewarmed->end()) cached = it->second;
                }
            }
            const auto encoded = cached ? std::optional<EncodedRuntimeGeometry>{} :
                EncodeRuntimeGeometry(*packet);
            if (!cached && !encoded) { ++snapshot->omissions["invalid-runtime-packet"]; continue; }
            if (cached && cacheHitCounter) ++*cacheHitCounter;
            const auto& payload = cached ? *cached : *encoded;
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
                    std::span(payload.geometry).subspan(16), budget - result->bytes,
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
                static_assert(sizeof(instance.bound) == sizeof(float) * 4);
                std::memcpy(&instance.bound, packet->bound.data(), sizeof(instance.bound));
                if (!payload.attributes.empty() && budget > result->bytes) {
                    try {
                        auto attrs = std::make_shared<VertexAttributes>();
                        attrs->vertexCount = mesh->vertexCount;
                        attrs->stride = payload.attributeStride;
                        attrs->hasTangentFrame = payload.tangentFrame;
                        attrs->buffer = UploadRuntimeGeometryBuffer(backend,
                            std::span(payload.attributes).subspan(16), budget - result->bytes,
                            attrs->allocationBytes, L"DX12NativeScene::RuntimeAttributes");
                        result->bytes += attrs->allocationBytes;
                        instance.material.attributes = std::move(attrs);
                        ++result->shadedInstances;
                        if (payload.tangentFrame) ++result->tangentInstances;
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


    /** @brief Build an immutable CELL/TLAS input from already owned prepared and
     * runtime geometry. CPU composition happens on the geometry worker, not the
     * frame thread. The offline material source, lights and rooms remain
     * authoritative; only distinct runtime instances are appended.
     */
    inline std::shared_ptr<const PreparedCell> ComposeCurrentSceneGeometry(
        const std::shared_ptr<const PreparedCell>& baseline,
        const std::shared_ptr<const PreparedCell>& runtime, uint64_t sceneEpoch)
    {
        if (!baseline || !baseline->snapshot || !runtime || !runtime->snapshot ||
            baseline->snapshot->scope.scene != sceneEpoch ||
            runtime->snapshot->scope.scene != sceneEpoch) return {};

        auto result = std::make_shared<PreparedCell>(*baseline);
        auto snapshot = std::make_shared<Snapshot>(*baseline->snapshot);
        std::unordered_set<std::string> occupied;
        occupied.reserve(snapshot->instances.size() + runtime->snapshot->instances.size());
        const auto key = [](const Instance& value) {
            return value.preparedIdentity.referenceKey + '\0' + value.preparedIdentity.shapeName;
        };
        for (const auto& instance : snapshot->instances)
            if (!instance.preparedIdentity.referenceKey.empty() && !instance.preparedIdentity.shapeName.empty())
                occupied.insert(key(instance));
        size_t appended = 0;
        for (const auto& incoming : runtime->snapshot->instances) {
            if (snapshot->instances.size() >= 8192) {
                ++snapshot->omissions["current-scene-instance-limit"];
                break;
            }
            if (incoming.preparedIdentity.referenceKey.empty() ||
                incoming.preparedIdentity.shapeName.empty() ||
                !occupied.insert(key(incoming)).second) continue;
            if (!incoming.mesh || !runtime->geometries.contains(incoming.mesh.get())) {
                ++snapshot->omissions["current-scene-geometry-missing"];
                continue;
            }
            Instance instance = incoming;
            instance.identity = snapshot->instances.size() + 1;
            snapshot->instances.push_back(std::move(instance));
            ++appended;
            if (!incoming.material.opaqueCandidate) ++result->maskedInstances;
            if (incoming.material.attributes) {
                ++result->shadedInstances;
                if (incoming.material.attributes->hasTangentFrame) ++result->tangentInstances;
            }
        }
        if (!appended) return {};
        result->geometries.insert(runtime->geometries.begin(), runtime->geometries.end());
        result->bytes += runtime->bytes;
        snapshot->generation = baseline->snapshot->generation + runtime->snapshot->generation;
        snapshot->sweep = snapshot->generation;
        snapshot->scope.batch = snapshot->generation;
        snapshot->scope.policy = 2; // prepared baseline plus eligible runtime geometry
        snapshot->offline = false;
        // Material tables must still use the validated prepared CELL package.
        snapshot->materialSource = baseline->snapshot->materialSource;
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
            uint32_t cacheHits = 0;
            std::vector<std::string> keys;
            std::shared_ptr<const PreparedCell> result;
            std::shared_ptr<const PreparedCell> composed;
            std::string failure;
        };
    public:
        void Reset()
        {
            pending.reset(); // Detached worker owns all its input data.
            prepared.reset();
            composed.reset();
            attempted.clear();
            epoch = 0;
            batches = 0;
            failures = 0;
            cacheHits = 0;
            scannedMailboxRevision = UINT64_MAX;
        }
        void Advance(const GraphicsBackend& backend, const RuntimeGeometryMailbox& mailbox,
            uint64_t sceneEpoch, uint64_t budget,
            std::shared_ptr<const PrewarmedRuntimeGeometry> prewarmed = {},
            std::shared_ptr<const PreparedCell> baseline = {})
        {
            if (epoch != sceneEpoch) { Reset(); epoch = sceneEpoch; }
            if (pending && pending->done.load(std::memory_order_acquire)) {
                for (const auto& key : pending->keys) attempted.insert(key);
                if (pending->result) { prepared = pending->result; composed = pending->composed; ++batches; }
                else ++failures;
                cacheHits += pending->cacheHits;
                pending.reset();
            }
            if (pending || !backend.d3d12Device || !budget ||
                scannedMailboxRevision == mailbox.Revision()) return;
            scannedMailboxRevision = mailbox.Revision();
            const auto items = mailbox.Export(0, mailbox.Count());
            std::vector<std::shared_ptr<const RuntimeGeometryPacket>> next;
            auto work = std::make_shared<Work>();
            work->epoch = sceneEpoch;
            const uint64_t batchBytes = std::max<uint64_t>(512ull * 1024, budget / 32);
            uint64_t selectedBytes = 0;
            for (const auto& item : items) {
                const auto key = item->referenceKey + '\0' + item->shapeName;
                if (attempted.contains(key)) continue;
                if (selectedBytes && item->Bytes() > batchBytes - selectedBytes) break;
                next.push_back(item);
                work->keys.push_back(key);
                selectedBytes += item->Bytes();
            }
            if (next.empty()) return;
            try {
                static BS::thread_pool<> pool{ 1 };
                pool.detach_task([work, source = std::move(next), backend,
                    previous = prepared, budget, sceneEpoch, prewarm = std::move(prewarmed),
                    baseline = std::move(baseline)] {
                    try {
                        work->result = PrepareRuntimeGeometryBatch(
                            backend, source, previous, sceneEpoch, budget, prewarm, &work->cacheHits);
                        work->composed = ComposeCurrentSceneGeometry(baseline, work->result, sceneEpoch);
                    }
                    catch (const std::exception& error) { work->failure = error.what(); }
                    work->done.store(true, std::memory_order_release);
                });
                pending = std::move(work);
            } catch (const std::exception&) { ++failures; }
        }

        std::shared_ptr<const PreparedCell> GetCell() const { return prepared; }
        std::shared_ptr<const PreparedCell> GetCurrentSceneCell() const { return composed; }
        bool Busy() const { return pending != nullptr; }
        uint32_t Batches() const { return batches; }
        uint32_t Failures() const { return failures; }
        uint32_t CacheHits() const { return cacheHits; }
        size_t Attempted() const { return attempted.size(); }

    private:
        uint64_t epoch = 0, scannedMailboxRevision = UINT64_MAX;
        uint32_t batches = 0, failures = 0, cacheHits = 0;
        std::unordered_set<std::string> attempted;
        std::shared_ptr<const PreparedCell> prepared, composed;
        std::shared_ptr<Work> pending;
    };
}
