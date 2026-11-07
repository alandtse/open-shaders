#pragma once

#include "RuntimeGeometryDiskFormat.h"
#include "RuntimeGeometryEncoding.h"
#include "MaterialDiskCache.h"
#include <BS_thread_pool.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
    using PrewarmedRuntimeGeometry =
        std::unordered_map<std::string, std::shared_ptr<const EncodedRuntimeGeometry>>;

    /** @brief Saves runtime-observed geometry and prewarms verified CPU data on a worker.
     *
     * Never imports cache contents as scene instances. Prewarmed data is only
     * eligible for reuse after an identical runtime packet has been observed.
     * No DX11/DX12 readback or GPU work occurs in this cache worker.
     */
    class RuntimeGeometryCacheWorker
    {
        struct Shared
        {
            MaterialDiskCache disk{
                "Data/SKSE/Plugins/CommunityShaders/NativePBRCache/geometry",
                256ull * 1024 * 1024, ".rgc" };
        };
        struct Work
        {
            std::atomic<bool> done{ false };
            bool menu = false;
            uint64_t epoch = 0;
            uint32_t processed = 0, accepted = 0, rejected = 0;
            uint64_t bytes = 0;
            std::vector<std::shared_ptr<const RuntimeGeometryPacket>> inputs;
            std::shared_ptr<const PrewarmedRuntimeGeometry> prepared;
            std::string error;
        };

    public:
        /** @brief Keep validated CPU cache data but invalidate per-scene capture scheduling. */
        void ResetScene()
        {
            ++epoch;
            scheduled.clear();
        }

        /** @brief Called on the main thread; only schedules background I/O and reads completed stats. */
        void Advance(bool menu, const RuntimeGeometryMailbox* mailbox)
        {
            if (work && work->done.load(std::memory_order_acquire)) {
                if (work->menu) {
                    if (work->prepared) prewarmed = work->prepared;
                    prewarmComplete = true;
                    prewarmChecked = work->processed;
                    prewarmAccepted = work->accepted;
                    prewarmBytes = work->bytes;
                } else {
                    stored += work->accepted;
                    rejected += work->rejected;
                }
                if (!work->error.empty()) lastError = work->error;
                work.reset();
            }
            if (work) return;
            if (menu && !menuRequested) {
                menuRequested = true;
                auto next = std::make_shared<Work>();
                next->menu = true;
                next->epoch = epoch;
                Start(next);
                return;
            }
            if (menu || !mailbox || mailbox->Count() == 0) return;
            auto next = std::make_shared<Work>();
            next->epoch = epoch;
            for (const auto& packet : mailbox->Export(0, mailbox->Count())) {
                const auto key = packet->referenceKey + '\0' + packet->shapeName;
                if (!scheduled.insert(key).second) continue;
                next->inputs.push_back(packet);
                if (next->inputs.size() >= 4) break;
            }
            if (!next->inputs.empty()) Start(next);
        }

        std::shared_ptr<const PrewarmedRuntimeGeometry> GetPrewarmed() const { return prewarmed; }
        bool Busy() const { return bool(work); }
        bool MenuReady() const { return prewarmComplete; }
        uint32_t MenuChecked() const { return prewarmChecked; }
        uint32_t MenuAccepted() const { return prewarmAccepted; }
        uint64_t MenuBytes() const { return prewarmBytes; }
        uint32_t Stored() const { return stored; }
        uint32_t Rejected() const { return rejected; }
        const std::string& Failure() const { return lastError; }

    private:
        void Start(const std::shared_ptr<Work>& next)
        {
            work = next;
            try {
                static BS::thread_pool<> pool{ 1 };
                pool.detach_task([state = shared, result = next] {
                    try {
                        if (result->menu) {
                            auto items = std::make_shared<PrewarmedRuntimeGeometry>();
                            uint64_t used = 0;
                            for (const auto& digest : state->disk.ListDigests(256)) {
                                ++result->processed;
                                const auto data = state->disk.Read(digest, 512 * 1024 + 4096);
                                if (!data) { ++result->rejected; continue; }
                                const auto packet = DeserializeRuntimeGeometryCache(*data);
                                if (!packet) { ++result->rejected; continue; }
                                auto encoded = EncodeRuntimeGeometry(*packet);
                                if (!encoded) { ++result->rejected; continue; }
                                const auto bytes = uint64_t(encoded->geometry.size()) + encoded->attributes.size();
                                if (used > 64ull * 1024 * 1024 ||
                                    bytes > 64ull * 1024 * 1024 - used) break;
                                used += bytes;
                                items->emplace(digest,
                                    std::make_shared<const EncodedRuntimeGeometry>(std::move(*encoded)));
                                ++result->accepted;
                            }
                            result->bytes = used;
                            result->prepared = std::move(items);
                        } else {
                            for (const auto& packet : result->inputs) {
                                ++result->processed;
                                const auto bytes = SerializeRuntimeGeometryCache(*packet);
                                if (!bytes) { ++result->rejected; continue; }
                                const auto digest = Util::FileDigest::Sha256BytesHex(*bytes);
                                if (!digest) { ++result->rejected; continue; }
                                const auto status = state->disk.Store(*digest, *bytes);
                                if (status == MaterialDiskCache::StoreResult::Stored ||
                                    status == MaterialDiskCache::StoreResult::Existing)
                                    ++result->accepted;
                                else ++result->rejected;
                            }
                        }
                    } catch (const std::exception& error) {
                        result->error = error.what();
                        ++result->rejected;
                    }
                    result->done.store(true, std::memory_order_release);
                });
            } catch (const std::exception& error) {
                lastError = error.what();
                work.reset();
            }
        }

        std::shared_ptr<Shared> shared = std::make_shared<Shared>();
        std::shared_ptr<Work> work;
        std::shared_ptr<const PrewarmedRuntimeGeometry> prewarmed;
        std::unordered_set<std::string> scheduled;
        uint64_t epoch = 0;
        uint32_t stored = 0, rejected = 0, prewarmChecked = 0, prewarmAccepted = 0;
        uint64_t prewarmBytes = 0;
        bool menuRequested = false, prewarmComplete = false;
        std::string lastError;
    };
}
