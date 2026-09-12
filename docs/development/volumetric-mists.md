# Nearby volumetric mists

An optional artistic addition to EHF's weather-matched fog. Enable it in
**Exponential Height Fog > Mists > Enable Nearby Mists**. Both EHF and
Volumetric Fog must also be enabled. It is disabled by default.

## Scope

The pattern is world-positioned, with horizontal drift. Only the nearby
part is evaluated: the spherical support follows the camera (the midpoint
of the eyes in VR) and fades over the outer half of its radius. Support
ends at the smaller of Mist Range and 90% of Volumetric View Distance.
There is no additional grid, texture, lighting pass, or analytical mist
tail. Distant geometry can still be obscured by mist between it and the
camera; the distant atmosphere itself is not filled with new mist.

Existing sky exclusions and mountain-mist material handling are unchanged.
In automatic mode, sky-only pixels do not receive this mist either. This
first iteration therefore needs particular attention at tree/sky edges
when testing; it does not claim complete sky/reflection participation.

## Density and lighting

In Follow vanilla fog mode, optical depth at a fixed 8192-game-unit
reference distance supplies mist strength. It includes Weather Fog
Strength, Volumetric Extinction Scale, and the EHF height weight. Weather
whose fog starts beyond that reference distance produces no nearby
mists. Manual mode uses the existing height-fog extinction multiplied by
the same reference distance. This is an artistic weather calibration,
not a physical conversion of distant haze into local mist.

Reference optical depth is clamped to [0, 0.75], multiplied by 3 and Mist
Strength, then divided by Mist Pocket Size to obtain peak extinction.
Thus the optical thickness is calibrated across a pocket, independently
of rendering range. Noise, height, and resolution filtering still affect
the actual thickness along a ray; Pocket Size is a noise scale, not a
guaranteed cloud diameter. At Strength 1 the upper bound for a constant
peak-density segment one pocket-size long is optical depth 2.25 (about
89% opacity). This is not the average pocket opacity.

Positive noise pockets add extinction and scattering together before
the existing lighting and integration passes. Gaps retain the original
weather fog; they do not subtract it. Range only changes support and its
outer fade, though seeing through more pockets can increase total
opacity. This intentionally differs from exact vanilla matching.

The material uses quintic-interpolated 3D value noise and a warped detail
sample, using the shared PCG hash. Pockets are stretched 2.5 times along
the drift direction and compressed to half height. Their orientation
follows drift with a ten-second response, anchored near the player while
turning to avoid rotation around the world origin. Broad and detail noise
evolve in opposing directions at a slow fixed rate; speed zero and pauses
freeze this evolution too. The detail frequency is three times the broad
frequency, with stronger warping for uneven edges. This still uses two
noise evaluations, not an additional octave or rendering pass.
Noise is not sampled in screen space
or held constant along a view ray. The cell's world-space footprint
accounts for the stretched coordinates and attenuates unresolved detail,
then the entire mist contribution, instead
of leaving high-frequency stripes or a uniform added veil.

Spatially varying optical properties are a standard volume-rendering
technique; see [NVIDIA's Volume Rendering Techniques](https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-39-volume-rendering-techniques).
This implementation is procedural art direction, not a fluid simulation.

## Starting controls

-   **Mist Strength: 0.1.** Scales only the added pockets; zero restores the
    baseline. It cannot create mist if the weather reference is zero.
-   **Mist Range: 8192 game units.** With the existing 8030-unit volumetric
    distance, support ends at 7227. Increasing this control alone cannot
    enlarge the volumetric grid.
-   **Mist Pocket Size: 4096 game units.** Controls the broad noise scale.
    Density scales inversely with size to retain pocket optical thickness.
    Very small pockets fade sooner as depth cells become too large.
-   **Follow Outdoor Wind: on.** Outdoor drift follows the live sky wind
    angle and normalized strength. Velocity transitions have a one-second
    response time. Interiors and disabling this option use manual drift.
    This does not change density or simulate shelter behind geometry.
-   **Mist Wind Multiplier: 1.0.** Scales outdoor wind-driven movement only.
    Final speed is capped at 2000 game units/second.
-   **Mist Movement Speed: 1000 game units/second.** Manual speed, or speed
    at full wind strength before the wind multiplier. Adjustable from zero
    to 2000. Zero freezes the field in world space, not to the camera.
    Movement accumulates from a monotonic per-frame clock and is uploaded
    directly to the fog compute buffer. Changing speed does not reposition
    the mist. Pauses, missing render frames, and stalls over 0.25 seconds
    do not accumulate catch-up movement. The UI shows the movement offset
    supplied to the shader; this readout does not verify GPU output.

Turning this off skips noise evaluation. Turning it on adds material
evaluation work within its support, but no extra dispatch or allocation.
No GPU cost or visual quality claim has been measured yet.
