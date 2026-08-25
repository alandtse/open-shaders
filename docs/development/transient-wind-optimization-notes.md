# Transient Wind Optimization Notes

## Status

The shared transient-wind source pool has a bounded capacity of 24 entries.
Current and previous snapshots are compact and carry active counts, so consumers
scan only live sources. This note records the next optimization if live-source
scans become too expensive.

## Current cost model

`State` owns compact current and previous transient impulse snapshots. The CPU
lifecycle update scans the active range once per frame. The shared HLSL sampler
scans the active range for each world-position query, and temporal vegetation
consumers may perform that query for both snapshots.

The current design keeps CPU/GPU sampling semantically aligned. With no live
impulses, the counted shader loops execute zero iterations.

## Deferred optimization: spatial source lookup

Keep `SampleAmbientWind(worldPosition)` as the public abstraction. Change only
the source lookup behind it:

1. Give every transient source a conservative finite influence bound.
2. Build a world-space spatial hash/grid, quadtree, or equivalent index for
   active sources.
3. Query the index at the listener position and evaluate only nearby source
   indices.
4. Preserve the same source falloff, additive velocity composition, and
   current/previous snapshots for both CPU and GPU consumers.

Bounds are only a broad-phase optimization. A source's bound must include its
wavefront, trailing wake, cone, and decay width for the entire time it remains
active, or valid contributions will be culled.

## Deferred optimization: rasterized shared field

If source counts become large enough that per-listener indexing is still too
expensive, accumulate all transient sources into the shared world-space wind
field once per frame. Consumers then sample the field at constant cost. The
field must retain current and previous snapshots needed by temporal vegetation
motion, and CPU consumers should sample the same representation or an equivalent
CPU field.

### Staged 3D-field implementation

The first practical version should be a camera- or player-centered 3D volume,
not a world-sized texture. A compute pass can evaluate the active transient
sources into a modest volume (for example, 64^3 or 96^3), storing velocity and
intensity in a packed format. Grass and other GPU consumers then replace the
per-sample source loop with one volume lookup. Current and previous volumes are
needed for temporal vegetation motion.

The existing `Texture3D` wrapper and compute-pass resource plumbing are enough
to support this, but the work is larger than changing the source capacity. It
requires field origin/voxel-scale metadata, UAV/SRV lifetime and binding,
history management, and a CPU-side field or spatial representation for
non-GPU listeners. Ambient procedural wind can remain analytic initially and
be folded into the volume later.

The field bounds must follow the active simulation region. Sources outside the
volume need an explicit policy (for example, expiration, clipmap coverage, or
fallback analytic sampling) so a moving wave does not disappear at the volume
edge. Sparse bricks or clipmap levels can be evaluated later if one volume is
not large enough for the desired world coverage.

## Composition and ownership

Transient sources remain part of the abstract wind signal so grass, trees,
particles, physics, and future consumers observe the same event. Source lists
from different producers may be merged before indexing or rasterization;
producer-specific consumers should not silently maintain divergent wind
semantics.

The existing additive velocity composition should remain the default. A
bounded-composition policy may be considered separately if overlapping events
produce excessive motion, but it is a stability control, not a source-lookup
optimization.

## Performance checks when this resumes

-   Measure CPU sampling time for representative listener counts.
-   Measure GPU wind-sampling pass time with 0, 1, 24, and dense overlapping
    sources.
-   Compare spatial-index and rasterized-field results against the current
    linear sampler at identical positions and historical snapshots.
-   Verify source expiration, moving wavefronts, cone falloff, and temporal
    motion vectors across cell boundaries and scene transitions.
