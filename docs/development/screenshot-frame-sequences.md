# Native frame sequences

`openshaders.screenshot` extends the existing DevBench bridge with bounded,
lossless sequences for temporal image comparisons. It requires the
Screenshot feature to be loaded and a build with `DEVBENCH_BRIDGE=ON`.
It needs no additional recorder or CSX Build ID.

In VR, each output contains the left and right images accepted by OpenVR
in the same successful `WaitGetPoses` cycle and engine frame. Submission
bounds and orientation are applied at native resolution. SE/AE uses the
D3D11 desktop backbuffer at the existing screenshot capture point.
The contract accepts single-sample RGBA8/BGRA8 SDR textures only. HDR,
linear-colour submissions, arrays, resizing, fallback sources and preview
videos are rejected. This is a CSX-style request subset with an OS contract
name, not a claim of complete CSX screenshot-API compatibility.

## Request and completion

Use a new `commandId` for each command or poll. Retry an uncertain command
with exactly the same arguments and IDs to avoid starting another capture.
The process-local command cache retains up to 1,024 commands for one hour;
sequence receipts retain the last eight requests. An expired sequence
receipt produces `receipt_expired` on a retained command retry. New game
processes have new `server.sessionId` values.

Send this to the existing DevBench tool-call transport:

```json
{
    "contractMajor": 1,
    "clientId": "temporal-comparison",
    "commandId": "start-dragonsreach-1",
    "action": "sequence_start",
    "sequence": {
        "frameCount": 30,
        "schedule": {
            "basis": "game_frames",
            "intervalFrames": 1,
            "startDelayFrames": 0,
            "pausePolicy": "hold"
        },
        "backpressure": {
            "policy": "skip",
            "maximumConsecutiveSkips": 30
        },
        "failurePolicy": "stop",
        "capture": {
            "source": { "kind": "hmd_submission", "fallback": "reject" },
            "outputs": [
                {
                    "view": "side_by_side",
                    "encoding": { "format": "bmp", "colourContract": "sdr_srgb" }
                }
            ],
            "destination": {
                "policy": "absolute",
                "directory": "C:/Captures/dragonsreach",
                "overwrite": "never"
            }
        },
        "packaging": { "frameManifest": true },
        "useSettings": false
    }
}
```

For SE/AE, change the source to `desktop_mirror` and view to `source_native`.
PNG is also lossless but adds encoding work. No capture changes user
settings. Existing still capture requests wait while a sequence is active;
sequence start returns `busy` while a still capture is pending or saving.

The successful response contains `result.requestId`. Poll with
`action: "request_get"` and that `requestId`, retaining the usual contract
and client fields. `status` reports the active and retained receipts;
`capabilities` reports supported actions, sources and bounds.

`sequence_stop` with the same `requestId` stops scheduling and drains
already paired frames. `recording`, `draining` and `finalizing` are
nonterminal states. Terminal states are `completed`,
`completed_with_warnings`, `stopped`, `failed_partial` and `failed`.
Feature disable and process shutdown cancel unfinished GPU readbacks and
report the failure while draining already queued encodes. No wall-clock
capture deadline is imposed. Draining GPU copies needs continued renders.
The bridge's existing main-thread dispatch guard applies to command
dispatch, not to sequence duration or file completion.

## Evidence and resource ownership

Each request owns a new `CS_sequence_<requestId>` directory. Images and
the final `manifest.json` are published without replacing existing files.
Receipts include verified file sizes and uppercase SHA-256 digests. The
directory lease is released after finalization. A publication failure
never counts as a written frame; a missing manifest is not success.

The manifest records requested options, producer/build description,
session/request identity, final counts and every scheduled slot. Frame
entries contain scheduled and observed engine frames, compositor cycle,
UTC and monotonic acquisition timestamps, state and failure/drop reason.
Written artifacts also contain native dimensions and each eye's source
format, colour space, bounds, flips, submission flags and timestamp.
Unscheduled slots after an early stop are accounted for by
`requested - scheduled`; they are not manufactured frames.

The scheduler keeps fixed frame spacing and records missed slots rather
than compressing the timeline. Pause/loading holds scheduling. Incomplete
pairs are discarded at the next compositor cycle; duplicate eyes or a
changed engine frame are rejected. A regressing frame counter fails the
sequence. Encoder completion order does not reset the consecutive
acquisition-skip count.

For ordinary streaming sequences, limits are 10,000 scheduled slots, a 216,000-frame scheduled span, four
frames in flight, 128 MiB of native pixels per frame and 256 MiB of native
pixels in flight. The byte limits describe image payloads, not total
process memory: staging resources, CPU copies and encoder buffers add
overhead. Readback maps use `DO_NOT_WAIT` on the render thread. The worker
only combines CPU images, encodes and writes files. Backpressure records
drops without delaying later capture slots; the caller chooses the skip
limit. Device, thread, layout and file failures produce explicit failures.

## Comparison use and validation

Use identical replay, camera/HMD pose, scene conditions, settings and
requested frame cadence on both renderers. Check actual timestamps,
native dimensions, eye ordering, hashes and dropped/failed counts before
analysing shimmer, ghosting or temporal stereo stability. Missing frames
cannot establish continuous temporal behaviour. This path does not
guarantee full-rate recording or measure its own perturbation; establish
that experimentally and keep image capture separate from timed benchmarks.

The focused `[screenshot-sequence]` C++ tests cover schema bounds, rejected
requests, cadence gaps, pairing identity, pixel orientation/channel order,
idempotent commands, expired receipts, directory ownership, never-replace
publication and digest results. Run the existing `cpp_tests` target.
The PNG regression uses the production sequence encoder, publishes 64
varied Unicode paths through the terminated Win32 rename buffer, checks
digests and decodes every result for exact native pixel equality. It also
exercises the existing still-PNG codec flags and file-saving call.

Before runtime qualification, exercise VR and one SE/AE installation via
DevBench: normal completion, stop/drain, pause/resume, both submission
bounds orientations, still/sequence exclusion, feature disable, rejected
source formats, unwritable/full storage, and deliberate backpressure.
Verify each output against its manifest and check the images in both eyes.
In-game validation and sustained cadence are separate from a successful
build or unit-test result.

## Implementation provenance and review

The command identity/envelope and owned-directory publication code are
adapted from CSX `2467ba77cbfc1ed886a062cdea27b4f031b864ba`
(`ServiceFoundation` and `ScreenshotStorageSecurity`). Unused event-journal
and metadata-provider machinery is excluded. Hashing reuses OS's
`Util::FileDigest`; the handle overload keeps publication verification on
the locked file. OS's settings JSON writer intentionally replaces files
and does not provide this ownership/integrity contract.

The pre-build adversarial review covered scope, exception boundaries,
stereo/cadence identity, queue accounting, thread ownership, lifecycle,
schema/parser agreement and existing utilities. Corrections include
exception containment, readback cancellation on failure, acquisition-based
skip counting, still-worker exclusion, receipt expiry, path validation and
releasing directory leases after finalization. No central engine file, shader,
render setting, package dependency or unrelated UI code is changed.

## Consecutive native-region bursts

For temporal quality, opt into `sequence.burst`. Ordinary still PNG and
streamed sequence behavior remain available. The same burst descriptor is
accepted by CSX and Open Shaders:

```json
"burst": {
    "maximumBytes": 536870912,
    "regions": [
        { "x": 564, "y": 712, "width": 384, "height": 256 },
        { "x": 944, "y": 712, "width": 384, "height": 256 },
        { "x": 1128, "y": 1040, "width": 384, "height": 256 }
    ]
}
```

Coordinates above only illustrate the descriptor for a sufficiently large
eye image. Select center, mask-boundary and periphery rectangles from the
full native reference for the actual projection; do not assume those
example positions track the FOV mask on every machine. The coordinates are
integer pixels relative to each oriented submission, applied identically
to both eyes. All regions must have equal widths. Each eye's regions stack
vertically in array order; VR places the left atlas before the right atlas.
There is no scaling, filtering or padding. The example yields 768 x 768
pixels containing six 384 x 256 tiles. Bounds, orientation, source
dimensions and region definitions remain in the manifest.

Use explicit `frameCount` (1–240), `useSettings: false`, `game_frames`,
`intervalFrames: 1`, one `side_by_side` output in VR (`source_native` on
SE/AE), SDR PNG or BMP, and `fallback: reject`. Keep the frame manifest
enabled. Omit `failurePolicy` for a shared request: the burst always stops
acquisition on failure regardless of the ordinary sequence default.
Additional output cropping, resizing and clipboard export are unavailable
in this mode. Bursts require exclusive ownership through finalization.

Only cropped GPU copies are collected during acquisition. Readback and
PNG/BMP encoding start after acquisition ends; saved images use the same
existing encoders and verified publication paths as ordinary captures.
Admission checks the entire raw payload before allocating any textures.
Its ceiling is 512 MiB, with 128 MiB per frame and at most eight regions;
native texture and region bounds are also checked at acquisition. These
are resource bounds, not wall-clock deadlines. Driver allocation padding,
metadata and bounded readback/encoder scratch space are additional memory.
Acquisition does not wait for the disk or encoder to make room.

The receipt and final manifest expose `continuity`: observed first/last
engine frames, acquired/requested counts, `failure`, and `complete`.
Every acquired frame must advance the engine counter and, in VR, the
compositor cycle by exactly one. A missing or incompatible eye, source
change, pause after capture begins, readback failure or failed publication
prevents qualification. Early stop retains acquired images but cannot
qualify an incomplete requested burst. Require final manifest publication,
all requested images and `continuity.complete == true` together; image
count or a successful start response alone is insufficient. Memory owned
by each frame is released during drain; receipts retain metadata only.

For a compact comparison, preserve one full stereo reference at each
existing viewpoint, then initially capture two seconds: 0.5 seconds of
stationary detail, a one-second repeatable pan, and 0.5 seconds of stationary
recovery. At a measured 60 rendered FPS this is 30 + 60 + 30 = 120 frames.
Three 384 x 256 regions per eye consume 283,115,520 raw bytes per viewpoint
(270 MiB, about 1.70 GB over six viewpoints), before lossless compression.
This is an initial evidence budget, not a claim that all temporal artifacts
settle within two seconds. Extend only the affected phase if the saved
samples show an unsettled trail, insufficient motion or an unresolved
fluctuation; do not routinely repeat the entire viewpoint or capture at
the 240-frame ceiling. Stabilize camera and history before acquisition.

Both builds require an explicit burst frame count. Choose it from actual
rendered cadence, not the nominal HMD refresh rate, and match motion speed
and phase durations between builds. The selected frame count ends source
acquisition; it is not an encoding, publication or command timeout. Keep
requested raw bytes within the advertised bound; reduce region dimensions
if the chosen cadence would exceed it. Retain acquisition timestamps and
correlated DevBench camera actions for phase boundaries. Match the original
settings, fixed HMD pose, viewpoint, game time and weather.

Validate the first short burst's continuity, stereo orientation, crop
locations and publication before proceeding to the other viewpoints. Reuse
that valid burst. Do not repeatedly capture long full-frame sequences or
include image collection in a performance benchmark. Consecutive counters
do not prove negligible capture overhead or uniform wall-clock cadence;
inspect timestamps before comparing shimmer, ghosting and stereo history.
Runtime qualification on VR and one flat runtime remains necessary.

## Screenshot menu and DevBench goldens

The Screenshot feature's **Frame sequence** section offers frame count,
frame interval, optional start delay, Start, Stop and completion counts.
It defaults to 30 frames and uses the same sequence controller as DevBench,
with the existing folder
and SDR format choice. Start delay defaults to zero; it is a requested
frame offset, not a timeout. Normal still-image crop and HDR controls are
unchanged. Native ROI bursts remain an explicit automation descriptor.

DevBench already owns golden-sample comparison and checkpoint routing.
The bridge registers Screenshot as `RegisterToolExtension("capture",
"openshaders", ...)`; it does not introduce another golden registry or
SSIM implementation. Discover it with `capture {"kind":"providers"}`
or `capture {"kind":"extensions"}`, then select `kind: "openshaders"`.
The provider uses one frame from the same native sequence acquisition and
PNG publisher, honors the host's absolute `outputPath` and correlation
`requestId`, and emits `capture.ready` after verified completion. Existing
paths are refused, so use a fresh checkpoint/variant output location.

The base `capture` tool accepts `golden`, `threshold` and optional normalized
`regions`; `record replay` accepts per-checkpoint `goldens`. Previously
captured matching native images can be supplied as those reference paths.
DevBench supplies checkpoint sidecars and scoring; the provider retains its
own one-frame manifest. A golden is an explicit reference, not proof that
one renderer has better visual quality. Do not replace temporal-continuity
analysis with still-image SSIM, and do not score cropped atlases against
full-frame references. Preserve native dimensions, source kind, scene and
1x time scale. The provider reports `uiExcluded: false`: it does not claim
to remove HUD/menu pixels, and DevBench keeps that limitation visible.

The DRY review checked `Utils/StringUtils`, `Utils/Format`, serialization,
file helpers and the full source tree. The UTF-8 path conversion is shared
by capture and menu code in `Util::PathToUtf8`; `WStringToString` only narrows
characters and is unsuitable for Unicode file paths. No matching typed
JSON string fallback or process/session ID utility exists in those helpers.
The local JSON fallback intentionally handles wrong-typed error-envelope
fields without throwing. Existing file digests, native pixel conversion,
PNG/BMP codecs, directory leases and atomic publication are reused.

## Validation (2026-09-30)

The universal SE/AE/VR Dev-Fast DLL built with `TRACY_SUPPORT=ON` and
`DEVBENCH_BRIDGE=ON`, directly above dev
`d6bfd6b7b552e70d23eec00fce4e7f14cbece0af`. No deployment or packaging ran.
Commands below use the parent workspace's existing CMake launcher:

```powershell
pwsh ./tools/cmake.ps1 --build build/os-stereo-sequences --target cpp_tests CommunityShaders --parallel 8
./build/os-stereo-sequences/tests/cpp/cpp_tests.exe --reporter compact
```

All 235 C++ cases passed with 4,179 assertions under normal Windows file
access. The PNG regression publishes 64 varied Unicode filenames through
the production sequence encoder and checks decoded native pixels and hashes.
It also exercises the existing still-PNG saving call and a host reference
filename, including refusal to overwrite that reference. Burst tests cover
atlas orientation/pixel identity, bounds, memory budgets and continuity.
Reference tests cover request identity, native VR/flat output, Unicode and
invalid paths; directory-lease tests cover both owned sequence children and
host destination children. Scoped hooks and diff checks passed.

The review checked scope, stereo source ownership, deferred readback,
allocation bounds, cancellation, atomic publication and shared utilities.
The menu and reference provider use the same sequence implementation;
menu sequences use the existing game-relative screenshot-folder resolver.
Existing SDR/HDR still-image encoders remain unchanged. No runtime capture,
end-to-end DevBench golden score, sustained cadence or overhead measurement
was made. VR and one flat runtime remain required in-game qualification;
this implementation does not establish temporal comparison results yet.

The follow-up adversarial pass removed the implicit burst-length default:
`frameCount` is required for bursts in the parser and discovery schema.
Normal menu/API sequences default to 30 frames; the menu derives its value
from the same plan default. Comparison guidance starts with a two-second
ROI burst after history stabilization and extends only an unresolved phase.
No acquisition duration is reused as an encoding or publication deadline.

### Retrying rejected commands

A failed dispatch with `error.retryable: true`, including temporary capture
`busy` responses, is not retained in the completed command cache. Retry
with the same arguments, `clientId` and `commandId` after the temporary
condition clears. Successful commands and non-retryable failures remain
cached; retries of accepted captures return their receipt without starting
another capture. Concurrent identical commands remain `command_in_progress`.
