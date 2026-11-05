#!/usr/bin/env python3
"""Launch a local Towerfall session, with separate processes and SDL windows."""
import argparse
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--mode", choices=["solo", "client", "hybrid", "peer"], default="solo")
parser.add_argument("--players", type=int, choices=[2, 3, 4], default=3)
parser.add_argument("--bots", type=int, default=0, help="AI-controlled windows after the first human player")
parser.add_argument("--port", type=int, default=19400)
parser.add_argument("--demo", action="store_true", help="Time controls and telemetry, with .5/1/2 client speeds")
parser.add_argument("--log", action="store_true", help="Every process prints its network traffic to this terminal")
parser.add_argument("--binary", default=str(root / "build-towerfall/game/towerfall"))
args = parser.parse_args()
if not 0 <= args.bots < args.players:
    parser.error("--bots must be between zero and players minus one")
binary = Path(args.binary).resolve()
if not binary.is_file():
    parser.error("Build first: cmake -S . -B build-towerfall && cmake --build build-towerfall -j")
processes = []


def launch(flags):
    process = subprocess.Popen([str(binary), "--port", str(args.port), "--players", str(args.players)] + flags
                               + (["--log"] if args.log else []))
    processes.append(process)
    return process


try:
    if args.mode in ("client", "hybrid"):
        server = launch(["--mode", "server"] + (["--hybrid"] if args.mode == "hybrid" else []))
        time.sleep(.5)
        if server.poll() is not None:
            raise SystemExit("Server failed to start; check its error above.")
    windows = []
    for identity in range(1 if args.mode == "solo" else args.players):
        flags = ["--mode", args.mode]
        if args.mode == "peer":
            flags += ["--id", str(identity)]
        if identity >= args.players - args.bots:
            flags += ["--bot"]
        if args.demo:
            flags += ["--demo", "--telemetry", "--scale", str([.5, 1, 2, 1][identity])]
        windows.append(launch(flags))
        time.sleep(.3)
    if args.mode == "peer":
        print("UP readies each human archer.", flush=True)
    else:
        print("Archers join as their windows open; start another window any time.", flush=True)
    print("UP + DOWN opens options. Ctrl-C closes the whole session.", flush=True)
    while any(window.poll() is None for window in windows):
        time.sleep(.3)
except KeyboardInterrupt:
    pass
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
    for process in processes:
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
