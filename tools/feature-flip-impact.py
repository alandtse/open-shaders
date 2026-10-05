#!/usr/bin/env python3
"""Estimate how much shader compiling turning each feature on or off causes.

For every shader define a feature adds, removes the define from each validation-config variant,
preprocesses the variant with and without it using the real d3dcompiler, and groups variants by
identical preprocessed code. A group whose code changes must be recompiled after the flip; the rest
can be reused. Each group is weighted by its measured compile time (the hlslkit --timing-report
the release build already writes), so the result is the share of compile time a flip puts back.

The preprocessor is Windows-only (d3dcompiler_47 through ctypes), like the compile it models.

    python tools/feature-flip-impact.py --runtime SE \\
        --config .github/configs/shader-validation.yaml --timing shader-compile-timing-SE.json \\
        --output package/SKSE/Plugins/CommunityShaders/FeatureFlipImpact.json

Running it again with another --runtime adds that runtime to an existing output file.
Release CI builds each runtime on its own job, then joins the results (no Windows needed):

    python tools/feature-flip-impact.py --merge FeatureFlipImpact-SE.json FeatureFlipImpact-VR.json --output FeatureFlipImpact.json
"""

import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from ctypes import POINTER, Structure, byref, c_char_p, c_size_t, c_void_p
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
SCHEMA_VERSION = 1
PLATFORM_DEFINES = {"PSHADER", "VSHADER", "CSHADER", "WINPC", "DX11", "VR"}
STANDARD_FILE_INCLUDE = 1  # D3D_COMPILE_STANDARD_FILE_INCLUDE


class Macro(Structure):
    _fields_ = [("Name", c_char_p), ("Definition", c_char_p)]


def load_preprocessor():
    d3d = ctypes.WinDLL("d3dcompiler_47.dll")
    d3d.D3DPreprocess.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p, c_void_p, POINTER(c_void_p), POINTER(c_void_p)]
    return d3d


def take_blob(blob):
    vtable = ctypes.cast(blob, POINTER(POINTER(c_void_p)))[0]
    data = ctypes.string_at(ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vtable[3])(blob), ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vtable[4])(blob))
    ctypes.WINFUNCTYPE(ctypes.c_ulong, c_void_p)(vtable[2])(blob)
    return data


def code_hash(d3d, stage, relative, defines):
    """Hash of the preprocessed code without #line bookkeeping, or None if preprocessing fails."""
    source = (stage / relative).read_bytes()
    macros = (Macro * (len(defines) + 1))()
    for i, define in enumerate(defines):
        name, _, value = define.partition("=")
        macros[i] = Macro(name.encode(), (value or "1").encode())
    macros[len(defines)] = Macro(None, None)
    out, err = c_void_p(), c_void_p()
    hr = d3d.D3DPreprocess(source, len(source), relative.as_posix().encode(), ctypes.cast(macros, c_void_p), c_void_p(STANDARD_FILE_INCLUDE), byref(out), byref(err))
    if err.value:
        take_blob(err.value)
    if hr < 0 or not out.value:
        return None
    text = take_blob(out.value)
    return hashlib.sha1(b"".join(line for line in text.splitlines(True) if not line.startswith(b"#line"))).hexdigest()


def stage_shaders():
    """Merged Data/Shaders layout, built by the same function the prebuilt cache tooling uses."""
    spec = importlib.util.spec_from_file_location("build_shader_cache", REPO / "tools" / "build-shader-cache.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    stage = Path(tempfile.mkdtemp(prefix="flip-impact-")) / "Shaders"
    module.stage_merged_shaders(stage)
    return stage


def collect_variants(config, stage):
    variants = []
    for shader in config["shaders"]:
        relative = Path(shader["file"])
        if not (stage / relative).exists():
            continue
        for shader_type, group in shader["configs"].items():
            if shader_type == "CSHADER":
                continue
            for entry in group["entries"]:
                defines = sorted(d for d in set(group["common_defines"]) | set(entry["defines"]) if not d.startswith("D3DCOMPILE_"))
                variants.append((shader["file"], entry["entry"], shader_type, relative, defines))
    return variants


def candidate_defines(config):
    found = {d for shader in config["shaders"] for group in shader["configs"].values() for d in group["common_defines"]}
    return sorted(d for d in found if "=" not in d and d not in PLATFORM_DEFINES and not d.startswith("D3DCOMPILE_"))


def mean_by_file_and_type(report):
    """Average compile seconds per (file, shader type), for entries the report has no timing for."""
    sums = {}
    for t in report:
        total, count = sums.get((t["file"], t["type"]), (0.0, 0))
        sums[(t["file"], t["type"])] = (total + t["duration_seconds"], count + 1)
    return {key: total / count for key, (total, count) in sums.items()}


def group_share(variants, before_hashes, after_hashes, timings, type_means):
    """Share of compile time in groups of identical code (per shader type) that some member changes after a flip, and the changed group count."""
    groups = {}
    for variant, before, after in zip(variants, before_hashes, after_hashes):
        seconds = timings.get((variant[0], variant[1]), type_means.get((variant[0], variant[2])))
        if before is None or after is None or seconds is None:
            continue
        group = groups.setdefault(variant[2] + ":" + before, {"cost": seconds, "changed": False})
        group["cost"] = min(group["cost"], seconds)
        group["changed"] = group["changed"] or before != after
    total = sum(g["cost"] for g in groups.values())
    changed = [g for g in groups.values() if g["changed"]]
    return (sum(g["cost"] for g in changed) / total if total > 0 else None), len(changed)


def flip_impact(d3d, stage, variants, timings, type_means, defines, workers):
    def hash_all(drop):
        with ThreadPoolExecutor(workers) as pool:
            return list(pool.map(lambda v: code_hash(d3d, stage, v[3], [d for d in v[4] if d != drop]), variants))

    with_define = hash_all(None)
    impact = {}
    for define in defines:
        share, changed = group_share(variants, with_define, hash_all(define), timings, type_means)
        if share is not None:
            impact[define] = {"share": round(share, 3), "groups": changed}
    return impact


def merge_tables(paths):
    """Union of the runtimes in several tables; tables with an unknown schema are skipped."""
    document = {"schemaVersion": SCHEMA_VERSION, "runtimes": {}}
    for path in paths:
        table = json.loads(path.read_text(encoding="utf-8"))
        if table.get("schemaVersion") != SCHEMA_VERSION:
            print(f"skipping {path}: unsupported schema {table.get('schemaVersion')}", file=sys.stderr)
            continue
        document["runtimes"].update(table.get("runtimes", {}))
    return document


def write_table(document, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", choices=["SE", "VR"])
    parser.add_argument("--config", type=Path, help="validation config for this runtime")
    parser.add_argument("--timing", type=Path, help="hlslkit --timing-report JSON for this runtime")
    parser.add_argument("--merge", nargs="+", type=Path, metavar="TABLE", help="join per-runtime tables instead of generating one")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    args.output = args.output.resolve()

    if args.merge:
        document = merge_tables(args.merge)
        write_table(document, args.output)
        print(f"merged {sorted(document['runtimes'])} -> {args.output}")
        return 0
    if not (args.runtime and args.config and args.timing):
        parser.error("--runtime, --config and --timing are required unless --merge is given")

    config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    report = json.loads(args.timing.read_text(encoding="utf-8"))
    timings = {(t["file"], t["entry"]): t["duration_seconds"] for t in report}
    type_means = mean_by_file_and_type(report)
    stage = stage_shaders()
    os.chdir(stage)  # the standard include handler resolves includes against the working directory
    d3d = load_preprocessor()
    variants = collect_variants(config, stage)
    impact = flip_impact(d3d, stage, variants, timings, type_means, candidate_defines(config), args.workers)

    document = {"schemaVersion": SCHEMA_VERSION, "runtimes": {}}
    if args.output.exists():
        existing = json.loads(args.output.read_text(encoding="utf-8"))
        if existing.get("schemaVersion") == SCHEMA_VERSION:
            document = existing
    document["runtimes"][args.runtime] = impact
    write_table(document, args.output)
    print(f"{args.runtime}: {len(impact)} defines from {len(variants)} variants -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
