#!/usr/bin/env python3
"""Estimate how much a change shifts shader compile cost, and render a PR comment.

Every shader variant in the validation configs gets a digest over its source's
`#include` closure. Diffing the merge base against the head gives the variants
that must recompile, the ones added or removed, and how much the touched
sources grew. Cost is weighted by a recent per-entry timing report
(`hlslkit-compile --timing-report`), so the numbers follow what the change
touches rather than runner noise. Everything is read from git objects: no
checkout, no fxc.

    python tools/shader_cost_report.py --base <sha> --head <sha> \
        [--timing-vr vr.json] [--timing-se se.json] [--out report.md]
"""

import argparse
import collections
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys

import yaml

CONFIGS = {
    "VR": ".github/configs/shader-validation-vr.yaml",
    "SE": ".github/configs/shader-validation.yaml",
}
FLAG_FILES = ("src/Utils/ShaderCompileFlags.h", "tools/build-shader-cache.py")
FLAG_LINES = re.compile(
    r"DEFAULT_STANDALONE_FLAGS|\"/O\d\"|/Ges|/Vd|--optimization-level|D3DCOMPILE_|AVOID_FLOW|Flags?\b.*="
)
SHADER_EXTS = (".hlsl", ".hlsli", ".fx", ".h")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
THRESHOLD_PCT = 5.0
NEUTRAL_PCT = 1.0
MARKER = "<!-- shader-cost-report -->"


class Repo:
    """Reads blobs of one repository through a single `git cat-file --batch`."""

    def __init__(self, path="."):
        self.path = path
        self._cat = None
        self._cache = {}

    def git(self, *args):
        return subprocess.run(
            ["git", "-C", self.path, *args], capture_output=True, check=True
        ).stdout.decode("utf-8", "replace")

    def blob(self, sha):
        if sha not in self._cache:
            if self._cat is None:
                self._cat = subprocess.Popen(
                    ["git", "-C", self.path, "cat-file", "--batch"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                )
            self._cat.stdin.write(sha.encode() + b"\n")
            self._cat.stdin.flush()
            size = int(self._cat.stdout.readline().split()[2])
            self._cache[sha] = self._cat.stdout.read(size + 1)[:-1]
        return self._cache[sha]

    def shader_tree(self, rev):
        """AIO-layout map of shader path (relative to Shaders/) to blob sha; feature files fill gaps in package/."""
        tree, features = {}, []
        for line in self.git(
            "ls-tree", "-r", rev, "--", "package/Shaders", "features"
        ).splitlines():
            meta, path = line.split("\t", 1)
            if not path.lower().endswith(SHADER_EXTS):
                continue
            sha = meta.split()[2]
            if path.startswith("package/Shaders/"):
                tree[path[len("package/Shaders/") :]] = sha
            elif m := re.match(r"features/[^/]+/Shaders/(.+)$", path):
                features.append((m.group(1), sha))
        for path, sha in features:
            tree.setdefault(path, sha)
        return tree

    def file_sha(self, rev, path):
        try:
            return self.git("rev-parse", f"{rev}:{path}").strip()
        except subprocess.CalledProcessError:
            return ""


def parse_variants(config_text):
    """(file, shader type, entry, defines) per variant of a validation config."""
    data = yaml.safe_load(config_text) or {}
    rows = []
    for shader in data.get("shaders", []):
        for shader_type, config in (shader.get("configs") or {}).items():
            common = config.get("common_defines") or []
            for entry in config.get("entries") or []:
                defines = json.dumps(
                    common + (entry.get("defines") or []), sort_keys=True, default=str
                )
                rows.append((shader["file"], shader_type, entry["entry"], defines))
    return rows


def closure_index(tree, read):
    """Returns (digest(path), size(path)) over the include closure of a shader source."""
    lower = {p.lower(): p for p in tree}
    includes, closures = {}, {}

    def direct(path):
        if path not in includes:
            found = []
            for raw in INCLUDE.findall(read(tree[path]).decode("utf-8", "replace")):
                raw = raw.replace("\\", "/")
                for candidate in (
                    os.path.normpath(os.path.join(os.path.dirname(path), raw)),
                    os.path.normpath(raw),
                ):
                    hit = lower.get(candidate.replace("\\", "/").lower())
                    if hit:
                        found.append(hit)
                        break
            includes[path] = found
        return includes[path]

    def closure(path):
        if path not in closures:
            seen, stack = set(), [path]
            while stack:
                item = stack.pop()
                if item not in seen:
                    seen.add(item)
                    stack.extend(direct(item))
            closures[path] = sorted(seen)
        return closures[path]

    def digest(path):
        h = hashlib.sha1()
        for item in closure(path):
            h.update(item.encode())
            h.update(tree[item].encode())
        return h.hexdigest()

    def size(path):
        return sum(len(read(tree[item])) for item in closure(path))

    return digest, size, lower


def variant_map(rows, tree, read):
    """Variant key (file basename, entry, type) -> (digest, closure bytes)."""
    digest, size, lower = closure_index(tree, read)
    by_name = collections.defaultdict(list)
    for path in tree:
        by_name[path.lower().rsplit("/", 1)[-1]].append(path)
    per_source, out = {}, {}
    for file_name, shader_type, entry, defines in rows:
        name = str(file_name).replace("\\", "/")
        source = lower.get(name.lower()) or next(
            iter(by_name.get(name.lower().rsplit("/", 1)[-1], [])), None
        )
        if source is None:
            fingerprint, nbytes = "missing", 0
        else:
            if source not in per_source:
                per_source[source] = (digest(source), size(source))
            fingerprint, nbytes = per_source[source]
        out[(name.rsplit("/", 1)[-1], entry, shader_type)] = (
            hashlib.sha1(f"{fingerprint}|{defines}".encode()).hexdigest(),
            nbytes,
        )
    return out


class Weights:
    """Per-entry compile seconds from a timing report; falls back to the file mean, then to 1 per variant."""

    def __init__(self, path=""):
        self.by_entry, per_file = {}, collections.defaultdict(list)
        if path and os.path.isfile(path):
            for row in json.load(open(path, encoding="utf-8")):
                self.by_entry[(row["file"], row["entry"])] = row["duration_seconds"]
                per_file[row["file"]].append(row["duration_seconds"])
        self.timed = bool(self.by_entry)
        self.by_file = {f: statistics.mean(v) for f, v in per_file.items()}
        self.fallback = statistics.mean(sum(per_file.values(), [])) if per_file else 1.0

    def __call__(self, key):
        return self.by_entry.get(
            (key[0], key[1]), self.by_file.get(key[0], self.fallback)
        )


def compare(base, head, weight):
    changed = [k for k in head if k in base and base[k][0] != head[k][0]]
    added = [k for k in head if k not in base]
    removed = [k for k in base if k not in head]
    total = sum(weight(k) for k in head) or 1.0
    growth = sum(
        weight(k) * (head[k][1] / base[k][1] - 1)
        for k in changed
        if base[k][1] and head[k][1]
    )
    added_cost = sum(weight(k) for k in added)
    removed_cost = sum(weight(k) for k in removed)
    net = added_cost - removed_cost + growth
    files = collections.Counter(k[0] for k in changed + added)
    return {
        "base_total": len(base),
        "total": len(head),
        "changed": len(changed),
        "added": len(added),
        "removed": len(removed),
        "recompile_s": sum(weight(k) for k in changed + added),
        "total_s": total,
        "added_pct": 100 * added_cost / total,
        "removed_pct": 100 * removed_cost / total,
        "growth_pct": 100 * growth / total,
        "net_s": net,
        "net_pct": 100 * net / total,
        "top_files": files.most_common(3),
    }


def verdict(result, flags_changed):
    if result["net_pct"] >= THRESHOLD_PCT:
        return "up"
    if result["net_pct"] <= -THRESHOLD_PCT:
        return "down"
    if flags_changed:
        return "flags"
    return (
        "flat"
        if abs(result["net_pct"]) < NEUTRAL_PCT
        and result["added"] == 0
        and result["removed"] == 0
        else "small"
    )


def _minutes(seconds):
    return (
        f"{seconds / 60:.0f} CPU-min" if abs(seconds) >= 60 else f"{seconds:.0f} CPU-s"
    )


def render(results, flags_changed, timed):
    """Markdown comment; `results` maps runtime name to a compare() dict (or None when unavailable)."""
    kinds = {name: verdict(r, flags_changed) for name, r in results.items() if r}
    if "up" in kinds.values():
        head = "🔺 **Shader compile cost goes up**"
    elif "down" in kinds.values():
        head = "🟢 **Shader compile cost goes down**"
    elif flags_changed:
        head = "⚠️ **Compile flags changed**"
    elif kinds and all(k == "flat" for k in kinds.values()):
        head = "⚪ **Shader compile cost is unchanged**"
    else:
        head = "⚪ **Small shader compile cost change**"
    names = list(results)
    lines = [
        MARKER,
        head,
        "",
        "| | " + " | ".join(names) + " |",
        "|---|" + "---|" * len(names),
    ]

    def row(label, fn):
        lines.append(
            f"| {label} | "
            + " | ".join(fn(r) if r else "n/a" for r in results.values())
            + " |"
        )

    row(
        "Variants",
        lambda r: f"{r['base_total']} → {r['total']} (+{r['added']} / −{r['removed']})",
    )
    row(
        "Must recompile",
        lambda r: (
            f"{r['changed'] + r['added']} of {r['total']}"
            + (f", ~{_minutes(r['recompile_s'])}" if timed else "")
        ),
    )
    if timed:
        row("Added variants", lambda r: f"{r['added_pct']:+.1f}% of validation CPU")
        row("Source growth (est.)", lambda r: f"{r['growth_pct']:+.1f}%")
        row(
            "Net (est.)",
            lambda r: (
                f"{r['net_pct']:+.1f}% ({'+' if r['net_s'] >= 0 else '−'}{_minutes(abs(r['net_s']))})"
            ),
        )
    first = next((r for r in results.values() if r and r["top_files"]), None)
    if first:
        lines += [
            "",
            "Most affected: "
            + ", ".join(f"`{f}` ({n})" for f, n in first["top_files"]),
        ]
    if flags_changed:
        lines += [
            "",
            "A compile-flag source file changed, which can move runtime compile cost in ways this estimate cannot see.",
        ]
    lines += [
        "",
        "<sub>Estimated from the validation configs and recent `dev` compile timings"
        + ("" if timed else " (no timing report available, so counts only)")
        + f". Informational; alerts at ±{THRESHOLD_PCT:.0f}%. Source growth is a size proxy.</sub>",
    ]
    return "\n".join(lines) + "\n"


def flag_signature(repo, rev):
    parts = []
    for path in FLAG_FILES:
        sha = repo.file_sha(rev, path)
        text = repo.blob(sha).decode("utf-8", "replace") if sha else ""
        if path.endswith(".py"):
            text = "\n".join(
                line.strip() for line in text.splitlines() if FLAG_LINES.search(line)
            )
        parts.append(hashlib.sha1(text.encode()).hexdigest())
    return tuple(parts)


def touches_shaders(repo, base, head):
    for path in repo.git("diff", "--name-only", base, head).splitlines():
        if path.lower().endswith(SHADER_EXTS) or path.startswith(
            ".github/configs/shader-validation"
        ):
            return True
    return False


def runtime_variants(repo, rev, config_path):
    sha = repo.file_sha(rev, config_path)
    if not sha:
        return None
    return variant_map(
        parse_variants(repo.blob(sha).decode("utf-8")), repo.shader_tree(rev), repo.blob
    )


def snapshot(repo, rev, weights, runtimes):
    """Per-runtime totals for one revision, for appending to a long-term history."""
    out = {
        "rev": repo.git("rev-parse", rev).strip(),
        "date": repo.git("log", "-1", "--format=%cI", rev).strip(),
        "runtimes": {},
    }
    for name, config in CONFIGS.items():
        variants = runtime_variants(repo, rev, config) if name in runtimes else None
        if variants is None:
            continue
        per_file = collections.defaultdict(lambda: {"variants": 0, "cpu_s": 0.0})
        for key in variants:
            per_file[key[0]]["variants"] += 1
            per_file[key[0]]["cpu_s"] += weights[name](key)
        out["runtimes"][name] = {
            "variants": len(variants),
            "weighted": weights[name].timed,
            "cpu_s": round(sum(v["cpu_s"] for v in per_file.values()), 1),
            "files": {
                f: {"variants": v["variants"], "cpu_s": round(v["cpu_s"], 1)}
                for f, v in sorted(per_file.items())
            },
        }
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--repo", default=".")
    ap.add_argument("--base", default="")
    ap.add_argument("--head", required=True)
    ap.add_argument(
        "--runtime",
        default="",
        choices=["", "VR", "SE"],
        help="limit --snapshot to one runtime",
    )
    ap.add_argument(
        "--snapshot",
        default="",
        help="write a per-revision totals JSON for --head and exit",
    )
    ap.add_argument("--timing-vr", default="")
    ap.add_argument("--timing-se", default="")
    ap.add_argument("--out", default="shader-cost-report.md")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    repo = Repo(args.repo)
    weights = {"VR": Weights(args.timing_vr), "SE": Weights(args.timing_se)}
    if args.snapshot:
        with open(args.snapshot, "w", encoding="utf-8") as f:
            json.dump(
                snapshot(
                    repo,
                    args.head,
                    weights,
                    [args.runtime] if args.runtime else list(CONFIGS),
                ),
                f,
                indent=1,
            )
        return 0
    if not args.base:
        ap.error("--base is required unless --snapshot is given")
    base = repo.git("merge-base", args.base, args.head).strip()
    if not touches_shaders(repo, base, args.head):
        print("No shader or validation-config change; no report.")
        return 0
    results = {}
    for name, config in CONFIGS.items():
        before, after = (
            runtime_variants(repo, base, config),
            runtime_variants(repo, args.head, config),
        )
        results[name] = (
            compare(before, after, weights[name])
            if before is not None and after is not None
            else None
        )
    flags_changed = flag_signature(repo, base) != flag_signature(repo, args.head)
    report = render(results, flags_changed, all(w.timed for w in weights.values()))
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(report)
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
