#!/usr/bin/env python3
"""Run incremental HLSL validation: compile only shaders affected by a change.

This is a thin wrapper around ``hlslkit-compile``. It figures out which shader
files changed, translates their repo-relative paths into the assembled-AIO
layout that the validation configs use, and passes them to hlslkit via
``--changed-files``. hlslkit builds the ``#include`` dependency graph and
compiles only the entry-point shaders that transitively include a changed file.

Two ways to supply the change set:

* ``--changed-list-file FILE`` — a precomputed list, one path per line (not
  whitespace-separated, so feature directory names with spaces survive). CI
  passes the list produced by ``tj-actions/changed-files``. An *empty* file
  means "unknown change set" and triggers FULL validation (push/release runs).
* git diff (default when no list file is given) — compares the working tree
  against ``--base`` (default ``HEAD``). An empty diff means "nothing changed"
  and exits 0 without compiling. This is the local-dev path.

Safety: validation is only narrowed when it is provably safe. Any of the
following forces a FULL run:

* a validation config (``.github/configs/**``), CMake, or submodule change
  (``.gitmodules`` or a gitlink bump under ``extern/``) — these can alter the
  entry-point/define set itself, so the graph is no longer authoritative;
* a changed shader path that doesn't map into the AIO shader tree (an
  unexpected location we can't reason about).

hlslkit applies a second layer of the same safety net: if any path we pass
isn't found in the shader tree, it falls back to full validation on its own.

Everything after ``--`` is the ``hlslkit-compile`` command to run; this script
only appends ``--changed-files`` to it (or runs it unchanged for a full run).

Example::

    python tools/validate_changed_shaders.py --changed-list-file changed.txt -- \\
        hlslkit-compile --fxc fxc.exe --shader-dir build/ALL/aio/Shaders \\
        --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml \\
        --max-warnings 0 --suppress-warnings X1519
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

SHADER_EXTENSIONS = (".hlsl", ".hlsli")

# Repo-relative prefixes that map onto the assembled AIO shader root.
# package/Shaders/<rest>            -> <rest>
# features/<Feature>/Shaders/<rest> -> <rest>
_PACKAGE_PREFIX = re.compile(r"^package/Shaders/(.+)$")
_FEATURE_PREFIX = re.compile(r"^features/[^/]+/Shaders/(.+)$")

# Changes that invalidate the dependency-graph assumption and force a full run.
_FULL_TRIGGERS = (
    re.compile(r"^\.github/configs/"),  # validation configs define entries/defines
    re.compile(r"^CMakeLists\.txt$"),
    re.compile(r"^CMakePresets\.json$"),
    re.compile(r"^cmake/"),
    re.compile(r"^\.gitmodules$"),
    # Submodule pointer bumps surface as a gitlink change under extern/ without
    # touching .gitmodules; treat both as a full-validation trigger.
    re.compile(r"^extern/"),
)


def _normalize(path: str) -> str:
    norm = path.replace("\\", "/").strip()
    if norm.startswith("./"):
        norm = norm[2:]
    return norm


def read_changed_list_file(path: str) -> list[str]:
    """Read a precomputed changed-files list (one path per line).

    Newline-separated, not whitespace-separated: feature directory names
    contain spaces (e.g. ``features/Light Limit Fix/Shaders/...``).
    """
    with open(path, encoding="utf-8") as f:
        return [line.strip() for line in f.read().splitlines() if line.strip()]


def git_changed_files(base: str, repo_root: str) -> list[str]:
    """Return working-tree changes vs ``base`` plus untracked files."""

    def _run(args: list[str]) -> list[str]:
        out = subprocess.run(
            ["git", *args],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
        return [line for line in out.stdout.splitlines() if line.strip()]

    tracked = _run(["diff", "--name-only", base])
    untracked = _run(["ls-files", "--others", "--exclude-standard"])
    return sorted(set(tracked) | set(untracked))


def find_full_trigger(changed: list[str]) -> str | None:
    """Return the first changed path that forces a full run, or None."""
    for path in changed:
        norm = _normalize(path)
        for trigger in _FULL_TRIGGERS:
            if trigger.search(norm):
                return norm
    return None


class _Unmappable:
    """Sentinel type: a shader file outside the known AIO shader roots."""


# A shader file we can't map to the AIO layout (e.g. tests/shaders/**). Distinct
# from None (not a shader at all) so the caller widens to full validation rather
# than silently dropping it.
UNMAPPABLE = _Unmappable()


def translate_to_aio(path: str) -> "str | None | _Unmappable":
    """Map a repo-relative shader path to its AIO-relative form.

    Returns the AIO-relative path for a mappable shader, ``None`` for a
    non-shader file, or :data:`UNMAPPABLE` for a shader outside the known roots.
    """
    norm = _normalize(path)
    if not norm.lower().endswith(SHADER_EXTENSIONS):
        return None
    for pattern in (_PACKAGE_PREFIX, _FEATURE_PREFIX):
        m = pattern.match(norm)
        if m:
            return m.group(1)
    return UNMAPPABLE


def classify_changes(changed: list[str]) -> tuple[list[str] | None, str]:
    """Decide full vs incremental from a changed-files list.

    Returns ``(aio_files, reason)``. ``aio_files is None`` means run FULL
    validation; otherwise it is the (possibly empty) list of AIO-relative
    shader paths to validate incrementally.
    """
    trigger = find_full_trigger(changed)
    if trigger is not None:
        return None, f"full validation: '{trigger}' can change the entry-point/define set"

    aio_files: list[str] = []
    for path in changed:
        mapped = translate_to_aio(path)
        if mapped is None:
            continue  # not a shader file
        if mapped is UNMAPPABLE:
            return None, f"full validation: shader path '{_normalize(path)}' is outside the AIO shader tree"
        aio_files.append(mapped)

    # Deduplicate, preserve determinism.
    aio_files = sorted(set(aio_files))
    return aio_files, f"incremental: {len(aio_files)} changed shader file(s)"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Run hlslkit-compile incrementally for changed shaders.",
        epilog="Pass the hlslkit-compile command after a '--' separator.",
    )
    parser.add_argument(
        "--changed-list-file",
        help="File with a precomputed list of changed repo-relative paths. "
        "Empty file => full validation. Omit to derive the list from git diff.",
    )
    parser.add_argument(
        "--base",
        default="HEAD",
        help="Git ref to diff against in local mode (default: HEAD).",
    )
    parser.add_argument(
        "--repo-root",
        default=os.getcwd(),
        help="Repository root for git operations (default: cwd).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the resolved hlslkit-compile command instead of running it.",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="The hlslkit-compile command, after a '--' separator.",
    )
    args = parser.parse_args(argv)

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("missing hlslkit-compile command after '--'")

    # --- Resolve the change set and decide full vs incremental ---
    from_list = args.changed_list_file is not None
    if from_list:
        if not os.path.isfile(args.changed_list_file):
            print(f"[validate-changed] list file '{args.changed_list_file}' not found; running FULL validation.")
            changed = []
            empty_means_full = True
        else:
            changed = read_changed_list_file(args.changed_list_file)
            empty_means_full = True  # provided-but-empty => unknown => full
    else:
        try:
            changed = git_changed_files(args.base, args.repo_root)
        except (subprocess.CalledProcessError, OSError) as e:
            # CalledProcessError: not a git repo / bad ref. OSError: git not on PATH.
            print(f"[validate-changed] git diff failed ({e}); running FULL validation.", file=sys.stderr)
            changed = []
            empty_means_full = True
        else:
            empty_means_full = False  # clean empty diff => nothing to do

    if not changed:
        if empty_means_full:
            print("[validate-changed] no change set provided; running FULL validation.")
            return _run(command, args.dry_run)
        print("[validate-changed] no changed files detected; nothing to validate.")
        return 0

    aio_files, reason = classify_changes(changed)
    print(f"[validate-changed] {reason}")

    if aio_files is None:
        return _run(command, args.dry_run)

    if not aio_files:
        # Change set had no shader files and no full-trigger (e.g. only docs).
        print("[validate-changed] no shader files in the change set; nothing to validate.")
        return 0

    for f in aio_files:
        print(f"[validate-changed]   changed shader: {f}")

    # Write the AIO-relative list and hand it to hlslkit via @file. The real run
    # blocks until hlslkit has read the file, so it is safe to remove afterward;
    # always clean up (including --dry-run) so no stray temp files accumulate.
    fd, list_path = tempfile.mkstemp(prefix="changed_shaders_", suffix=".txt", text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("\n".join(aio_files) + "\n")
        full_command = [*command, "--changed-files", f"@{list_path}"]
        return _run(full_command, args.dry_run)
    finally:
        try:
            os.remove(list_path)
        except OSError:
            pass


def _run(command: list[str], dry_run: bool) -> int:
    printable = " ".join(command)
    if dry_run:
        print(f"[validate-changed] DRY RUN: {printable}")
        return 0
    print(f"[validate-changed] running: {printable}")
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
