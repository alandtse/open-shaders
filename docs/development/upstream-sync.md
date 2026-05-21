# Upstream Sync

How Open Shaders stays current with upstream `community-shaders/skyrim-community-shaders` without losing the fork-specific CI policy, branding, and feature setup.

## Mechanism

Upstream syncs land as **merge commits** on `dev`, never as rebases. The scheduled `Maint: Sync upstream/dev` workflow runs a three-way merge of `upstream/dev` into our `dev` and pushes the result. Two pieces make this safe:

1. **`.gitattributes` with `merge=ours` entries** for every file the fork owns end-to-end (CI workflows, `.releaserc`, README). During the merge, git's `ours` driver keeps the fork's version of those paths verbatim — upstream's changes to them are discarded without surfacing as conflicts.
2. **The `ours` merge driver itself** must be defined locally. `merge=ours` in `.gitattributes` _references_ a driver but doesn't define one. The sync workflow defines it as a no-op (`git config merge.ours.driver true`). **Local contributors who run the merge by hand must run the same command once per clone** — see [Setup](#setup-once-per-clone) below.

## Why merge, not rebase

We previously used `git rebase upstream/dev`. It silently regressed fork-owned files on every sync. The mechanism: upstream cherry-picks one of our fork commits → we rebase later → git detects the duplicate via patch-id and skips our commit as "already applied" → an upstream follow-up that deletes or edits the same file then applies cleanly. End result: the fork loses content with zero merge conflicts and zero log noise. The rebase reports success.

A 3-way merge consults both sides at every path independently of patch-id. Our `merge=ours` driver fires for fork-owned paths; everything else gets a real 3-way merge. Either a clean result or a visible conflict — nothing silent.

See `.gitattributes` for the current fork-owned list. When a file should join or leave that list, update both the attributes and the comment block above the list explaining why.

## Setup (once per clone)

```bash
git config merge.ours.driver true
```

That's it. The `ours` driver is intentionally not git-builtin (for security — driver definitions can run arbitrary commands), so each clone declares it locally. The sync CI does this in its own setup step.

If you've already run an upstream merge without this config, git would have raised an "unknown merge driver 'ours'" warning and used the default 3-way merge for those files, potentially producing surprising conflicts in fork-owned paths. Re-run with the driver configured and the conflicts disappear.

## Running a sync manually

```bash
git fetch upstream dev
git switch dev
git merge --no-ff --no-edit \
    -m "chore(sync): merge upstream/dev as of $(git rev-parse --short upstream/dev)" \
    upstream/dev
# resolve conflicts (only in non-fork-owned paths), then:
git push origin dev
```

The scheduled workflow does exactly this on Monday 08:00 UTC. Manual dispatch via `gh workflow run "Maint: Sync upstream/dev"` is available for urgent syncs and accepts a `dry_run` flag.

## Versioning and changelog interaction

The merge commit's message is `chore(sync): merge upstream/dev as of <sha>`. semantic-release sees it as a `chore` and doesn't release on the commit itself.

**However**, semantic-release's default DAG walk follows the merge into upstream's commit history. Upstream's `feat:` and `fix:` commits that came in via the merge are visible to the commit analyzer and **do** drive version bumps in our release stream. This is deliberate: the fork's version reflects everything actually shipped to users, including upstream fixes that arrived via merge.

When upstream cherry-picks one of our commits and that cherry-pick lands in our merge, both copies of the same logical change get walked. Version-wise this is harmless (one release can only bump once at the max severity). Changelog-wise it produces a duplicate entry. If this becomes annoying, the fix is a `writerOpts.transform` in `.releaserc` that dedupes by patch-id — not done preemptively.

## When the workflow halts

A real conflict (in a file _not_ on the fork-owned list) means upstream and the fork have both meaningfully changed the same code. Examples we'd expect:

-   Both forks bump the same feature INI version.
-   We add a method to a class upstream also modified.
-   We rename a function upstream also renamed.

The workflow `git merge --abort`s, posts the conflicted file list to the workflow summary, and exits non-zero. Resolution is manual: clone, run the same merge locally, resolve, push.

If you do recurring syncs, enabling `git rerere` is worth the one-time setup — it caches each conflict resolution and replays it the next time the same hunks conflict. Per-clone setting, not repo-wide:

```bash
git config rerere.enabled true
git config rerere.autoupdate true
```

Caches live in `.git/rr-cache/` and aren't pushed, so each maintainer builds their own. CI runners start with empty caches every run and benefit nothing from rerere — only the maintainers doing the merges locally see the time savings.

## Inspecting what a sync did

Each sync workflow run leaves a summary on the run page with:

-   Upstream tip SHA
-   `git diff --stat` of files changed
-   `git log --oneline` of commits brought in

For deeper inspection after a push:

```bash
# all changes since the last sync merge
git log --first-parent --merges --grep='chore(sync)' -1   # find the merge commit
git diff <merge-commit>~1..<merge-commit>                  # changes the merge introduced
```
