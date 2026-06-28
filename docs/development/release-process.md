# Release Process and Branch Model

This document describes the branching model, semantic release workflow, patch flows, manual packaging targets, and release stages (Alpha / Beta) used in Open Shaders.

## Conventional Commit Version Impact

PRs are squash-merged, so the **PR title** becomes the commit message that semantic-release reads. The type of commit determines the version bump for the next release.

### Type → release impact table

| Type       | Use for                                                   | Release impact          |
| ---------- | --------------------------------------------------------- | ----------------------- |
| `feat`     | New user-facing feature or capability                     | **minor** (1.X.0)       |
| `fix`      | Bug fix to user-facing behavior                           | **patch** (1.5.X)       |
| `perf`     | Performance improvement to user-facing behavior           | **patch** (1.5.X)       |
| `revert`   | Revert of a prior commit                                  | follows reverted commit |
| `build`    | Build system, packaging, dependencies (CMake, vcpkg, AIO) | none                    |
| `chore`    | Maintenance, misc tooling, repo hygiene                   | none                    |
| `ci`       | CI workflows, GitHub Actions, lint configs                | none                    |
| `docs`     | Documentation, comments, READMEs, CLAUDE.md               | none                    |
| `refactor` | Code restructuring with no behavior change                | none                    |
| `style`    | Formatting, whitespace, missing semicolons                | none                    |
| `test`     | Tests, test fixtures, test infrastructure                 | none                    |

-   **Breaking Changes:** Append `!` to the type (or add a `BREAKING CHANGE:` footer) for **major** release bumps (X.0.0).

---

## Release Branch Model

| Branch         | Role                            | Releases produced                                                               |
| -------------- | ------------------------------- | ------------------------------------------------------------------------------- |
| `main`         | Stable release channel          | `vX.Y.Z`                                                                        |
| `dev`          | Integration / RC                | `vX.Y.Z-rc.N` prereleases                                                       |
| `hotfix/X.Y.x` | Maintenance for **older** lines | `vX.Y.Z` on the `X.Y` channel (also reused as staging for current-line patches) |

### Branch Lineage Invariant

After every release reconciles, `main` is an ancestor of `dev`, so every tag on `main` is reachable from `dev`. The `Release: Semantic Version` workflow keeps this invariant in two ways depending on the promotion source:

-   **dev → main promotion** (minor/major): `main` fast-forwards to the `dev` SHA, semantic-release appends a `chore(release):` commit on top, then `dev` fast-forwards to absorb that commit. No history rewrites.
-   **hotfix-staging → main promotion** (current-line patch): `main` fast-forwards to the hotfix-staging SHA, semantic-release appends the `chore(release):` commit, then `dev` is **rebase-reconciled** onto the new `main`. `git rebase` drops `dev`'s originals of the cherry-picked fixes (patch-id match) and replays any unique `dev` work on top. This is the only place the workflow force-pushes (`--force-with-lease`).

After a hotfix release, open PRs targeting `dev` are auto-rebased by the `Auto-rebase open PRs` workflow. PRs from forks need "Allow edits by maintainers" enabled or the action silently skips them; drafts and PRs labeled `no-auto-rebase` are also excluded.

---

## Step-by-Step Walkthroughs

### Patch Flow (Current line or Older line)

1. Land the fix on `dev` via normal PR (if applicable).
2. Dispatch **Actions → Release: Hotfix Candidate** — auto-creates/reuses `hotfix/X.Y.x` from the latest stable tag, cherry-picks eligible `fix:`/`perf:` commits, opens a PR.
3. PR checks build a `vX.Y.Z-prNNNN` prerelease for verification.
4. Merge the candidate PR.
5. Cut the release:
    - **Current line** (`main` is on `X.Y`): dispatch **Release: Semantic Version** on `main` with `ff_target = <hotfix/X.Y.x tip SHA>`.
    - **Older line** (`main` has shipped a newer minor/major): dispatch **Release: Semantic Version** on `hotfix/X.Y.x` with `ff_target` empty.

### Minor / Major Release Flow

1. Cut RCs from `dev`: dispatch **Release: Semantic Version** on `dev`, `ff_target` empty → `vX.Y.Z-rc.N`.
2. When ready, dispatch **Release: Semantic Version** on `main` with `ff_target = <dev SHA>` (typically the latest RC's SHA). The workflow FFs `main`, runs semantic-release to cut stable, then FFs `dev` to absorb the `chore(release):` commit.

---

## Release Stages (Alpha / Beta)

Features can declare a release-maturity stage in their `.ini` `[Info]` section. This drives the default-enabled state, a UI marker, and the version-audit policy.

### Declaring a Stage

In `features/<Feature>/Shaders/Features/<Feature>.ini`:

```ini
[Info]
Version = 0-2-0
Beta = True
```

-   **Flags:** `Alpha` or `Beta`. Truthy values are `true`, `1`, `yes`, `on` (case-insensitive). Absent or non-truthy means full **Release**.
-   `Alpha` takes precedence over `Beta` when both are set.
-   The flag line must start the line (after optional whitespace). The CMake parser in `CMakeLists.txt` and the Python parser in `tools/feature_version_audit.py` are both line-anchored; **keep these two regexes in sync**.

### Build-time Baking

`CMakeLists.txt` collects flagged features into `FEATURE_ALPHA_NAMES` / `FEATURE_BETA_NAMES` in the generated `FeatureVersions.h`.

### Runtime API (`src/Feature.h`)

-   `Feature::GetReleaseStage()` returns `ReleaseStage::{Release, Beta, Alpha}` by looking the short name up in the baked sets. Resolve it once and pass it around; it is not cached.
-   `IsAlpha()` / `IsBeta()` convenience predicates.
-   `static GetReleaseStageTag(ReleaseStage)` returns the localized `[ALPHA]` / `[BETA]` marker (empty for Release). It takes the stage so callers that already resolved it avoid a redundant lookup.
-   `IsDisabledByDefault()` returns `true` for any non-Release stage, so **Alpha/Beta features start disabled on first install**. Users can still enable them via the "Disable at Boot" menu; do not add a redundant `IsDisabledByDefault` override on a feature that already carries a stage flag.

### UI

-   `FeatureListRenderer` draws the stage tag next to the feature name. Alpha uses the theme `StatusPalette.Error` color, Beta uses `StatusPalette.Warning`.

### Versioning Convention (Enforced by `tools/feature_version_audit.py`)

-   Pre-release features use `0.x` versions.
-   Beta starts at `0-2-0`, Alpha at `0-1-0`.
-   `alpha -> beta` bumps the minor and resets the patch.
-   Within the same pre-release stage, normal semver applies inside `0.x`.
-   A breaking change (`feat!:` / `BREAKING CHANGE:`) on a pre-release feature **promotes it to release `1-0-0` and strips the Alpha/Beta flag**. `--apply-bumps` performs both the version bump and the flag removal automatically.
-   Stage transitions are exact-match enforced (and may legitimately LOWER the version, e.g. release `1.x` -> beta `0-2-0`), unlike the lenient `>` check used within a stage.

---

## Manual Packaging Targets

These targets are defined in `CMakeLists.txt` and are useful when you want precise control over packaging (CI artifacts, local QA, or manual deployment):

```bash
# Create the Core package (includes CORE features + plugin DLL)
cmake --build ./build/ALL --target Package-Core

# Create a manual AIO package (.7z) via install + tar
cmake --build ./build/ALL --target Package-AIO-Manual

# Create an individual feature package (non-CORE features)
cmake --build ./build/ALL --target Package-<Feature>

# Install into the AIO folder (installs to build/<preset>/aio)
cmake --build ./build/ALL --target AIO

# Alternatively use cmake --install to install to a custom prefix
cmake --install ./build/ALL --prefix <TARGET_DIR>
```

-   `Package-Core` collects everything marked as CORE and the built plugin into a temporary folder, then tars it to `dist/${PROJECT_NAME}-${UTC_NOW}.7z`.
-   `Package-<Feature>` targets create `${FEATURE}-${UTC_NOW}.7z` in `dist/`.
-   `Package-AIO-Manual` performs an install to the AIO folder and then creates a single AIO archive (similar to the automated `AIO_ZIP_PACKAGE`, but wired as an explicit file-producing target).
-   `AIO` target runs `cmake --install` with the `aio` prefix to inspect the folder layout without creating an archive.
