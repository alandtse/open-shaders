#!/usr/bin/env python3
"""Build a distributable prebuilt shader disk cache (issue #148).

Produces the exact layout the runtime consumes at <GameData>/Data/ShaderCache/:
  ShaderCache/<ShaderName>/<descriptor:HEX>.{pso,vso,cso}   (raw DXBC)
  ShaderCache/Info.ini                                      (validation manifest)

Pipeline per runtime (SE and VR):
  1. Stage the merged shader tree (package/Shaders + every features/*/Shaders),
     mirroring the deployed Data/Shaders layout the game compiles against.
  2. hlslkit-compile with RELEASE-PARITY flags (--strip-debug-defines,
     --optimization-level 3) over the game-log-derived validation config. The
     validation configs' own D3DCOMPILE_DEBUG/SKIP_OPTIMIZATION pseudo-defines
     are stripped by --strip-debug-defines; without /O3 the blobs would not
     match what the runtime produces for users.
  3. Write Info.ini: [Cache] PluginVersion plus one section per staged feature
     ini (Enabled=true, Version from the ini). Validation is EXACT-MATCH on the
     loaded feature set, so the cache is valid only for a default full install;
     any feature uninstall/boot-disable falls back to a one-time recompile.

The game performs no bytecode comparison (loads any valid DXBC with
SKIP_VALIDATION), so fxc-built blobs are interchangeable with runtime
D3DCompile output. Per-shader staleness uses mtimes (source newer than cache
=> recompile): this script compiles after staging, so cache mtimes are always
newer, and 7z preserves them through mod-manager extraction.

Usage:
  python tools/build-shader-cache.py --plugin-version 1-7-1-0 [--runtime SE|VR|both]
      [--out dist/shader-cache] [--jobs N] [--skip-compile]

The plugin version must byte-match Plugin::VERSION.string() ("1-7-1-0" form,
visible in CommunityShaders.log as "Saved disk cache info (plugin version: X)").
"""

import argparse
import configparser
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CONFIGS = {
    "SE": REPO / ".github/configs/shader-validation.yaml",
    "VR": REPO / ".github/configs/shader-validation-vr.yaml",
}


def stage_merged_shaders(stage: Path) -> None:
    """Replicate the deployed Data/Shaders merge: package/Shaders + features/*/Shaders."""
    if stage.exists():
        shutil.rmtree(stage)
    shutil.copytree(REPO / "package/Shaders", stage)
    features_root = REPO / "features"
    for feature in sorted(features_root.iterdir()):
        shaders = feature / "Shaders"
        if shaders.is_dir():
            shutil.copytree(shaders, stage, dirs_exist_ok=True)


# Features that only register on one runtime: the runtime's Info.ini has no section
# for them at all on the other (verified against live SE and VR installs).
RUNTIME_EXCLUDED_FEATURES = {"SE": {"VR"}, "VR": set()}

# Features with IsDisabledByDefault() == true are NOT loaded on a default install:
# the manifest must say Enabled=false AND their global define must be stripped from
# every compiled permutation (feature defines change every shader's bytecode, and
# cache paths don't encode the feature set -- mismatched blobs would silently load).
DEFAULT_DISABLED_FEATURES = {"UnifiedWater": "UNIFIED_WATER"}


def write_info_ini(cache_dir: Path, stage: Path, plugin_version: str, runtime: str) -> int:
    """Emit Info.ini matching ShaderCache::WriteDiskCacheInfo's output format."""
    lines = ["[Cache]", f"PluginVersion = {plugin_version}", "", ""]
    count = 0
    for ini_path in sorted((stage / "Features").glob("*.ini")):
        if ini_path.stem in RUNTIME_EXCLUDED_FEATURES[runtime]:
            continue
        cp = configparser.ConfigParser()
        cp.read(ini_path, encoding="utf-8-sig")
        version = cp.get("Info", "Version", fallback=None)
        if not version:
            print(f"WARN: {ini_path.name} has no Info/Version; skipped", file=sys.stderr)
            continue
        if ini_path.stem in DEFAULT_DISABLED_FEATURES:
            lines += [f"[{ini_path.stem}]", "Enabled = false", "Version = ", "", ""]
        else:
            lines += [f"[{ini_path.stem}]", "Enabled = true", f"Version = {version}", "", ""]
        count += 1
    # UTF-8 BOM and CRLF to byte-match the runtime's SimpleIni output.
    (cache_dir / "Info.ini").write_bytes(
        b"\xef\xbb\xbf" + "\r\n".join(lines).encode("utf-8")
    )
    return count


def filter_default_disabled_defines(config: Path, out: Path) -> Path:
    """Strip default-disabled features' defines from every define list in the config."""
    import yaml

    drop = set(DEFAULT_DISABLED_FEATURES.values())
    cfg = yaml.safe_load(config.read_text(encoding="utf-8"))

    def scrub(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k in ("common_defines", "defines") and isinstance(v, list):
                    node[k] = [d for d in v if d.split("=", 1)[0] not in drop]
                else:
                    scrub(v)
        elif isinstance(node, list):
            for v in node:
                scrub(v)

    scrub(cfg)
    out.write_text(yaml.safe_dump(cfg, sort_keys=False), encoding="utf-8")
    return out


def prune_non_cache_files(cache_dir: Path) -> None:
    """hlslkit may emit logs/sidecars; ship only DXBC blobs + Info.ini."""
    keep = {".pso", ".vso", ".cso"}
    for p in cache_dir.rglob("*"):
        if p.is_file() and p.suffix.lower() not in keep and p.name != "Info.ini":
            p.unlink()
    for d in sorted((p for p in cache_dir.rglob("*") if p.is_dir()), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()


def default_plugin_version() -> str:
    """Derive Plugin::VERSION's dash form (X-Y-Z-0) from CMakeLists' project VERSION."""
    import re

    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"project\s*\([^)]*?VERSION\s+(\d+)\.(\d+)\.(\d+)", text, re.IGNORECASE | re.DOTALL)
    if not m:
        raise SystemExit("cannot derive plugin version from CMakeLists.txt; pass --plugin-version")
    return "-".join(m.groups()) + "-0"


def roots_referencing(shaders: Path, token: str) -> list[Path]:
    """Root .hlsl files whose include closure references token (identifier-bounded)."""
    import re

    inc_re = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
    tok_re = re.compile(r"\b" + re.escape(token) + r"\b")

    def refs(root: Path) -> bool:
        seen, queue = set(), [root]
        while queue:
            f = queue.pop()
            f = f.resolve()
            if f in seen or not f.exists():
                continue
            seen.add(f)
            text = f.read_text(encoding="utf-8", errors="replace")
            if tok_re.search(text):
                return True
            for line in text.splitlines():
                m = inc_re.match(line)
                if m:
                    for cand in (shaders / m.group(1), f.parent / m.group(1)):
                        if cand.exists():
                            queue.append(cand)
                            break
        return False

    return [p for p in sorted(shaders.glob("*.hlsl")) if refs(p)]


def finalize_existing(cache_dir: Path, shaders: Path, plugin_version: str, runtime: str, jobs: int) -> int:
    """Turn a validation-produced compile dir into a shippable cache: recompile the
    default-disabled features' referencing roots with their defines stripped (the
    validation configs keep them on for coverage), prune sidecars, write Info.ini."""
    for short, define in DEFAULT_DISABLED_FEATURES.items():
        affected = roots_referencing(shaders, define)
        print(f"{short} ({define}): re-profiling {len(affected)} roots: {[p.stem for p in affected]}")
        if not affected:
            continue
        config = filter_default_disabled_defines(CONFIGS[runtime], cache_dir.parent / f"config-default-{runtime}.yaml")
        for root in affected:
            cmd = [
                "hlslkit-compile",
                "--shader-dir", str(root),
                "--output-dir", str(cache_dir),
                "--config", str(config),
                "--strip-debug-defines",
                "--optimization-level", "3",
                "--suppress-warnings", "X1519",
                "--max-warnings", "999999",
                "--jobs", str(jobs),
            ]
            r = subprocess.run(cmd)
            if r.returncode != 0:
                print(f"re-profile failed for {root.name} (exit {r.returncode})", file=sys.stderr)
                return r.returncode
    prune_non_cache_files(cache_dir)
    n = write_info_ini(cache_dir, shaders, plugin_version, runtime)
    blobs = sum(1 for p in cache_dir.rglob("*") if p.suffix in (".pso", ".vso", ".cso"))
    print(f"{runtime}: finalized {blobs} cache blobs, Info.ini with {n} feature sections -> {cache_dir}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-version", help='Plugin::VERSION string, e.g. "1-7-1-0" (default: derived from CMakeLists.txt)')
    ap.add_argument("--finalize-existing", help="finalize an already-compiled cache dir (from CI shader validation) instead of compiling")
    ap.add_argument("--shader-dir", help="merged shader tree used for --finalize-existing (e.g. build/ALL/aio/Shaders)")
    ap.add_argument("--runtime", choices=["SE", "VR", "both"], default="both")
    ap.add_argument("--out", default="dist/shader-cache", help="output root")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--skip-compile", action="store_true", help="stage + Info.ini only (plumbing test)")
    args = ap.parse_args()
    args.jobs = max(1, args.jobs)

    plugin_version = args.plugin_version or default_plugin_version()
    if args.finalize_existing:
        if not args.shader_dir or args.runtime == "both":
            raise SystemExit("--finalize-existing requires --shader-dir and a single --runtime")
        return finalize_existing(Path(args.finalize_existing), Path(args.shader_dir), plugin_version, args.runtime, args.jobs)

    out_root = Path(args.out)
    stage = out_root / "staged-shaders"
    stage_merged_shaders(stage)
    print(f"staged merged shader tree: {stage}")

    runtimes = ["SE", "VR"] if args.runtime == "both" else [args.runtime]
    for rt in runtimes:
        cache_dir = out_root / rt / "ShaderCache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        if not args.skip_compile:
            config = filter_default_disabled_defines(CONFIGS[rt], out_root / f"config-{rt}.yaml")
            cmd = [
                "hlslkit-compile",
                "--shader-dir", str(stage),
                "--output-dir", str(cache_dir),
                "--config", str(config),
                "--strip-debug-defines",
                "--optimization-level", "3",
                # Cache build, not validation: known-benign warnings must not fail the job
                # (X1519 WATER macro redefinition is long-standing and suppressed in CI validation).
                "--suppress-warnings", "X1519",
                "--max-warnings", "999999",
                "--jobs", str(args.jobs),
            ]
            print("run:", " ".join(cmd))
            r = subprocess.run(cmd)
            if r.returncode != 0:
                print(f"hlslkit-compile failed for {rt} (exit {r.returncode})", file=sys.stderr)
                return r.returncode
            prune_non_cache_files(cache_dir)
        n = write_info_ini(cache_dir, stage, plugin_version, rt)
        blobs = sum(1 for _ in cache_dir.rglob("*") if _.suffix in (".pso", ".vso", ".cso"))
        print(f"{rt}: {blobs} cache blobs, Info.ini with {n} feature sections -> {cache_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
