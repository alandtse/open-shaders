#!/usr/bin/env python3
"""Exterior weather sweep for base-vs-PR GPU A/B runs, driven through devbench.

Runs against an already launched game (SE/AE or VR) that you have exclusive use of. It
moves to Whiterun exterior, freezes time at noon, then forces clear, rain and snow weather,
holding each for --hold seconds after a settle. Each window's wall-clock bounds go to
windows.json so a Tracy capture recorded alongside can be sliced per weather.

See docs/development/shader-runtime-ab.md ("Weather sweep") for the A/B procedure.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

CELL = "WhiterunExterior01"
GAME_HOUR = 12
CELL_LOAD_SECONDS = 10
LOAD_POLLS = 60
LOAD_POLL_SECONDS = 3
COMPILE_IDLE_POLLS = 3
COMPILE_POLL_SECONDS = 6
DEFAULT_PORTS = (8920, 8921, 8922, 8923)

# name, weather editor id, default settle seconds. Snow needs the longest settle because
# the accumulation fades in.
WINDOWS = (
    ("clear", "SkyrimClear", 20),
    ("rain", "SkyrimOvercastRain", 30),
    ("snow", "SkyrimOvercastSnow", 60),
)


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
        self.call("console", {"action": "exec", "command": command})
        time.sleep(1.0)


def find_port(requested):
    for port in [requested] if requested else DEFAULT_PORTS:
        try:
            return port, Devbench(port).call("inspect", {"kind": "state"}, timeout=5)
        except Exception:
            continue
    sys.exit("No devbench endpoint answered; launch the game first.")


def wait_loaded(bench):
    for _ in range(LOAD_POLLS):
        try:
            if bench.call("inspect", {"kind": "state"}, timeout=20).get("playerLoaded"):
                return
        except Exception:
            pass
        time.sleep(LOAD_POLL_SECONDS)
    sys.exit("Player did not finish loading.")


def wait_compile_idle(bench):
    idle = 0
    while idle < COMPILE_IDLE_POLLS:
        compiling = bench.call("inspect", {"kind": "shadercache"}, timeout=20).get("compiling")
        idle = idle + 1 if compiling is False else 0
        time.sleep(COMPILE_POLL_SECONDS)


def clear_modals(bench):
    for _ in range(10):
        menus = bench.call("menu", {"action": "list"})
        if menus.get("messageBoxOpen"):
            bench.call("menu", {"action": "accept", "index": 0})
            time.sleep(8)
        elif menus.get("openMenus") == ["HUD Menu"]:
            return
        else:
            time.sleep(5)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True, help="directory for windows.json and screenshots")
    parser.add_argument("--port", type=int, help="devbench port; default probes SE 8920 then VR 8921")
    parser.add_argument("--hold", type=int, default=30, help="seconds each weather is held for measurement")
    parser.add_argument("--label", default="run", help="tag stored in windows.json, e.g. base or pr")
    parser.add_argument("--capture", action="store_true", help="save one screenshot per weather before the hold")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    port, state = find_port(args.port)
    bench = Devbench(port)
    runtime = "VR" if state.get("vr") else "flat"
    print(f"devbench port {port} ({runtime})", flush=True)

    clear_modals(bench)
    log = {"label": args.label, "runtime": runtime, "hold": args.hold, "windows": []}
    for index, (name, weather, settle) in enumerate(WINDOWS):
        if index == 0:
            bench.console(f"coc {CELL}")
            time.sleep(CELL_LOAD_SECONDS)
            wait_loaded(bench)
            bench.console(f"set gamehour to {GAME_HOUR}")
            bench.console("set timescale to 0")
        bench.console(f"fw {weather}")
        wait_compile_idle(bench)
        time.sleep(settle)
        wait_compile_idle(bench)
        if args.capture:
            shot = bench.call("capture", {"kind": "auto", "excludeUi": False}, timeout=90)
            print(f"{name} capture {shot.get('path')}", flush=True)
        start = time.time()
        time.sleep(args.hold)
        end = time.time()
        log["windows"].append({"name": name, "weather": weather, "start": start, "end": end})
        print(f"{name} window {time.strftime('%H:%M:%S', time.localtime(start))}-{time.strftime('%H:%M:%S', time.localtime(end))}", flush=True)

    (out / "windows.json").write_text(json.dumps(log, indent=1))
    print("SWEEP_DONE", flush=True)


if __name__ == "__main__":
    main()
