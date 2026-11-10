#include <catch2/catch_test_macros.hpp>
#include "../../features/DX12 Native Scene/Shaders/DX12NativeScene/NativeTraceDensity.hlsli"

using DX12Native::NativeTraceDensityPixel;

namespace
{
    uint32_t Sampled(uint32_t mode, uint32_t frame, uint32_t feature)
    {
        uint32_t count = 0;
        // This 18x18 grid contains whole triplets, checkerboard pairs,
        // quarters and 3x2/3x3 tiles, so every density is exact.
        for (uint32_t y = 0; y < 18; ++y)
            for (uint32_t x = 0; x < 18; ++x)
                if (NativeTraceDensityPixel(x,y,mode,frame,feature)) ++count;
        return count;
    }
}

TEST_CASE("Native lighting and shadow sparse modes preserve full-size tracking", "[dx12-native][density]")
{
    constexpr uint32_t pixels = 18 * 18;
    constexpr uint32_t expected[8]{
        pixels, pixels * 2 / 3, pixels / 2, pixels / 3,
        pixels / 4, pixels / 6, pixels / 9, pixels * 4 / 9
    };
    // Native trace density is an invocation mask, not a resized render
    // target. Both feature families address the original pixel coordinates.
    for (uint32_t mode = 0; mode < 8; ++mode)
        for (uint32_t frame = 0; frame < 4; ++frame) {
            CHECK(Sampled(mode,frame,0x51a3f62bu) == expected[mode]);
            CHECK(Sampled(mode,frame,0x62c01daeu) == expected[mode]);
        }
    CHECK(NativeTraceDensityPixel(0,0,0,0,0));
    CHECK(NativeTraceDensityPixel(17,17,0,0,0));
    // Half-density A/B alternation switches occupancy at the SAME pixel.
    CHECK(NativeTraceDensityPixel(4,4,2,0,0) !=
          NativeTraceDensityPixel(4,4,2,1,0));
}
