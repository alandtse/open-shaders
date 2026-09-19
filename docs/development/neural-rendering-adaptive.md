# Neural Rendering Adaptive Paths

This document describes the public neural-rendering controls in the Open
Shaders pull request. They are opt-in experimental paths and do not change the
normal DLSS route when disabled or when a runtime contract is unavailable.

## Adaptive model resolution

Adaptive NR uses a hysteretic frame-budget controller. It selects native model
resolution tiers in this order:

`100 -> 95 -> 90 -> 85 -> 80 -> 75 -> 70`

The controller changes one tier at a time, waits through a dwell period, and
pre-warms the native Feature 18 resources used by the adaptive tiers. The
manual 50% and 33% model-resolution presets remain available, but are not
selected automatically by the controller. The budget can follow the supported
70/72/80/90 Hz headset targets or an explicit 15-60 FPS target. SteamVR
compositor workload samples, rather than the engine's paced callback interval,
drive the controller when available.

The controller also records a session memory ceiling after a Streamline VRAM
warning. Sustained headroom or a target-FPS edit cannot immediately climb above
that rejected tier; Neural Rendering Reset or a restart is required to clear
the learned ceiling. Tier residency and adjacent-tier prewarming are bounded
to keep adaptive changes from allocating the whole ladder at once. A transition
invalidates the affected temporal state and uses a short per-eye history
handoff.

## Adaptive centered crop

Adaptive crop is a separate opt-in companion. It runs only after adaptive NR
has reached its configured floor and changes one regular centered crop tier at
a time. Restoration is ordered in the opposite direction: NR returns to its
maximum tier first, then crop coverage expands on a longer headroom window.

The crop companion accepts the regular coverage tiers from 100% through 70%.
It refuses asymmetric or smaller saved regions, so it cannot take ownership of
nasal/asymmetric foveated geometry. Each crop transition invalidates the
crop-sensitive guides and NR history, then uses a guarded full-SBS bridge with
motion, depth, and stereo-boundary checks while the new resources settle.

## Other neural-rendering controls

The same public path contains the existing neural-rendering controls and their
interaction rules:

- reduced model resolution with valid-source-envelope sampling;
- classic bounded resolve and matched-residual resolve;
- optional pre-upscale NR with route eligibility checks;
- sequential 2x and 3x NR modes using separate cascade resources and history;
- adaptive UI status and warning text;
- safe reset/fallback behavior when a shader, resource, or route contract fails.

Streamline VRAM warnings are treated as successful-but-pressure samples so the
current frame remains valid while the adaptive controller reduces future
workload. Other DLSS failures remain a separate fallback/recovery path.

Sequential NR remains a screenshot/benchmark-oriented experiment. It is
disabled for pre-upscale NR and cropped VR regions, where multiplying work or
sharing the wrong history would violate the route's resource and temporal
constraints.

## Public scope

This public change contains Open Shaders feature code, shaders, tests, and
development documentation only. It does not add private model/runtime
payloads, deployment packages, profile automation, or capture integration.
Live Skyrim VR/HMD, stereo, temporal-quality, and VR-budget acceptance remain
separate validation gates after review.
