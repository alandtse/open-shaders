#pragma once

// Shared spatial rules for a future DX12-native incremental scene streamer.
// This is a planning/identity contract, not an active renderer switch.
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

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
}
