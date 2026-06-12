#!/usr/bin/env python3
"""Generate permutation lists for verify-shader-refactor.ps1 / shader-compile-bench.ps1.

Reads the shader-validation YAML configs (the game-log-derived permutation matrix)
and emits, per shader and stage, plain-text permutation files: one space-separated
define set per line. These feed the verify script's -PermutationsFile (Tier 0/1
gates) and the bench script -- the verify script's built-in default sweep
(VR x HDR_OUTPUT) is far too weak for the big shaders.

Outputs per shader+stage:
  <out>/<Shader>.<STAGE>.full.txt        every entry from every config (deduped)
  <out>/<Shader>.<STAGE>.stratified.txt  small reproducible subset: every define
                                         symbol covered + known-hot combos + a
                                         fixed-seed sample

D3DCOMPILE_* pseudo-defines (hlslkit compile *flags*, not preprocessor defines)
are stripped. Pass the produced file to fxc-based tools with the matching
profile (PSHADER->ps_5_0, VSHADER->vs_5_0, CSHADER->cs_5_0), entry "main".

Usage:
  python tools/gen-verify-perms.py [--shader Lighting.hlsl] [--out build/verify-perms]
         [--config <yaml> ...] [--stratified-size 24]
"""

import argparse
import random
import sys
from pathlib import Path

import yaml

# Define groups that dominate compile cost (measured via [ShaderTiming] log data,
# see docs/development/shader-decomposition-plan.md). Every stratified list must
# cover each hot symbol that appears anywhere in the shader's full list.
HOT_SYMBOLS = [
    "MULTI_TEXTURE",
    "LANDSCAPE",
    "LOD_LAND_BLEND",
    "TRUE_PBR",
    "ANISO_LIGHTING",
    "GLINT",
    "DEFERRED",
]


def strip_pseudo(defines):
    return [d for d in defines if not d.startswith("D3DCOMPILE_")]


def load_perms(config_paths, shader_file):
    """Return {stage: [frozenset(defines), ...]} merged across configs, order-preserving."""
    perms = {}
    seen = {}
    for cfg_path in config_paths:
        cfg = yaml.safe_load(open(cfg_path, encoding="utf-8"))
        for shader in cfg.get("shaders", []):
            if shader["file"] != shader_file:
                continue
            for stage, scfg in (shader.get("configs") or {}).items():
                common = strip_pseudo(scfg.get("common_defines") or [])
                for e in scfg.get("entries") or []:
                    defs = frozenset(common) | frozenset(strip_pseudo(e.get("defines") or []))
                    if defs not in seen.setdefault(stage, set()):
                        seen[stage].add(defs)
                        perms.setdefault(stage, []).append(defs)
    return perms


def symbol(define):
    return define.split("=", 1)[0]


def stratify(full, size, seed=0):
    """Greedy cover of every define symbol, then hot combos, then a seeded sample."""
    picked = []
    covered = set()
    all_symbols = {symbol(d) for p in full for d in p}

    # 1) Hot symbols first: take the LARGEST perm containing each (worst-case cost).
    for hot in HOT_SYMBOLS:
        if hot not in all_symbols:
            continue
        candidates = [p for p in full if any(symbol(d) == hot for d in p) and p not in picked]
        if candidates:
            best = max(candidates, key=len)
            picked.append(best)
            covered |= {symbol(d) for d in best}

    # 2) Greedy set-cover for the remaining symbols.
    while covered < all_symbols and len(picked) < size:
        best = max(
            (p for p in full if p not in picked),
            key=lambda p: len({symbol(d) for d in p} - covered),
            default=None,
        )
        if best is None or not ({symbol(d) for d in best} - covered):
            break
        picked.append(best)
        covered |= {symbol(d) for d in best}

    # 3) Fixed-seed fill so re-runs are reproducible.
    rng = random.Random(seed)
    pool = [p for p in full if p not in picked]
    rng.shuffle(pool)
    picked.extend(pool[: max(0, size - len(picked))])
    return picked


def fmt(perm):
    return " ".join(sorted(perm))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shader", action="append", help="shader file name(s), e.g. Lighting.hlsl; default: all")
    ap.add_argument(
        "--config",
        action="append",
        help="validation YAML(s); default: .github/configs/shader-validation.yaml + -vr.yaml",
    )
    ap.add_argument("--out", default="build/verify-perms", help="output directory")
    ap.add_argument("--stratified-size", type=int, default=24)
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    configs = args.config or [
        str(repo / ".github/configs/shader-validation.yaml"),
        str(repo / ".github/configs/shader-validation-vr.yaml"),
    ]

    shader_files = args.shader
    if not shader_files:
        names = set()
        for c in configs:
            for s in yaml.safe_load(open(c, encoding="utf-8")).get("shaders", []):
                names.add(s["file"])
        shader_files = sorted(names)

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    wrote = 0
    for sf in shader_files:
        perms = load_perms(configs, sf)
        if not perms:
            print(f"WARN: no entries for {sf} in {configs}", file=sys.stderr)
            continue
        stem = Path(sf).stem
        for stage, full in perms.items():
            strat = stratify(full, args.stratified_size)
            for kind, plist in (("full", full), ("stratified", strat)):
                path = out_dir / f"{stem}.{stage}.{kind}.txt"
                path.write_text("\n".join(fmt(p) for p in plist) + "\n", encoding="utf-8")
                wrote += 1
            print(f"{stem}.{stage}: full={len(full)} stratified={len(strat)}")
    if not wrote:
        sys.exit(1)


if __name__ == "__main__":
    main()
