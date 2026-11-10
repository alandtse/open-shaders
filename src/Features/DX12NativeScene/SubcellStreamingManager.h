#pragma once
#include "SubcellSceneComposition.h"
#include "SpatialSubcells.h"
#include "VideoMemoryBudget.h"
#include <BS_thread_pool.hpp>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace DX12Native
{
    /** GPU-upload candidate manager. Owns partial tile loaders and produces a
     *  combined CPU snapshot; deliberately cannot replace the live TLAS. */
    class SubcellStreamingManager
    {
        using Coord = SpatialSubcells::Coordinate;
        using Key = std::pair<int64_t,int64_t>;
        struct Tile
        {
            PreparedScene loader;
            bool requested = false, failed = false;
            std::shared_ptr<const PreparedCell> cell;
        };
        struct Job
        {
            std::atomic<bool> done{false};
            uint64_t epoch = 0, serial = 0;
            Coord center{};
            SubcellComposition output;
        };
    public:
        void Reset()
        {
            ++serial;
            entries.clear();
            pending.reset();
            combined.reset();
            publishedInputs.clear();
            scope.clear();
            center = {};
            resident = loading = 0;
        }
        void Advance(const GraphicsBackend& backend, const std::filesystem::path& parent,
            std::string cellKey, uint64_t epoch, uint32_t size,
            double px, double py, double dx, double dy, uint64_t budgetBytes)
        {
            if (!SpatialSubcells::ValidSize(size) || !std::isfinite(px) || !std::isfinite(py)) return;
            const auto newScope = cellKey + ":" + std::to_string(epoch) + ":" + std::to_string(size);
            if (scope != newScope) { Reset(); scope = newScope; }
            const auto target = SpatialSubcells::FromPosition(px,py,size);
            if (std::abs(target.x) > 1'000'000 || std::abs(target.y) > 1'000'000) return;
            if (target != center) {
                center = target;
                combined.reset();
                publishedInputs.clear();
                ++serial;
            }
            const auto plan = SpatialSubcells::OrderedWork(center,dx,dy).active;
            std::erase_if(entries,[&](const auto& item) {
                return std::find(plan.begin(),plan.end(),
                    Coord{item.first.first,item.first.second}) == plan.end();
            });

            resident = loading = 0;
            uint64_t tracked = 0;
            std::vector<std::shared_ptr<const PreparedCell>> ready;
            for (auto position : plan) {
                auto it = entries.find({position.x,position.y});
                if (it == entries.end()) continue;
                auto& tile = *it->second;
                if (auto complete = tile.loader.GetCell()) tile.cell = std::move(complete);
                if (tile.cell) {
                    ++resident;
                    tracked += tile.cell->bytes;
                    ready.push_back(tile.cell);
                } else if (tile.requested) {
                    const auto report = tile.loader.GetDiagnostics();
                    if (report.value("loading",false)) ++loading;
                    else if (!report.value("failure",std::string{}).empty()) tile.failed = true;
                    else ++loading;
                }
            }

            if (pending && pending->done.load(std::memory_order_acquire)) {
                if (pending->epoch == epoch && pending->center == center &&
                    pending->serial == serial)
                    combined = pending->output.cell;
                pending.reset();
            }
            std::vector<const PreparedCell*> inputs;
            for (const auto& cell : ready) inputs.push_back(cell.get());
            if (!pending && !ready.empty() && inputs != publishedInputs) {
                publishedInputs = std::move(inputs);
                auto job = std::make_shared<Job>();
                job->epoch = epoch;
                job->serial = ++serial;
                job->center = center;
                try {
                    static BS::thread_pool<> pool{1};
                    pool.detach_task([job, ready=std::move(ready)] {
                        try { job->output = ComposeActiveSubcells(ready,job->epoch,job->serial); }
                        catch (...) {}
                        job->done.store(true,std::memory_order_release);
                    });
                    pending = std::move(job);
                } catch (...) { publishedInputs.clear(); }
            }

            if (loading || resident == 9) return;
            const auto memory = videoBudget.Query(backend.d3d12Device.get(),tracked,budgetBytes);
            if (!memory.dxgiAvailable || memory.effectiveBytes <= tracked) return;
            // One disk-to-GPU upload request per update. All additional GPU
            // allocations are rechecked against DXGI on the loader worker.
            for (auto pos : plan) {
                const Key key{pos.x,pos.y};
                auto& entry = entries[key];
                if (!entry) entry = std::make_unique<Tile>();
                if (entry->requested) continue;
                const auto folder = parent / "subcells" / std::to_string(size) /
                    (std::to_string(pos.x) + "_" + std::to_string(pos.y));
                std::error_code ec;
                if (!std::filesystem::is_regular_file(folder / "cell-layout.json",ec) || ec ||
                    !std::filesystem::is_regular_file(folder / "cell-geometry.rsgp",ec) || ec ||
                    !std::filesystem::is_regular_file(folder / "cell-shading.rssp",ec) || ec) continue;
                entry->loader.Request(backend,folder,cellKey,epoch,budgetBytes,budgetBytes);
                entry->requested = true;
                ++loading;
                break;
            }
        }
        std::shared_ptr<const PreparedCell> GetCombined() const { return combined; }
        json GetDiagnostics() const
        {
            return {{"scope",scope},{"centerX",center.x},{"centerY",center.y},
                {"resident",resident},{"loading",loading},
                {"composing",pending && !pending->done.load(std::memory_order_acquire)},
                {"candidateInstances",combined && combined->snapshot ? combined->snapshot->instances.size() : 0},
                {"candidateBytes",combined ? combined->bytes : 0},
                {"publishedToLiveTLAS",false},{"reserveBytes",videoBudget.last.reserve}};
        }
    private:
        std::map<Key,std::unique_ptr<Tile>> entries;
        std::shared_ptr<Job> pending;
        std::shared_ptr<const PreparedCell> combined;
        std::vector<const PreparedCell*> publishedInputs;
        VideoMemoryBudget videoBudget;
        std::string scope;
        Coord center{};
        uint64_t serial = 0;
        uint32_t resident = 0, loading = 0;
    };
}
