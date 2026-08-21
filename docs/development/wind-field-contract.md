# Ambient Wind-Field Contract

## Scope

The wind field is a deterministic, stateless mathematical sampler. It is
currently an ambient weather field only; vegetation deformation and local 3D
wind sources are not part of this contract.

The canonical CPU implementation is in `src/Utils/WindField.h` and
`src/Utils/WindField.cpp`. The equivalent GPU implementation is
`package/Shaders/Common/WindField.hlsli`.

## Sampling API

The canonical point query is:

```cpp
WindField::WindSample SampleWind(
    float3 worldPosition,
    float gustTravelDistance,
    float3 windDirection,
    float windSpeed,
    const WindTuning& tuning);
```

`WindSample` contains:

-   `velocity`: the actual local 3D ambient air velocity in engine wind units.
-   `ambientGust`: normalized gust pressure in `[0, 1]`.

The sampler has no frame history, persistent state, texture dependency, GPU
readback, or simulation step. Identical explicit inputs produce identical
outputs on CPU and GPU.

`SampleAmbientGust` is the scalar gust query. `SampleAmbientWind` returns the
velocity portion of the same canonical sample.

## Gust field

The gust field uses coherent XY world-space gradient noise. It projects the
world position into along-wind and cross-wind coordinates, combines a broad
front with a turbulent detail layer, then applies the configured contrast.

The normalized pressure is converted into a centered fractional deviation from
the mean wind speed:

```text
gustDeviation = ambientGust * 2 - 1
localSpeed = windSpeed * max(1 + gustDeviation * gustAmplitude, 0)
```

`windSpeed` therefore remains the mean air velocity. `gustAmplitude` changes
only the local deviation around that mean; the default `0.35` produces the
same `0.65x` to `1.35x` range as the original formulation.

The along-wind coordinate is advected by the accumulated travel distance:

```text
alongCoordinate = (alongWind - gustTravelDistance) / gustScale
```

The direction, seeds, PCG constants, scales, and interpolation math are kept
equivalent between `WindField.cpp` and `WindField.hlsli`. `WindTuning` is the
authoritative set of shared constants and seeds.

## Travel phase ownership

`SampleWind` remains pure. The frame-level owner is `State`, which calculates
an explicit world-space advection speed and integrates a single non-negative
travel distance once per frame:

```cpp
gustAdvectionSpeed =
    selectedWindSpeed * gustAdvectionBaseSpeed * gustAdvectionMultiplier;
gustTravelDistance += max(gustAdvectionSpeed, 0.0f) * deltaTime;
```

`gustAdvectionBaseSpeed` converts Skyrim's normalized wind magnitude into world
units per second. `gustAdvectionMultiplier` is an independent artistic control
for transport speed. Neither value changes the local air velocity returned by
the sampler. A decrease in speed can reduce the next frame's advance, but
cannot move the field backward because the accumulated distance never
decreases.

The selected speed and direction are formed outside the sampler:

-   Real speed/direction use the current ambient weather input.
-   The debug override speed slider supplies the speed when real speed is off.
-   The debug direction toggle selects either the weather direction or world
    `+X`.

The raw engine velocity remains available for diagnostics. The selected
ambient velocity and accumulated travel distance are published to the GPU in
the shared-data constant buffer.

## CPU/GPU path

On the CPU, `State::SampleAmbientWind(worldPosition)` queries the selected
ambient source through the canonical C++ sampler.

On the GPU, `SharedData::SampleAmbientWind(worldPosition)` reads the selected
ambient velocity, shared tuning, and accumulated travel distance, then calls
the HLSL counterpart.

The Wind Field debug view in `DeferredCompositeCS.hlsl` displays only
`ambientGust` using the Turbo colormap. It does not alter or encode the sample
to make velocity visible. The Wind Field utility tab separately reports raw
inputs, selected inputs, travel delta, accumulated distance, and CPU samples.

## Verification contract

The current verification surface is the Wind Field utility tab and GPU debug
view:

1. Override speed `0` must produce zero travel delta and a stationary field.
2. Higher advection multiplier must advance gust fronts faster without changing
   sampled local velocity at a fixed gust value.
3. Accumulated travel distance must be monotonically increasing.
4. Disabling real direction must advect fronts along world `+X`.
5. CPU samples and GPU samples at identical inputs must agree within the
   configured parity tolerance.

The existing CPU parity samples in `tests/cpp/test_windfield.cpp` and
`package/Shaders/Tests/WindFieldParitySamples.hlsli` provide fixed conformance
inputs for the shared algorithm.

## Future extension

Future local sources contribute XYZ velocity after the ambient sample:

```text
finalWind = ambientWeatherWind + localWindSources
```

The ambient sampler and its `float3` velocity result must remain independent
of vegetation-specific response logic so dragon downwash, explosions, wakes,
and similar sources can be added without changing this contract.
