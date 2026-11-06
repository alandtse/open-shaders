#ifndef NATIVE_TRACE_SAMPLING
#define NATIVE_TRACE_SAMPLING
#ifdef __cplusplus
#include <cstdint>
namespace DX12Native
{
using uint = uint32_t;
#define NATIVE_TRACE_CONSTEXPR constexpr
#else
#define NATIVE_TRACE_CONSTEXPR
#endif
NATIVE_TRACE_CONSTEXPR uint NativeTraceHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
NATIVE_TRACE_CONSTEXPR uint NativeGIPhase(uint frame, bool temporalSampling)
{
    return temporalSampling ? frame : 0u;
}
NATIVE_TRACE_CONSTEXPR bool NativeTraceSample(uint x, uint y, uint mode, bool stochastic, uint frame, uint salt)
{
    if (mode == 0u) return true;
    const uint span = mode == 1u ? 2u : mode == 2u ? 4u : 3u;
    if (!stochastic) {
        if (mode == 3u) return x % 3u != 1u && y % 3u != 1u;
        return x % span == 0u && y % span == 0u;
    }
    const uint phase = (frame % (span * span) + NativeTraceHash((x / span) * 0x9e3779b9u ^ (y / span) * 0x85ebca6bu ^ salt) % (span * span)) % (span * span);
    const uint firstX = phase % span, firstY = phase / span;
    if (mode == 3u)
        return (x % span == firstX || x % span == (firstX + 2u) % 3u) &&
            (y % span == firstY || y % span == (firstY + 2u) % 3u);
    return x % span == firstX && y % span == firstY;
}
#ifdef __cplusplus
}
#endif
#undef NATIVE_TRACE_CONSTEXPR
#endif
