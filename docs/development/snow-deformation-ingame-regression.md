# Snow Deformation in-game regression test

`tools/snow-deformation-ingame-regression.py` drives a running game through devbench and records
what the snow features do at a fixed spot. It exists because the important failures here are
visual (one-eye veil, tint, missing prints) and one-off manual runs do not catch regressions.
An agent or a person runs it, then looks at the listed screenshots.

## What it covers

| Scenario         | Checks (FAIL)                                                                                           | Looks at (REVIEW)                                                           |
| ---------------- | ------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| `blanket`        | blanket active, depth and lighting draws, landscape layout accepted, no layout rejects                  | raised snow drift versus flat, more than 1% of pixels changed               |
| `sink`           | player sinks on landscape (`SunkActors` >= 1, `DeepestSink` > 0), sinking stops when the setting is off | trail of prints behind the player                                           |
| `accumulation`   | Snow Cover on with gating active, at least 6 game hours advance under a snowstorm                       | frame not darker after the storm; same pose, nothing buried or tinted oddly |
| `eyes` (VR only) | none                                                                                                    | left and right eye match (no veil, no tint, no unclipped geometry)          |

FAIL exits non-zero. REVIEW means a screenshot metric is outside its expected range, so open the
images named in `report.md` and judge them; do not call the run green until each REVIEW is explained.

## Setup

1. Use the game exclusively: nobody else may be running or testing the same Skyrim install (SE and
   VR are separate installs), and no timing or perf test may be in progress. If several people or
   agents share the machine, agree on who holds it before launching and hand it back when done.
2. Deploy the DLL under test and hash-check it against the build output.
3. Back up the game's `CommunityShaders/SettingsUser.json`, then set `Disable at Boot.SnowDeformation`
   to `false` and make sure `SnowCover` is not disabled at boot (Snow Deformation is disabled at boot by
   default). Restore the file afterwards.
4. Launch with the SKSE loader, call `openshaders.shadercache {"action":"backgroundCompile"}`, then
   `game {"action":"loadLast"}` and wait for `inspect state.playerLoaded`.
5. VR: the null driver, SteamVR and the helper overlay need the usual VR launch procedure.

## Run

```
python tools/snow-deformation-ingame-regression.py --out <dir> [--port 8920|8921] [--save <name>] [--only blanket sink accumulation eyes]
```

The port is probed (SE 8920, VR 8921) and `inspect state.vr` picks the runtime. The script moves the
player to the Winterhold test spot (`coc WinterholdExterior01`, then `player.setpos` 110183, 103754,
-8890), clears tutorial pop-ups, and runs each scenario with fixed freecam poses. It unequips the
helmet, turns Survival off and sets health high before moving. `--save <name>` loads that save first;
use `--save GuardianStonesWater`, because the default `ce_letters` save dies of cold during the
accumulation time jumps and reloads to Whiterun. Output in `<dir>`:
`results.json`, `report.md` and the screenshots.

## Agent follow-up checklist

1. Read `report.md`. Any FAIL is a regression until proven a setup problem (feature not loaded, other
   agent changed the scene, game crashed: check `OpenShaders.log` and the newest CrashLogger file).
2. Open every image under "Screenshots to review" and compare against its expectation line.
3. For VR also compare `blanket_on.png` left and right halves by eye.
4. Quit the game with `qqq`, restore `SettingsUser.json` and the previous DLL, hand the game back.
5. Say in the PR which runtimes ran, and attach or describe the report.

## Known limits

- The spot is Winterhold town: the player starts on boardwalk (static collision), where sinking is
  blocked on purpose. The `sink` scenario walks onto landscape first.
- Accumulation needs several game hours; the script calls `game advanceTime` in 3 hour steps with
  the weather forced to snowstorm (`fw c8221`) at hour 12, and leaves the weather that way. The
  Winterhold ground is already white, so the luma check only flags a darker frame; judge buildup by eye.
- Keyboard input does not move the VR player, so on VR the `sink` scenario teleports onto landscape instead of walking.
- The tutorial message box pauses the game; the script accepts it before shots and after walks.
- `freecam` is not a toggle: the script turns it off with `{"action":"freecam","on":false}` before walking.
- The pixel metrics are coarse guards, not image comparisons. Prints and veils are judged by eye.
