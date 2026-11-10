# Upstream Sync

How Open Shaders stays current with upstream `community-shaders/skyrim-community-shaders` without losing the fork-specific CI policy, branding, and feature setup.

## Mechanism

Upstream syncs land as **merge commits** on `dev`, never as rebases, and **always through a reviewed PR — never a direct push to `dev`**, even for a maintainer with bypass permissions on the `dev` ruleset. The scheduled `Maint: Sync upstream/dev` workflow runs a three-way merge of `upstream/dev` into a `sync/upstream-dev-<date>` branch and opens a PR against `dev`; it does not push to `dev` itself. Two pieces make the merge itself safe:

1. **`.gitattributes` with `merge=ours` entries** for every file the fork owns end-to-end (CI workflows, `.releaserc.js`, README). During the merge, git's `ours` driver keeps the fork's version of those paths verbatim — upstream's changes to them are discarded without surfacing as conflicts.
2. **The `ours` merge driver itself** must be defined locally. `merge=ours` in `.gitattributes` _references_ a driver but doesn't define one. The sync workflow defines it as a no-op (`git config merge.ours.driver true`). **Local contributors who run the merge by hand must run the same command once per clone** — see [Setup](#setup-once-per-clone) below.

## Why merge, not rebase

We previously used `git rebase upstream/dev`. It silently regressed fork-owned files on every sync. The mechanism: upstream cherry-picks one of our fork commits → we rebase later → git detects the duplicate via patch-id and skips our commit as "already applied" → an upstream follow-up that deletes or edits the same file then applies cleanly. End result: the fork loses content with zero merge conflicts and zero log noise. The rebase reports success.

A 3-way merge consults both sides at every path independently of patch-id. Our `merge=ours` driver fires for fork-owned paths; everything else gets a real 3-way merge. Either a clean result or a visible conflict — nothing silent.

See `.gitattributes` for the current fork-owned list. When a file should join or leave that list, update both the attributes and the comment block above the list explaining why.

## Setup (once per clone)

```bash
git config merge.ours.driver true
```

That's it. The `ours` driver is intentionally not built into git (for security — driver definitions can run arbitrary commands), so each clone declares it locally. The sync CI does this in its own setup step.

If you've already run an upstream merge without this config, git would have raised an "unknown merge driver 'ours'" warning and used the default 3-way merge for those files, potentially producing surprising conflicts in fork-owned paths. Re-run with the driver configured and the conflicts disappear.

## Running a sync manually

```bash
# Make sure local dev matches origin/dev before merging upstream.
# A stale local dev would produce a PR based on an out-of-date branch
# or have you re-resolving conflicts already resolved by a prior run.
git fetch origin dev upstream/dev
BRANCH="sync/upstream-dev-$(date +%Y%m%d)"
git switch -c "$BRANCH" origin/dev

git merge --no-ff --no-edit \
    -m "chore(sync): merge upstream/dev as of $(git rev-parse --short upstream/dev)" \
    upstream/dev
# On conflicts (in non-fork-owned paths once the ours driver is configured --
# see .gitattributes and Setup, above; an unconfigured driver produces
# conflicts in fork-owned paths too): resolve each hunk by hand, never with
# -X theirs/-X ours or checkout --theirs/--ours.
# See "Commit structure for a conflicted sync" below for the per-hunk process
# and what to record. Then:
git add <resolved-files>
git commit --no-edit
git push origin "$BRANCH"
gh pr create --base dev \
    --title "chore(sync): merge upstream/dev as of $(git rev-parse --short upstream/dev)" \
    --body "Merges upstream community-shaders/skyrim-community-shaders dev as of $(git rev-parse --short upstream/dev)."
```

For a clean sync, record the per-commit outcomes from the conflict guidelines in the merge commit body by rewriting its message before pushing (`git commit --amend`).

**Never push the merge straight to `dev`**, and never use an admin/PAT bypass of the `dev` ruleset to skip the PR — this applies even to a trivial, conflict-free sync. Merge the PR with **"Create a merge commit"** (never squash, never rebase-merge — either would break the ancestry a future sync's 3-way merge depends on; see [Commit structure](#commit-structure-for-a-conflicted-sync)).

The scheduled workflow opens this PR automatically every Monday 08:00 UTC. Manual dispatch via `gh workflow run "Maint: Sync upstream/dev"` is available for urgent syncs and accepts a `dry_run` flag (fetches and merges locally in the runner, but skips pushing the branch/opening the PR).

## Versioning and changelog interaction

The merge commit's message is `chore(sync): merge upstream/dev as of <sha>`. semantic-release sees it as a `chore` and doesn't release on the commit itself.

**Release analysis walks `--first-parent` only** (`tools/release-first-parent-plugin.js`, wired in as the `commitAnalyzer`/`generateNotes` plugin in `.releaserc.js`), not semantic-release's default full-history DAG walk. A sync's second-parent commits — upstream's own history, potentially thousands of commits after a rewrite, or the 40-60 commits a normal weekly gap brings in — are never inspected for version bumps or changelog entries; the merge itself is the only node the analyzer sees, and as a `chore` it contributes neither. This was introduced to fix a real incident (a single upstream history rewrite made one sync's range include ~2700 already-shipped commits, corrupting both the changelog and the computed bump), and it applies uniformly to every sync since, not just that one.

**The consequence worth knowing:** an upstream `feat:`/`fix:` that arrives purely via a sync merge does **not**, on its own, bump our version or appear in our changelog — a change from this doc's earlier stated design ("the fork's version reflects everything actually shipped, including upstream fixes that arrived via merge"), which described pre-`--first-parent` behavior and is no longer accurate. If a sync's tail commit or its own follow-up work carries a `fix:`/`feat:` on our own first-parent line, that still bumps normally; the upstream content merged alongside it does not add to or reduce that bump. Restoring "upstream fixes count toward our version" would mean walking the second-parent range too, scoped to genuinely new commits and deduped by patch-id against first-parent history — a bigger, riskier change to the release pipeline, not done.

**GitHub's PR "Commits" tab is unaffected by any of this** — it lists the full second-parent history regardless of what the release pipeline analyzes, since that's inherent to what a real merge is. A large commit count there (including upstream's own cherry-picked duplicates of content already in our history under a different SHA — common, since both forks pull from the same PRs) is normal and not a sign the sync did anything wrong; the reviewable surface is the PR's Files changed tab.

## When the workflow halts

A real conflict (in a file _not_ on the fork-owned list) means upstream and the fork have both meaningfully changed the same code. Examples we'd expect:

-   Both forks bump the same feature INI version.
-   We add a method to a class upstream also modified.
-   We rename a function upstream also renamed.

The workflow `git merge --abort`s, posts the conflicted file list to the workflow summary, and exits non-zero. Resolution is manual: clone, run the same merge locally, resolve, push the branch, and open the PR (see [Running a sync manually](#running-a-sync-manually)).

### Conflict Resolution Guidelines

> **Open Shaders prerelease invariant:** every Alpha and Beta feature remains disabled by default, regardless of core status. Do not take upstream's core-only behavior in `src/Feature.h`, and keep the default-disabled profile in `tools/build-shader-cache.py` aligned with it.

1. **Resolve conflicts favoring the fork's side by default** (this fork is the VR maintainer) — see [Commit structure](#commit-structure-for-a-conflicted-sync) for the per-hunk process this now runs through. This is a _strategy default for hunks git can't merge automatically_, distinct from `.gitattributes`' `merge=ours` driver (see Mechanism, above), which protects specific whole files that never reach a hunk-level decision at all. Typical hunk conflicts are unlisted CI workflows or feature `.ini` `[Info]` versions, where keeping ours is almost always right; still check per rule 8 whether upstream's side also fixes something real. (Whole-file `merge=ours` would be the wrong tool here even for a fork-feeling file like a version-bumped `.ini` — it would silently drop any new key upstream adds alongside that bump, which is exactly the class of loss this document exists to prevent.)
2. If upstream ships a VR removal, revert it and keep VR.
3. **Verify ancestry after landing:** `git merge-base --is-ancestor <upstream-sha> HEAD` must pass for each adopted upstream commit.
4. **`CSEditor` vs `SceneSelector`:** this fork split weather-editor UI/logic out of `CSEditor` into its own `Features/SceneSelector` class; upstream never made that split and still lands weather-lock/weather-editor changes directly in `CSEditor.cpp`/`.h`. Taking upstream's side of a `CSEditor` conflict wholesale (e.g. re-adding `WeatherDetailsWindowSettings`, `DrawSettings()`, `PostPostLoad()`) reintroduces state this fork already owns on `SceneSelector`, producing undefined-symbol compile errors. Redirect any new feature-owning override to `SceneSelector` instead; generic engine-level hooks (e.g. `EditorWindow::InstallWeatherLockHooks()`/`MaintainWeatherLock()`) can stay called from either side. Verify with `grep -rn "WeatherDetailsWindowSettings\|SceneSelector" src/Features/CSEditor.*` (expect no matches) plus a clean `BuildRelease.bat Dev-Fast` link.

5. **Incompatible-DLL blocklist:** upstream keeps this list inline in `src/XSEPlugin.cpp`; this fork moved it to `src/Compatibility.h` so each entry can carry a user-facing reason. An upstream `chore: block <mod>` commit conflicts on the removed array — translate the new entry into `Compatibility.h` rather than restoring the array, or the block silently stops applying. Verify with `grep -c "Data/SKSE/Plugins" src/Compatibility.h` against the upstream array length.
6. **A shader-touching sync needs a live render check, not just a diff audit.** A fork-loss/upstream-loss pass only verifies that each side's own content is textually still present after the merge -- it cannot catch a case where our preserved fork code and upstream's newly-merged code are each individually correct in their own original context but semantically disagree once combined (e.g. upstream's new C++ constant-buffer binding logic assuming a shader register layout our fork's `.hlsl` doesn't use). That class of bug is invisible to any line-level diff; it only shows up as a wrong pixel on screen. If a sync touches anything under `package/Shaders/`, boot the deployed build and look at the affected feature in-game (devbench screenshot of an exterior cell is enough for grass/lighting) before calling the sync verified -- compiling clean and passing both structural-preservation passes is not sufficient on its own. Where the change reaches a transition point (a resize, a loading screen, a menu open/close), check that too, not only a steady-state frame -- a fix keyed to a specific trigger can regress silently if only the common frame gets exercised. This is a required gate, not a last resort, for any sync touching `src/Features/Upscaling/`, `src/ShaderCache.*`, or `src/Hooks.cpp`: run it as soon as the tail commit (if any) builds, before the deeper per-commit and fork-loss passes below -- it catches a whole class of regression cheaply that those passes otherwise have to reconstruct by hand from code history.
7. **A fork-loss/upstream-loss pass must cover every changed file, not a self-reported sample.** Rule 13's survival check is the starting checklist: give each verification agent its flagged lines plus the full `git diff <fork-pre-sync>..<HEAD> --stat` file list, and cross-check its report against both before trusting a PASS verdict -- a summary that never names a changed file is not evidence that file was reviewed, no matter how confident it sounds. The files most likely to still need a human pass despite a clean survival-check run are exactly the ones item 6 warns about: shader sources paired with new upstream C++ that assumes a specific contract.

8. **Review every upstream commit for intent, not just the conflicted hunks.** Read each commit's message and PR description (`git log --format='%h %s%n%b' <merge-base>..upstream/dev`, `gh pr view <n> --repo community-shaders/skyrim-community-shaders`) and decide, per commit, what problem it fixes and whether that problem also exists in our code. Do this for clean merges and for files the fork has diverged from (renamed, reimplemented, or split, such as `CSEditor` vs `SceneSelector`, `Compatibility.h`, or the `BackgroundBlur` retained buffers), not only for textual conflicts. Fork identity stays (branding, CI policy, deliberate exclusions like the landscape tree directory), but never discard an upstream fix wholesale because our side "already differs": port the fix's intent onto our implementation, or write down why our implementation already covers it. State the outcome for each such commit in the merge commit body by default (rewriting the unpushed merge commit message before pushing, per line 55, above) so the next sync can see what was ported, adapted, or dropped — the tail commit is optional under the commit structure below, so a conflicted-but-otherwise-clean sync would have nowhere else to put this. Use the tail commit body instead only for outcomes specifically about adaptation the tail commit itself performs. A commit bundling several fixes (for example a menu fix plus a texture-path fix) needs each fix judged separately.

9. **Upstream never tests VR, so a clean (non-conflicting) sync can still silently break it.** After landing, scan the sync's own diff for new runtime-version gating (`REL::Module::IsAE()`/`IsAtLeast(...)`, new "legacy compatibility" layers) and ask for each one whether VR needs it too. A 2-arg `RelocationID(se, ae)` is the correct, preferred pattern for a function VR can share: once its address is registered in `skyrim_vr_address_library`'s `database.csv`, VR resolves it with no C++ change, so the constructor itself is not a red flag. The real risks are a runtime or version gate (for example a flat-runtime-only helper) that silently keeps VR from reaching an otherwise correct call, and an id with no VR row in `database.csv`, which is a fatal plugin-load abort on VR rather than a graceful skip (confirmed live: addresslib id `69180` from a clean sync had no VR row, hard-crashing SkyrimVR on ordinary gameplay — `skyrim_vr_address_library` PR #229). Check both.

10. **A restored fork-specific line is not proof the fix it belonged to survived intact.** When rule 13's check (or a manual fork-loss pass) flags fork-added content missing after a merge, trace it to the fork commit that introduced it (`git log -S'<removed text>'`, or blame the pre-sync tree) and read that commit's own message before restoring just the missing fragment. A fork fix can touch more than one call site, or depend on an invariant that lives elsewhere in the same file; a hunk or function upstream rewrote wholesale can drop several of them at once even where it registers as a real conflict, not only in a hunk that merged silently. Restoring the one instance a check happened to flag is not evidence the fix is whole again -- confirm every site the original commit touched is still present, not only the one that was missing.
11. **A replacement is not verified merely because it looks intentional.** When a conflict resolution or a clean merge swaps a fork counter, predicate, or invariant for an upstream equivalent -- especially one carrying its own explanatory comment -- check that the replacement's actual coverage (what it counts, what code paths feed it, what set it draws from) matches what it replaced. Two mechanisms can each be correct in their own original context and still disagree once combined; a plausible comment on the new code is evidence of that code's own internal reasoning, not of this cross-check.
12. **Re-adaptation itself can introduce a fresh regression, not just repair one from the merge.** A build-break fix written while assembling the tail commit -- a renamed local to dodge a shadow warning, a quick patch for a compile error -- needs the same scrutiny as anything else in the diff. Before adding a new variable or mechanism, check whether the enclosing function already computes or holds an equivalent value that should be reused instead; a fix for one problem that quietly duplicates existing state, or drops a property the code around it relied on, is itself worth catching before it ships.
13. **Verify every fork-added line survived, mechanically, not just by reviewer impression.** Scope to the files the merge actually touched (`git diff --name-only <pre-sync>..HEAD`) — a repo-wide diff against a fork that's diverged across thousands of files elsewhere produces an unusable flag list. For each touched file:
    ```bash
    git diff $(git merge-base <pre-sync> upstream/dev) <pre-sync> -- <file>              # fork's own + lines before this sync
    git show HEAD:<file>                                                                 # the file's actual current content
    git diff $(git merge-base <pre-sync> upstream/dev) upstream/dev -- <file>            # what upstream itself changed/deleted
    ```
    A fork `+` line is a suspected loss if it's absent from the current file content and it's not also a `-` line in the third diff (upstream deleted it itself; not this sync's doing). Check presence in the file directly, not by diffing `HEAD` against `upstream/dev` — a fork line that happens to match content upstream also independently carries produces no diff there even though the line is still present and fine, which would otherwise under-count as a false loss. Skip lines that are pure punctuation or boilerplate (`}`, `return true;`) — they pass trivially either way and flagging them is noise, not signal. A symbol still existing elsewhere in the file is not evidence that a specific line survived — check the line itself, not just a name it happens to share with unrelated code nearby. Record any real flag as a restoration or a stated reason, in the merge commit body per rule 8's default. No script implements this yet; a `tools/verify-fork-lines.py` plus a CI step on `sync/*` PRs is a worthwhile follow-up (tracked separately, not scoped into whichever change adds this rule) — until it exists, the commands above are the check, run by hand but run exactly, not by impression. This doesn't replace rules 6-12; it catches, before a person has to reconstruct it by hand, the exact class of loss those rules describe.
14. **When fork-loss review sees removed fork or upstream content, trace the commit that originally added it before deciding it's a loss.** Comparing pre/post-merge text tells you content is gone; it does not tell you whether the PROBLEM that content solved is still solved. Find the commit that introduced the removed code (`git log -S"<distinctive token>" --oneline -- <file>`), read its message/diff to learn what it was actually for, then check whether the sync's replacement still covers that same problem by a different mechanism — not just "is there anything else nearby." Confirmed both ways in one sync: tracing `tools/feature_version_audit.py`'s removed `nexusfileid` override back to its origin commit showed the ambiguous-multi-file-Nexus-page problem it solved is now handled better, automatically, by upstream's replacement (a real non-issue, correctly cleared rather than flagged); the same technique on a different removed block would have been the only way to catch a genuine regression instead of assuming "not currently used" is sufficient.
15. **A clean (zero-conflict) merge can still break a fork-only build configuration, and no diff-reading pass catches it.** Rule 6 covers shader/C++ contract mismatches; the same failure class applies to build-system files (CMake, presets) for a different reason: upstream's new code can be entirely self-consistent and still assume every target/flag it references always exists, when a fork-specific preset (here, `Linux-ClangCL` with `FFX_FI`/`FFX_OF` off) deliberately disables one. This isn't a loss of fork content (nothing was deleted) and isn't a semantic mismatch between two diffs (nothing conflicted) — it's new code whose assumptions don't hold under a configuration upstream never runs. The only way to catch it is to actually configure/build under every fork-specific preset before calling the sync verified, the same way rule 6 requires an in-game check for shader-touching syncs. Confirmed live: an upstream CMake `foreach` over 5 FFX library targets (added cleanly, no conflict) failed `set_target_properties` on `Linux-ClangCL` specifically, because that preset is the one place `ffx_frameinterpolation_x64`/`ffx_opticalflow_x64` don't exist — caught only by the CI job actually running the preset, not by any fork-loss/upstream-loss diff pass (fixed via `if(TARGET ...)` guards, `open-shaders` PR #758 tail commit).
16. **Local `Linux-ClangCL` verification (see the WSL toolchain reference) must run the actual `cmake --build`, not just `cmake --preset` configure.** Configure only proves vcpkg/CMake target wiring; it never compiles a single line of this repo's own C++, so it cannot catch a clang-cl-vs-MSVC source divergence (the well-known brace-init-to-existing-variable ambiguity class, e.g. `PR #634`/`#665`) no matter how clean it comes back. CI's build step is a genuinely slow round-trip (~15-25 min) — running the equivalent build locally first is materially faster and catches this same class of bug before a push, not after. Confirmed live: a configure-only local pass on PR #758 correctly ruled out a suspected vcpkg/Wine flake, then falsely read as "fully verified" -- a subsequent CI run still failed on an unrelated, genuine `float2 = { ... }` ambiguity in `TerrainShadows.cpp` that only the actual build step, run locally, would have caught up front.

### Commit structure for a conflicted sync

**Never resolve a sync's conflicts with `-X theirs`, `-X ours`, or `git checkout --theirs/--ours <file>`.** `-X theirs`/`-X ours` auto-resolve every _conflicting_ hunk toward one side (non-conflicting changes from both sides still merge normally, per git's own semantics); `git checkout --theirs/--ours <file>` goes further and replaces the whole unmerged file with one side regardless. Both let a real conflict close with no visible marker and no `--diff-filter=U` entry — and on this fork, that has cost real fork content: a hunk auto-resolved this way leaves no trace, so a later review pass has to rediscover the loss from scratch instead of ever seeing it as a conflict in the first place. (An automated job that hits any conflict at all already aborts and hands off to manual resolution — see [When the workflow halts](#when-the-workflow-halts) — so this only affects the manual path, not the weekly unattended run.)

Merge with no strategy option instead, and resolve what's left by hand, one hunk at a time:

1. Before resolving anything, capture the unresolved state — attach `git diff > conflicts.patch` to the PR description or an initial PR comment (it isn't committed, so it has nowhere else to persist), or just paste the conflicted-file list (`git diff --name-only --diff-filter=U`) directly into the PR description. Either way, capture it before rerere or manual resolution touches anything: a rerere replay drops a hunk out of the unmerged list the moment it applies.
2. For each conflicting hunk, default to keeping the fork's side (rule 1), then decide per rule 8 whether upstream's change also needs porting onto our implementation. This is file-granularity rule 1 applied per hunk, not a new policy.
3. In the merge commit body (rule 8's default location for this), list every conflicting hunk as `file:function -> kept fork / took upstream / combined`, with the reason. This list — not "does the tree look like a clean upstream snapshot" — is the audit trail the next sync reads; conflicts tend to recur in the same spots, so the prior merge commit's list tells you where to expect them.
4. A separate tail commit (`fix(sync): re-apply fork divergences for <ref>`, or similar) is for adaptation work **outside** the conflicting hunks only: build breaks a clean merge introduced, renames, relocations, and other semantic follow-through. Rule 12 applies to it just as much as to the merge itself. A trivial sync with no such follow-through doesn't need one.

The merge's second parent must still be the real upstream ref — this is not a cherry-pick/replay of upstream's commits plus one extra, which rewrites SHAs and breaks ancestry the same way squashing does.

**Extract fork logic out of functions upstream owns wholesale, don't just keep re-patching it back in.** When a sync's conflict shows fork-only logic living inside a function or file upstream keeps rewriting from scratch, consider pulling that logic into its own fork-owned symbol, header, or translation unit in the same PR — scoped to the file already being touched; this doesn't license a drive-by refactor of code the sync doesn't otherwise need to change. A future upstream rewrite of the original site then produces a visible conflict on the extraction point instead of a silent deletion of logic riding inside code upstream no longer recognizes as containing anything fork-specific. Treat this as expected, not optional, once a step-3 decision table shows the same function landing on "kept fork" two syncs in a row — that recurrence is the pattern this guidance exists for, not a one-off judgment call. `src/Compatibility.h` (rule 5) is the working precedent: upstream's inline incompatible-DLL array became its own fork-owned file specifically because upstream kept touching it on every sync. `ShaderCache`'s disk-cache rollback/partial-invalidation subsystem is the next candidate with the same shape.

If you do recurring syncs, enabling `git rerere` is worth the one-time setup — it caches each conflict resolution and replays it the next time the same hunks conflict. Per-clone setting, not repo-wide:

```bash
git config rerere.enabled true
```

Deliberately not `rerere.autoupdate true`: autoupdate both replays a cached resolution _and_ stages it, which drops the hunk out of `git diff --diff-filter=U` before anyone sees it as a conflict — the same silent-loss shape `-X theirs` caused, just cached instead of blanket. With plain `rerere.enabled`, a replay lands in the working tree unstaged, so it still needs an explicit `git add` and still gets a row in step 3's decision table (tag it e.g. "kept fork (rerere replay)").

Caches live in `.git/rr-cache/` and aren't pushed, so each maintainer builds their own. CI runners start with empty caches every run and benefit nothing from rerere — only the maintainers doing the merges locally see the time savings.

## Inspecting what a sync did

Each sync workflow run leaves a summary on the run page with:

-   Upstream tip SHA
-   `git diff --stat` of files changed
-   `git log --oneline` of commits brought in

The PR itself is the primary review surface — read its diff and description before approving/merging, same as any other PR.

For deeper inspection after the PR is merged:

```bash
# all changes since the last sync merge
git log --first-parent --merges --grep='chore(sync)' -1   # find the merge commit
git diff <merge-commit>~1..<merge-commit>                  # changes the merge introduced
```
