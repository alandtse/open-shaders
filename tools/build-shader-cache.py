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


def feature_define_map(source_root: Path) -> dict:
    """shortName -> shader define, parsed from src/Features headers (empty define
    if the feature declares none)."""
    import re

    out = {}
    for h in sorted((source_root / "src/Features").rglob("*.h")):
        text = h.read_text(encoding="utf-8", errors="replace")
        short = re.search(r'GetShortName\(\)[^{]*\{\s*return\s+"(\w+)"', text)
        if not short:
            continue
        define = re.search(r'GetShaderDefineName\(\)[^{]*\{\s*return\s+"(\w+)"', text)
        out[short.group(1)] = define.group(1) if define else ""
    return out


def default_disabled_features(source_root: Path) -> set:
    """Feature short names with IsDisabledByDefault() == true, parsed from headers."""
    import re

    out = set()
    for h in sorted((source_root / "src/Features").rglob("*.h")):
        text = h.read_text(encoding="utf-8", errors="replace")
        if re.search(r"IsDisabledByDefault\(\)[^{]*\{\s*return\s+true", text):
            short = re.search(r'GetShortName\(\)[^{]*\{\s*return\s+"(\w+)"', text)
            if short:
                out.add(short.group(1))
    return out


def aio_feature_stems(source_root: Path) -> set:
    """Feature ini stems shipped in the AIO: CORE marker present or autoupload=true."""
    stems = set()
    for feature_dir in sorted((source_root / "features").iterdir()):
        inis = list(feature_dir.glob("Shaders/Features/*.ini"))
        if not inis:
            continue
        is_core = (feature_dir / "CORE").exists()
        auto = any(line.split("=", 1)[1].split(";", 1)[0].strip().lower() == "true"
            for ini in inis for line in ini.read_text(encoding="utf-8-sig", errors="replace").splitlines()
            if line.lower().replace(" ", "").startswith("autoupload="))
        if is_core or auto:
            stems.update(i.stem for i in inis)
    return stems

IMAGESPACE_DIRS = {
    # (SE enum, VR enum) -> runtime fxpFilename dir; from RE/I/ImageSpaceManager.h X/X2 macros.
    (0, 0): "WorldMap",
    (1, 1): "Refraction",
    (2, 2): "ISFXAA",
    (3, 3): "DepthOfField",
    (5, 5): "RadialBlur",
    (6, 6): "FullScreenBlur",
    (7, 7): "GetHit",
    (8, 8): "Map",
    (9, 9): "Blur3",
    (10, 10): "Blur5",
    (11, 11): "Blur7",
    (12, 12): "Blur9",
    (13, 13): "Blur11",
    (14, 14): "Blur13",
    (15, 15): "Blur15",
    (16, 16): "BlurNonHDR3",
    (17, 17): "BlurNonHDR5",
    (18, 18): "BlurNonHDR7",
    (19, 19): "BlurNonHDR9",
    (20, 20): "BlurNonHDR11",
    (21, 21): "BlurNonHDR13",
    (22, 22): "BlurNonHDR15",
    (23, 23): "BlurBrightPass3",
    (24, 24): "BlurBrightPass5",
    (25, 25): "BlurBrightPass7",
    (26, 26): "BlurBrightPass9",
    (27, 27): "BlurBrightPass11",
    (28, 28): "BlurBrightPass13",
    (29, 29): "BlurBrightPass15",
    (30, 30): "HDR",
    (31, 31): "WaterDisplacement",
    (32, 32): "VolumetricLighting",
    (33, 33): "Noise",
    (34, 34): "ISCopy",
    (35, 35): "ISCopyDynamicFetchDisabled",
    (36, 36): "ISCopyScaleBias",
    (37, 37): "ISCopyCustomViewport",
    (38, 38): "ISCopyGrayScale",
    (39, 39): "ISRefraction",
    (40, 40): "ISDoubleVision",
    (41, 41): "ISCopyTextureMask",
    (42, 42): "ISMap",
    (43, 43): "ISWorldMap",
    (44, 44): "ISWorldMapNoSkyBlur",
    (45, 45): "ISDepthOfField",
    (46, 46): "ISDepthOfFieldFogged",
    (47, 47): "ISDepthOfFieldMaskedFogged",
    (49, 49): "ISDistantBlur",
    (50, 50): "ISDistantBlurFogged",
    (51, 51): "ISDistantBlurMaskedFogged",
    (52, 52): "ISRadialBlur",
    (53, 53): "ISRadialBlurMedium",
    (54, 54): "ISRadialBlurHigh",
    (55, 55): "ISHDRTonemapBlendCinematic",
    (56, 56): "ISHDRTonemapBlendCinematicFade",
    (57, 57): "ISHDRDownSample16",
    (58, 58): "ISHDRDownSample4",
    (59, 59): "ISHDRDownSample16Lum",
    (60, 60): "ISHDRDownSample4RGB2Lum",
    (61, 61): "ISHDRDownSample4LumClamp",
    (62, 62): "ISHDRDownSample4LightAdapt",
    (63, 63): "ISHDRDownSample16LumClamp",
    (64, 64): "ISHDRDownSample16LightAdapt",
    (65, 65): "ISBlur3",
    (66, 66): "ISBlur5",
    (67, 67): "ISBlur7",
    (68, 68): "ISBlur9",
    (69, 69): "ISBlur11",
    (70, 70): "ISBlur13",
    (71, 71): "ISBlur15",
    (72, 72): "ISNonHDRBlur3",
    (73, 73): "ISNonHDRBlur5",
    (74, 74): "ISNonHDRBlur7",
    (75, 75): "ISNonHDRBlur9",
    (76, 76): "ISNonHDRBlur11",
    (77, 77): "ISNonHDRBlur13",
    (78, 78): "ISNonHDRBlur15",
    (79, 79): "ISBrightPassBlur3",
    (80, 80): "ISBrightPassBlur5",
    (81, 81): "ISBrightPassBlur7",
    (82, 82): "ISBrightPassBlur9",
    (83, 83): "ISBrightPassBlur11",
    (84, 84): "ISBrightPassBlur13",
    (85, 85): "ISBrightPassBlur15",
    (86, 86): "ISWaterDisplacementClearSimulation",
    (87, 87): "ISWaterDisplacementTexOffset",
    (88, 88): "ISWaterDisplacementWadingRipple",
    (89, 89): "ISWaterDisplacementRainRipple",
    (90, 90): "ISWaterWadingHeightmap",
    (91, 91): "ISWaterRainHeightmap",
    (92, 92): "ISWaterBlendHeightmaps",
    (93, 93): "ISWaterSmoothHeightmap",
    (94, 94): "ISWaterDisplacementNormals",
    (95, 95): "ISNoiseScrollAndBlend",
    (96, 96): "ISNoiseNormalmap",
    (97, 97): "ISVolumetricLighting",
    (98, 101): "ISLocalMap",
    (99, 102): "ISAlphaBlend",
    (100, 103): "ISLensFlare",
    (101, 104): "ISLensFlareVisibility",
    (102, 105): "ISApplyReflections",
    (103, 106): "ISApplyVolumetricLighting",
    (104, 107): "ISBasicCopy",
    (105, 108): "ISBlur",
    (106, 109): "ISVolumetricLightingBlurHCS",
    (107, 110): "ISVolumetricLightingBlurVCS",
    (108, 111): "ISReflectionBlurHCS",
    (109, 112): "ISReflectionBlurVCS",
    (110, 113): "ISParallaxMaskBlurHCS",
    (111, 114): "ISParallaxMaskBlurVCS",
    (112, 115): "ISDepthOfFieldBlurHCS",
    (113, 116): "ISDepthOfFieldBlurVCS",
    (114, 117): "ISCompositeVolumetricLighting",
    (115, 118): "ISCompositeLensFlare",
    (116, 119): "ISCompositeLensFlareVolumetricLighting",
    (117, 120): "ISCopySubRegionCS",
    (118, 121): "ISDebugSnow",
    (119, 122): "ISDownsample",
    (120, 123): "ISDownsampleIgnoreBrightest",
    (121, 124): "ISDownsampleCS",
    (122, 125): "ISDownsampleIgnoreBrightestCS",
    (123, 128): "ISExp",
    (124, 130): "ISIBLensFlares",
    (125, 131): "ISLightingComposite",
    (126, 132): "ISLightingCompositeNoDirectionalLight",
    (127, 133): "ISLightingCompositeMenu",
    (128, 134): "ISPerlinNoiseCS",
    (129, 135): "ISPerlinNoise2DCS",
    (130, 145): "ReflectionsRayTracing",
    (131, 146): "ISReflectionsDebugSpecMask",
    (132, 147): "ISSAOBlurH",
    (133, 148): "ISSAOBlurV",
    (134, 149): "ISSAOBlurHCS",
    (135, 150): "ISSAOBlurVCS",
    (136, 151): "ISSAOCameraZ",
    (137, 152): "ISSAOCameraZAndMipsCS",
    (138, 153): "ISSAOCompositeSAO",
    (139, 154): "ISSAOCompositeFog",
    (140, 155): "ISSAOCompositeSAOFog",
    (141, 156): "ISMinify",
    (142, 157): "ISMinifyContrast",
    (143, 158): "ISSAORawAO",
    (144, 159): "ISSAORawAONoTemporal",
    (145, 160): "ISSAORawAOCS",
    (146, 161): "ISSILComposite",
    (147, 162): "ISSILRawInd",
    (148, 163): "ISSimpleColor",
    (149, 164): "ISDisplayDepth",
    (150, 165): "ISSnowSSS",
    (151, 166): "ISTemporalAA",
    (152, 167): "ISTemporalAA_UI",
    (153, 168): "ISTemporalAA_Water",
    (154, 169): "ISUpsampleDynamicResolution",
    (155, 170): "ISWaterBlend",
    (156, 171): "ISUnderwaterMask",
    (157, 172): "ISWaterFlow",
}



def write_info_ini(cache_dir: Path, stage: Path, plugin_version: str, runtime: str, include_stems=None) -> int:
    """Emit Info.ini matching ShaderCache::WriteDiskCacheInfo's output format.
    include_stems limits sections to the target profile (AIO default install)."""
    lines = ["[Cache]", f"PluginVersion = {plugin_version}", "", ""]
    count = 0
    for ini_path in sorted((stage / "Features").glob("*.ini")):
        if ini_path.stem in RUNTIME_EXCLUDED_FEATURES[runtime]:
            continue
        if include_stems is not None and ini_path.stem not in include_stems:
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


def filter_profile_defines(config: Path, out: Path, drop: set) -> Path:
    """Strip the given defines from every define list in the config."""
    import yaml

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


def remap_imagespace_dirs(cache_dir: Path, runtime: str) -> None:
    """hlslkit names output dirs by source-file stem, but the runtime reads
    ImageSpace blobs from per-technique dirs named by fxpFilename, selected by
    the descriptor (= ImageSpaceManager effect enum, which differs SE vs VR for
    X2 entries). Move each IS blob to its technique dir; verified byte-identical
    naming against runtime-written caches on both runtimes."""
    idx = 1 if runtime == "VR" else 0
    by_desc = {k[idx]: v for k, v in IMAGESPACE_DIRS.items()}
    keep = {".pso", ".vso", ".cso"}
    for d in sorted(cache_dir.iterdir()):
        if not d.is_dir() or not d.name.startswith("IS"):
            continue
        for f in sorted(d.iterdir()):
            if f.suffix.lower() not in keep:
                continue
            try:
                desc = int(f.stem, 16)
            except ValueError:
                continue
            target = by_desc.get(desc)
            if target and target != d.name:
                dest = cache_dir / target
                dest.mkdir(exist_ok=True)
                f.replace(dest / f.name)
        if not any(d.iterdir()):
            d.rmdir()


def profile_strip_defines(source_root: Path, profile: str) -> tuple:
    """(strip_defines, include_stems): defines absent from the target profile's
    runtime (non-AIO features + default-disabled), and the manifest stems."""
    defines = feature_define_map(source_root)
    disabled = default_disabled_features(source_root) or set(DEFAULT_DISABLED_FEATURES)
    all_stems = {p.stem for p in (source_root / "features").glob("*/Shaders/Features/*.ini")}
    include = aio_feature_stems(source_root) if profile == "aio" else all_stems
    excluded = (all_stems - include) | disabled
    strip = {defines[stem] for stem in excluded if defines.get(stem)}
    return strip, include


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


def finalize_existing(cache_dir: Path, shaders: Path, plugin_version: str, runtime: str, profile: str) -> int:
    """Turn a validation-produced compile dir into a shippable cache. The compile
    itself must have used the profile config (--emit-profile-config), so this only
    prunes sidecars, remaps ImageSpace dirs, and writes the profile manifest."""
    _, include = profile_strip_defines(REPO, profile)
    prune_non_cache_files(cache_dir)
    remap_imagespace_dirs(cache_dir, runtime)
    n = write_info_ini(cache_dir, shaders, plugin_version, runtime, include)
    blobs = sum(1 for p in cache_dir.rglob("*") if p.suffix in (".pso", ".vso", ".cso"))
    print(f"{runtime}: finalized {blobs} cache blobs, Info.ini with {n} feature sections -> {cache_dir}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-version", help='Plugin::VERSION string, e.g. "1-7-1-0" (default: derived from CMakeLists.txt)')
    ap.add_argument("--finalize-existing", help="finalize an already-compiled cache dir (from CI shader validation) instead of compiling")
    ap.add_argument("--shader-dir", help="merged shader tree used for --finalize-existing (e.g. build/ALL/aio/Shaders)")
    ap.add_argument("--runtime", choices=["SE", "VR", "both"], default="both")
    ap.add_argument("--profile", choices=["aio", "full"], default="aio",
        help="feature profile the cache targets; aio = default install (the cache is INVALID once any extra feature is added)")
    ap.add_argument("--emit-profile-config", nargs=2, metavar=("IN", "OUT"),
        help="write the profile-stripped copy of a validation config and exit (used by CI so one compile serves validation and the cache)")
    ap.add_argument("--source-root", help="repo checkout to take shaders/configs/version from (default: this repo; use a release-tag checkout to seed an old release)")
    ap.add_argument("--out", default="dist/shader-cache", help="output root")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--skip-compile", action="store_true", help="stage + Info.ini only (plumbing test)")
    args = ap.parse_args()
    args.jobs = max(1, args.jobs)

    global REPO, CONFIGS
    if args.source_root:
        REPO = Path(args.source_root).resolve()
        CONFIGS = {
            "SE": REPO / ".github/configs/shader-validation.yaml",
            "VR": REPO / ".github/configs/shader-validation-vr.yaml",
        }
    plugin_version = args.plugin_version or default_plugin_version()
    if args.emit_profile_config:
        strip, _ = profile_strip_defines(REPO, args.profile)
        filter_profile_defines(Path(args.emit_profile_config[0]), Path(args.emit_profile_config[1]), strip)
        print(f"profile config ({args.profile}) -> {args.emit_profile_config[1]}; stripped: {sorted(strip)}")
        return 0

    if args.finalize_existing:
        if not args.shader_dir or args.runtime == "both":
            raise SystemExit("--finalize-existing requires --shader-dir and a single --runtime")
        return finalize_existing(Path(args.finalize_existing), Path(args.shader_dir), plugin_version, args.runtime, args.profile)

    out_root = Path(args.out)
    stage = out_root / "staged-shaders"
    stage_merged_shaders(stage)
    print(f"staged merged shader tree: {stage}")

    runtimes = ["SE", "VR"] if args.runtime == "both" else [args.runtime]
    for rt in runtimes:
        cache_dir = out_root / rt / "ShaderCache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        if not args.skip_compile:
            strip, _ = profile_strip_defines(REPO, args.profile)
            config = filter_profile_defines(CONFIGS[rt], out_root / f"config-{rt}.yaml", strip)
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
            remap_imagespace_dirs(cache_dir, rt)
        _, include = profile_strip_defines(REPO, args.profile)
        n = write_info_ini(cache_dir, stage, plugin_version, rt, include)
        blobs = sum(1 for _ in cache_dir.rglob("*") if _.suffix in (".pso", ".vso", ".cso"))
        print(f"{rt}: {blobs} cache blobs, Info.ini with {n} feature sections -> {cache_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
