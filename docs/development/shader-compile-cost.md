# Shader Compile Cost Report

Every PR that touches shaders or the validation configs gets a **Shader compile cost** comment.
It answers one question: will players wait noticeably longer for the shader cache to build?
This page explains how to read the comment and what to do when it flags your PR.

## What the comment shows

| Line                      | Meaning                                                                                                                                                                                                                      |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Variants                  | Permutations in the validation config before and after (added / removed).                                                                                                                                                    |
| Must recompile            | Variants whose preprocessed source changed, so the cache must rebuild them.                                                                                                                                                  |
| Source growth, Net (est.) | Size proxy for the cost change, weighted by recent `dev` compile times. An estimate, not a measurement.                                                                                                                      |
| **VR / SE measured**      | Real compile seconds of the shaders your PR recompiled, against the same shaders in the latest full `dev` build.                                                                                                             |
| Runner-adjusted           | GitHub runners differ in speed run to run. The measured total is divided by the median per-shader ratio between your run and `dev`, so a slow runner does not look like a regression. The raw number is kept for comparison. |
| Flagged beyond ±N%        | The measured change is only flagged when it exceeds the run-to-run noise for that many shaders. Few shaders means a wider band (about 14% at 100 shaders, 8% at 1000).                                                       |
| dev trend                 | `dev` against a run at least two weeks old. This is where many small increases that no single PR trips show up.                                                                                                              |

Treat the **measured** lines as the truth when they disagree with the estimate. The estimate only
sees source size, not what fxc does with it.

## If your PR is flagged

1. Read **Slowest regressions** in the comment. Those are the entries that grew most.
2. Reproduce locally on the one shader (see below) and compare against `dev`.
3. Cut the cost:
    - Remove a define from the permutation set, or fold two defines into one.
    - Move branches that do not change resource bindings behind a runtime constant instead of a compile define.
    - Keep new code out of shared headers that every shader includes. Put it in its own `.hlsli` so only its users recompile.
4. If the cost is the point of the change (a new feature, a quality option), say so in the PR body with the numbers. The comment is informational and does not block merging.

## If the dev trend is flagged

Several merged changes each moved compile time a little. Find the commits that moved it:

```bash
git log --first-parent --format='%h %s' origin/dev --since='14 days ago' -- package/Shaders 'features/*/Shaders'
```

Then compare compile timings across them. The `shader-compile-timing-VR` and `-SE` artifacts of
the dev builds in the release-dev workflow hold per-entry seconds; `tools/shader_cost_report.py`
can diff any two of them (`--timing-*` against `--pr-timing-*`).

## Reproduce locally

```bash
cmake --build ./build/ALL --target prepare_shaders
hlslkit-compile --shader-dir build/ALL/aio/Shaders/Lighting.hlsl --output-dir build/ShaderCache \
  --config .github/configs/shader-validation.yaml --timing-report timing.json
```

Run it on `dev` and on your branch and compare the entries in `timing.json`. Single-run numbers
vary by 5-10% on a quiet machine; run each side three times and take the minimum, or use
[the A/B workflow](./shader-runtime-ab.md) for an in-game view. See
[Shader Workflow](./shader-workflow.md#manual-shader-validation-commands) for more commands.

## Why the thresholds are what they are

They come from the dev timing artifacts of late October to late November 2026 (about 90 full runs per runtime):

- Rerunning the **same commit** moves the runner-adjusted total by up to 10-12% at worst, and by
  8-10% (95th percentile) over a few hundred shaders. A flat 5% limit would flag noise on many PRs.
- The noise falls with the number of shaders compared, so the limit is `6% + 80% / sqrt(shaders)`,
  never below 5%.
- Real regressions in that window moved the full-run total by 10-19% (for example reusing precompiled
  release shaders, specializing the linear ACEScg path, and an upstream sync) and clear this band.
- Real changes of 5-8% hide in the noise for one PR, which is why the dev trend line exists:
  the same noise band applies to a two-week window, but ten small increases add up to far more than it.
