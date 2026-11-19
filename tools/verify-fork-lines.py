#!/usr/bin/env python3
"""List fork-authored lines that a merge dropped without a later fork commit removing them.

Replays the fork's own first-parent, non-merge history and tracks every added line until a
later fork commit removes it. A line still tracked at the fork tip is one the fork meant to
keep, so if it is gone from --head (anywhere, not just in its original file) a merge lost it.
Unlike a diff against the previous sync, this also finds losses that were already baked into
the pre-sync tree: they are reported as "already lost" rather than "lost by this merge".

Exit codes: 0 nothing lost (after --allow), 1 lost lines found, 2 usage or git error.
"""

import argparse
import collections
import re
import subprocess
import sys
import tarfile

MIN_LINE_LENGTH = 8
TRIVIAL = re.compile(r"^[\s{}();,#\\/*\-+=\[\]<>!&|.:'\"]*$")
TEXT_SUFFIXES = (".cpp", ".h", ".hpp", ".hlsl", ".hlsli", ".json", ".ini", ".py", ".cmake", ".txt", ".md", ".yaml", ".yml")


class ToolError(Exception):
    pass


def git(repo, *args):
    result = subprocess.run(["git", "-C", repo, *args], capture_output=True)
    if result.returncode != 0:
        raise ToolError(f"git {' '.join(args)}: {result.stderr.decode('utf-8', 'replace').strip()}")
    return result.stdout.decode("utf-8", "replace")


def significant(line):
    return len(line) >= MIN_LINE_LENGTH and not TRIVIAL.match(line) and not line.startswith(("//", "/*", "*"))


def replay_history(repo, fork_ref, since, exclude):
    """Return {(file, line): (sha, subject)} for fork-added lines no later fork commit removed."""
    log = git(repo, "log", "--first-parent", "--no-merges", "--reverse", "-M", "-p", "-U0", "--no-color",
              f"--since={since}", "--format=%x00%H%x00%s%x00", fork_ref)
    live = collections.defaultdict(dict)
    parts = log.split("\x00")
    for i in range(1, len(parts) - 2, 3):
        sha, subject, body = parts[i], parts[i + 1], parts[i + 2]
        apply_commit(live, sha, subject, body, exclude)
    return {(f, l): meta for f, lines in live.items() for l, meta in lines.items()}


def apply_commit(live, sha, subject, body, exclude):
    removed, added, renames = collections.defaultdict(set), collections.defaultdict(dict), {}
    old = new = None
    in_hunk = False
    for raw in body.split("\n"):
        if raw.startswith("diff --git "):
            old = new = None
            in_hunk = False
        elif not in_hunk and raw.startswith("rename from "):
            old = raw[len("rename from "):]
        elif not in_hunk and raw.startswith("rename to "):
            renames[old] = raw[len("rename to "):]
        elif not in_hunk and raw.startswith("--- "):
            old = raw[6:] if raw.startswith("--- a/") else None
        elif not in_hunk and raw.startswith("+++ "):
            new = raw[6:] if raw.startswith("+++ b/") else None
            if new is None and old:
                live.pop(old, None)
        elif raw.startswith("@@"):
            in_hunk = True
        elif in_hunk and raw.startswith("-") and (old or new):
            removed[old or new].add(raw[1:].strip())
        elif in_hunk and raw.startswith("+") and new and not exclude.search(new):
            text = raw[1:].strip()
            if significant(text):
                added[new][text] = (sha, subject)
    for src, dst in renames.items():
        if src in live:
            live[dst] = live.pop(src)
    for file, lines in removed.items():
        for text in lines:
            live.get(file, {}).pop(text, None)
    for file, lines in added.items():
        live[file].update(lines)


def tree_lines(repo, ref, exclude):
    """Every stripped line of the text files at ref, for the 'relocated elsewhere' test."""
    proc = subprocess.Popen(["git", "-C", repo, "archive", "--format=tar", ref], stdout=subprocess.PIPE)
    seen = set()
    with tarfile.open(fileobj=proc.stdout, mode="r|") as archive:
        for member in archive:
            if member.isfile() and member.name.lower().endswith(TEXT_SUFFIXES) and not exclude.search(member.name):
                data = archive.extractfile(member).read().decode("utf-8", "replace")
                seen.update(line.strip() for line in data.splitlines())
    proc.stdout.close()
    if proc.wait() != 0:
        raise ToolError(f"git archive {ref} failed")
    return seen


def file_lines(repo, ref, path, cache):
    key = (ref, path)
    if key not in cache:
        try:
            cache[key] = {line.strip() for line in git(repo, "show", f"{ref}:{path}").splitlines()}
        except ToolError:
            cache[key] = None
    return cache[key]


def find_losses(repo, fork_ref, head, since, exclude):
    live = replay_history(repo, fork_ref, since, exclude)
    head_all = tree_lines(repo, head, exclude)
    cache, lost = {}, []
    for (path, text), (sha, subject) in live.items():
        in_head_file = file_lines(repo, head, path, cache)
        if in_head_file is not None and text in in_head_file:
            continue
        if text in head_all:
            continue
        at_fork = file_lines(repo, fork_ref, path, cache)
        lost.append({"sha": sha, "subject": subject, "file": path, "line": text,
                     "at_fork": at_fork is not None and text in at_fork})
    return lost


def report(lost, allowed, verbose, out=None):
    out = out or sys.stdout
    by_commit = collections.defaultdict(list)
    for item in lost:
        if not any(item["sha"].startswith(prefix) for prefix in allowed):
            by_commit[(item["sha"], item["subject"])].append(item)
    for (sha, subject), items in sorted(by_commit.items(), key=lambda kv: -len(kv[1])):
        fresh = sum(1 for i in items if i["at_fork"])
        print(f"{len(items):4d} lines  {sha[:10]} {subject}  [{fresh} lost by this merge, {len(items) - fresh} already lost]", file=out)
        shown = items if verbose else items[:3]
        for item in shown:
            print(f"        {item['file']}: {item['line'][:100]}", file=out)
    print(f"{sum(len(v) for v in by_commit.values())} fork-added lines missing from head across {len(by_commit)} commits", file=out)
    return bool(by_commit)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", default=".")
    parser.add_argument("--fork-ref", default="origin/dev", help="fork tip whose first-parent history is replayed")
    parser.add_argument("--head", default="HEAD", help="tree that must still carry the fork's lines")
    parser.add_argument("--since", default="90 days ago", help="how far back to replay fork commits (git --since)")
    parser.add_argument("--exclude", default=r"Translations/", help="regex of paths to ignore")
    parser.add_argument("--allow", action="append", default=[], metavar="SHA", help="commit whose loss is intentional and recorded")
    parser.add_argument("--verbose", action="store_true", help="list every lost line, not three per commit")
    args = parser.parse_args(argv)
    try:
        lost = find_losses(args.repo, args.fork_ref, args.head, args.since, re.compile(args.exclude))
    except ToolError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 1 if report(lost, args.allow, args.verbose) else 0


if __name__ == "__main__":
    sys.exit(main())
