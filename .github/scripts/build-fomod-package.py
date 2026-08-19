#!/usr/bin/env python3
"""Stage a FOMOD-wrapped AIO package: the plain AIO tree as required files,
plus each available shader cache as a mutually-exclusive optional install
step, so a mod manager installs only the cache matching the player's runtime
instead of the player having to pick and drop in the right archive by hand.

Mod metadata and cache-variant definitions live in
.github/configs/fomod-metadata.yaml, not here; the mod name and Nexus mod ID
(also needed by nexus-upload.yaml) live in .github/configs/project.yaml
instead, so both stay in sync. This script is just the logic that turns
that config into FOMOD XML via pyfomod.

Each option's type (Recommended vs Optional) is gameDependency-conditioned
so the mod manager pre-selects the matching runtime automatically, same
mechanism powerof3's mods (e.g. po3-Tweaks) use across SE/AE/VR variants --
see pack-skse-mod's make-fomod.yml. gameDependency has no comparison
operator or negation, only "installed game version >= X"; each option's
pattern list checks every variant's threshold ordered highest-to-lowest so
the first (most specific) match wins, rather than a lower/looser threshold
also matching and overriding it. cache_variants in the config must stay
ordered highest game_version first for this to hold.

Usage:
    build-fomod-package.py --core DIR --output DIR --version VER
        [--se-cache DIR] [--vr-cache DIR] [--config PATH]

DIR arguments are extracted trees (not archives); --output is a staging
directory this script creates fresh -- the caller 7z's it afterward.
"""

import argparse
import shutil
import sys
from pathlib import Path

import pyfomod
import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CONFIGS_DIR = REPO_ROOT / ".github" / "configs"
DEFAULT_CONFIG = CONFIGS_DIR / "fomod-metadata.yaml"
DEFAULT_PROJECT_CONFIG = CONFIGS_DIR / "project.yaml"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path, help="Extracted AIO tree (required install files)")
    parser.add_argument("--output", required=True, type=Path, help="Staging directory to create")
    parser.add_argument("--version", required=True, help="Mod version for fomod/info.xml")
    parser.add_argument("--se-cache", type=Path, help="Extracted SE/AE ShaderCache/ tree")
    parser.add_argument("--vr-cache", type=Path, help="Extracted VR ShaderCache/ tree")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="fomod-metadata.yaml path")
    parser.add_argument("--project-config", type=Path, default=DEFAULT_PROJECT_CONFIG, help="project.yaml path")
    return parser.parse_args()


def build_root(args, config, project, available_variants):
    mod = config["mod"]
    root = pyfomod.Root()
    # moduleName renders as the wizard's header on every page -- append the
    # version here too, not just in info.xml's <Version> (metadata some mod
    # managers only show in a separate panel, easy to miss during install).
    root.name = f"{project['mod_display_name']} {args.version}"
    root.author = mod["author"]
    root.version = args.version
    root.description = mod["description"]
    root.website = f"https://www.nexusmods.com/skyrimspecialedition/mods/{project['nexus_mod_id']}"
    root.files["Core/"] = "."
    if mod.get("header_image"):
        root.image = f"fomod/images/{Path(mod['header_image']).name}"

    if not available_variants:
        return root

    step = config["install_step"]
    page = pyfomod.Page()
    page.name = step["page_name"]

    group = pyfomod.Group()
    group.name = step["group_name"]
    # ATMOSTONE (not EXACTLYONE): skipping is valid -- the game just compiles
    # locally on first launch, same as it always has.
    group.type = pyfomod.GroupType.ATMOSTONE

    for variant in available_variants:
        option = pyfomod.Option()
        option.name = variant["name"]
        option.description = variant["description"]
        option.files[f"{variant['staging_subdir']}/ShaderCache/"] = "ShaderCache"

        option_type = pyfomod.Type()
        option_type.default = pyfomod.OptionType.OPTIONAL
        # All configured variants, not just available_variants: if one variant's
        # cache wasn't staged this run, its higher/lower threshold must still be
        # present here to keep intercepting so a surviving option's own (now
        # unopposed) threshold doesn't over-match a runtime it doesn't own.
        for other in config["cache_variants"]:
            conditions = pyfomod.Conditions()
            conditions[None] = other["game_version"]
            is_self = other["name"] == variant["name"]
            if is_self:
                # Only recommend this variant if it's also a fresh install --
                # an existing install's cache is only recommendable if its
                # settings are still default, which isn't checkable here.
                conditions[step["existing_install_marker"]] = pyfomod.FileType.MISSING
            option_type[conditions] = pyfomod.OptionType.RECOMMENDED if is_self else pyfomod.OptionType.OPTIONAL
        option.type = option_type

        group.append(option)

    page.append(group)
    root.pages.append(page)
    return root


def main():
    args = parse_args()

    if not args.core.is_dir():
        print(f"error: --core {args.core} is not a directory", file=sys.stderr)
        return 1

    config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    project = yaml.safe_load(args.project_config.read_text(encoding="utf-8"))

    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)

    shutil.copytree(args.core, args.output / "Core")

    available_variants = []
    for variant in config["cache_variants"]:
        cache_dir = getattr(args, variant["cli_arg"])
        if cache_dir is None:
            continue
        if not cache_dir.is_dir():
            arg_flag = "--" + variant["cli_arg"].replace("_", "-")
            print(f"error: {arg_flag} {cache_dir} is not a directory", file=sys.stderr)
            return 1
        shutil.copytree(cache_dir, args.output / variant["staging_subdir"] / "ShaderCache")
        available_variants.append(variant)

    root = build_root(args, config, project, available_variants)
    errors = root.validate()
    if errors:
        print("error: generated ModuleConfig.xml failed validation:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    pyfomod.write(root, str(args.output))

    header_image = config["mod"].get("header_image")
    if header_image:
        src = REPO_ROOT / header_image
        if not src.is_file():
            print(f"error: mod.header_image {src} does not exist", file=sys.stderr)
            return 1
        images_dir = args.output / "fomod" / "images"
        images_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, images_dir / src.name)

    print(f"Staged FOMOD package at {args.output} ({len(available_variants)} shader-cache option(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
