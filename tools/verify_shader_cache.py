#!/usr/bin/env python3
"""Check a release cache offline, then prove reuse during a fresh game session."""

import argparse
import configparser
import hashlib
import importlib.util
import json
import re
import struct
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("build_shader_cache", ROOT / "tools/build-shader-cache.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)
BLOB_STAGES = {".pso": 0, ".vso": 1, ".cso": 5}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def compare(label, expected, actual):
    differences = sorted(key for key in expected.keys() | actual.keys()
                         if expected.get(key) != actual.get(key))
    require(not differences, f"{label}: {len(differences)} mismatches: {', '.join(differences[:12])}")


def read_info(path):
    ini = configparser.ConfigParser()
    with path.open(encoding="utf-8-sig") as stream:
        ini.read_file(stream)
    return {section: dict(ini[section]) for section in ini.sections()}


def read_manifest(cache):
    manifest = json.loads((cache / "Manifest.json").read_text(encoding="utf-8-sig"))
    require(isinstance(manifest, dict), "Cache manifest must be an object")
    require(manifest.get("schemaVersion") == 1, "Unknown cache manifest schema")
    entries = manifest.get("entries")
    require(isinstance(entries, dict) and entries, "Cache manifest is empty or invalid")
    require(all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{32}", value)
                for value in entries.values()), "Invalid manifest digest")
    return entries


def blob_paths(cache):
    paths = {path.relative_to(cache).as_posix(): path for path in cache.rglob("*")
             if path.is_file() and path.suffix in BLOB_STAGES}
    require(paths, f"No shader blobs in {cache}")
    return paths


def check_dxbc(path):
    data = path.read_bytes()
    require(len(data) >= 32 and data[:4] == b"DXBC", f"Invalid DXBC: {path}")
    version, size, count = struct.unpack_from("<III", data, 20)
    require(version == 1 and size == len(data) and 0 < count <= (size - 32) // 4,
            f"Invalid DXBC header: {path}")
    shader_found = False
    for offset in struct.unpack_from(f"<{count}I", data, 32):
        require(32 + count * 4 <= offset <= size - 8, f"Invalid DXBC chunk: {path}")
        length = struct.unpack_from("<I", data, offset + 4)[0]
        require(offset + 8 + length <= size, f"Truncated DXBC chunk: {path}")
        if data[offset:offset + 4] in (b"SHEX", b"SHDR"):
            require(length >= 8, f"Empty shader program: {path}")
            token = struct.unpack_from("<I", data, offset + 8)[0]
            require(token >> 16 == BLOB_STAGES[path.suffix], f"Wrong shader stage: {path}")
            shader_found = True
    require(shader_found, f"Missing shader program: {path}")


def verify_artifact(cache, shaders, source_root, runtime, plugin_version=None):
    """Validate every captured permutation against the current default AIO profile."""
    from hlslkit.shader_digest import combine_hashes, compute_shader_content_digest, hash_string, to_hex

    require(sys.platform == "win32", "Artifact validation requires Windows include ordering")
    strip, include, disabled = builder.profile_strip_defines(source_root, "aio")
    config = source_root / ".github/configs" / (
        "shader-validation-vr.yaml" if runtime == "VR" else "shader-validation.yaml")
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        profile = builder.filter_profile_defines(config, temporary / "profile.yaml", strip, source_root)
        permutations = builder.cache_shader_permutations(profile, runtime)
        required_features = include - builder.RUNTIME_EXCLUDED_FEATURES[runtime]
        present_features = {path.stem for path in (shaders / "Features").glob("*.ini")}
        require(not required_features - present_features,
                f"Missing packaged feature INIs: {sorted(required_features - present_features)}")
        builder.write_info_ini(temporary, shaders,
                               plugin_version or builder.default_plugin_version(source_root),
                               runtime, include, disabled)
        compare("Feature profile / Info.ini", read_info(temporary / "Info.ini"), read_info(cache / "Info.ini"))

    blobs = blob_paths(cache)
    compare("Captured permutation coverage", dict.fromkeys(permutations, True), dict.fromkeys(blobs, True))
    entries = read_manifest(cache)
    compare("Manifest coverage", dict.fromkeys(blobs, True), dict.fromkeys(entries, True))
    global_digest = hash_string("VR;" if runtime == "VR" else "")
    source_digests = {}
    expected = {}
    for relative, (source, key) in permutations.items():
        if source not in source_digests:
            digest = compute_shader_content_digest(shaders / source, shaders)
            require(digest is not None, f"Cannot hash packaged shader {source}")
            source_digests[source] = combine_hashes(digest, global_digest)
        expected[relative] = to_hex(combine_hashes(source_digests[source], hash_string(key)))
    compare("Runtime content / permutation digests", expected, entries)
    for path in blobs.values():
        check_dxbc(path)
    return {"runtime": runtime, "entries": len(entries), "defaultDisabledFeatures": sorted(disabled & include)}


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cache_snapshot(cache):
    blobs = blob_paths(cache)
    entries = read_manifest(cache)
    compare("Manifest coverage", dict.fromkeys(blobs, True), dict.fromkeys(entries, True))
    return {"entries": entries, "info": read_info(cache / "Info.ini"),
            "blobs": {name: {"sha256": file_hash(path), "mtime_ns": path.stat().st_mtime_ns}
                      for name, path in blobs.items()}}


def installation_hashes(data):
    dll = data / "SKSE/Plugins/CommunityShaders.dll"
    require(dll.is_file(), f"Missing plugin: {dll}")
    paths = [dll] + [path for path in (data / "Shaders").rglob("*") if path.is_file()]
    require(len(paths) > 1, "Installed shader tree is missing")
    return {path.relative_to(data).as_posix(): file_hash(path) for path in paths}


def take_snapshot(data, reference, log, runtime):
    """Require the installed cache to match the fresh build before the game starts."""
    require(reference.resolve() != (data / "ShaderCache").resolve(),
            "Reference cache must be a separate fresh build artifact")
    installed = cache_snapshot(data / "ShaderCache")
    shipped = cache_snapshot(reference)
    for label in ("entries", "info"):
        compare(f"Installed vs shipped {label}", shipped[label], installed[label])
    compare("Installed vs shipped bytecode",
            {k: v["sha256"] for k, v in shipped["blobs"].items()},
            {k: v["sha256"] for k, v in installed["blobs"].items()})
    old_log = log.read_text(encoding="utf-8-sig", errors="replace") if log.exists() else ""
    return {"schemaVersion": 1, "runtime": runtime, "data": str(data.resolve()), "log": str(log.resolve()),
            "previousLogStart": old_log.splitlines()[0] if old_log else "",
            "installation": installation_hashes(data), "cache": installed, "created_ns": time.time_ns()}


def verify_log(text, snapshot, modified_ns):
    require(modified_ns > snapshot["created_ns"], "The game log predates the snapshot")
    lines = text.splitlines()
    require(lines and lines[0] != snapshot["previousLogStart"], "No fresh game session after the snapshot")
    version = snapshot["cache"]["info"]["Cache"]["pluginversion"]
    logged_version = re.search(r"\bCommunityShaders v(\S+)", lines[0])
    require(logged_version and logged_version.group(1) == version,
            "Game log does not match the installed plugin version")
    failures = re.findall(
        r"^.*(?:Shader compilation (?:started|completed)|Disk cache mismatch|Disk cache HELD|"
        r"Partial disk cache invalidation|Disk-cached.*outdated|Compiling Task failed|"
        r"Unhandled .*compiling shader).*$", text, re.MULTILINE)
    require(not failures, "Runtime compiled or invalidated cache entries:\n" + "\n".join(failures[:12]))
    require("Using disk cache" in text, "Runtime did not accept the shipped feature profile")


def check_runtime_status(status):
    require(status.get("compiling") is False, "Shaders are still compiling")
    for key in ("failedTasks", "currentFailedCount", "digestMissTasks", "totalTasks", "completedTasks",
                "diskHitTasks", "digestHitTasks"):
        require(type(status.get(key)) is int and status[key] >= 0, f"Invalid live counter: {key}")
    for key in ("failedTasks", "currentFailedCount", "digestMissTasks"):
        require(status.get(key) == 0, f"{key} must be zero, got {status.get(key)}")
    total = status.get("totalTasks", 0)
    require(total > 0 and status.get("completedTasks") == total, "No completed shader workload")
    require(status.get("diskHitTasks") == total, "Some shader tasks were not disk-cache hits")
    require(status.get("digestHitTasks", 0) > 0, "No content-digest cache hits")
    require(status.get("cacheMismatches") == [], "Live feature profile differs from the cache")
    for key in ("diskCacheHeld", "featureSetChanged", "featureSetRevertPending"):
        require(status.get(key) is False, f"Unexpected live cache state: {key}")


def live_status(endpoint, runtime):
    """Probe the requested live host before reading its Open Shaders counters."""
    def call(tool, kind):
        request = urllib.request.Request(endpoint.rstrip("/") + "/api/tool/" + tool,
                                         data=json.dumps({"kind": kind}).encode(),
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=10) as response:
            value = json.load(response)
        require(isinstance(value, dict) and not value.get("error"), f"devbench {tool}: {value}")
        return value

    host = call("inspect", "state")
    require(host.get("vr") is (runtime == "VR"), "Wrong live Skyrim runtime")
    require(host.get("playerLoaded") is True, "Load the test save before verifying")
    before = call("inspect", "openshaders")
    require(before.get("plugin") == "CommunityShaders", "Open Shaders is not available on this host")
    time.sleep(0.3)
    after = call("inspect", "openshaders")
    require(after.get("plugin") == "CommunityShaders" and after.get("vr") is (runtime == "VR") and
            after.get("frame_count", 0) > before.get("frame_count", 0), "Live frames are not advancing")
    status = call("inspect", "shadercache")
    check_runtime_status(status)
    return status


def verify_session(snapshot, endpoint):
    require(snapshot.get("schemaVersion") == 1, "Unknown snapshot schema")
    data, log = Path(snapshot["data"]), Path(snapshot["log"])
    initial_log = log.read_text(encoding="utf-8-sig", errors="replace")
    verify_log(initial_log, snapshot, log.stat().st_mtime_ns)
    status = live_status(endpoint, snapshot["runtime"])
    compare("Installed plugin / shaders changed during test", snapshot["installation"], installation_hashes(data))
    current = cache_snapshot(data / "ShaderCache")
    for label in ("entries", "info", "blobs"):
        compare(f"Cache {label} changed during game session", snapshot["cache"][label], current[label])
    final_status = live_status(endpoint, snapshot["runtime"])
    counters = ("totalTasks", "completedTasks", "diskHitTasks", "digestHitTasks", "digestMissTasks")
    compare("Shader workload changed during inspection; rerun once it is idle",
            {key: status[key] for key in counters}, {key: final_status[key] for key in counters})
    final_log = log.read_text(encoding="utf-8-sig", errors="replace")
    verify_log(final_log, snapshot, log.stat().st_mtime_ns)
    require(final_log.splitlines()[0] == initial_log.splitlines()[0], "Game session changed during inspection")
    return {"runtime": snapshot["runtime"], "unchangedEntries": len(current["entries"]),
            "diskHitTasks": status["diskHitTasks"], "digestMissTasks": status["digestMissTasks"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    artifact = commands.add_parser("artifact", help="Validate an extracted default AIO cache on Windows")
    artifact.add_argument("--cache", type=Path, required=True)
    artifact.add_argument("--shaders", type=Path, required=True)
    artifact.add_argument("--source-root", type=Path, default=ROOT)
    artifact.add_argument("--runtime", choices=["SE", "VR"], required=True)
    artifact.add_argument("--plugin-version")
    snapshot = commands.add_parser("snapshot", help="Before launch: snapshot a fresh installed build")
    snapshot.add_argument("--data", type=Path, required=True)
    snapshot.add_argument("--reference-cache", type=Path, required=True)
    snapshot.add_argument("--log", type=Path, required=True)
    snapshot.add_argument("--runtime", choices=["SE", "VR"], required=True)
    snapshot.add_argument("--out", type=Path, required=True)
    verify = commands.add_parser("session", help="After loading the test save: verify live cache reuse")
    verify.add_argument("--snapshot", type=Path, required=True)
    verify.add_argument("--endpoint", required=True, help="Live devbench base URL, e.g. http://127.0.0.1:8920")
    args = parser.parse_args()
    try:
        if args.command == "artifact":
            result = verify_artifact(args.cache, args.shaders, args.source_root, args.runtime, args.plugin_version)
        elif args.command == "snapshot":
            result = take_snapshot(args.data, args.reference_cache, args.log, args.runtime)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
            result = {"snapshot": str(args.out), "entries": len(result["cache"]["entries"])}
        else:
            result = verify_session(json.loads(args.snapshot.read_text(encoding="utf-8")), args.endpoint)
    except (OSError, ValueError, KeyError, configparser.Error) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("PASS: " + json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
