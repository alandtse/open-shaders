#pragma once

#include <RE/B/BSGeometry.h>
#include <RE/P/PlayerCharacter.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace DX12Native
{
    // CPU-only draw-hook observation. Bounds remain in Skyrim world space;
    // the compositor converts them to the current raster camera origin.
    struct RasterCharacterFrame
    {
        static constexpr uint32_t kMaxBounds = 64;
        uint32_t frame = 0, count = 0;
        std::array<std::array<float, 4>, kMaxBounds> bounds{};
    };

    class PlayerRasterBounds
    {
    public:
        void Reset()
        {
            std::scoped_lock lock(mutex);
            frame = 0;
            shapes.clear();
            observed = dropped = 0;
        }

        void Observe(RE::BSGeometry* geometry, uint32_t currentFrame)
        {
            if (!geometry || !currentFrame ||
                !geometry->GetGeometryRuntimeData().skinInstance)
                return;
            const auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            // The draw's BSGeometry often has no direct reference userdata.
            // Restrict capture to the player-owned scenegraph. Shared meshes
            // used by NPCs do not qualify.
            bool playerOwned = false;
            const RE::NiAVObject* parent = geometry;
            for (uint32_t level = 0; parent && level < 32; ++level, parent = parent->parent) {
                if (parent->GetUserData() == player) { playerOwned = true; break; }
            }
            if (!playerOwned) return;
            const auto& bound = geometry->worldBound;
            if (!std::isfinite(bound.center.x) ||
                !std::isfinite(bound.center.y) ||
                !std::isfinite(bound.center.z) ||
                !std::isfinite(bound.radius) ||
                bound.radius <= 0.0f || bound.radius > 5000.0f)
                return;
            std::scoped_lock lock(mutex);
            if (frame != currentFrame) {
                frame = currentFrame;
                shapes.clear();
            }
            const auto key = reinterpret_cast<std::uintptr_t>(geometry);
            if (shapes.size() >= RasterCharacterFrame::kMaxBounds && !shapes.contains(key)) {
                ++dropped;
                return;
            }
            shapes[key] = { bound.center.x, bound.center.y,
                bound.center.z, bound.radius };
            ++observed;
        }

        RasterCharacterFrame Get(uint32_t requestedFrame) const
        {
            RasterCharacterFrame result;
            std::scoped_lock lock(mutex);
            if (!requestedFrame || requestedFrame != frame) return result;
            result.frame = frame;
            for (const auto& [key, bound] : shapes) {
                if (result.count == RasterCharacterFrame::kMaxBounds) break;
                result.bounds[result.count++] = bound;
            }
            return result;
        }
        uint64_t Observations() const
        {
            std::scoped_lock lock(mutex);
            return observed;
        }
        uint64_t Dropped() const
        {
            std::scoped_lock lock(mutex);
            return dropped;
        }
        uint32_t LatestFrame() const
        {
            std::scoped_lock lock(mutex);
            return frame;
        }

    private:
        mutable std::mutex mutex;
        uint32_t frame = 0;
        uint64_t observed = 0, dropped = 0;
        std::unordered_map<std::uintptr_t, std::array<float, 4>> shapes;
    };
}
