#!/usr/bin/env python3
"""DX12 shadow offload validation driven through devbench (SE/AE only).

Proves, or disproves, that DX12 shadow offload engages and is worth its cost, on the
standard DragonsreachTorchThirdPerson route. Run against an already launched game you hold.

  ab        DX12 enabled at boot. Compares raster comparison mode (Mode 1) with shadow
            offload (Mode 0) in one session: readiness, ownership, screenshots, replay timing.
  baseline  DX12 disabled at boot. Captures the same screenshots and replay timing.
  compare   Merges an ab and a baseline output directory into one report.

Writes results.json, report.md and PNGs to --out. FAIL means offload is broken or never
engages; REVIEW means a metric needs a person to look at the listed images.
See docs/development/dx12-offload-validation.md.
"""
import argparse
import json
import math
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

CELL = "WhiterunDragonsreach"
RECORDING = "DragonsreachTorchThirdPerson.json"
RECORDINGS_DIR = Path("Data/SKSE/Plugins/devbench/recordings")
POSE_YAWS_DEG = (0, 120, 240)
POSE_RAISE = 120.0
POSE_PITCH_DEG = -12.0
SETTLE_SECONDS = 3.0
MODE_SETTLE_SECONDS = 8.0  # Cached shadow maps must redraw after a mode switch, or both shadow sources show at once.
MIN_LAYER_DARKER_FRACTION = 0.05
READY_TIMEOUT_SECONDS = 90
REPLAY_TIMEOUT_SECONDS = 120
SAMPLE_SECONDS = 1.0
DIFF_THRESHOLD = 8.0
MAX_DIFF_OVER_NOISE = 3.0
SHADOW_TIMER_KEYS = ("shadow", "scm", "llf")


class Devbench:
    def __init__(self, port):
        self.base = f"http://127.0.0.1:{port}/api/tool"

    def call(self, tool, body=None, timeout=30):
        request = urllib.request.Request(
            f"{self.base}/{tool}", data=json.dumps(body or {}).encode(), headers={"content-type": "application/json"}
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                text = response.read().decode()
        except urllib.error.HTTPError as error:
            raise RuntimeError(f"{tool} {body}: HTTP {error.code} {error.read().decode()[:200]}")
        return json.loads(text) if text else {}

    def status(self):
        result = self.call("openshaders.actions", {"action": "invokeQuery", "shortName": "DX12NativeScene", "name": "status"})
        return result.get("result", result)

    def set_mode(self, mode):
        self.call("openshaders.feature", {"action": "set", "shortName": "DX12", "settings": {"Mode": mode}})
        time.sleep(MODE_SETTLE_SECONDS)

    def dx12_settings(self):
        result = self.call("openshaders.feature", {"action": "get", "shortName": "DX12"})
        return result.get("settings", result)

    def set_dx12(self, settings):
        self.call("openshaders.feature", {"action": "set", "shortName": "DX12", "settings": settings})


def find_port(requested):
    for port in [requested] if requested else [8920, 8921, 8922, 8923]:
        try:
            return port, Devbench(port).call("inspect", {"kind": "state"}, timeout=5)
        except Exception:
            continue
    sys.exit("No devbench endpoint answered; launch the game first.")


class Report:
    def __init__(self, out, title):
        self.out, self.title = out, title
        self.checks, self.review, self.data = [], [], {}

    def check(self, name, status, detail):
        self.checks.append({"name": name, "status": status, "detail": detail})
        print(f"[{status}] {name}: {detail}", flush=True)

    def look(self, image, expectation):
        self.review.append({"image": str(image), "expect": expectation})

    def write(self, meta):
        (self.out / "results.json").write_text(json.dumps({"meta": meta, "checks": self.checks, "review": self.review, "data": self.data}, indent=1))
        lines = [f"# {self.title} ({meta.get('runtime', '?')}, {meta.get('time', '')})", "", "| Check | Status | Detail |", "|---|---|---|"]
        lines += [f"| {c['name']} | {c['status']} | {c['detail']} |" for c in self.checks]
        if self.review:
            lines += ["", "## Images to review", ""] + [f"- `{r['image']}`: {r['expect']}" for r in self.review]
        (self.out / "report.md").write_text("\n".join(lines) + "\n")


def clear_modals(bench):
    for _ in range(4):
        if not bench.call("menu", {"action": "list"}).get("messageBoxOpen"):
            return
        bench.call("menu", {"action": "accept", "index": 0})
        time.sleep(1.5)


def wait_shaders(bench, seconds=180):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        cache = bench.call("inspect", {"kind": "shadercache"})
        if cache.get("failedTasks", 0) or cache.get("currentFailedCount", 0):
            raise RuntimeError("Shader compilation failed; image comparisons would be invalid")
        if not cache.get("compiling", True):
            return
        time.sleep(1)
    raise RuntimeError("Shader compilation did not finish")


def enter_cell(bench):
    bench.call("console", {"command": f"coc {CELL}"})
    for _ in range(60):
        time.sleep(2)
        clear_modals(bench)
        scene = bench.call("inspect", {"kind": "scene"})
        if scene.get("cell", {}).get("editorId", "") == CELL and bench.call("inspect", {"kind": "state"}).get("playerLoaded", True):
            time.sleep(5)
            return
    raise RuntimeError(f"Did not reach {CELL}")


def player_position(bench):
    x, y, z = bench.call("inspect", {"kind": "scene"})["position"]
    return float(x), float(y), float(z)


def set_pose(bench, x, y, z, yaw_deg):
    if not bench.call("camera", {"action": "get"}).get("freeCam"):
        bench.call("camera", {"action": "freecam"})
    bench.call("camera", {"action": "drive", "x": x, "y": y, "z": z, "yaw": math.radians(yaw_deg), "pitch": math.radians(POSE_PITCH_DEG)})
    time.sleep(SETTLE_SECONDS)


def freecam_off(bench):
    if bench.call("camera", {"action": "get"}).get("freeCam"):
        bench.call("camera", {"action": "freecam", "on": False})
        time.sleep(1)


def capture(bench, out, name):
    clear_modals(bench)
    wait_shaders(bench)
    result = bench.call("capture", {"kind": "openshaders", "checkpointId": name, "variant": "dx12", "outDir": str(out)}, timeout=40)
    path = Path(result.get("path", ""))
    return path if path.is_file() else out / result.get("file", f"{name}.png")


def luma(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


def image_diff(a_path, b_path, heatmap_path=None):
    a, b = luma(a_path), luma(b_path)
    if a.shape != b.shape:
        return {"error": f"size mismatch {a.shape} vs {b.shape}"}
    delta = b - a
    if heatmap_path:
        scale = np.clip(np.abs(delta) * 8.0, 0, 255).astype(np.uint8)
        rgb = np.zeros((*delta.shape, 3), dtype=np.uint8)
        rgb[..., 0] = np.where(delta < 0, scale, 0)
        rgb[..., 1] = np.where(delta > 0, scale, 0)
        Image.fromarray(rgb).save(heatmap_path)
    return {
        "meanAbs": float(np.mean(np.abs(delta))),
        "meanSigned": float(np.mean(delta)),
        "changedFraction": float(np.mean(np.abs(delta) > DIFF_THRESHOLD)),
        "darkerFraction": float(np.mean(delta < -DIFF_THRESHOLD)),
    }


def wait_ready(bench, report):
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    status = {}
    while time.monotonic() < deadline:
        status = bench.status()
        tiles = [t for t in status.get("grid", {}).get("activeTiles", []) if t]
        if status.get("tlas", {}).get("instances", 0) and any(t["shapes"] for t in tiles):
            break
        time.sleep(2)
    report.data["readiness"] = status
    tiles = [t for t in status.get("grid", {}).get("activeTiles", []) if t]
    states = {}
    for tile in tiles:
        states[tile["state"]] = states.get(tile["state"], 0) + 1
    reasons = {}
    for tile in tiles:
        for entry in tile.get("rejections", []):
            reasons[entry["reason"]] = reasons.get(entry["reason"], 0) + entry["count"]
    top = ", ".join(f"{k} x{v}" for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:5]) or "none"
    shapes = sum(t["shapes"] for t in tiles)
    report.check("tiles prepared", "PASS" if shapes else "FAIL", f"{shapes} prepared shapes; active tile states {states}; top rejections: {top}")
    tlas = status.get("tlas", {})
    report.check("TLAS published", "PASS" if tlas.get("instances", 0) else "FAIL", json.dumps(tlas))
    return shapes > 0


def ownership_delta(before, after):
    keys = ("granted", "castersTraced", "castersKeptRaster")
    return {k: after.get("ownership", {}).get(k, 0) - before.get("ownership", {}).get(k, 0) for k in keys}


def screenshots(bench, report, out, modes):
    """modes: list of (label, mode or None). Two captures of the first label measure the noise floor."""
    x, y, z = player_position(bench)
    shots = []
    for index, yaw in enumerate(POSE_YAWS_DEG):
        set_pose(bench, x, y, z + POSE_RAISE, yaw)
        pose = {"pose": index, "yaw": yaw}
        for label, mode in modes:
            if mode is not None:
                bench.set_mode(mode)
            before = bench.status() if mode == 0 else None
            pose[label] = str(capture(bench, out, f"pose{index}_{label}"))
            if before is not None:
                time.sleep(1)
                pose[f"{label}_ownership"] = ownership_delta(before, bench.status())
                pose[f"{label}_ownedLights"] = bench.status().get("lights", {}).get("ownedShadowLights", 0)
        first = modes[0][0]
        if modes[0][1] is not None:
            bench.set_mode(modes[0][1])
        pose[f"{first}_repeat"] = str(capture(bench, out, f"pose{index}_{first}_repeat"))
        shots.append(pose)
    freecam_off(bench)
    report.data["screenshots"] = shots
    return shots


def sample_replay(bench, out_name, mode=None):
    if mode is not None:
        bench.set_mode(mode)
    before = bench.status() if mode == 0 else None
    run = bench.call("record", {"action": "replay", "path": str(RECORDINGS_DIR / RECORDING), "async": True, "closeMenus": True})
    run_id = run.get("runId")
    samples, timers = [], {}
    deadline = time.monotonic() + REPLAY_TIMEOUT_SECONDS
    time.sleep(3)
    while time.monotonic() < deadline:
        profile = bench.call("inspect", {"kind": "profiler"})
        frame = profile.get("frame", {})
        if frame.get("ms"):
            samples.append({"ms": frame.get("ms"), "avgMs": frame.get("avgMs"), "p99Ms": frame.get("p99Ms")})
        for timer in profile.get("timers", []):
            if timer.get("hasGpu"):
                timers.setdefault(timer["name"], []).append(timer.get("avgMs") or timer.get("gpuMs") or 0.0)
        state = bench.call("record", {"action": "status", "runId": run_id} if run_id else {"action": "status"})
        if state.get("done") or state.get("finished") or state.get("state") in ("done", "completed", "failed", "error", "cancelled"):
            break
        time.sleep(SAMPLE_SECONDS)
    result = {
        "label": out_name,
        "samples": len(samples),
        "frameAvgMs": statistics.median(s["avgMs"] for s in samples if s["avgMs"]) if samples else None,
        "frameP99Ms": max((s["p99Ms"] for s in samples if s["p99Ms"]), default=None),
        "shadowGpuMs": sum(statistics.median(v) for k, v in timers.items() if any(key in k.lower() for key in SHADOW_TIMER_KEYS)),
        "gpuTimers": {k: statistics.median(v) for k, v in timers.items()},
    }
    if before is not None:
        result["ownership"] = ownership_delta(before, bench.status())
    return result


def check_layer_applied(bench, report, out):
    """The RT shadow layer must visibly darken the frame at full strength, or offload only removes raster shadows."""
    original = bench.dx12_settings()
    x, y, z = player_position(bench)
    set_pose(bench, x, y, z + POSE_RAISE, POSE_YAWS_DEG[0])
    bench.set_mode(0)
    try:
        bench.set_dx12({"ShadowStrength": 0.0, "ShadowBlend": 1.0})
        time.sleep(SETTLE_SECONDS)
        off = capture(bench, out, "layer_strength0")
        bench.set_dx12({"ShadowStrength": 4.0, "ShadowBlend": 1.0})
        time.sleep(SETTLE_SECONDS)
        full = capture(bench, out, "layer_strength4")
        stages = bench.status()
    finally:
        bench.set_dx12({k: original[k] for k in ("ShadowStrength", "ShadowBlend") if k in original})
        freecam_off(bench)
    diff = image_diff(off, full, out / "layer_strength4_vs_0_diff.png")
    darker = diff.get("darkerFraction", 0.0)
    report.data["layer"] = diff
    report.check("RT shadow layer reaches the frame", "PASS" if darker >= MIN_LAYER_DARKER_FRACTION else "FAIL",
                 f"strength 4 darkens {darker:.1%} of pixels vs strength 0 (need {MIN_LAYER_DARKER_FRACTION:.0%}); mean {diff.get('meanSigned', 0):+.2f} luma; "
                 f"trace {stages.get('trace', {}).get('lastOutcome')}, composite {stages.get('composite', {}).get('lastShadowResult')}")
    if darker < MIN_LAYER_DARKER_FRACTION:
        report.look(out / "layer_strength4_vs_0_diff.png", "should be mostly red where owned lights cast static shadows")


def run_ab(bench, report, out):
    status = bench.status()
    if not status.get("active"):
        report.check("DX12 active", "FAIL", "DX12 is not active; enable it and restart for the ab phase")
        return
    report.check("DX12 active", "PASS", "DX12 enabled at boot")
    bench.set_mode(0)
    enter_cell(bench)
    wait_shaders(bench)
    wait_ready(bench, report)
    check_layer_applied(bench, report, out)
    shots = screenshots(bench, report, out, [("raster", 1), ("offload", 0)])
    engaged = sum(1 for s in shots if s.get("offload_ownership", {}).get("castersTraced", 0) > 0)
    report.check("offload engages at poses", "PASS" if engaged else "FAIL",
                 f"{engaged}/{len(shots)} poses moved static casters from raster to RT; "
                 + "; ".join(f"pose {s['pose']}: {s.get('offload_ownership', {})}" for s in shots))
    for shot in shots:
        noise = image_diff(shot["raster"], shot["raster_repeat"])
        change = image_diff(shot["raster"], shot["offload"], out / f"pose{shot['pose']}_offload_vs_raster_diff.png")
        shot["noise"], shot["change"] = noise, change
        floor = max(noise.get("changedFraction", 0.0), 0.002)
        ratio = change.get("changedFraction", 0.0) / floor
        status_value = "PASS" if ratio <= MAX_DIFF_OVER_NOISE else "REVIEW"
        report.check(f"pose {shot['pose']} image", status_value,
                     f"offload vs raster changed {change.get('changedFraction', 0):.4f} (noise {noise.get('changedFraction', 0):.4f}), mean {change.get('meanSigned', 0):+.2f} luma")
        if status_value == "REVIEW":
            report.look(out / f"pose{shot['pose']}_offload_vs_raster_diff.png", "red = offload darker, green = brighter; shadows should match raster")
    # Raster, offload, raster: averaging both raster runs keeps warm-up from reading as an offload win.
    raster_a = sample_replay(bench, "raster_first", 1)
    offload = sample_replay(bench, "offload", 0)
    raster_b = sample_replay(bench, "raster_second", 1)
    raster = {"label": "raster", "samples": raster_a["samples"] + raster_b["samples"]}
    for key in ("frameAvgMs", "frameP99Ms", "shadowGpuMs"):
        values = [r[key] for r in (raster_a, raster_b) if r[key] is not None]
        raster[key] = sum(values) / len(values) if values else None
    report.data["replay"] = [raster_a, offload, raster_b, raster]
    drift = abs((raster_a["frameAvgMs"] or 0) - (raster_b["frameAvgMs"] or 0))
    report.check("raster run-to-run drift", "PASS" if drift <= 0.5 else "REVIEW",
                 f"{raster_a['frameAvgMs']:.2f} vs {raster_b['frameAvgMs']:.2f} ms; differences smaller than this are noise")
    own = offload.get("ownership", {})
    traced, kept = own.get("castersTraced", 0), own.get("castersKeptRaster", 0)
    share = traced / max(traced + kept, 1)
    report.check("raster casters offloaded during replay", "PASS" if traced else "FAIL",
                 f"{traced} static caster draws moved to RT, {kept} kept in raster ({share:.0%} offloaded); light-frames owned {own.get('granted', 0)}")
    if raster["frameAvgMs"] and offload["frameAvgMs"]:
        delta = offload["frameAvgMs"] - raster["frameAvgMs"]
        report.check("frame time offload vs raster", "PASS" if delta <= 0.05 * raster["frameAvgMs"] else "REVIEW",
                     f"{raster['frameAvgMs']:.2f} -> {offload['frameAvgMs']:.2f} ms ({delta:+.2f} ms, {delta / 16.7:+.1%} of 60 fps); "
                     f"p99 {raster['frameP99Ms']:.2f} -> {offload['frameP99Ms']:.2f}; shadow GPU {raster['shadowGpuMs']:.2f} -> {offload['shadowGpuMs']:.2f} ms")
    bench.set_mode(0)


def run_baseline(bench, report, out):
    if bench.status().get("active"):
        report.check("DX12 inactive", "FAIL", "DX12 is active; disable it and restart for the baseline phase")
        return
    enter_cell(bench)
    wait_shaders(bench)
    screenshots(bench, report, out, [("baseline", None)])
    report.data["replay"] = [sample_replay(bench, "baseline")]
    report.check("baseline captured", "PASS", json.dumps({k: report.data["replay"][0][k] for k in ("frameAvgMs", "frameP99Ms", "shadowGpuMs")}))


def run_compare(report, ab_dir, baseline_dir):
    ab = json.loads((ab_dir / "results.json").read_text())["data"]
    base = json.loads((baseline_dir / "results.json").read_text())["data"]
    rows = {r["label"]: r for r in ab.get("replay", []) + base.get("replay", [])}
    lines = ["| Config | Frame avg ms | Frame p99 ms | Shadow GPU ms |", "|---|---|---|---|"]
    for label in ("baseline", "raster", "offload"):
        r = rows.get(label)
        if r:
            lines.append(f"| {label} | {r['frameAvgMs']:.2f} | {r['frameP99Ms']:.2f} | {r['shadowGpuMs']:.2f} |")
    report.data["table"] = lines
    if "baseline" in rows and "offload" in rows:
        delta = rows["offload"]["frameAvgMs"] - rows["baseline"]["frameAvgMs"]
        report.check("offload vs DX12 off", "PASS" if delta <= 0 else "REVIEW",
                     f"{delta:+.2f} ms avg ({delta / 16.7:+.1%} of 60 fps budget)")
    for a, b in zip(ab.get("screenshots", []), base.get("screenshots", [])):
        change = image_diff(b["baseline"], a["offload"], report.out / f"pose{a['pose']}_offload_vs_off_diff.png")
        report.check(f"pose {a['pose']} vs DX12 off", "REVIEW" if change.get("changedFraction", 0) > 0.02 else "PASS",
                     f"changed {change.get('changedFraction', 0):.4f}, mean {change.get('meanSigned', 0):+.2f} luma")
        report.look(report.out / f"pose{a['pose']}_offload_vs_off_diff.png", "camera poses come from separate launches; check shadow shape, not noise")
    (report.out / "table.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("phase", choices=("ab", "baseline", "compare"))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--port", type=int)
    parser.add_argument("--ab", type=Path, help="ab output directory (compare)")
    parser.add_argument("--baseline", type=Path, help="baseline output directory (compare)")
    parser.add_argument("--game-dir", type=Path, help="Skyrim SE/AE install, used to locate devbench recordings")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    global RECORDINGS_DIR
    if args.game_dir:
        RECORDINGS_DIR = args.game_dir / "Data" / "SKSE" / "Plugins" / "devbench" / "recordings"
    report = Report(args.out, f"DX12 offload validation: {args.phase}")
    meta = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "phase": args.phase}
    try:
        if args.phase == "compare":
            run_compare(report, args.ab, args.baseline)
        else:
            port, state = find_port(args.port)
            if state.get("vr"):
                sys.exit("DX12 offload is SE/AE only; point --port at a flat runtime")
            meta["runtime"] = state.get("runtime", "SE")
            bench = Devbench(port)
            (run_ab if args.phase == "ab" else run_baseline)(bench, report, args.out)
    except Exception as error:
        report.check("run", "FAIL", f"{type(error).__name__}: {error}")
    report.write(meta)
    return 1 if any(c["status"] == "FAIL" for c in report.checks) else 0


if __name__ == "__main__":
    sys.exit(main())
