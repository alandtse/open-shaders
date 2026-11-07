#ifndef NATIVE_TRACE_DENSITY_HLSLI
#define NATIVE_TRACE_DENSITY_HLSLI

// Trace density is not texture resolution. Each feature keeps native-size
// raw output textures and marks omitted pixels explicitly. Independent
// shadow modes 3..7 rotate deterministic native-pixel tile phases.
#ifdef __cplusplus
#include <cstdint>
namespace DX12Native
{
using uint = uint32_t;
#define NATIVE_DENSITY_INLINE inline
#else
#define NATIVE_DENSITY_INLINE
#endif

NATIVE_DENSITY_INLINE uint NativeDensityHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

NATIVE_DENSITY_INLINE bool NativeTraceDensityPixel(uint x, uint y, uint mode, uint frame, uint featureSalt)
{
    if (mode == 1u) {
        // Exactly one omitted pixel in each complete horizontal triplet.
        // The fresh per-frame hash is independent for lighting and shadows.
        uint group = x / 3u;
        uint randomPhase = NativeDensityHash(group * 0x9e3779b9u ^
            y * 0x85ebca6bu ^ frame * 0xc2b2ae35u ^ featureSalt) % 3u;
        return x % 3u != randomPhase;
    }
    if (mode == 2u) {
        // One checkerboard parity per frame, alternating A/B every frame.
        return ((x ^ y ^ frame ^ featureSalt) & 1u) == 0u;
    }
    // Shadow-only extensions: legacy native-lighting mode IDs stay unchanged.
    // Tile phase is fixed per tile and visits each residue before repeating.
    if (mode >= 3u && mode <= 7u) {
        uint columns = (mode == 4u) ? 2u : 3u;
        uint rows = (mode == 5u) ? 2u : (mode == 4u) ? 2u : 3u;
        uint count = columns * rows;
        uint groupX = x / columns;
        uint groupY = y / rows;
        uint offset = NativeDensityHash(groupX * 0x9e3779b9u ^
            groupY * 0x85ebca6bu ^ featureSalt) % count;
        uint rank = (x % columns) + (y % rows) * columns;
        if (mode == 3u)
            return ((x + y + frame + offset) % 3u) == 0u;
        uint phase = (frame + offset) % count;
        if (mode == 7u)
            return (rank + ((frame * 4u + offset) % 9u)) % 9u < 4u;
        return rank == phase;
    }
    // Full: preserve the original per-pixel trace path.
    return true;
}

#ifdef __cplusplus
}
#endif
#undef NATIVE_DENSITY_INLINE
#endif
