# Dynamic Wind Integration Notes

## Status

This is a deferred compatibility design. It is not implemented. The tree-wind
work remains responsible for its own weather-driven gust controller while we
wait for feedback from the Dynamic Wind Framework developer.

Stable tree-wind baseline: `8eff40850` (`feat(trees): add tunable wind
response`).

## Goal

Use the Open Shaders wind configuration as the global wind-strength source,
then let Dynamic Wind Framework distribute that result to rain, physics,
animations, grass, and its other consumers. Wind direction can continue to
come from the shared Skyrim/Dynamic Wind state.

All consumers should experience the same gust event, while retaining their own
response sensitivity.

## Important Distinction

The gust controller is not just a stored multiplier. Open Shaders owns state
that includes:

-   Random gust targets.
-   Gust hold timers.
-   Rapid transitions between targets.
-   Continuous low-amplitude random variation during a gust.
-   Current and previous-frame values for tree motion vectors.
-   Live editor configuration for all of the above.

Despite that internal state, the controller produces one evaluated global wind
strength for the current frame:

```text
weather baseline
    * transitioned gust strength
    * continuous gust variation
    = current global wind strength
```

Dynamic Wind does not need to reproduce or store the timers. It only needs the
evaluated current-frame output. Open Shaders must remain the sole owner of the
gust simulation so that a second gust or interpolation function is not applied.

## Consumer-Specific Response

The shared global signal must not include per-tree or per-material response:

```text
current global wind strength
|-- Dynamic Wind consumers use their configured sensitivity
|-- grass uses its configured sensitivity
`-- tree macro bend
    * trunk wind sensitivity
    * deterministic per-tree response
    `-- leaf flutter also uses leaf wind sensitivity
```

This allows a weather strength of `0.5` to drive every system consistently
while, for example, a tree can react as if the wind were stronger without
forcing physics objects to use that same tree-specific amplification.

## Proposed Integration

Add a small, versioned inter-plugin interface rather than writing into Dynamic
Wind's private memory or relying on DLL offsets.

1. Open Shaders evaluates its gust controller every frame.
2. Open Shaders publishes the current global strength through the interface.
3. Dynamic Wind samples that value in its manager update.
4. When an external value is available, Dynamic Wind uses it as
   `finalStrength` and skips its own strength interpolation and procedural gust.
5. Dynamic Wind passes the value to its existing central distribution call:
   `WindFramework::Update(finalStrength, finalAngle, deltaTime)`.
6. If the interface or provider is unavailable, Dynamic Wind retains its
   original behavior.

Dynamic Wind should continue owning distribution. Open Shaders should not try
to update every external consumer independently.

The interface could expose a compact state such as:

```cpp
struct ExternalWindState
{
    float strength;
    bool active;
};
```

The production interface should be versioned and validate that strength is
finite and within an agreed range. A pull-style provider called during Dynamic
Wind's update would avoid ambiguous plugin update ordering. A push-style API is
also possible if the call order is explicitly guaranteed.

## Why `SetTargets` Is Not Sufficient

Dynamic Wind has an internal `SetTargets(angle, speed)` method, but its normal
update subsequently interpolates toward the target and adds its own procedural
gust. Sending our evaluated value through that path would double-process it.
The method is also not currently exposed as a stable cross-plugin API.

Merely overwriting `RE::Sky::windSpeed` is also insufficient. Dynamic Wind
passes its locally calculated `finalStrength` directly to its handlers, so some
consumers could receive a different value even if the Sky value were changed
later in the frame.

## Open Questions for the Dynamic Wind Developer

-   Is there an existing supported cross-plugin interface that is not visible in
    the public source?
-   Would they accept a versioned external-strength provider or override?
-   At what point in the manager update should an external value be sampled?
-   Should the external signal replace only strength, leaving Dynamic Wind's
    direction and angle turbulence intact?
-   What strength range do all handlers expect: strictly `[0, 1]`, or can they
    safely accept amplified values?
-   Should Dynamic Wind expose both weather baseline and final wind, or only
    accept the final evaluated strength?
-   What behavior is expected during interiors, menus, paused time, weather
    transitions, and save loading?
-   What redistribution or compatibility-patch permissions apply to a custom
    Dynamic Wind DLL?

## Validation Plan

When this work resumes:

1. Log the Open Shaders global output and the value consumed by Dynamic Wind.
2. Confirm they match during gust holds, transitions, and continuous variation.
3. Confirm rain, grass, Dynamic Wind objects, trunks, and leaves enter the same
   gust at the same time.
4. Confirm consumer-specific sensitivities still differ as configured.
5. Confirm disabling either mod falls back cleanly without stale wind state.
6. Test weather transitions, interiors/exteriors, pause/unpause, loading saves,
   and low/high frame rates.
7. Verify tree motion vectors still use Open Shaders' actual previous-frame
   controller state.

## Avoid

-   Hard-coded offsets into `DynamicWind.dll`.
-   Pattern scanning private manager fields.
-   Competing per-frame writes to `RE::Sky::windSpeed` without an ownership
    contract.
-   Running both mods' gust generators on the same strength.
-   Applying tree sensitivity or per-tree variation to the shared global signal.
