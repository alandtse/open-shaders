#pragma once

// Shared spatial rules for a future DX12-native incremental scene streamer.
// This is a planning/identity contract, not an active renderer switch.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace DX12Native::SpatialSubcells
{
    inline constexpr uint32_t kMinimumSize = 2048;
    inline constexpr uint32_t kDefaultSize = 4096;
    inline constexpr uint32_t kMaximumSize = 16384;
    inline constexpr uint32_t kStep = 1024;
    inline constexpr uint32_t kCatalogVersion = 1;

    inline constexpr bool ValidSize(uint32_t size)
    {
        return size >= kMinimumSize && size <= kMaximumSize && size % kStep == 0;
    }

    struct Coordinate
    {
        int64_t x = 0, y = 0;
        bool operator==(const Coordinate&) const = default;
    };

    // Worldspace/cell identity and content revision remain separate from
    // GPU residency. Changing size changes the spatial cache namespace.
    struct Key
    {
        std::string scope;
        std::string contentDigest;
        uint32_t size = kDefaultSize;
        Coordinate coordinate{};
        bool operator==(const Key&) const = default;
    };

    inline Coordinate FromPosition(double x, double y, uint32_t size)
    {
        // Floor is required: C++ truncation misclassifies negative coordinates.
        return { static_cast<int64_t>(std::floor(x / double(size))),
                 static_cast<int64_t>(std::floor(y / double(size))) };
    }

    inline std::array<Coordinate, 9> Active3x3(Coordinate center)
    {
        std::array<Coordinate, 9> result{};
        size_t i = 0;
        for (int64_t y = -1; y <= 1; ++y)
            for (int64_t x = -1; x <= 1; ++x)
                result[i++] = { center.x + x, center.y + y };
        return result;
    }

    struct Predictive5x5
    {
        std::array<Coordinate, 12> sides{};
        std::array<Coordinate, 4> corners{};
    };

    inline Predictive5x5 OuterRing(Coordinate center)
    {
        Predictive5x5 result{};
        size_t side = 0, corner = 0;
        for (int64_t y = -2; y <= 2; ++y)
            for (int64_t x = -2; x <= 2; ++x)
            {
                if (std::max(std::abs(x), std::abs(y)) != 2) continue;
                if (std::abs(x) == 2 && std::abs(y) == 2)
                    result.corners[corner++] = { center.x + x, center.y + y };
                else
                    result.sides[side++] = { center.x + x, center.y + y };
            }
        return result;
    }
    // Order active cells first, predictive side cells second, outer corners
    // last. Movement direction only reorders *within* these fixed stages.
    struct WorkOrder
    {
        std::array<Coordinate, 9> active{};
        Predictive5x5 predictive{};
    };

    template <typename Iter>
    void PrioritizeForMovement(Iter begin, Iter end, Coordinate center, double movementX, double movementY)
    {
        if (!std::isfinite(movementX) || !std::isfinite(movementY)) return;
        const auto projection = [&](Coordinate point) {
            return double(point.x - center.x) * movementX +
                   double(point.y - center.y) * movementY;
        };
        std::stable_sort(begin, end, [&](Coordinate left, Coordinate right) {
            const double a = projection(left), b = projection(right);
            return a > b;
        });
    }

    inline WorkOrder OrderedWork(Coordinate center, double movementX, double movementY)
    {
        WorkOrder order{ Active3x3(center), OuterRing(center) };
        std::swap(order.active[0], order.active[4]); // Center before any neighbors.
        PrioritizeForMovement(order.active.begin() + 1, order.active.end(), center, movementX, movementY);
        PrioritizeForMovement(order.predictive.sides.begin(), order.predictive.sides.end(),
            center, movementX, movementY);
        PrioritizeForMovement(order.predictive.corners.begin(), order.predictive.corners.end(),
            center, movementX, movementY);
        return order;
    }

    template <typename IsPrepared>
    std::vector<Coordinate> PendingWork(Coordinate center, double movementX, double movementY,
        IsPrepared&& isPrepared)
    {
        const auto order = OrderedWork(center, movementX, movementY);
        std::vector<Coordinate> result;
        result.reserve(25);
        // Do not issue predictive work until all nine are ready. This stage
        // checks disk preparation state, not GPU residency. Callers must still
        // respect independent DXGI limits for speculative uploads.
        for (auto coordinate : order.active)
            if (!isPrepared(coordinate)) result.push_back(coordinate);
        if (!result.empty()) return result;

        for (auto coordinate : order.predictive.sides)
            if (!isPrepared(coordinate)) result.push_back(coordinate);
        if (!result.empty()) return result;

        for (auto coordinate : order.predictive.corners)
            if (!isPrepared(coordinate)) result.push_back(coordinate);
        return result;
    }

    inline uint32_t ReusedActiveCount(Coordinate before, Coordinate after)
    {
        const auto oldWindow = Active3x3(before);
        const auto newWindow = Active3x3(after);
        uint32_t overlap = 0;
        for (auto now : newWindow)
            if (std::find(oldWindow.begin(), oldWindow.end(), now) != oldWindow.end())
                ++overlap;
        return overlap;
    }

}
