#!/usr/bin/env python3
"""Rerun benchmark campaign: manifest, schedule, launching, validation, ledger.

    python3 benchmark/tools/campaign.py create --name NAME
    python3 benchmark/tools/campaign.py run    --name NAME [--suites a,b] [--max-units N]
    python3 benchmark/tools/campaign.py status --name NAME
    python3 benchmark/tools/campaign.py pilot  --name NAME

`create` writes results/NAME/manifest.json: the environment, source and
executable hashes, every suite's definition, and the complete randomized run
schedule. The manifest is frozen: `run` refuses to start if the manifest,
the harness/engine/game sources or the executables have changed since, because
a changed workload or executable starts a new campaign.

`run` executes the schedule in order and is resumable. Every attempt is
appended to ledger.jsonl and never erased. A unit (a pair of A/B runs, or a
set of runs that are compared with each other) whose run fails validation is
repeated with new run IDs, at most twice. Units still failing are reported,
and the command exits nonzero.

Standard library only.
"""
import argparse
import hashlib
import json
import math
import os
import platform
import random
import signal
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent                    # benchmark/
PROJECT = ROOT.parent                  # the project root (engine/, game/)
BUILD = ROOT / "build"
BENCH = BUILD / "rerun_bench"
FAULTS = BUILD / "rerun_faults"
RESULTS = ROOT / "results"

WARMUP_BLOCKS = 250        # 500 ticks
MEASURE_BLOCKS = 50000     # 100,000 ticks, publication every second tick
WINDOW = 4                 # blocks = 8 ticks
HWM = 1000
KEYFRAME = 60
SEEDS = list(range(1, 11))
TIMEOUT_S = 900
MAX_ATTEMPTS = 3           # the original attempt plus two replacements
SCHEDULE_SEED = 20261021

# --- what is compared ------------------------------------------------------------------

SIDES = {
    "cs-state": {"topology": "cs", "world": "native", "upstream": "state"},
    "cs-input": {"topology": "cs", "world": "native", "upstream": "input"},
    "cs-relay": {"topology": "cs", "world": "local", "upstream": "state"},
    "p2p-state": {"topology": "p2p", "upstream": "state"},
    "p2p-input": {"topology": "p2p", "upstream": "input"},
}


def side(name, **over):
    flags = {"scenario": "engine", "encoding": "binary", "down-delta": "off", "up-delta": "off", "keyframe": KEYFRAME}
    flags.update(SIDES[name])
    flags.update(over)
    return flags


ENGINE_SWEEPS = [
    ("moving", [10, 100, 500], {"static": 100, "players": 2}),
    ("static", [10, 100, 500], {"moving": 100, "players": 2}),
    ("players", [2, 3, 4], {"moving": 100, "static": 100}),
]
GAME_SWEEPS = [   # the original Clockwork Dash sweeps
    ("moving", [10, 100, 500], {"static": 0, "players": 2}),
    ("static", [10, 100, 500], {"moving": 0, "players": 2}),
    ("clients", [1, 2, 4], {"moving": 0, "static": 0}),
]


def conditions(sweeps):
    out = []
    for sweep, levels, fixed in sweeps:
        key = {"clients": "players"}.get(sweep, sweep)
        for level in levels:
            c = dict(fixed)
            c[key] = level
            c.update({"sweep": sweep, "level": level})
            out.append(c)
    return out


BASELINE = {"sweep": "baseline", "level": 0, "moving": 100, "static": 100, "players": 2}
STATIC500 = {"sweep": "static", "level": 500, "moving": 100, "static": 500, "players": 2}

PRIMARY = {
    "information": (("state", side("cs-state")), ("input", side("cs-input"))),
    "representation": (("text", side("cs-input", encoding="text")), ("binary", side("cs-input"))),
    "update": (("full", side("cs-input")), ("delta", side("cs-input", **{"down-delta": "on"}))),
    "topology": (("cs-relay", side("cs-relay")), ("p2p", side("p2p-state"))),
}
GAME = {
    "game-encoding": (("text", side("cs-input", scenario="game", encoding="text")), ("binary", side("cs-input", scenario="game"))),
    "game-snapshots": (("full", side("cs-input", scenario="game")),
                       ("delta", side("cs-input", scenario="game", **{"down-delta": "on"}))),
}


# --- the schedule ----------------------------------------------------------------------

def pair_units(suite, comparisons, conds, rng, seeds=SEEDS, extra=None):
    """Blocks of paired A/B runs: five AB and five BA pairs per block, the
    assignment and the block order randomized and saved."""
    blocks = []
    for comp, ((la, fa), (lb, fb)) in comparisons.items():
        for c in conds:
            ab = set(rng.sample(range(len(seeds)), len(seeds) // 2))
            units = []
            for i, seed in enumerate(seeds):
                a = {"side": "A", "label": la, "flags": fa}
                b = {"side": "B", "label": lb, "flags": fb}
                runs = [a, b] if i in ab else [b, a]
                uid = f"{suite}.{comp}.{c['sweep']}{c['level']}.s{seed:02d}"
                units.append({"unit": uid, "suite": suite, "kind": "pair", "comparison": comp, "condition": c,
                              "seed": seed, "order": "AB" if i in ab else "BA", "runs": runs, **(extra or {})})
            blocks.append(units)
    rng.shuffle(blocks)
    return [u for b in blocks for u in b]


def build_schedule():
    rng = random.Random(SCHEDULE_SEED)
    suites = {}
    engine_conds = conditions(ENGINE_SWEEPS)

    # Correctness gates first.
    suites["faults"] = [{"unit": "faults.inprocess", "suite": "faults", "kind": "inprocess"}] + [
        {"unit": f"faults.live.{name}", "suite": "faults", "kind": "live", "fault": name, "seed": 1} for name in
        ["delayed_startup", "slow_receiver", "saturated_queue", "disconnect", "late_join"]]

    smoke = []
    for topo in ["cs", "p2p"]:
        for enc in ["text", "binary"]:
            for delta in ["off", "on"]:
                for up in ["state", "input"]:
                    name = f"{topo}-{up}"
                    flags = side(name, encoding=enc, **{"down-delta": delta, "up-delta": delta})
                    uid = f"smoke.{topo}.{enc}.delta-{delta}.{up}"
                    smoke.append({"unit": uid, "suite": "smoke", "kind": "single", "comparison": "combination",
                                  "condition": dict(BASELINE, players=3), "seed": 1, "measure_blocks": 6000,
                                  "checkpoint_every": 250, "fixtures": True,
                                  "runs": [{"side": "S", "label": f"{topo} {enc} delta-{delta} {up}", "flags": flags}]})
    suites["smoke"] = smoke

    validation = []
    configs = {
        "cs-state-binary-full": side("cs-state"),
        "cs-input-text-full": side("cs-input", encoding="text"),
        "cs-input-binary-full": side("cs-input"),
        "cs-input-binary-downdelta": side("cs-input", **{"down-delta": "on"}),
        "cs-relay-binary-full": side("cs-relay"),
        "p2p-state-binary-full": side("p2p-state"),
        "game-text-full": side("cs-input", scenario="game", encoding="text"),
        "game-binary-full": side("cs-input", scenario="game"),
        "game-binary-downdelta": side("cs-input", scenario="game", **{"down-delta": "on"}),
    }
    vconds = [BASELINE, {"sweep": "moving", "level": 500, "moving": 500, "static": 100, "players": 2}, STATIC500,
              {"sweep": "players", "level": 4, "moving": 100, "static": 100, "players": 4}]
    gconds = [{"sweep": "clients", "level": 2, "moving": 0, "static": 0, "players": 2},
              {"sweep": "moving", "level": 500, "moving": 500, "static": 0, "players": 2},
              {"sweep": "static", "level": 500, "moving": 0, "static": 500, "players": 2},
              {"sweep": "clients", "level": 4, "moving": 0, "static": 0, "players": 4}]
    for name, flags in configs.items():
        for c in (gconds if name.startswith("game") else vconds):
            validation.append({"unit": f"validation.{name}.{c['sweep']}{c['level']}", "suite": "validation",
                               "kind": "single", "comparison": name, "condition": c, "seed": 1,
                               "checkpoint_every": 500, "fixtures": not name.startswith("game"),
                               "runs": [{"side": "V", "label": name, "flags": flags}]})
    suites["validation"] = validation

    suites["primary"] = pair_units("primary", PRIMARY, engine_conds, rng)
    suites["native-topology"] = pair_units("native-topology", {
        "native-topology": (("cs", side("cs-state")), ("p2p", side("p2p-state")))}, engine_conds, rng)

    keyframe = []
    for c in [BASELINE, STATIC500]:
        for i, seed in enumerate(SEEDS):
            runs = [{"side": f"K{k}", "label": f"keyframe {k}", "flags": side("cs-input", keyframe=k, **{"down-delta": "on"})}
                    for k in (15, 60, 240)]
            shift = i % 3
            runs = runs[shift:] + runs[:shift]   # each setting first, second and third equally often
            keyframe.append({"unit": f"keyframe.{c['sweep']}{c['level']}.s{seed:02d}", "suite": "keyframe",
                             "kind": "set", "comparison": "keyframe", "condition": c, "seed": seed,
                             "order": "-".join(r["side"] for r in runs), "runs": runs})
    suites["keyframe"] = keyframe

    suites["bidirectional-delta"] = pair_units("bidirectional-delta", {
        "bidirectional-delta": (("full", side("cs-input")),
                                ("delta", side("cs-input", **{"down-delta": "on", "up-delta": "on"})))},
        [BASELINE, STATIC500], rng)

    dconds = [{"sweep": "density", "level": d, "moving": 0, "static": 100, "players": 2, "density": d} for d in (0, 10, 50, 100)]
    suites["density"] = pair_units("density", {
        "density": (("full", side("cs-input")), ("delta", side("cs-input", **{"down-delta": "on"})))}, dconds, rng)

    suites["game"] = pair_units("game", GAME, conditions(GAME_SWEEPS), rng)
    return suites


SUITE_ORDER = ["faults", "smoke", "validation", "primary", "native-topology", "keyframe", "bidirectional-delta",
               "density", "game"]

# --- environment and hashes ------------------------------------------------------------


def sh(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception as e:  # noqa: BLE001 - environment probing only
        return f"unavailable: {e}"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def source_hashes():
    files = []
    for base, patterns in [(PROJECT / "engine", ["**/*.cpp", "**/*.hpp", "CMakeLists.txt"]),
                           (PROJECT / "game" / "clockwork_dash", ["*.cpp", "*.hpp", "CMakeLists.txt"]),
                           (ROOT / "harness", ["*.cpp", "*.hpp"]),
                           (ROOT, ["CMakeLists.txt"]),
                           (TOOLS, ["campaign.py"])]:
        for pattern in patterns:
            files.extend(sorted(base.glob(pattern)))
    out = {}
    for f in sorted(set(files)):
        out[str(f.relative_to(PROJECT))] = sha256(f)
    aggregate = hashlib.sha256("".join(f"{k}={v}\n" for k, v in sorted(out.items())).encode()).hexdigest()
    return out, aggregate


def build_info():
    cache = {}
    try:
        for line in open(BUILD / "CMakeCache.txt"):
            if "=" in line and ":" in line.split("=")[0]:
                k, v = line.rstrip("\n").split("=", 1)
                cache[k.split(":")[0]] = v
    except OSError:
        pass
    return {
        "build_type": cache.get("CMAKE_BUILD_TYPE"),
        "cxx_compiler": cache.get("CMAKE_CXX_COMPILER"),
        "cxx_flags": cache.get("CMAKE_CXX_FLAGS"),
        "cxx_flags_release": cache.get("CMAKE_CXX_FLAGS_RELEASE"),
        "compiler_version": sh(["c++", "--version"]).splitlines()[0] if sh(["c++", "--version"]) else "",
        "cmake_version": sh(["cmake", "--version"]).splitlines()[0],
        "libzmq": sh(["pkg-config", "--modversion", "libzmq"]),
        "executables": {p.name: sha256(p) for p in (BENCH, FAULTS)},
    }


def power_source():
    """'AC Power' or 'Battery Power', from pmset."""
    out = sh(["pmset", "-g", "ps"])
    return out.split("'")[1] if out.count("'") >= 2 else out[:40]


def environment():
    return {
        "os": sh(["sw_vers"]),
        "uname": platform.uname()._asdict(),
        "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "cores": sh(["sysctl", "-n", "hw.ncpu"]),
        "perf_cores": sh(["sysctl", "-n", "hw.perflevel0.physicalcpu"]),
        "efficiency_cores": sh(["sysctl", "-n", "hw.perflevel1.physicalcpu"]),
        "memory_bytes": sh(["sysctl", "-n", "hw.memsize"]),
        "python": sys.version,
        "power": sh(["pmset", "-g", "batt"]),
        "thermal": sh(["pmset", "-g", "therm"]),
        "loadavg": os.getloadavg(),
    }


# --- launching one run -----------------------------------------------------------------


def common_args(run_id, campaign, unit, run, port_base, oracle=None, timeout=TIMEOUT_S):
    flags = dict(run["flags"])
    c = unit["condition"]
    args = ["--run", run_id, "--campaign", campaign, "--seed", str(unit["seed"]),
            "--players", str(c["players"]), "--moving", str(c["moving"]), "--static", str(c["static"]),
            "--warmup-blocks", str(WARMUP_BLOCKS), "--measure-blocks", str(unit.get("measure_blocks", MEASURE_BLOCKS)),
            "--window", str(WINDOW), "--hwm", str(unit.get("hwm", HWM)), "--timeout", str(unit.get("timeout", timeout)),
            "--port-base", str(port_base), "--checkpoint-every", str(unit.get("checkpoint_every", 0)),
            "--fixtures", "on" if unit.get("fixtures") else "off"]
    if "density" in c:
        args += ["--density", str(c["density"])]
    for k, v in flags.items():
        args += [f"--{k}", str(v)]
    if unit.get("no_flow_control"):
        args += ["--no-flow-control", "on"]
    return args


def oracle_for(cdir, unit, run):
    """The canonical states for this workload, generated once and cached."""
    args = common_args("oracle", "oracle", unit, run, 20000)
    key_fields = {k: v for k, v in zip(args[::2], args[1::2]) if k not in
                  ("--run", "--campaign", "--port-base", "--timeout", "--hwm", "--window", "--encoding",
                   "--down-delta", "--up-delta", "--keyframe")}
    key = hashlib.sha256(json.dumps(key_fields, sort_keys=True).encode()).hexdigest()[:20]
    odir = cdir / "oracle"
    odir.mkdir(exist_ok=True)
    path = odir / f"{key}.txt"
    meta = odir / f"{key}.json"
    if not path.exists() or not meta.exists():
        tmp = odir / f"{key}.tmp"
        r = subprocess.run([str(BENCH), "oracle"] + args + ["--out", str(tmp)], capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            raise RuntimeError(f"oracle failed: {r.stderr}")
        os.replace(tmp, path)
        os.replace(str(tmp) + ".trace.csv", str(path) + ".trace.csv")
        line = next(l for l in r.stdout.splitlines() if l.startswith("RESULT"))
        res = json.loads(line[7:])
        res["key_fields"] = key_fields
        meta.write_text(json.dumps(res, indent=1))
    return path


def endpoint_names(unit, run):
    p = unit["condition"]["players"]
    if run["flags"]["topology"] == "cs":
        return ["server"] + [f"client{i}" for i in range(p)]
    return [f"peer{i}" for i in range(p)]


def launch(cdir, campaign, unit, run, run_id, port_base, extra=None, late=None):
    """Starts the coordinator and every endpoint; returns per-process results."""
    rdir = cdir / "runs" / run_id
    rdir.mkdir(parents=True, exist_ok=True)
    oracle = oracle_for(cdir, unit, run)
    args = common_args(run_id, campaign, unit, run, port_base)
    extra = extra or {}
    procs = {}

    def start(name, role, more):
        out = open(rdir / f"{name}.out", "w")
        err = open(rdir / f"{name}.err", "w")
        cmd = [str(BENCH), role] + args + more
        (rdir / f"{name}.cmd").write_text(" ".join(cmd) + "\n")
        p = subprocess.Popen(cmd, stdout=out, stderr=err, start_new_session=True)
        procs[name] = (p, out, err)

    load_before = os.getloadavg()
    power = power_source()
    t0 = time.time()
    start("coordinator", "coordinator", [])
    time.sleep(0.05)
    for name in endpoint_names(unit, run):
        role = "server" if name == "server" else "client" if name.startswith("client") else "peer"
        more = ["--oracle", str(oracle)]
        if role != "server":
            more += ["--id", name.lstrip("clientpeer")]
        start(name, role, more + extra.get(name, []))
    if late:
        time.sleep(late["after_s"])
        start(late["name"], late["role"], ["--oracle", str(oracle)] + late["flags"])

    deadline = time.time() + unit.get("timeout", TIMEOUT_S) + 180
    early = None
    coord = procs["coordinator"][0]
    while time.time() < deadline:
        if coord.poll() is not None:
            break
        for name, (p, _, _) in procs.items():
            if name != "coordinator" and p.poll() not in (None, 0) and early is None:
                early = f"{name} exited with code {p.returncode} before the run finished"
        if early:
            break
        time.sleep(0.05)
    if early or coord.poll() is None:
        time.sleep(0.5)   # let the coordinator's abort reach the others
    grace = time.time() + (5 if early else 60)
    for name, (p, _, _) in procs.items():
        while p.poll() is None and time.time() < grace:
            time.sleep(0.05)
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
    for p, out, err in procs.values():
        out.close()
        err.close()
    wall = time.time() - t0
    exits = {name: p.returncode for name, (p, _, _) in procs.items()}
    results = {}
    for name in procs:
        lines = [l for l in (rdir / f"{name}.out").read_text().splitlines() if l.startswith("RESULT ")]
        results[name] = lines
    return {"exits": exits, "results": results, "early": early, "wall_s": wall, "load_before": load_before, "power": power,
            "power_after": power_source(),
            "load_after": os.getloadavg(), "oracle": str(oracle.relative_to(cdir)), "dir": str(rdir.relative_to(cdir))}


# --- validation ------------------------------------------------------------------------


def expected_streams(unit, run):
    """Producer stream -> its consumers, and per-endpoint block/tick counts."""
    f = run["flags"]
    n = unit["condition"]["players"]
    cs = f["topology"] == "cs"
    native = cs and f.get("world", "native") == "native"
    measure = unit.get("measure_blocks", MEASURE_BLOCKS)
    streams, counts = {}, {}
    if native:
        streams[0x0100] = ("server", [f"client{i}" for i in range(n)])
        for i in range(n):
            streams[0x0200 + i] = (f"client{i}", ["server"])
            counts[f"client{i}"] = (measure, 2 * measure if f["upstream"] == "state" else 0)
        counts["server"] = (measure, 2 * measure)
    elif cs:
        for i in range(n):
            streams[0x0200 + i] = (f"client{i}", ["server"])
            streams[0x0300 + i] = ("server", [f"client{j}" for j in range(n) if j != i])
            counts[f"client{i}"] = (measure, 2 * measure)
        counts["server"] = (n * measure, 0)
    else:
        for i in range(n):
            streams[0x0400 + i] = (f"peer{i}", [f"peer{j}" for j in range(n) if j != i])
            if f["upstream"] == "input":
                streams[0x0500 + i] = (f"peer{i}", [f"peer{j}" for j in range(n) if j != i])
            counts[f"peer{i}"] = (measure, 2 * measure)
    return streams, counts


def validate(unit, run, run_id, launched):
    reasons = []
    names = ["coordinator"] + endpoint_names(unit, run)
    parsed = {}
    for name in names:
        code = launched["exits"].get(name)
        lines = launched["results"].get(name, [])
        if code != 0:
            reasons.append(f"{name}: exit code {code}")
        if len(lines) != 1:
            reasons.append(f"{name}: {len(lines)} RESULT lines")
            continue
        try:
            r = json.loads(lines[0][7:])
        except json.JSONDecodeError as e:
            reasons.append(f"{name}: malformed RESULT ({e})")
            continue
        parsed[name] = r
        if r.get("run") != run_id:
            reasons.append(f"{name}: run id {r.get('run')!r}")
        if r.get("status") != "ok":
            reasons.append(f"{name}: status {r.get('status')} ({r.get('error')})")
    for extra in set(launched["results"]) - set(names):
        reasons.append(f"unexpected process {extra}")
    if launched.get("early"):
        reasons.append(launched["early"])
    if reasons or len(parsed) != len(names):
        return False, reasons, parsed

    coord = parsed["coordinator"]
    e2e = coord.get("end_to_end_ns", 0)
    if not (isinstance(e2e, int) and e2e > 0):
        reasons.append("coordinator: no end-to-end time")
    options = {parsed[n]["options"] for n in names}
    if len(options) != 1:
        reasons.append("endpoints disagree on options")
    measure = unit.get("measure_blocks", MEASURE_BLOCKS)
    want = {"encoding": run["flags"]["encoding"], "measure_blocks": str(measure), "players": str(unit["condition"]["players"]),
            "moving": str(unit["condition"]["moving"]), "static": str(unit["condition"]["static"]), "seed": str(unit["seed"])}
    opt = dict(kv.split("=", 1) for kv in coord["options"].split())
    for k, v in want.items():
        if opt.get(k) != v:
            reasons.append(f"option {k}={opt.get(k)} expected {v}")

    streams, counts = expected_streams(unit, run)
    for name in names[1:]:
        r = parsed[name]
        m = r["measure"]
        if not (m["wall_ns"] > 0 and m["busy_ns"] >= 0 and math.isfinite(m["wall_ns"])):
            reasons.append(f"{name}: non-positive measured time")
        blocks, ticks = counts[name]
        if m["blocks_produced"] != blocks:
            reasons.append(f"{name}: produced {m['blocks_produced']} blocks, expected {blocks}")
        if m["ticks"] != ticks:
            reasons.append(f"{name}: simulated {m['ticks']} ticks, expected {ticks}")
        v = r["verify"]
        if not r.get("oracle") or not v.get("ok") or r["verify_final"].get("compared", 0) <= 0:
            reasons.append(f"{name}: state verification failed {v.get('first', [])[:2]}")
        if not r["probe"]["ok"]:
            reasons.append(f"{name}: clock probe failed")
        if r["control"].get("rejected"):
            reasons.append(f"{name}: rejected control messages")
        ri = r.get("role_info", {})
        if ri.get("corrections_mismatched", 0):
            reasons.append(f"{name}: correction mismatches")
        for s in r["out"]:
            if s["measure"]["send_failures"] or s["warmup"]["send_failures"]:
                reasons.append(f"{name}: send failures on stream {s['stream']:#06x}")

    # Every produced message on every stream was applied by every consumer.
    for sid, (producer, consumers) in streams.items():
        outs = {s["stream"]: s for s in parsed[producer]["out"]}
        if sid not in outs:
            reasons.append(f"{producer}: stream {sid:#06x} missing")
            continue
        o = outs[sid]
        produced_measure = o["measure"]["frames"]
        if sid in (0x0100,) or 0x0200 <= sid < 0x0300 or 0x0300 <= sid < 0x0400 or 0x0400 <= sid < 0x0500:
            if produced_measure != measure:
                reasons.append(f"{producer}: {produced_measure} measured publications on {sid:#06x}, expected {measure}")
        for cname in consumers:
            ins = {s["stream"]: s for s in parsed[cname]["in"]}
            if sid not in ins:
                reasons.append(f"{cname}: never received stream {sid:#06x}")
                continue
            i = ins[sid]
            for ph in ("warmup", "measure"):
                c = i[ph]
                bad = {k: c[k] for k in ("gaps", "missing", "duplicates", "stale", "unsynced", "malformed") if c[k]}
                if bad:
                    reasons.append(f"{cname}: stream {sid:#06x} {ph} {bad}")
            if i["measure"]["applied"] != produced_measure:
                reasons.append(f"{cname}: applied {i['measure']['applied']} of {produced_measure} on {sid:#06x}")
            if i["last_seq"] != o["next_seq"] - 1:
                reasons.append(f"{cname}: last seq {i['last_seq']} on {sid:#06x}, producer at {o['next_seq'] - 1}")
    return not reasons, reasons, parsed


# --- summaries -------------------------------------------------------------------------


def summarize(unit, run, parsed):
    names = endpoint_names(unit, run)
    eps = [parsed[n] for n in names]
    S = lambda f: sum(f(r) for r in eps)  # noqa: E731

    def stream(r, sid, direction="out"):
        for s in r[direction]:
            if s["stream"] == sid:
                return s["measure"]
        return None

    row = {
        "e2e_s": parsed["coordinator"]["end_to_end_ns"] / 1e9,
        "setup_s": parsed["coordinator"]["setup_ns"] / 1e9,
        "warmup_s": parsed["coordinator"]["warmup_ns"] / 1e9,
        "busy_total_s": S(lambda r: r["measure"]["busy_ns"]) / 1e9,
        "wait_total_s": S(lambda r: r["measure"]["times"]["wait_ns"]) / 1e9,
        "cpu_total_s": S(lambda r: r["measure"]["cpu_user_ns"] + r["measure"]["cpu_sys_ns"]) / 1e9,
        "rss_total_mb": S(lambda r: r["measure"]["max_rss_bytes"]) / 1e6,
        "rss_max_mb": max(r["measure"]["max_rss_bytes"] for r in eps) / 1e6,
        "sim_total_s": S(lambda r: r["measure"]["times"]["sim_ns"]) / 1e9,
        "encode_total_s": S(lambda r: r["measure"]["times"]["encode_ns"]) / 1e9,
        "decode_total_s": S(lambda r: r["measure"]["times"]["decode_ns"]) / 1e9,
        "frame_total_s": S(lambda r: r["measure"]["times"]["frame_ns"]) / 1e9,
        "send_total_s": S(lambda r: r["measure"]["times"]["send_ns"]) / 1e9,
        "received_frame_bytes": S(lambda r: sum(s["measure"]["frame_bytes"] for s in r["in"]) +
                                  sum(c["in_measure"]["frame_bytes"] for c in r["control"].values())),
        "received_payload_bytes": S(lambda r: sum(s["measure"]["payload_bytes"] for s in r["in"])),
        "sent_payload_bytes": S(lambda r: sum(s["measure"]["payload_bytes"] for s in r["out"])),
        "control_frames_sent": S(lambda r: sum(c["out_measure"]["frames"] for c in r["control"].values())),
        "control_bytes_sent": S(lambda r: sum(c["out_measure"]["frame_bytes"] for c in r["control"].values())),
        "decoded_messages": S(lambda r: sum(s["measure"]["applied"] for s in r["in"])),
        "max_rss_endpoint": max(names, key=lambda n: parsed[n]["measure"]["max_rss_bytes"]),
        "objects_total": parsed[names[0]]["counts"].get("total"),
        "objects_networked": parsed[names[0]]["counts"].get("networked"),
    }
    if "server" in parsed:
        s = parsed["server"]
        row.update({
            "server_wall_s": s["measure"]["wall_ns"] / 1e9,
            "server_busy_s": s["measure"]["busy_ns"] / 1e9,
            "server_cpu_s": (s["measure"]["cpu_user_ns"] + s["measure"]["cpu_sys_ns"]) / 1e9,
            "server_sim_s": s["measure"]["times"]["sim_ns"] / 1e9,
            "server_encode_s": s["measure"]["times"]["encode_ns"] / 1e9,
            "server_rss_mb": s["measure"]["max_rss_bytes"] / 1e6,
        })
        w = stream(s, 0x0100)
        if w:
            row.update({"down_publications": w["frames"], "down_payload_per_pub": w["payload_bytes"] / max(1, w["frames"]),
                        "down_frame_per_pub": w["frame_bytes"] / max(1, w["frames"]), "down_keyframes": w["keyframes"],
                        "down_objects_per_pub": w["objects"] / max(1, w["frames"]), "down_removals": w["removals"],
                        "down_encode_us": w["encode_ns"] / max(1, w["frames"]) / 1e3})
    clients = [parsed[n] for n in names if n.startswith("client")]
    if clients:
        up = [stream(c, 0x0200 + c["id"]) for c in clients]
        row.update({
            "client_wall_max_s": max(c["measure"]["wall_ns"] for c in clients) / 1e9,
            "client_busy_mean_s": sum(c["measure"]["busy_ns"] for c in clients) / len(clients) / 1e9,
            "client_cpu_total_s": sum(c["measure"]["cpu_user_ns"] + c["measure"]["cpu_sys_ns"] for c in clients) / 1e9,
            "client_decode_total_s": sum(c["measure"]["times"]["decode_ns"] for c in clients) / 1e9,
            "client_sim_total_s": sum(c["measure"]["times"]["sim_ns"] for c in clients) / 1e9,
            "client_send_total_s": sum(c["measure"]["times"]["send_ns"] for c in clients) / 1e9,
            "up_payload_per_msg": sum(u["payload_bytes"] for u in up) / max(1, sum(u["frames"] for u in up)),
            "up_frame_per_msg": sum(u["frame_bytes"] for u in up) / max(1, sum(u["frames"] for u in up)),
        })
        downs = [stream(c, 0x0100, "in") for c in clients]
        if all(downs):
            row["client_decode_us_per_msg"] = sum(d["decode_ns"] for d in downs) / max(1, sum(d["applied"] for d in downs)) / 1e3
    peers = [parsed[n] for n in names if n.startswith("peer")]
    if peers:
        own = [stream(p, 0x0400 + p["id"]) for p in peers]
        row.update({
            "peer_wall_max_s": max(p["measure"]["wall_ns"] for p in peers) / 1e9,
            "peer_busy_mean_s": sum(p["measure"]["busy_ns"] for p in peers) / len(peers) / 1e9,
            "peer_cpu_total_s": sum(p["measure"]["cpu_user_ns"] + p["measure"]["cpu_sys_ns"] for p in peers) / 1e9,
            "up_payload_per_msg": sum(u["payload_bytes"] for u in own) / max(1, sum(u["frames"] for u in own)),
            "up_frame_per_msg": sum(u["frame_bytes"] for u in own) / max(1, sum(u["frames"] for u in own)),
        })
    return row


# --- executing the schedule ------------------------------------------------------------


class Campaign:
    def __init__(self, name):
        self.name = name
        self.dir = RESULTS / name
        self.manifest_path = self.dir / "manifest.json"
        self.ledger_path = self.dir / "ledger.jsonl"
        self.runs_path = self.dir / "runs.jsonl"
        self.port_counter = 0
        self.allow_battery = False

    def load(self):
        text = self.manifest_path.read_text()
        stored = (self.dir / "manifest.sha256").read_text().split()[0]
        if hashlib.sha256(text.encode()).hexdigest() != stored:
            sys.exit("manifest.json does not match manifest.sha256: the frozen manifest was edited")
        self.manifest = json.loads(text)
        _, aggregate = source_hashes()
        if aggregate != self.manifest["sources"]["aggregate"]:
            sys.exit("sources changed since the manifest was frozen; a changed workload starts a new campaign")
        for exe, h in self.manifest["build"]["executables"].items():
            if sha256(BUILD / exe) != h:
                sys.exit(f"{exe} changed since the manifest was frozen; a changed executable starts a new campaign")
        if self.ledger_path.exists():
            self.port_counter = sum(1 for _ in open(self.ledger_path))

    def ledger(self, entry):
        with open(self.ledger_path, "a") as f:
            f.write(json.dumps(entry) + "\n")

    def completed_units(self):
        done = {}
        if self.ledger_path.exists():
            for line in open(self.ledger_path):
                e = json.loads(line)
                if e.get("type") == "unit":
                    done[e["unit"]] = e
        return done

    def wait_for_ac(self):
        """Machine conditions must stay stable: runs start only on mains power."""
        if self.allow_battery or power_source() == "AC Power":
            return
        self.ledger({"type": "pause", "reason": "battery power", "time": time.time()})
        print("    paused: running on battery power; waiting for the charger", flush=True)
        while power_source() != "AC Power":
            time.sleep(15)
        self.ledger({"type": "resume", "time": time.time()})
        print("    resumed on AC power", flush=True)

    def next_port(self):
        self.port_counter += 1
        return 22000 + (self.port_counter % 300) * 60

    def run_unit(self, unit):
        if unit["kind"] == "inprocess":
            return self.run_inprocess_faults(unit)
        if unit["kind"] == "live":
            return self.run_live_fault(unit)
        attempts = []
        for attempt in range(1, MAX_ATTEMPTS + 1):
            ok_all = True
            rows = []
            for run in unit["runs"]:
                run_id = f"{unit['unit']}.a{attempt}.{run['side']}"
                self.wait_for_ac()
                started = time.time()
                launched = launch(self.dir, self.name, unit, run, run_id, self.next_port())
                ok, reasons, parsed = validate(unit, run, run_id, launched)
                if not self.allow_battery and (launched["power"] != "AC Power" or launched["power_after"] != "AC Power"):
                    ok = False
                    reasons.append(f"power source was {launched['power']} at start and {launched['power_after']} at end")
                (self.dir / launched["dir"] / "validation.json").write_text(json.dumps({"valid": ok, "reasons": reasons}, indent=1))
                entry = {"type": "run", "run": run_id, "unit": unit["unit"], "suite": unit["suite"], "attempt": attempt,
                         "side": run["side"], "label": run["label"], "valid": ok, "reasons": reasons[:20],
                         "start": started, "end": time.time(), "wall_s": launched["wall_s"],
                         "load_before": launched["load_before"], "load_after": launched["load_after"],
                         "power": launched["power"], "power_after": launched["power_after"],
                         "exits": launched["exits"], "dir": launched["dir"]}
                self.ledger(entry)
                if ok:
                    row = {"run": run_id, "unit": unit["unit"], "suite": unit["suite"], "comparison": unit["comparison"],
                           "sweep": unit["condition"]["sweep"], "level": unit["condition"]["level"],
                           "moving": unit["condition"]["moving"], "static": unit["condition"]["static"],
                           "players": unit["condition"]["players"], "density": unit["condition"].get("density"),
                           "seed": unit["seed"], "order": unit.get("order"), "side": run["side"], "label": run["label"],
                           "attempt": attempt, "position": unit["runs"].index(run), "load_before": launched["load_before"][0],
                           "oracle": launched["oracle"]}
                    row.update(summarize(unit, run, parsed))
                    rows.append(row)
                else:
                    ok_all = False
                    print(f"    INVALID {run_id}: {reasons[:3]}", flush=True)
                    break
            attempts.append(attempt)
            if ok_all:
                with open(self.runs_path, "a") as f:
                    for row in rows:
                        f.write(json.dumps(row) + "\n")
                self.ledger({"type": "unit", "unit": unit["unit"], "suite": unit["suite"], "status": "complete",
                             "attempts": attempt, "time": time.time()})
                return True
        self.ledger({"type": "unit", "unit": unit["unit"], "suite": unit["suite"], "status": "failed",
                     "attempts": len(attempts), "time": time.time()})
        return False

    def run_inprocess_faults(self, unit):
        r = subprocess.run([str(FAULTS)], capture_output=True, text=True, timeout=600)
        out = self.dir / "faults"
        out.mkdir(exist_ok=True)
        (out / "inprocess.txt").write_text(r.stdout + r.stderr)
        cases = [json.loads(l[5:]) for l in r.stdout.splitlines() if l.startswith("CASE ")]
        ok = r.returncode == 0 and cases and all(c["pass"] for c in cases)
        self.ledger({"type": "fault", "unit": unit["unit"], "pass": ok, "cases": cases, "time": time.time()})
        self.ledger({"type": "unit", "unit": unit["unit"], "suite": "faults", "status": "complete" if ok else "failed",
                     "attempts": 1, "time": time.time()})
        return ok

    def run_live_fault(self, unit):
        """Live fixtures against real processes. Each has its own expected
        outcome; none of these runs enters any performance average."""
        fault = unit["fault"]
        base = {"unit": unit["unit"], "suite": "faults", "comparison": "fault", "seed": 1,
                "condition": dict(BASELINE), "measure_blocks": 2000, "timeout": 30}
        run = {"side": "F", "label": fault, "flags": side("cs-input", **({"down-delta": "on"} if fault == "saturated_queue" else {}))}
        extra, late, expect = {}, None, ""
        if fault == "delayed_startup":
            extra = {"client1": ["--delay-start-ms", "3000"]}
            expect = "valid run; only setup takes longer; final state equals the oracle"
        elif fault == "slow_receiver":
            extra = {"client0": ["--slow-us", "300"]}
            expect = "valid run: flow control holds the server back instead of losing messages"
        elif fault == "saturated_queue":
            base.update({"no_flow_control": True, "hwm": 2})
            extra = {"client0": ["--slow-us", "500"]}
            expect = "invalid run: queue overflow drops messages; gaps detected, delta stream unsynchronized"
        elif fault == "disconnect":
            extra = {"client1": ["--kill-at-block", "1000"]}
            expect = "invalid run: a client vanishes mid-measurement; nothing hangs and the failure is recorded"
        elif fault == "late_join":
            base["measure_blocks"] = 50000
            late = {"after_s": 1.0, "name": "client1-late", "role": "client", "flags": ["--id", "1", "--timeout", "5"]}
            expect = "invalid run: an endpoint joining after the warm-up release is refused and the run aborts"
        unit = {**unit, **base}
        run_id = f"{unit['unit']}.a1.F"
        launched = launch(self.dir, self.name, unit, run, run_id, self.next_port(), extra=extra, late=late)
        if late:
            launched["results"].pop(late["name"], None)
            launched["exits"].pop(late["name"], None)
        ok, reasons, parsed = validate(unit, run, run_id, launched)
        detail = {}
        if fault == "delayed_startup":
            passed = ok and parsed["coordinator"]["setup_ns"] > 3e9
            detail = {"setup_s": parsed.get("coordinator", {}).get("setup_ns", 0) / 1e9}
        elif fault == "slow_receiver":
            passed = ok
            detail = {"e2e_s": parsed.get("coordinator", {}).get("end_to_end_ns", 0) / 1e9}
        elif fault == "saturated_queue":
            stream = next((s for s in parsed.get("client0", {}).get("in", []) if s["stream"] == 0x0100), None)
            evidence = {}
            if stream:
                w, m = stream["warmup"], stream["measure"]
                evidence = {"gaps": w["gaps"] + m["gaps"], "missing": w["missing"] + m["missing"],
                            "unsynced_rejected": w["unsynced"] + m["unsynced"],
                            "unsync_history": stream["unsync_history"][:5], "applied": w["applied"] + m["applied"],
                            "outcome": parsed.get("coordinator", {}).get("error", "")}
            passed = (not ok) and bool(evidence) and evidence["gaps"] > 0
            detail = {"client0_world_stream": evidence}
        elif fault == "disconnect":
            passed = (not ok) and any("client1" in x for x in reasons)
        else:
            passed = (not ok) and any("late" in x or "beacon" in x or "aborted" in x for x in reasons)
        entry = {"type": "fault", "unit": unit["unit"], "fault": fault, "expect": expect, "pass": passed, "valid_run": ok,
                 "reasons": reasons[:8], "detail": detail, "dir": launched["dir"], "time": time.time()}
        self.ledger(entry)
        self.ledger({"type": "unit", "unit": unit["unit"], "suite": "faults", "status": "complete" if passed else "failed",
                     "attempts": 1, "time": time.time()})
        print(f"    fault {fault}: {'PASS' if passed else 'FAIL'} ({expect})", flush=True)
        return passed


def cmd_create(a):
    c = Campaign(a.name)
    if c.manifest_path.exists():
        sys.exit(f"{c.manifest_path} exists; a campaign is created once")
    c.dir.mkdir(parents=True)
    sources, aggregate = source_hashes()
    schedule = build_schedule()
    manifest = {
        "campaign": a.name,
        "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "purpose": a.purpose,
        "protocol": {"warmup_blocks": WARMUP_BLOCKS, "measure_blocks": MEASURE_BLOCKS, "ticks_per_block": 2,
                     "tick_seconds": 1 / 120, "window_blocks": WINDOW, "hwm_messages": HWM, "keyframe": KEYFRAME,
                     "seeds": SEEDS, "timeout_s": TIMEOUT_S, "max_attempts_per_unit": MAX_ATTEMPTS,
                     "schedule_seed": SCHEDULE_SEED},
        "rules": {
            "validity": "every expected process exits 0 with exactly one RESULT line for this run id; identical options; "
                        "exact block/tick counts; every produced message on every stream applied by every consumer "
                        "with no gap, duplicate, stale, unsynchronized or malformed message; no send failure; "
                        "initial, warm-up-end and final canonical state equal to the oracle; clock probe ok",
            "retries": "a unit with any invalid run is repeated with new run ids, at most twice; every attempt stays in the ledger",
            "machine_conditions": "runs start only on AC power (the runner pauses otherwise); a run not on AC power at both "
                                  "start and end is invalid and its unit is repeated",
            "machine_health_exclusions": "none: no valid run is excluded; load averages are recorded per run",
            "analysis": "paired run-level differences and ratios, mean, sample variance, standard deviation, "
                        "95% paired t interval (df = pairs - 1); ratio interval from log ratios",
        },
        "environment": environment(),
        "build": build_info(),
        "sources": {"files": sources, "aggregate": aggregate},
        "suite_order": SUITE_ORDER,
        "schedule": schedule,
    }
    text = json.dumps(manifest, indent=1)
    c.manifest_path.write_text(text)
    (c.dir / "manifest.sha256").write_text(hashlib.sha256(text.encode()).hexdigest() + "  manifest.json\n")
    total = sum(len(u.get("runs", [1])) for s in schedule.values() for u in s)
    print(f"created {c.manifest_path}: {sum(len(s) for s in schedule.values())} units, {total} runs")


def cmd_run(a):
    c = Campaign(a.name)
    c.load()
    if power_source() != "AC Power" and not a.allow_battery:
        sys.exit("refusing to start on battery power: plug in the charger (or pass --allow-battery)")
    c.allow_battery = a.allow_battery
    c.ledger({"type": "session", "start": time.time(), "power": power_source(), "thermal": sh(["pmset", "-g", "therm"]),
              "loadavg": os.getloadavg()})
    suites = a.suites.split(",") if a.suites else c.manifest["suite_order"]
    done = c.completed_units()
    executed = failed = 0
    for suite in suites:
        units = c.manifest["schedule"][suite]
        print(f"suite {suite}: {len(units)} units", flush=True)
        for i, unit in enumerate(units):
            prev = done.get(unit["unit"])
            if prev and prev["status"] in ("complete", "failed"):
                continue
            if a.max_units and executed >= a.max_units:
                print("stopping: --max-units reached")
                return 0
            t = time.time()
            ok = c.run_unit(unit)
            executed += 1
            failed += 0 if ok else 1
            print(f"  [{suite} {i + 1}/{len(units)}] {unit['unit']}: {'ok' if ok else 'FAILED'} ({time.time() - t:.1f}s)", flush=True)
    done = c.completed_units()
    bad = [u for u, e in done.items() if e["status"] != "complete"]
    print(f"finished: {executed} units run now, {len(bad)} failed overall")
    return 1 if bad else 0


def cmd_status(a):
    c = Campaign(a.name)
    manifest = json.loads(c.manifest_path.read_text())
    done = c.completed_units()
    for suite in manifest["suite_order"]:
        units = manifest["schedule"][suite]
        complete = sum(1 for u in units if done.get(u["unit"], {}).get("status") == "complete")
        failed = sum(1 for u in units if done.get(u["unit"], {}).get("status") == "failed")
        print(f"{suite:22s} {complete:4d}/{len(units):4d} complete, {failed} failed")
    attempts = sum(1 for l in open(c.ledger_path) if '"type": "run"' in l) if c.ledger_path.exists() else 0
    print(f"run attempts in ledger: {attempts}")


def cmd_pilot(a):
    """A short pilot at the extreme conditions, to check runtimes, queue depths
    and instrumentation. Its numbers never enter the final campaign."""
    c = Campaign(a.name)
    c.dir.mkdir(parents=True, exist_ok=True)
    pilots = []
    extremes = [{"sweep": "moving", "level": 500, "moving": 500, "static": 100, "players": 2},
                {"sweep": "static", "level": 500, "moving": 100, "static": 500, "players": 2},
                {"sweep": "players", "level": 4, "moving": 100, "static": 100, "players": 4}]
    for comp, ((la, fa), (lb, fb)) in list(PRIMARY.items()) + list(GAME.items()):
        conds = extremes if not comp.startswith("game") else [{"sweep": "moving", "level": 500, "moving": 500, "static": 0, "players": 2}]
        for cond in conds:
            for label, flags in ((la, fa), (lb, fb)):
                pilots.append({"unit": f"pilot.{comp}.{cond['sweep']}{cond['level']}.{label}", "suite": "pilot", "kind": "single",
                               "comparison": comp, "condition": cond, "seed": 1,
                               "runs": [{"side": "P", "label": label, "flags": flags}]})
    (c.dir / "pilot_schedule.json").write_text(json.dumps(pilots, indent=1))
    rows = []
    for u in pilots:
        run = u["runs"][0]
        run_id = f"{u['unit']}.a1.P"
        launched = launch(c.dir, a.name, u, run, run_id, c.next_port())
        ok, reasons, parsed = validate(u, run, run_id, launched)
        row = {"unit": u["unit"], "valid": ok, "reasons": reasons[:3], "wall_s": launched["wall_s"]}
        if ok:
            s = summarize(u, run, parsed)
            row.update({k: s.get(k) for k in ("e2e_s", "server_wall_s", "peer_wall_max_s", "cpu_total_s", "rss_max_mb",
                                               "down_payload_per_pub", "frame_total_s")})
            row["max_queued"] = parsed.get("server", {}).get("role_info", {}).get("max_queued")
        rows.append(row)
        print(json.dumps(row), flush=True)
    (c.dir / "pilot_results.json").write_text(json.dumps(rows, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("create")
    p.add_argument("--name", required=True)
    p.add_argument("--purpose", default="final rerun campaign per final_benchmark_plan.md")
    p = sub.add_parser("run")
    p.add_argument("--name", required=True)
    p.add_argument("--suites")
    p.add_argument("--max-units", type=int, default=0)
    p.add_argument("--allow-battery", action="store_true")
    p = sub.add_parser("status")
    p.add_argument("--name", required=True)
    p = sub.add_parser("pilot")
    p.add_argument("--name", required=True)
    a = ap.parse_args()
    for exe in (BENCH, FAULTS):
        if not exe.exists():
            sys.exit(f"{exe} not built: cmake -S {ROOT} -B {BUILD} -DCMAKE_BUILD_TYPE=Release && cmake --build {BUILD}")
    return {"create": cmd_create, "run": cmd_run, "status": cmd_status, "pilot": cmd_pilot}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
