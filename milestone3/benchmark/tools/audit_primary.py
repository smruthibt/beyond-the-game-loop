#!/usr/bin/env python3
"""Independent audit of a campaign's correctness gates and primary suite.

    python3 benchmark/tools/audit_primary.py --name NAME

Recounts from the raw per-process RESULT lines in results/NAME/runs/ and the
ledger, without using the runner's verdicts or runs.jsonl, and writes
results/NAME/analysis/audit_primary.json. Checks, for every scheduled unit of
the smoke, validation and primary suites:

  - the unit completed on its first attempt; every run attempt is valid;
  - every run started and ended on AC power; every process exited 0;
  - each endpoint produced exactly the measured block count on every data
    stream, and every consumer applied all of them with no gap, duplicate,
    stale, unsynchronized or malformed message;
  - every endpoint's state equalled the oracle at the initial, warm-up-end
    and final checkpoints;
  - primary only: each comparison x condition has ten seeds, five run AB and
    five BA, and the ledger's start times follow the scheduled order;
  - sources, executables and the manifest still match their frozen hashes.

analyze.py's audit expects a server world stream in every client-server run;
relay runs have none (the server only forwards client streams), so it reports
"0 downstream publications" for them. This audit counts the streams each mode
actually has.
"""
import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent.parent
RESULTS = TOOLS.parent / "results"
DATA = lambda sid: 0x0100 <= sid < 0x0500  # noqa: E731  world, upstream, relay forward, peer streams
BAD = ("gaps", "missing", "duplicates", "stale", "unsynced", "malformed")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def results(run_dir):
    out = {}
    for f in sorted(run_dir.glob("*.out")):
        lines = [l for l in f.read_text().splitlines() if l.startswith("RESULT ")]
        out[f.stem] = [json.loads(l[7:]) for l in lines]
    return out


def check_run(run_dir, run_id, measure):
    problems = []
    res = results(run_dir)
    if set(res) == set():
        return [f"{run_id}: no RESULT files"], {}
    for name, lines in res.items():
        if len(lines) != 1:
            problems.append(f"{run_id}/{name}: {len(lines)} RESULT lines")
    res = {n: l[0] for n, l in res.items() if len(l) == 1}
    coord = res.get("coordinator")
    if not coord or coord.get("status") != "ok" or not coord.get("end_to_end_ns"):
        problems.append(f"{run_id}: coordinator result missing or not ok")
    eps = {n: r for n, r in res.items() if n != "coordinator"}
    produced = {}
    for name, r in eps.items():
        if r.get("run") != run_id or r.get("status") != "ok":
            problems.append(f"{run_id}/{name}: run id or status wrong")
        opt = dict(kv.split("=", 1) for kv in r["options"].split())
        if opt.get("measure_blocks") != str(measure):
            problems.append(f"{run_id}/{name}: measure_blocks {opt.get('measure_blocks')}")
        if not (r.get("verify", {}).get("ok") and r.get("verify_final", {}).get("ok", True)
                and r.get("verify_final", {}).get("compared", 0) > 0):
            problems.append(f"{run_id}/{name}: oracle verification not ok")
        for s in r["out"]:
            # A relay server (world=local) owns no world: its world stream exists but never carries a frame.
            if s["stream"] == 0x0100 and opt.get("world") == "local" and name == "server":
                if any(s[ph]["frames"] for ph in ("setup", "warmup", "measure")):
                    problems.append(f"{run_id}/{name}: relay server published on the world stream")
                continue
            if DATA(s["stream"]):
                produced[s["stream"]] = (name, s["measure"]["frames"], s["next_seq"])
                if s["measure"]["frames"] != measure:
                    problems.append(f"{run_id}/{name}: {s['measure']['frames']} measured publications on {s['stream']:#06x}")
            if s["measure"]["send_failures"] or s["warmup"]["send_failures"]:
                problems.append(f"{run_id}/{name}: send failures on {s['stream']:#06x}")
    consumed = defaultdict(list)
    for name, r in eps.items():
        for s in r["in"]:
            if not DATA(s["stream"]):
                continue
            consumed[s["stream"]].append(name)
            for ph in ("warmup", "measure"):
                bad = {k: s[ph][k] for k in BAD if s[ph][k]}
                if bad:
                    problems.append(f"{run_id}/{name}: {s['stream']:#06x} {ph} {bad}")
            if s["measure"]["applied"] != measure:
                problems.append(f"{run_id}/{name}: applied {s['measure']['applied']} on {s['stream']:#06x}")
            if s["stream"] in produced and s["last_seq"] != produced[s["stream"]][2] - 1:
                problems.append(f"{run_id}/{name}: last seq behind producer on {s['stream']:#06x}")
    for sid in produced:
        if not consumed.get(sid):
            problems.append(f"{run_id}: stream {sid:#06x} has no consumer")
    for sid in consumed:
        if sid not in produced:
            problems.append(f"{run_id}: stream {sid:#06x} consumed but not produced")
    info = {"endpoints": len(eps), "data_streams": len(produced),
            "consumer_streams": sum(len(v) for v in consumed.values()),
            "e2e_ns": coord.get("end_to_end_ns") if coord else None}
    return problems, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    a = ap.parse_args()
    cdir = RESULTS / a.name
    manifest = json.loads((cdir / "manifest.json").read_text())
    ledger = [json.loads(l) for l in open(cdir / "ledger.jsonl")]
    measure = manifest["protocol"]["measure_blocks"]
    problems = []

    frozen = {"manifest.sha256": (cdir / "manifest.sha256").read_text().split()[0] == sha256(cdir / "manifest.json")}
    changed = [p for p, h in manifest["sources"]["files"].items() if not (ROOT / p).exists() or sha256(ROOT / p) != h]
    exes = {n: sha256(TOOLS.parent / "build" / n) == h for n, h in manifest["build"]["executables"].items()}
    frozen.update({"sources_unchanged": not changed, "executables_unchanged": all(exes.values())})
    if not all(frozen.values()):
        problems.append(f"frozen inputs changed: {frozen} {changed}")

    units = {e["unit"]: e for e in ledger if e.get("type") == "unit"}
    attempts = defaultdict(list)
    for e in ledger:
        if e.get("type") == "run":
            attempts[e["unit"]].append(e)
    pauses = [e for e in ledger if e.get("type") == "pause"]
    sessions = [e for e in ledger if e.get("type") == "session"]

    out = {"campaign": a.name, "measure_blocks": measure, "frozen": frozen, "sessions": len(sessions),
           "battery_pauses": len(pauses), "suites": {}}
    for suite in ("smoke", "validation", "primary"):
        s = {"units": 0, "complete_first_attempt": 0, "runs_checked": 0, "runs_on_ac": 0, "streams_checked": 0,
             "consumer_streams_checked": 0}
        balance = defaultdict(lambda: defaultdict(int))
        seeds = defaultdict(set)
        for u in manifest["schedule"][suite]:
            s["units"] += 1
            done = units.get(u["unit"], {})
            runs = attempts.get(u["unit"], [])
            if done.get("status") != "complete" or done.get("attempts") != 1 or len(runs) != len(u["runs"]):
                problems.append(f"{u['unit']}: status {done.get('status')}, {len(runs)} run attempts")
            else:
                s["complete_first_attempt"] += 1
            for e in runs:
                ok = e["valid"] and e.get("power") == "AC Power" and e.get("power_after") == "AC Power" \
                    and all(c == 0 for c in e["exits"].values())
                s["runs_on_ac"] += e.get("power") == "AC Power" and e.get("power_after") == "AC Power"
                if not ok:
                    problems.append(f"{e['run']}: valid={e['valid']} power={e.get('power')}/{e.get('power_after')} exits={e['exits']}")
                p, info = check_run(cdir / e["dir"], e["run"], u.get("measure_blocks", measure))
                problems += p
                s["runs_checked"] += 1
                s["streams_checked"] += info.get("data_streams", 0)
                s["consumer_streams_checked"] += info.get("consumer_streams", 0)
            if suite == "primary":
                key = f"{u['comparison']} / {u['condition']['sweep']} {u['condition']['level']}"
                balance[key][u["order"]] += 1
                seeds[key].add(u["seed"])
                started = [e["side"] for e in sorted(runs, key=lambda e: e["start"])]
                if "".join(started) != u["order"]:
                    problems.append(f"{u['unit']}: executed {''.join(started)}, scheduled {u['order']}")
        if suite == "primary":
            s["blocks"] = len(balance)
            s["balanced_blocks"] = sum(1 for k, v in balance.items() if v.get("AB") == 5 and v.get("BA") == 5
                                       and len(seeds[k]) == 10)
            for k, v in balance.items():
                if v.get("AB") != 5 or v.get("BA") != 5 or len(seeds[k]) != 10:
                    problems.append(f"{k}: AB={v.get('AB')} BA={v.get('BA')} seeds={len(seeds[k])}")
        out["suites"][suite] = s
    faults = [e for e in ledger if e.get("type") == "unit" and e.get("suite") == "faults"]
    out["suites"]["faults"] = {"units": len(manifest["schedule"]["faults"]),
                               "complete": sum(1 for e in faults if e.get("status") == "complete")}
    out["problems"] = problems
    (cdir / "analysis").mkdir(exist_ok=True)
    (cdir / "analysis" / "audit_primary.json").write_text(json.dumps(out, indent=1))
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
