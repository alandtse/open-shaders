#!/usr/bin/env python3
"""Snow Deformation in-game regression pass driven through devbench.

Runs against an already launched game (SE/AE or VR) that you have exclusive use of, with
Snow Deformation and Snow Cover loaded at boot. Writes screenshots, results.json and
report.md to --out. FAIL means a hard check broke; REVIEW means a screenshot metric is
outside the expected range and an agent or person must look at the listed images.

See docs/development/snow-deformation-ingame-regression.md for the full procedure.
"""
import argparse
import json
import math
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

# coc cell, then the player position of the Winterhold test spot (open snow plus boardwalk).
CELL = "WinterholdExterior01"
CELL_LOAD_SECONDS = 60
SPOT = (110183, 103754, -8890)
SNOWSTORM_WEATHER = "c8221"
ACCUMULATION_HOUR = 12
ACCUMULATION_STEPS = 5
ACCUMULATION_STEP_HOURS = 3
ACCUMULATION_STEP_SECONDS = 8
MIN_GAME_HOURS_ADVANCED = 6.0
MIN_BLANKET_CHANGED_FRACTION = 0.01
MAX_ACCUMULATION_LUMA_LOSS = -3.0
EYE_LUMA_RATIO = (0.85, 1.15)
VR_LANDSCAPE_OFFSETS = ((0, 400), (0, -400), (400, 0))
VR_DROP_Z = -8800


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

    def console(self, command):
        self.call("console", {"command": command})

    def diagnostics(self):
        return self.call("openshaders.feature", {"action": "diagnostics", "shortName": "SnowDeformation"})

    def set_feature(self, short_name, settings):
        self.call("openshaders.feature", {"action": "set", "shortName": short_name, "settings": settings})

    def scene(self):
        return self.call("inspect", {"kind": "scene"})


def find_port(requested):
    for port in [requested] if requested else [8920, 8921, 8922, 8923]:
        try:
            state = Devbench(port).call("inspect", {"kind": "state"}, timeout=5)
            return port, state
        except Exception:
            continue
    sys.exit("No devbench endpoint answered; launch the game first.")


class Report:
    def __init__(self, out):
        self.out = out
        self.checks = []
        self.review = []

    def check(self, name, status, detail):
        self.checks.append({"name": name, "status": status, "detail": detail})
        print(f"[{status}] {name}: {detail}", flush=True)

    def look(self, image, expectation):
        self.review.append({"image": str(image), "expect": expectation})

    def write(self, meta):
        (self.out / "results.json").write_text(json.dumps({"meta": meta, "checks": self.checks, "review": self.review}, indent=1))
        lines = [f"# Snow Deformation regression ({meta['runtime']}, {meta['time']})", "", "| Check | Status | Detail |", "|---|---|---|"]
        lines += [f"| {c['name']} | {c['status']} | {c['detail']} |" for c in self.checks]
        lines += ["", "## Screenshots to review", ""]
        lines += [f"- `{r['image']}`: {r['expect']}" for r in self.review]
        (self.out / "report.md").write_text("\n".join(lines) + "\n")


def luma(path):
    image = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return image, image @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


def clear_modals(bench):
    for _ in range(4):
        menus = bench.call("menu", {"action": "list"})
        if not menus.get("messageBoxOpen"):
            return
        bench.call("menu", {"action": "accept", "index": 0})
        time.sleep(1.5)


def wait_for_cell(bench, prefix, seconds):
    cell = ""
    for _ in range(seconds // 2):
        clear_modals(bench)
        cell = bench.scene().get("cell", {}).get("editorId", "")
        if cell.startswith(prefix):
            return cell
        time.sleep(2)
    return cell


def go_to_spot(bench):
    bench.console("player.unequipitem 12E4D")
    for survival in ("Survival_ModeEnabled", "Survival_ModeToggle"):
        bench.console(f"set {survival} to 0")
    bench.console("player.forceav health 100000")
    bench.console(f"coc {CELL}")
    time.sleep(3)
    wait_for_cell(bench, "Winterhold", CELL_LOAD_SECONDS)
    for axis, value in zip("xyz", SPOT):
        bench.console(f"player.setpos {axis} {value}")
    time.sleep(8)
    clear_modals(bench)


def verify_spot(bench, report):
    cell = wait_for_cell(bench, "Winterhold", 20)
    report.check("setup.cell", "PASS" if cell.startswith("Winterhold") else "FAIL", f"cell={cell!r} (expected {CELL} area)")
    return cell.startswith("Winterhold")


def freecam_pose(bench, x, y, z, yaw, pitch):
    camera = bench.call("camera", {"action": "get"})
    if not camera.get("freeCam"):
        bench.call("camera", {"action": "freecam"})
        time.sleep(2)
    bench.call("camera", {"action": "drive", "x": x, "y": y, "z": z, "yaw": yaw, "pitch": pitch})
    time.sleep(3)


def freecam_off(bench):
    if bench.call("camera", {"action": "get"}).get("freeCam"):
        bench.call("camera", {"action": "freecam", "on": False})
        time.sleep(2)


def shot(bench, report, name):
    clear_modals(bench)
    deadline = time.monotonic() + 180
    while True:
        cache = bench.call("inspect", {"kind": "shadercache"})
        if cache.get("failedTasks", 0) or cache.get("currentFailedCount", 0):
            raise RuntimeError("Shader compilation failed; a rendering comparison would be invalid")
        if not cache.get("compiling", True):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("Shader compilation did not finish before the capture")
        time.sleep(1)
    result = bench.call("capture", {"kind": "openshaders", "checkpointId": name, "variant": "snowreg", "outDir": str(report.out)}, timeout=40)
    path = Path(result.get("path", ""))
    if not path.is_file():
        path = report.out / result.get("file", f"{name}.png")
    return path


def scenario_blanket(bench, report):
    bench.set_feature("SnowDeformation", {"Enabled": True, "BlanketHeight": 0.0, "SinkActors": False})
    time.sleep(3)
    px, py, pz = SPOT
    freecam_pose(bench, px, py, pz + 60, 0.673, 0.25)
    off = shot(bench, report, "blanket_off")
    bench.set_feature("SnowDeformation", {"BlanketHeight": 20.0})
    time.sleep(5)
    on = shot(bench, report, "blanket_on")
    diag = bench.diagnostics()
    changed = float((np.abs(luma(off)[1] - luma(on)[1]) > 8).mean())
    report.check("blanket.active", "PASS" if diag.get("BlanketActive") else "FAIL", f"BlanketActive={diag.get('BlanketActive')}")
    report.check("blanket.draws", "PASS" if diag.get("DepthDraws", 0) > 0 and diag.get("LightingDraws", 0) > 0 else "FAIL", f"DepthDraws={diag.get('DepthDraws')} LightingDraws={diag.get('LightingDraws')}")
    report.check("blanket.layout", "PASS" if diag.get("LayoutRejects", 1) == 0 and diag.get("LandscapeLayoutSeen") else "FAIL", f"LayoutRejects={diag.get('LayoutRejects')} LandscapeLayoutSeen={diag.get('LandscapeLayoutSeen')}")
    report.check("blanket.pixels", "PASS" if changed >= MIN_BLANKET_CHANGED_FRACTION else "REVIEW", f"{changed:.1%} of pixels differ (need >= {MIN_BLANKET_CHANGED_FRACTION:.0%})")
    report.look(off, "snow surface flat, no displacement")
    report.look(on, "same view with a raised, smooth snow drift; no holes, spikes, tinted patches or one-eye veil")


def scenario_sink(bench, report):
    freecam_off(bench)
    bench.set_feature("SnowDeformation", {"Enabled": True, "BlanketHeight": 0.0, "SinkActors": True})
    walks = [("w", 3.0), ("d", 1.5), ("w", 2.0)]
    best = {"SunkActors": 0, "DeepestSink": 0.0}
    for index, (key, seconds) in enumerate(walks):
        if bench.vr:
            # Keyboard input does not move the VR player, so teleport onto landscape instead.
            dx, dy = VR_LANDSCAPE_OFFSETS[index]
            for axis, value in (("x", SPOT[0] + dx), ("y", SPOT[1] + dy), ("z", VR_DROP_Z)):
                bench.console(f"player.setpos {axis} {value}")
        else:
            bench.call("input", {"action": "sequence", "device": "keyboard", "events": [{"action": "down", "key": key}, {"action": "wait", "durationMs": int(seconds * 1000)}, {"action": "up", "key": key}]})
        clear_modals(bench)
        for _ in range(3):
            time.sleep(1.5)
            diag = bench.diagnostics()
            if diag.get("SunkActors", 0) > best["SunkActors"] or diag.get("DeepestSink", 0) > best["DeepestSink"]:
                best = {"SunkActors": diag.get("SunkActors", 0), "DeepestSink": diag.get("DeepestSink", 0.0), "Stamps": diag.get("Stamps")}
    report.check("sink.engages", "PASS" if best["SunkActors"] >= 1 and best["DeepestSink"] > 0 else "FAIL",
                 f"best {best}; FAIL usually means the player never reached landscape or IsSinkBlocked rejects the support layer")
    camera = bench.call("camera", {"action": "get"})
    yaw = camera.get("camYaw", 0.0)
    player = bench.call("inspect", {"kind": "scene"}).get("position", list(SPOT))
    ahead = 160
    freecam_pose(bench, player[0] + math.sin(yaw) * ahead, player[1] + math.cos(yaw) * ahead, player[2] + 70, yaw + math.pi, 0.35)
    image = shot(bench, report, "sink_trail")
    report.look(image, "player in the middle distance with a trail of prints behind them in the snow (not verified automatically)")
    freecam_off(bench)
    bench.set_feature("SnowDeformation", {"SinkActors": False})
    time.sleep(3)
    after = bench.diagnostics()
    report.check("sink.disable", "PASS" if after.get("SunkActors", 1) == 0 else "FAIL", f"SunkActors={after.get('SunkActors')} after Sink Into Prints off")


def hold_storm(bench):
    """Force the snowstorm at a fixed hour so before and after shots share lighting."""
    bench.console(f"fw {SNOWSTORM_WEATHER}")
    bench.console(f"set gamehour to {ACCUMULATION_HOUR}")
    time.sleep(15)
    clear_modals(bench)


def scenario_accumulation(bench, report):
    freecam_off(bench)
    bench.set_feature("SnowDeformation", {"Enabled": True, "BlanketHeight": 0.0, "SinkActors": False})
    bench.set_feature("SnowCover", {"Enable": 1, "SurvivalEnabled": False})
    px, py, pz = SPOT
    freecam_pose(bench, px, py, pz + 60, 0.673, 0.55)
    hold_storm(bench)
    before_image = shot(bench, report, "accumulation_before")
    advanced = 0.0
    for _ in range(ACCUMULATION_STEPS):
        bench.console(f"fw {SNOWSTORM_WEATHER}")
        reply = bench.call("game", {"action": "advanceTime", "hours": ACCUMULATION_STEP_HOURS})
        advanced += ACCUMULATION_STEP_HOURS if reply.get("gameHour") is not None else 0.0
        time.sleep(ACCUMULATION_STEP_SECONDS)
        clear_modals(bench)
    hold_storm(bench)
    scene = bench.scene()
    cell = scene.get("cell", {}).get("editorId", "")
    report.check("accumulation.same_cell", "PASS" if cell.startswith("Winterhold") else "FAIL", f"cell after the storm: {cell!r} (a reload means the player died; Snow Cover SurvivalEnabled must stay off)")
    after_image = shot(bench, report, "accumulation_after")
    diag = bench.diagnostics()
    before_luma, after_luma = luma(before_image)[1], luma(after_image)[1]
    lower = before_luma.shape[0] * 2 // 5
    gain = float(after_luma[lower:].mean() - before_luma[lower:].mean())
    report.check("accumulation.time", "PASS" if advanced >= MIN_GAME_HOURS_ADVANCED else "FAIL", f"{advanced:.1f} game hours advanced (need >= {MIN_GAME_HOURS_ADVANCED})")
    report.check("accumulation.gating", "PASS" if diag.get("SnowCoverGating") else "FAIL", f"SnowCoverGating={diag.get('SnowCoverGating')} weather={scene.get('weather', {}).get('editorId')}")
    report.check("accumulation.luma", "PASS" if gain >= MAX_ACCUMULATION_LUMA_LOSS else "REVIEW", f"lower-frame mean luma changed by {gain:+.1f} (REVIEW if below {MAX_ACCUMULATION_LUMA_LOSS}); Winterhold ground is already white, so a gain is not expected")
    report.look(before_image, "ground before the storm")
    report.look(after_image, "same pose after the storm: ground and objects visibly buried or whitened compared to the first image")


def scenario_eyes(bench, report):
    px, py, pz = SPOT
    freecam_pose(bench, px, py, pz + 60, 0.673, 0.25)
    bench.set_feature("SnowDeformation", {"Enabled": True, "BlanketHeight": 20.0, "SinkActors": False})
    time.sleep(5)
    path = shot(bench, report, "vr_eyes")
    _, grey = luma(path)
    half = grey.shape[1] // 2
    ratio = float(grey[:, :half].mean() / max(grey[:, half:].mean(), 1e-3))
    report.check("vr.eye_balance", "PASS" if EYE_LUMA_RATIO[0] <= ratio <= EYE_LUMA_RATIO[1] else "REVIEW", f"left/right mean luma {ratio:.2f} (expected {EYE_LUMA_RATIO[0]}-{EYE_LUMA_RATIO[1]})")
    report.look(path, "left and right eye show the same terrain and snow; no dark veil, purple tint or unclipped geometry in one eye")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="output directory (screenshots, results.json, report.md)")
    parser.add_argument("--port", type=int, help="devbench port; default probes 8920-8923")
    parser.add_argument("--save", help="load this save first (default: the game's latest save)")
    parser.add_argument("--only", nargs="*", choices=["blanket", "sink", "accumulation", "eyes"], help="run a subset")
    args = parser.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    port, state = find_port(args.port)
    bench = Devbench(port)
    bench.vr = bool(state.get("vr"))
    runtime = "VR" if state.get("vr") else "SE/AE"
    report = Report(out)
    print(f"devbench port {port}, runtime {runtime}", flush=True)

    try:
        bench.diagnostics()
    except Exception as error:
        sys.exit(f"Snow Deformation is not loaded ({error}). Set Disable at Boot.SnowDeformation=false and relaunch.")
    clear_modals(bench)
    if args.save:
        bench.call("game", {"action": "load", "name": args.save})
        time.sleep(30)
        clear_modals(bench)
    go_to_spot(bench)
    if not verify_spot(bench, report):
        report.write({"runtime": runtime, "port": port, "time": time.strftime("%Y-%m-%d %H:%M:%S")})
        sys.exit("Player is not at the Winterhold test spot; see report.md")

    scenarios = {"blanket": scenario_blanket, "sink": scenario_sink, "accumulation": scenario_accumulation}
    if state.get("vr"):
        scenarios["eyes"] = scenario_eyes
    for name, run in scenarios.items():
        if args.only and name not in args.only:
            continue
        try:
            run(bench, report)
        except Exception as error:
            report.check(f"{name}.run", "FAIL", f"scenario aborted: {error}")
    freecam_off(bench)

    report.write({"runtime": runtime, "port": port, "time": time.strftime("%Y-%m-%d %H:%M:%S")})
    failed = [c for c in report.checks if c["status"] == "FAIL"]
    print(f"\n{len(report.checks)} checks, {len(failed)} FAIL, {sum(c['status'] == 'REVIEW' for c in report.checks)} REVIEW; report: {out / 'report.md'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
