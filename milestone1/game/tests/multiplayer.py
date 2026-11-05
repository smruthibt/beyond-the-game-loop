#!/usr/bin/env python3
"""Exercise actual ZeroMQ processes; no Python dependencies beyond the stdlib."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", default="build-towerfall/game/towerfall")
parser.add_argument("--mode", choices=["all", "client", "hybrid", "peer"], default="all")
parser.add_argument("--combat", action="store_true")
parser.add_argument("--long-round", action="store_true", help="Check survival and networking beyond two minutes without combat")
parser.add_argument("--roster", action="store_true", help="Clients join and leave mid-match (client and hybrid modes)")
parser.add_argument("--render", action="store_true", help="Use SDL dummy windows instead of headless clients")
parser.add_argument("--output", default="/private/tmp/towerfall-network")
args = parser.parse_args()
if args.long_round and (args.combat or args.roster):
    parser.error("--long-round requires the timing scenario")
binary = str(Path(args.binary).resolve())
root = Path(args.output)
root.mkdir(parents=True, exist_ok=True)


def run(mode, port):
    directory = root / (mode + ("-combat" if args.combat else "-timing"))
    directory.mkdir(exist_ok=True)
    children = []
    logs = []
    duration = 136 if args.long_round else 48 if args.combat else 14
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

    def launch(name, extra, seconds):
        log = open(directory / (name + ".log"), "w")
        logs.append(log)
        command = [binary, "--port", str(port), "--players", "3", "--seconds", str(seconds)] + extra
        child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
        children.append(child)
        return child

    try:
        if mode != "peer":
            launch("server", ["--mode", "server"] + (["--hybrid"] if mode == "hybrid" else []), duration + 4)
            time.sleep(.4)
            assert children[0].poll() is None, "server failed to start (check local TCP permissions)"
        for identity, speed in enumerate([.5, 1, 2]):
            flags = ["--mode", mode, "--bot", "--report", str(directory / f"p{identity}.jsonl")]
            if not args.render:
                flags += ["--headless"]
            if mode == "peer":
                flags += ["--id", str(identity)]
            if not args.combat:
                flags += ["--passive", "--scale", str(speed)]
                if identity == 0:
                    flags += ["--pause-at", "8"]
            if args.render and identity == 0:
                flags += ["--capture", str(directory / "arena.bmp"), "--capture-at", "130" if args.long_round else "6", "--telemetry"]
            launch(f"p{identity}", flags, duration)
            time.sleep(.3)
        time.sleep(4)
        assert all(p.poll() is None for p in children), "a process exited early"
        observer_flags = ["--mode", mode, "--spectator", "--headless", "--report", str(directory / "observer.jsonl")]
        if mode == "peer":
            observer_flags += ["--id", "3"]
        launch("observer", observer_flags, duration - 6)
        deadline = time.monotonic() + duration + 8
        while any(p.poll() is None for p in children):
            assert time.monotonic() < deadline, "process shutdown timed out"
            time.sleep(.2)
        assert all(p.returncode == 0 for p in children), "a process failed"
        for log in logs:
            log.flush()
        for path in directory.glob("*.log"):
            assert "ThreadSanitizer" not in path.read_text(), f"thread sanitizer diagnostic in {path}"
        reports = [[json.loads(line) for line in (directory / f"p{i}.jsonl").read_text().splitlines()] for i in range(3)]
        observer = [json.loads(line) for line in (directory / "observer.jsonl").read_text().splitlines()]
        assert all(len(rows) > 10 for rows in reports), "missing client reports: inspect logs"
        assert len(observer) > 5, "late observer did not synchronize"
        assert all(any(r["connected"] and r["phase"] >= 2 for r in rows) for rows in reports), "game did not start"
        if args.combat:
            for rows in reports:
                # Archers fought. Whether an arrow lands is up to the bots' aim; that
                # a hit eliminates is checked deterministically by the rule tests.
                assert max(r["shots"] for r in rows) > 0, "no arrow was fired"
            outcomes = []
            for rows in reports + [observer]:
                # Phase 5 (complete) means the game's one round was decided: by
                # unanimous vote, or by the server.
                outcomes.append({(r["round"], r["resultHash"], tuple(r["scores"])) for r in rows if r["resultHash"] != "0" and r["phase"] == 5})
            if any(outcomes):
                assert set.intersection(*outcomes), "clients/observer did not record the same committed outcome"
                print(mode, "PASS: combat, agreed result hashes/scores, late reconstruction", flush=True)
            else:
                # Without a timed hazard, bots need not finish within this test's
                # duration. Deterministic rule tests cover winning and consensus.
                for rows in reports + [observer]:
                    assert rows[-1]["phase"] == 2 and rows[-1]["tick"] > 4000, "unfinished combat stopped progressing"
                probes = {rows[-1]["probe"] for rows in reports + [observer]}
                assert len(probes) == 1 and "0" not in probes, "combat history did not converge"
                print(mode, "PASS: ongoing combat without a forced timeout, late reconstruction, common replay hash", flush=True)
        else:
            if args.long_round:
                for rows in reports:
                    late = [r for r in rows if 125 < r["elapsed"] < 130]
                    assert late and all(r["alive"] and r["phase"] == 2 and r["round"] == 0 for r in late), "a timed hazard ended the round"
                    assert late[-1]["tick"] > 12000 and late[-1]["tick"] > late[0]["tick"], "long round stopped advancing"
            probes = []
            rates = []
            for rows in reports + [observer]:
                steady = [r for r in rows if r["probe"] != "0"]
                assert steady, "missing historical hash"
                probes.append(steady[-1]["probe"])
            assert len(set(probes)) == 1, f"historical replay differs: {probes}"
            for rows, expected in zip(reports, [15, 30, 60]):
                rates.append(statistics.median(r["tx"] for r in rows if 4 < r["elapsed"] < 7.5))
                assert abs(rates[-1] - expected) <= 5, f"expected {expected} updates/s, got {rates[-1]}"
                assert rows[-1]["workers"] > rows[2]["workers"] and rows[-1]["loops"] > rows[2]["loops"], "both loops must progress"
            if mode in ("client", "hybrid"):
                # With a server, commands travel only inside the scheduled state
                # messages; the one exception is the single message a pause sends.
                assert all(rows[-1]["batched"] > 0 for rows in reports), "commands did not travel in scheduled messages"
                assert reports[0][-1]["flushes"] == 1, f"expected one pause message, got {reports[0][-1]['flushes']}"
                assert all(rows[-1]["flushes"] == 0 for rows in reports[1:]), "an unpaused client sent outside its schedule"
            paused = [r for r in reports[0] if r["elapsed"] > 10]
            assert len({r["personal"] for r in paused}) == 1, "personal timeline did not freeze"
            assert len({(r["x"], r["y"], r["playerTime"]) for r in paused}) == 1, "paused player moved"
            assert all(r["tx"] == 0 for r in paused), "paused client kept scheduled updates running"
            assert paused[-1]["tick"] > paused[0]["tick"] + 100, "world stopped with paused client"
            for rows in reports[1:]:
                assert rows[-1]["playerTime"] > rows[-5]["playerTime"], "other player slowed/stopped"
            if mode in ("peer", "hybrid"):
                assert all(rows[-1]["rx"] > 100 for rows in reports), "direct player data not received"
            if mode == "peer":
                assert len({rows[-1]["epoch"] for rows in reports + [observer]}) == 1, "shared epoch did not converge"
            print(mode, f"PASS: state rates {rates}, pause isolation, three processes, late observer, common replay hash {probes[0]}", flush=True)
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for log in logs:
            log.close()


def roster(mode, port):
    """Section 2: clients start and quit at any time, and nobody waits for anybody."""
    directory = root / (mode + "-roster")
    directory.mkdir(exist_ok=True)
    children = []
    logs = []
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

    def launch(name, extra, seconds):
        log = open(directory / (name + ".log"), "w")
        logs.append(log)
        command = [binary, "--port", str(port), "--seconds", str(seconds)] + extra
        children.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env))

    def client(name, seconds):
        launch(name, ["--mode", mode, "--bot", "--headless", "--report", str(directory / f"{name}.jsonl")], seconds)

    def rows(name):
        return [json.loads(line) for line in (directory / f"{name}.jsonl").read_text().splitlines()]

    try:
        launch("server", ["--mode", "server"] + (["--hybrid"] if mode == "hybrid" else []), 62)
        time.sleep(.4)
        assert children[0].poll() is None, "server failed to start (check local TCP permissions)"
        client("first", 58)   # alone for the first eight seconds
        time.sleep(8)
        client("second", 46)  # joins mid-match
        time.sleep(6)
        client("third", 8)    # joins, then quits at about 22 s
        time.sleep(16)
        client("fourth", 20)  # starts later and takes the freed archer
        deadline = time.monotonic() + 40
        while any(p.poll() is None for p in children):
            assert time.monotonic() < deadline, "process shutdown timed out"
            time.sleep(.2)
        assert all(p.returncode == 0 for p in children), "a process failed"
        first, second, third, fourth = (rows(n) for n in ("first", "second", "third", "fourth"))
        alone = [r for r in first if r["elapsed"] < 7 and r["phase"] == 2]
        assert alone and all(sum(r["archers"]) == 1 for r in alone), "a lone client could not start playing"
        assert len({(r["x"], r["y"]) for r in alone}) > 1, "the lone archer never moved"
        assert max(sum(r["archers"]) for r in first) >= 3, "three archers never shared the arena"
        for late in (second, third, fourth):
            assert any(r["id"] >= 0 and r["archers"][r["id"]] for r in late), "a late client got no archer"
        left = max(r["elapsed"] for r in third) + 14  # "third" started 14 s after "first"
        after = [r for r in first if left + 2 < r["elapsed"] < left + 7]
        assert after and all(sum(r["archers"]) == 2 for r in after), "a departed client's archer stayed in the arena"
        before = max(r["commits"] for r in first if r["elapsed"] < left)
        assert first[-1]["commits"] > before, "games stopped being decided after a client left"
        print(mode, f"PASS: lone start, late joins, departure, {first[-1]['commits']} games decided", flush=True)
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for log in logs:
            log.close()


if args.roster:
    for number, mode in enumerate(["client", "hybrid"] if args.mode == "all" else [args.mode]):
        roster(mode, 25400 + number * 100)
else:
    for number, mode in enumerate(["client", "hybrid", "peer"] if args.mode == "all" else [args.mode]):
        run(mode, 23400 + number * 100 + (1000 if args.combat else 0))
print("Reports:", root)
