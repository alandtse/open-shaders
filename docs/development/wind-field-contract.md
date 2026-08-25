# Ambient Wind-Field Contract

## Scope

The shared wind field combines Skyrim's weather wind, CPU-authored ambient
gust bands, procedural breakup noise, and separate transient impulses. Tree,
grass, and leaf response remain consumers of this field rather than part of
its generation contract.

The canonical CPU sampling implementation is in `src/Utils/WindField.h` and
`src/Utils/WindField.cpp`. The equivalent GPU implementation is
`package/Shaders/Common/WindField.hlsli`.

## Sampling API

The canonical point query receives the base direction and speed plus the same
fixed gust pool that is uploaded to the GPU:

```cpp
WindField::WindSample SampleWind(
    float3 worldPosition,
    float3 windDirection,
    float windSpeed,
    const WindTuning& tuning,
    const std::array<AmbientGust, 8>& gusts,
    uint32_t activeGustCount);
```

`WindSample::velocity` is local ambient air velocity and
`WindSample::ambientGust` is normalized pressure in `[0, 1]`. Sampling is pure:
identical explicit inputs produce identical CPU and GPU results.

## Gust lifecycle

`State` owns a fixed eight-slot pool. It advances each active entry once per
frame using the current selected wind direction and the gust's stored speed,
increments age, and recycles expired slots without allocation:

```text
position += direction * speed * deltaTime
age += deltaTime
```

A varied spawn timer creates new bands at the configured upwind distance from
the player. Length, width, speed, strength, lifetime, lateral offset, and seed
are randomized within the configured ranges. Active and newly spawned gusts
use the current selected wind direction without random directional drift.

Current and previous pool snapshots are uploaded for temporal vegetation
sampling. The CPU and GPU never spawn independent gusts.

## Grass spring response

Grass resolves the sampled field into a persistent 128 by 128 bend-response
field covering 32768 world units around the player. Each cell stores bend and
compression together with their velocities. The update uses the closed-form
solution of a damped second-order oscillator, with natural frequency and
damping ratio exposed as the two artist controls.

The current and prior resolved response textures are sampled independently by
the grass vertex shader. Motion vectors therefore use the actual spring state
from each frame instead of subtracting two raw wind targets or reusing one
spring offset for both frames. A missing compute shader falls back to direct
current/previous wind targets so grass remains renderable.

## Band envelope and noise breakup

Each gust is a soft ellipse in its own world-space direction basis. Length is
the along-wind radius and width is the crosswind radius. The default ranges
keep length approximately five times width.

`SampleAmbientGustBands` sums strength-weighted envelopes and saturates the
result so overlaps remain bounded. The existing broad/detail gradient-noise
stack is then evaluated once in unrotated world XY and multiplied into the
summed macro envelope. Per-gust seeds offset that breakup domain, giving each
band distinct internal streaks and gaps without sampling noise inside the
eight-entry loop.

No world coordinate is rotated by the current weather direction, and there is
no accumulated origin-relative travel coordinate.

## Speed and strength

Gust speed affects only CPU translation. Gust strength affects only the band
pressure entering the existing amplitude response. Outside all bands,
`ambientGust` is `0.5`, which preserves the base weather velocity:

```text
gustDeviation = ambientGust * 2 - 1
localSpeed = windSpeed * max(1 + gustDeviation * gustAmplitude, 0)
```

With the default amplitude of `0.35`, a fully active band reaches `1.35x` base
velocity without imposing the old continuous `0.65x` procedural lull.

## Transient impulses

Fus Ro Dah and other event impulses remain a separate additive layer.
`State::SampleAmbientWind` and the matching shared-data HLSL helpers call the
ambient sampler first and then add transient impulse velocity exactly as
before. Ambient gust tuning never changes impulse lifecycle or configuration.

## Verification contract

The Wind Field utility tab reports the active band count, average band speed,
base weather input, and CPU sample at the camera. The GPU debug view removes
the neutral `0.5` baseline and displays normalized positive pressure, so calm
air occupies the low end of the heatmap and burst strength uses its full range.

Required invariants are:

1. A band's displacement depends on elapsed time and its speed, not distance
   from the world origin.
2. Increasing speed does not increase pressure or vegetation response.
3. Increasing strength does not change band translation.
4. A weather-direction change never rotates existing world-space bands.
5. CPU and GPU sampling use equivalent envelopes, breakup noise, and pool
   snapshots.
6. Transient impulses remain additive and independent.
7. Grass motion vectors use separate current and previous resolved spring
   states.
