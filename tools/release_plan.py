#!/usr/bin/env python3
"""Decide whether a Release: Semantic Version dispatch must promote main first.

Modes: `promote` (fast-forward main, then re-dispatch) or `release`.
Exit codes: 0 decided, 2 refused.
"""

import argparse
import os
import subprocess
import sys

DEFINITION_PATHS = (".github/workflows", ".github/actions")
FORWARDED_INPUTS = ("ff_target", "base_sha", "skip_feature_bumps")


class PlanError(Exception):
    pass


def _git(args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


def _rev(cwd, ref):
    result = _git(["rev-parse", "--verify", f"{ref}^{{commit}}"], cwd)
    return result.stdout.strip() if result.returncode == 0 else None


def definitions_differ(cwd, head_sha, ff_target):
    """True when workflow/action files differ; unknown errors count as differing."""
    result = _git(["diff", "--quiet", head_sha, ff_target, "--", *DEFINITION_PATHS], cwd)
    return result.returncode != 0


def plan(cwd, ref_name, ff_target, single_phase, base_sha, head_sha):
    if not ff_target:
        if base_sha:
            raise PlanError("base_sha requires ff_target")
        return "release", "", "no ff_target"
    if ref_name != "main":
        return "release", "", "not dispatched on main"
    if single_phase:
        return "release", base_sha, "single_phase escape hatch"

    target = _rev(cwd, ff_target)
    if target is None:
        raise PlanError(f"ff_target {ff_target} does not resolve to a commit")
    main = _rev(cwd, "origin/main")
    if main is None:
        raise PlanError("origin/main is not available")

    if base_sha:
        base = _rev(cwd, base_sha)
        if base is None or _git(["merge-base", "--is-ancestor", base, head_sha], cwd).returncode != 0:
            raise PlanError(f"base_sha {base_sha} is not an ancestor of the dispatched commit")
        if main != target:
            raise PlanError(f"phase 2 expects main == ff_target, got main {main[:9]} vs {target[:9]}")
        if definitions_differ(cwd, head_sha, target):
            raise PlanError("phase 2 dispatched from a ref whose workflow definitions differ from ff_target")
        return "release", base_sha, "phase 2 after promotion"

    if main == target:
        raise PlanError(
            "main already equals ff_target and base_sha is empty; the shader/build diff would be empty. "
            "Pass base_sha=<main before promotion> or use single_phase."
        )
    if definitions_differ(cwd, head_sha, target):
        return "promote", main, "workflow or action definitions differ from ff_target"
    return "release", "", "definitions identical"


def gh_dispatch_args(ff_target, base_sha, skip_feature_bumps):
    args = ["-f", f"ff_target={ff_target}", "-f", f"base_sha={base_sha}"]
    if skip_feature_bumps:
        args += ["-f", "skip_feature_bumps=true"]
    return args


def _flag(value):
    return str(value).strip().lower() == "true"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    decide = sub.add_parser("decide")
    decide.add_argument("--repo", default=".")
    decide.add_argument("--ref-name", required=True)
    decide.add_argument("--ff-target", default="")
    decide.add_argument("--single-phase", default="false")
    decide.add_argument("--base-sha", default="")
    decide.add_argument("--head-sha", required=True)
    dispatch = sub.add_parser("dispatch-args")
    dispatch.add_argument("--ff-target", required=True)
    dispatch.add_argument("--base-sha", required=True)
    dispatch.add_argument("--skip-feature-bumps", default="false")
    args = parser.parse_args(argv)

    if args.command == "dispatch-args":
        print("\n".join(gh_dispatch_args(args.ff_target, args.base_sha, _flag(args.skip_feature_bumps))))
        return 0

    try:
        mode, base, reason = plan(
            args.repo, args.ref_name, args.ff_target, _flag(args.single_phase), args.base_sha, args.head_sha
        )
    except PlanError as error:
        print(f"::error::{error}")
        return 2
    print(f"mode={mode} base_sha={base or '<empty>'} ({reason})")
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as handle:
            handle.write(f"mode={mode}\nbase_sha={base}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
