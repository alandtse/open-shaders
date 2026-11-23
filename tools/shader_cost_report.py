#!/usr/bin/env python3
"""Estimate how much a change shifts shader compile cost, and render a PR comment.

Every shader variant in the validation configs gets a digest over its source's
`#include` closure, taken after dropping the `#if` regions its defines rule out
(and comments), so an edit behind a macro the variant lacks does not count.
Diffing the merge base against the head gives the variants
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
import math
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
TIMING_MIN_SECONDS = 10.0
TIMING_MIN_PCT = 25.0
TIMING_MIN_ENTRIES = 20
RUNNER_MIN_ENTRIES = 100
RUNNER_MIN_BASE_SECONDS = 0.05
RUNNER_MAX_FACTOR = 2.5
# Run-to-run noise of the adjusted total shrinks with sample size: about floor + scale / sqrt(entries compared).
NOISE_FLOOR_PCT = 6.0
NOISE_SCALE_PCT = 80.0
DRIFT_PCT = 10.0
DOC_URL = "https://github.com/alandtse/open-shaders/blob/dev/docs/development/shader-compile-cost.md"
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


DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(\w+)(.*)$")
COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
COND_TOKEN = re.compile(
    r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)|0[xX][0-9a-fA-F]+|\w+|&&|\|\||[!=<>]=|[()!<>+\-*/%&|]"
)
# Names a variant may receive from the build rather than its config: never assume them undefined.
IMPLICIT_NAMES = frozenset({"VR", "WINPC", "DX11"})


def macro_names(text):
    """(names this source #defines, names it #undefs): a condition on a #defined name cannot be resolved from the config."""
    defined, undefined = set(), set()
    for line in COMMENTS.sub(" ", text).splitlines():
        m = DIRECTIVE.match(line)
        if m and m.group(1) in ("define", "undef"):
            if first := re.match(r"\s*(\w+)", m.group(2)):
                (defined if m.group(1) == "define" else undefined).add(first.group(1))
    return defined, undefined


class Condition:
    """Three-valued evaluation of a preprocessor condition: 1, 0 or None when the config cannot decide it."""

    def __init__(self, defines, assigned):
        self.defines, (self.assigned, self.undefined) = defines, assigned

    def known_absent(self, name):
        return not (
            name in self.defines
            or name in self.assigned
            or name in IMPLICIT_NAMES
            or name.startswith(("_", "D3D"))
        )

    def evaluate(self, text):
        self.tokens = [m for m in COND_TOKEN.finditer(text)]
        self.pos = 0
        try:
            value = self.or_()
        except (IndexError, ValueError, ZeroDivisionError):
            return None
        return value if self.pos == len(self.tokens) else None

    def peek(self):
        return self.tokens[self.pos].group(0) if self.pos < len(self.tokens) else None

    def take(self):
        self.pos += 1
        return self.tokens[self.pos - 1]

    def or_(self):
        value = self.and_()
        while self.peek() == "||":
            self.take()
            other = self.and_()
            if value or other:
                value = 1 if (value or other) else 0
            else:
                value = 0 if (value == 0 and other == 0) else None
        return value

    def and_(self):
        value = self.compare()
        while self.peek() == "&&":
            self.take()
            other = self.compare()
            if value == 0 or other == 0:
                value = 0
            else:
                value = 1 if (value and other) else None
        return value

    def compare(self):
        left = self.unary()
        while self.peek() in (
            "==",
            "!=",
            "<",
            ">",
            "<=",
            ">=",
            "+",
            "-",
            "*",
            "/",
            "%",
            "&",
            "|",
        ):
            op = self.take().group(0)
            right = self.unary()
            if left is None or right is None:
                left = None
            else:
                left = int(eval(f"{left}{op}{right}", {"__builtins__": {}}))
        return left

    def unary(self):
        if self.peek() == "!":
            self.take()
            value = self.unary()
            return None if value is None else int(not value)
        if self.peek() == "(":
            self.take()
            value = self.or_()
            if self.peek() != ")":
                raise ValueError
            self.take()
            return value
        token = self.take()
        if name := token.group(1) or token.group(2):
            if name in self.defines:
                return None if name in self.undefined else 1
            return 0 if self.known_absent(name) else None
        text = token.group(0)
        if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", text):
            return int(text, 0)
        if self.known_absent(text):
            return 0
        return None


def scan_source(text, defines, assigned):
    """(normalised active source lines, includes) of one file for a variant's defines.

    Regions a condition rules out are dropped; a condition the config cannot decide keeps every branch,
    so the result only over-approximates what the preprocessor would emit.
    """
    cond = Condition(defines, assigned)
    out, includes = [], []
    frames = []  # [parent_active, taken, active, unknown]
    joined = COMMENTS.sub(" ", text).replace("\\\r\n", " ").replace("\\\n", " ")
    for raw in joined.splitlines():
        line = raw.strip()
        m = DIRECTIVE.match(line)
        word = m.group(1) if m else ""
        rest = m.group(2).strip() if m else ""
        active = all(f[2] for f in frames)
        if word in ("if", "ifdef", "ifndef"):
            expr = (
                rest
                if word == "if"
                else (f"defined({rest.split()[0]})" if rest else "")
            )
            if word == "ifndef":
                expr = f"!{expr}"
            value = cond.evaluate(expr)
            frames.append([active, value == 1, active and value != 0, value is None])
            if value is None and active:
                out.append(line)
        elif word == "elif" and frames:
            frame = frames[-1]
            value = cond.evaluate(rest)
            frame[3] = frame[3] or value is None
            frame[2] = frame[0] and not frame[1] and value != 0
            frame[1] = frame[1] or value == 1
            if frame[3] and frame[0]:
                out.append(line)
        elif word == "else" and frames:
            frame = frames[-1]
            frame[2] = frame[0] and not frame[1]
            if frame[3] and frame[0]:
                out.append(line)
        elif word == "endif" and frames:
            frame = frames.pop()
            if frame[3] and frame[0]:
                out.append(line)
        elif active and line:
            out.append(" ".join(line.split()))
            if word == "include":
                if inc := INCLUDE.match(line):
                    includes.append(inc.group(1))
    return out, includes


def closure_index(tree, read):
    """Returns (digest(path, defines), size(path, defines), lower) over the include closure a variant's defines select."""
    lower = {p.lower(): p for p in tree}
    sources, scans, closures, cond_names, reach, macros = {}, {}, {}, {}, {}, {}

    def source(path):
        if path not in sources:
            sources[path] = read(tree[path]).decode("utf-8", "replace")
        return sources[path]

    def conditional_names(path):
        if path not in cond_names:
            names = set()
            for line in COMMENTS.sub(" ", source(path)).splitlines():
                m = DIRECTIVE.match(line)
                if m and m.group(1) in ("if", "ifdef", "ifndef", "elif"):
                    names.update(re.findall(r"\w+", m.group(2)))
            cond_names[path] = frozenset(names)
        return cond_names[path]

    def resolve(path, raw_includes):
        found = []
        for raw in raw_includes:
            raw = raw.replace("\\", "/")
            for candidate in (
                os.path.normpath(os.path.join(os.path.dirname(path), raw)),
                os.path.normpath(raw),
            ):
                hit = lower.get(candidate.replace("\\", "/").lower())
                if hit:
                    found.append(hit)
                    break
        return found

    def entry_macros(entry):
        """Macros #defined or #undef'd anywhere in the entry's unconditional include closure: only those can make a condition undecidable."""
        if entry not in macros:
            seen, stack = set(), [entry]
            while stack:
                item = stack.pop()
                if item not in seen:
                    seen.add(item)
                    if item not in reach:
                        reach[item] = resolve(
                            item, INCLUDE.findall(COMMENTS.sub(" ", source(item)))
                        )
                    stack.extend(reach[item])
            defined, undefined = set(), set()
            for item in seen:
                d, u = macro_names(source(item))
                defined |= d
                undefined |= u
            macros[entry] = (frozenset(defined), frozenset(undefined))
        return macros[entry]

    def scan(path, defines, entry):
        defines = defines & conditional_names(path)
        assigned = tuple(
            names & conditional_names(path) for names in entry_macros(entry)
        )
        key = (path, defines, assigned)
        if key not in scans:
            lines, raw_includes = scan_source(source(path), defines, assigned)
            found = resolve(path, raw_includes)
            text = "\n".join(lines)
            scans[key] = (hashlib.sha1(text.encode()).hexdigest(), len(text), found)
        return scans[key]

    def closure(path, defines):
        key = (path, defines)
        if key not in closures:
            seen, stack = set(), [path]
            while stack:
                item = stack.pop()
                if item not in seen:
                    seen.add(item)
                    stack.extend(scan(item, defines, path)[2])
            closures[key] = sorted(seen)
        return closures[key]

    def digest(path, defines=frozenset()):
        h = hashlib.sha1()
        for item in closure(path, defines):
            h.update(item.encode())
            h.update(scan(item, defines, path)[0].encode())
        return h.hexdigest()

    def size(path, defines=frozenset()):
        return sum(scan(item, defines, path)[1] for item in closure(path, defines))

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
            names = frozenset(str(d).split("=")[0] for d in json.loads(defines))
            if (source, names) not in per_source:
                per_source[(source, names)] = (
                    digest(source, names),
                    size(source, names),
                )
            fingerprint, nbytes = per_source[(source, names)]
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


def runner_factor(before, after, common, structural_pct):
    """Runner-speed ratio (PR over baseline) as the baseline-time-weighted median per-entry ratio, or None when untrustworthy.

    Weighting by baseline seconds keeps the factor consistent with the total, since light entries scale
    differently from heavy ones. A source change that slows every recompiled shader equally would be
    absorbed, so a structural growth estimate at or above the alert threshold disables the adjustment.
    """
    if abs(structural_pct or 0.0) >= THRESHOLD_PCT:
        return None
    ranked = sorted(
        (after[k] / before[k], before[k])
        for k in common
        if before[k] >= RUNNER_MIN_BASE_SECONDS
    )
    if len(ranked) < RUNNER_MIN_ENTRIES:
        return None
    half, seen = sum(w for _, w in ranked) / 2, 0.0
    for ratio, weight in ranked:
        seen += weight
        if seen >= half:
            break
    if not 1 / RUNNER_MAX_FACTOR <= ratio <= RUNNER_MAX_FACTOR:
        return None
    return ratio


def runner_info(timing_path):
    """CPU description CI recorded next to a timing report, or None when absent."""
    if not timing_path:
        return None
    path = os.path.join(
        os.path.dirname(timing_path),
        os.path.basename(timing_path).replace("compile-timing", "compile-runner"),
    )
    if path == timing_path:
        return None
    try:
        return json.load(open(path, encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None


def _runner_label(info):
    cpu = re.sub(r"\s+(\d+-Core )?Processor$", "", info["cpu"]).replace("AMD ", "")
    return f"{cpu} ({info['single_ms']} ms)"


def timing_diff(baseline, measured, structural_pct=None):
    """Compare per-entry compile seconds of a PR build against the dev baseline, over entries present in both.

    Runner speed differs run to run, so when enough entries are shared the PR times are divided by the
    median per-entry ratio before judging; the raw delta is kept for display.
    """

    def load(path):
        if not path or not os.path.isfile(path):
            return {}
        return {
            (r["file"], r["entry"]): r["duration_seconds"]
            for r in json.load(open(path, encoding="utf-8"))
        }

    before, after = load(baseline), load(measured)
    base_runner, pr_runner = runner_info(baseline), runner_info(measured)
    common = [k for k in after if k in before]
    if not common:
        return None
    runner = runner_factor(before, after, common, structural_pct)
    scale = runner or 1.0
    adjusted = {k: after[k] / scale for k in common}
    base_total, pr_total = sum(before[k] for k in common), sum(after[k] for k in common)
    adj_total = pr_total / scale

    def pct(total):
        return 100 * (total - base_total) / base_total if base_total else 0.0

    def moved(sign):
        return sorted(
            (
                k
                for k in common
                if sign * (adjusted[k] - before[k]) >= TIMING_MIN_SECONDS
                and sign * (adjusted[k] - before[k]) >= before[k] * TIMING_MIN_PCT / 100
            ),
            key=lambda k: -sign * (adjusted[k] - before[k]),
        )

    slower, faster = moved(1), moved(-1)
    worst = [(k[1], before[k], adjusted[k]) for k in slower[:3]]
    return {
        "base_runner": base_runner,
        "pr_runner": pr_runner,
        "threshold": timing_threshold(len(common)),
        "compared": len(common),
        "base_s": base_total,
        "pr_s": adj_total,
        "raw_pr_s": pr_total,
        "delta_pct": pct(adj_total),
        "raw_delta_pct": pct(pr_total),
        "runner": runner,
        "slower": len(slower),
        "faster": len(faster),
        "worst": worst,
    }


def timing_threshold(compared):
    """Smallest measured change (percent) worth flagging for a comparison over `compared` entries."""
    noise = NOISE_FLOOR_PCT + NOISE_SCALE_PCT / math.sqrt(max(compared, 1))
    return max(THRESHOLD_PCT, noise)


def timing_verdict(diff):
    if not diff or diff["compared"] < TIMING_MIN_ENTRIES:
        return None
    limit = timing_threshold(diff["compared"])
    if diff["delta_pct"] >= limit:
        return "up"
    return "down" if diff["delta_pct"] <= -limit else None


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


def render(results, flags_changed, timed, timing=None, drift=None):
    """Markdown comment; `results` maps runtime name to a compare() dict (or None when unavailable)."""
    timing, drift = timing or {}, drift or {}
    kinds = {name: verdict(r, flags_changed) for name, r in results.items() if r}
    measured = {timing_verdict(d) for d in timing.values()} - {None}
    if "up" in kinds.values():
        head = "🔺 **Shader compile cost goes up**"
    elif "down" in kinds.values():
        head = "🟢 **Shader compile cost goes down**"
    elif "up" in measured:
        head = "🔺 **Shaders compile slower (measured)**"
    elif "down" in measured:
        head = "🟢 **Shaders compile faster (measured)**"
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
    for name, diff in timing.items():
        if not diff:
            continue
        line = f"**{name} measured:** {diff['compared']} recompiled shaders, {diff['delta_pct']:+.1f}% total compile time against dev ({diff['base_s']:.0f} s → {diff['pr_s']:.0f} s); {diff['slower']} slower, {diff['faster']} faster."
        if diff.get("runner"):
            line += f" Runner-adjusted: this run was {diff['runner']:.2f}× the dev baseline's speed, raw {diff['raw_delta_pct']:+.1f}% ({diff['raw_pr_s']:.0f} s)."
        if diff.get("base_runner") and diff.get("pr_runner"):
            line += f" Hardware: this run {_runner_label(diff['pr_runner'])}, dev baseline {_runner_label(diff['base_runner'])} (single-thread benchmark; lower is faster)."
        if diff["worst"]:
            line += (
                " Slowest regressions: "
                + ", ".join(f"`{e}` {b:.0f} → {a:.0f} s" for e, b, a in diff["worst"])
                + "."
            )
        line += f" Flagged beyond ±{diff['threshold']:.0f}% (run-to-run noise at this sample size)."
        lines += ["", line]
    for name, diff in drift.items():
        if diff and diff["compared"] >= TIMING_MIN_ENTRIES:
            lines += [
                "",
                f"**{name} dev trend:** `dev` compiles {diff['delta_pct']:+.1f}% vs the {diff['ref_days']}-day-old dev run ({diff['compared']} shared shaders)"
                + (
                    "; many small increases are adding up, see the guide below."
                    if diff["delta_pct"] >= DRIFT_PCT
                    else "; within noise."
                ),
            ]
    if (
        "up" in kinds.values()
        or "up" in measured
        or any(
            d and d["compared"] >= TIMING_MIN_ENTRIES and d["delta_pct"] >= DRIFT_PCT
            for d in drift.values()
        )
    ):
        lines += [
            "",
            f"**If this is a problem:** find the slowest entries listed above, check them against the shaders you changed, "
            f"and either cut the added permutations or move the cost behind a define. [How to reproduce and bisect]({DOC_URL}).",
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
        + f". Informational; estimates alert at ±{THRESHOLD_PCT:.0f}%, measured times at the larger of that and the sample's noise band. Source growth is a size proxy; measured times are scaled by the median runner-speed ratio against the dev baseline.</sub>",
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
    ap.add_argument(
        "--pr-timing-vr",
        default="",
        help="timing report of this PR's own build (incremental)",
    )
    ap.add_argument("--pr-timing-se", default="")
    ap.add_argument(
        "--ref-timing-vr",
        default="",
        help="older full dev timing report, to measure dev's own drift",
    )
    ap.add_argument("--ref-timing-se", default="")
    ap.add_argument(
        "--ref-days", type=int, default=14, help="age of the --ref-timing reports"
    )
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
    growth = {n: r["growth_pct"] if r else None for n, r in results.items()}
    timing = {
        "VR": timing_diff(args.timing_vr, args.pr_timing_vr, growth.get("VR")),
        "SE": timing_diff(args.timing_se, args.pr_timing_se, growth.get("SE")),
    }
    drift = {
        "VR": timing_diff(args.ref_timing_vr, args.timing_vr),
        "SE": timing_diff(args.ref_timing_se, args.timing_se),
    }
    for d in drift.values():
        if d:
            d["ref_days"] = args.ref_days
    report = render(
        results, flags_changed, all(w.timed for w in weights.values()), timing, drift
    )
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(report)
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
