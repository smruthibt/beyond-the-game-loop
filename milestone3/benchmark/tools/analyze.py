#!/usr/bin/env python3
"""Analysis of a rerun campaign: audit, run-level statistics, paired
comparisons, tables and charts.

    python3 benchmark/tools/analyze.py --name NAME

Reads results/NAME/{manifest.json, ledger.jsonl, runs.jsonl, oracle/*.json,
faults/} and writes results/NAME/analysis/. Runs are the experimental units:
every statistic is computed over run-level values, never over packets.

Paired comparisons use the two runs of a pair (same seed, same condition,
adjacent in time, balanced AB/BA order): difference d = B - A and ratio
r = B / A per pair; mean, sample variance and standard deviation; a 95%
paired t interval (df = pairs - 1) for d, and for r via log ratios.
No significance tests are run across conditions, so no multiplicity
correction is applied; the intervals are descriptive.
"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
RESULTS = TOOLS.parent / "results"

# Two-sided 95% Student t quantiles by degrees of freedom.
T975 = {1: 12.7062, 2: 4.3027, 3: 3.1824, 4: 2.7764, 5: 2.5706, 6: 2.4469, 7: 2.3646, 8: 2.3060, 9: 2.2622,
        10: 2.2281, 11: 2.2010, 12: 2.1788, 13: 2.1604, 14: 2.1448, 15: 2.1314, 19: 2.0930, 29: 2.0452}

METRICS = [   # (key, label, unit, scale)
    ("e2e_s", "end-to-end completion", "s", 1),
    ("server_wall_s", "server loop wall time", "s", 1),
    ("peer_wall_max_s", "slowest peer loop wall time", "s", 1),
    ("client_wall_max_s", "slowest client loop wall time", "s", 1),
    ("busy_total_s", "busy time, all endpoints", "s", 1),
    ("cpu_total_s", "CPU time, all processes", "s", 1),
    ("server_cpu_s", "server CPU time", "s", 1),
    ("client_cpu_total_s", "client CPU time, all clients", "s", 1),
    ("peer_cpu_total_s", "peer CPU time, all peers", "s", 1),
    ("rss_total_mb", "peak memory, sum of processes", "MB", 1),
    ("rss_max_mb", "peak memory, largest process", "MB", 1),
    ("server_encode_s", "server encode time", "s", 1),
    ("client_decode_total_s", "client decode time, all clients", "s", 1),
    ("decode_total_s", "decode time, all endpoints", "s", 1),
    ("encode_total_s", "encode time, all endpoints", "s", 1),
    ("sim_total_s", "simulation time, all endpoints", "s", 1),
    ("send_total_s", "send time incl. REQ round trips, all endpoints", "s", 1),
    ("down_payload_per_pub", "downstream payload per publication", "bytes", 1),
    ("down_objects_per_pub", "objects per downstream publication", "objects", 1),
    ("up_payload_per_msg", "upstream/peer payload per message", "bytes", 1),
    ("received_frame_bytes", "bytes received, all endpoints (each copy once)", "MB", 1e-6),
    ("received_payload_bytes", "payload received, all endpoints", "MB", 1e-6),
    ("control_frames_sent", "control frames sent", "frames", 1),
    ("client_decode_us_per_msg", "client decode per message", "us", 1),
    ("down_encode_us", "server encode per publication", "us", 1),
]


def t975(df):
    if df in T975:
        return T975[df]
    keys = sorted(T975)
    for k in keys:
        if k > df:
            return T975[k]
    return 1.96


def describe(values):
    v = [x for x in values if x is not None and math.isfinite(x)]
    if not v:
        return None
    n = len(v)
    mean = statistics.fmean(v)
    var = statistics.variance(v) if n > 1 else float("nan")
    return {"n": n, "mean": mean, "var": var, "sd": math.sqrt(var) if n > 1 else float("nan"), "min": min(v), "max": max(v)}


def paired(a, b):
    """a, b: lists of (seed, value). Pairs by seed."""
    av, bv = dict(a), dict(b)
    seeds = sorted(set(av) & set(bv))
    pairs = [(av[s], bv[s]) for s in seeds if av[s] is not None and bv[s] is not None]
    if len(pairs) < 2:
        return None
    d = [y - x for x, y in pairs]
    n = len(d)
    dm, dv = statistics.fmean(d), statistics.variance(d)
    half = t975(n - 1) * math.sqrt(dv / n)
    out = {"pairs": n, "diff_mean": dm, "diff_var": dv, "diff_sd": math.sqrt(dv), "diff_lo": dm - half, "diff_hi": dm + half}
    if all(x > 0 and y > 0 for x, y in pairs):
        lr = [math.log(y / x) for x, y in pairs]
        lm, lv = statistics.fmean(lr), statistics.variance(lr)
        lh = t975(n - 1) * math.sqrt(lv / n)
        out.update({"ratio_gmean": math.exp(lm), "ratio_lo": math.exp(lm - lh), "ratio_hi": math.exp(lm + lh),
                    "ratio_mean": statistics.fmean(y / x for x, y in pairs)})
    return out


def load(name):
    cdir = RESULTS / name
    manifest = json.loads((cdir / "manifest.json").read_text())
    rows = [json.loads(l) for l in open(cdir / "runs.jsonl")] if (cdir / "runs.jsonl").exists() else []
    ledger = [json.loads(l) for l in open(cdir / "ledger.jsonl")] if (cdir / "ledger.jsonl").exists() else []
    return cdir, manifest, rows, ledger


def cond_key(r):
    return (r["sweep"], r["level"])


def cond_label(sweep, level):
    return {"baseline": "baseline (100 moving, 100 static, 2 players)", "moving": f"{level} moving", "static": f"{level} static",
            "players": f"{level} players", "clients": f"{level} clients", "density": f"{level}% changed"}.get(sweep, f"{sweep} {level}")


def fmt(x):
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return "-"
    ax = abs(x)
    if ax >= 1000:
        return f"{x:,.0f}"
    if ax >= 100:
        return f"{x:.1f}"
    if ax >= 1:
        return f"{x:.3f}"
    return f"{x:.3g}"


def audit(manifest, rows, ledger):
    """Recount everything the report relies on."""
    out = {"suites": {}, "problems": []}
    done = {}
    for e in ledger:
        if e.get("type") == "unit":
            done[e["unit"]] = e
    runs_by_unit = defaultdict(list)
    for r in rows:
        runs_by_unit[r["unit"]].append(r)
    for suite, units in manifest["schedule"].items():
        complete = [u for u in units if done.get(u["unit"], {}).get("status") == "complete"]
        failed = [u["unit"] for u in units if done.get(u["unit"], {}).get("status") == "failed"]
        missing = [u["unit"] for u in units if u["unit"] not in done]
        s = {"units": len(units), "complete": len(complete), "failed": failed, "not_run": len(missing)}
        if suite not in ("faults",):
            for u in complete:
                got = runs_by_unit.get(u["unit"], [])
                if len(got) != len(u.get("runs", [])):
                    out["problems"].append(f"{u['unit']}: {len(got)} valid runs recorded, expected {len(u['runs'])}")
                for r in got:
                    if r.get("down_publications") not in (None, u.get("measure_blocks", manifest["protocol"]["measure_blocks"])):
                        out["problems"].append(f"{r['run']}: {r.get('down_publications')} downstream publications")
            if suite not in ("smoke", "validation"):
                orders = defaultdict(lambda: defaultdict(int))
                for u in units:
                    if u.get("kind") == "pair":
                        orders[(u["comparison"], u["condition"]["sweep"], u["condition"]["level"])][u["order"]] += 1
                unbalanced = {str(k): dict(v) for k, v in orders.items() if v.get("AB") != v.get("BA")}
                if unbalanced:
                    out["problems"].append(f"{suite}: unbalanced AB/BA blocks {unbalanced}")
        attempts = [e for e in ledger if e.get("type") == "run" and e.get("suite") == suite]
        s["run_attempts"] = len(attempts)
        s["invalid_attempts"] = sum(1 for e in attempts if not e["valid"])
        s["replacement_attempts"] = sum(1 for e in attempts if e["attempt"] > 1)
        out["suites"][suite] = s
    return out


def group(rows, suite):
    g = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if r["suite"] == suite:
            g[(r["comparison"], r["sweep"], r["level"])][r["label"]].append(r)
    return g


def comparison_tables(manifest, rows, suite):
    """Per comparison and condition: per-side statistics and paired results for every metric."""
    out = []
    labels = {}
    for u in manifest["schedule"].get(suite, []):
        if u.get("kind") == "pair":
            labels[u["comparison"]] = {r["side"]: r["label"] for r in u["runs"]}
    for (comp, sweep, level), sides in sorted(group(rows, suite).items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        la, lb = labels[comp]["A"], labels[comp]["B"]
        A, B = sides.get(la, []), sides.get(lb, [])
        for key, label, unit, scale in METRICS:
            va = [(r["seed"], r[key] * scale) for r in A if r.get(key) is not None]
            vb = [(r["seed"], r[key] * scale) for r in B if r.get(key) is not None]
            if not va or not vb:
                continue
            out.append({"suite": suite, "comparison": comp, "sweep": sweep, "level": level, "metric": key, "label": label,
                        "unit": unit, "A": la, "B": lb, "stats_A": describe([v for _, v in va]),
                        "stats_B": describe([v for _, v in vb]), "paired": paired(va, vb)})
    return out


def set_tables(rows, suite, reference):
    """Keyframe sensitivity: each setting described, and paired with the reference setting."""
    out = []
    g = group(rows, suite)
    for (comp, sweep, level), sides in sorted(g.items()):
        ref = sides.get(reference, [])
        for lab, runs in sorted(sides.items()):
            for key, label, unit, scale in METRICS:
                v = [(r["seed"], r[key] * scale) for r in runs if r.get(key) is not None]
                rv = [(r["seed"], r[key] * scale) for r in ref if r.get(key) is not None]
                if not v:
                    continue
                out.append({"suite": suite, "comparison": comp, "sweep": sweep, "level": level, "metric": key, "label": label,
                            "unit": unit, "A": reference, "B": lab, "stats_A": describe([x for _, x in rv]),
                            "stats_B": describe([x for _, x in v]), "paired": paired(rv, v) if lab != reference else None})
    return out


def write_csv(path, table):
    keys = ["suite", "comparison", "sweep", "level", "metric", "unit", "A", "B"]
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(keys + ["n_A", "mean_A", "var_A", "sd_A", "n_B", "mean_B", "var_B", "sd_B", "pairs", "diff_mean",
                           "diff_sd", "diff_ci_lo", "diff_ci_hi", "ratio_gmean", "ratio_ci_lo", "ratio_ci_hi"])
        for t in table:
            a, b, p = t["stats_A"] or {}, t["stats_B"] or {}, t["paired"] or {}
            w.writerow([t[k] for k in keys] + [a.get("n"), a.get("mean"), a.get("var"), a.get("sd"), b.get("n"), b.get("mean"),
                                               b.get("var"), b.get("sd"), p.get("pairs"), p.get("diff_mean"), p.get("diff_sd"),
                                               p.get("diff_lo"), p.get("diff_hi"), p.get("ratio_gmean"), p.get("ratio_lo"),
                                               p.get("ratio_hi")])


def md_table(table, metric, title):
    rows = [t for t in table if t["metric"] == metric]
    if not rows:
        return ""
    unit = rows[0]["unit"]
    lines = [f"**{title}** ({unit}; mean ± SD, sample variance in parentheses; n = runs per side)", "",
             f"| Condition | {rows[0]['A']} | {rows[0]['B']} | B − A, 95% CI | B / A, 95% CI |", "|---|---:|---:|---:|---:|"]
    for t in rows:
        a, b, p = t["stats_A"], t["stats_B"], t["paired"] or {}

        def cell(s):
            return f"{fmt(s['mean'])} ± {fmt(s['sd'])} ({fmt(s['var'])}), n={s['n']}" if s else "-"
        diff = f"{fmt(p.get('diff_mean'))} [{fmt(p.get('diff_lo'))}, {fmt(p.get('diff_hi'))}]" if p else "-"
        ratio = f"{p['ratio_gmean']:.3f} [{p['ratio_lo']:.3f}, {p['ratio_hi']:.3f}]" if p.get("ratio_gmean") else "-"
        lines.append(f"| {cond_label(t['sweep'], t['level'])} | {cell(a)} | {cell(b)} | {diff} | {ratio} |")
    return "\n".join(lines) + "\n"


# --- charts ----------------------------------------------------------------------------

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]


def charts(adir, table, comparisons, metric, ylabel, fname, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    rows = [t for t in table if t["metric"] == metric and t["comparison"] in comparisons]
    if not rows:
        return None
    sweeps = []
    for t in rows:
        if t["sweep"] not in sweeps:
            sweeps.append(t["sweep"])
    fig, axes = plt.subplots(len(comparisons), len(sweeps), figsize=(3.4 * len(sweeps), 2.6 * len(comparisons)),
                             squeeze=False, facecolor=SURFACE)
    for i, comp in enumerate(comparisons):
        for j, sweep in enumerate(sweeps):
            ax = axes[i][j]
            ax.set_facecolor(SURFACE)
            sel = sorted([t for t in rows if t["comparison"] == comp and t["sweep"] == sweep], key=lambda t: t["level"])
            if not sel:
                ax.axis("off")
                continue
            x = range(len(sel))
            w = 0.36
            for k, side in enumerate(("A", "B")):
                means = [t[f"stats_{side}"]["mean"] for t in sel]
                sds = [t[f"stats_{side}"]["sd"] for t in sel]
                xs = [xi + (k - 0.5) * (w + 0.02) for xi in x]
                ax.bar(xs, means, width=w, color=SERIES[k], label=sel[0][side], zorder=2, edgecolor=SURFACE, linewidth=1)
                ax.errorbar(xs, means, yerr=sds, fmt="none", ecolor=INK2, elinewidth=1, capsize=2, zorder=3)
            ax.set_xticks(list(x))
            ax.set_xticklabels([str(t["level"]) for t in sel], color=INK2, fontsize=8)
            ax.tick_params(axis="y", colors=INK2, labelsize=8)
            ax.grid(axis="y", color=GRID, linewidth=0.6, zorder=0)
            for s in ("top", "right"):
                ax.spines[s].set_visible(False)
            for s in ("left", "bottom"):
                ax.spines[s].set_color(GRID)
            ax.set_xlabel({"players": "players", "clients": "clients", "density": "% of eligible objects changed"}.get(
                sweep, f"added {sweep} platforms"), color=INK2, fontsize=8)
            if j == 0:
                ax.set_ylabel(f"{comp}\n{ylabel}", color=INK, fontsize=8)
            if i == 0 and j == 0:
                ax.legend(frameon=False, fontsize=8, labelcolor=INK)
            elif j == 0:
                ax.legend(frameon=False, fontsize=8, labelcolor=INK)
    fig.suptitle(title, color=INK, fontsize=10, x=0.01, ha="left")
    fig.tight_layout()
    path = adir / fname
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return path.name


def ratio_chart(adir, table, comparisons, metric, fname, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    rows = [t for t in table if t["metric"] == metric and t["comparison"] in comparisons and t["paired"]
            and t["paired"].get("ratio_gmean")]
    if not rows:
        return None
    comparisons = [c for c in comparisons if any(t["comparison"] == c for t in rows)]
    fig, axes = plt.subplots(1, len(comparisons), figsize=(3.3 * len(comparisons), 3.2), squeeze=False, facecolor=SURFACE)
    for j, comp in enumerate(comparisons):
        ax = axes[0][j]
        ax.set_facecolor(SURFACE)
        sel = [t for t in rows if t["comparison"] == comp]
        y = list(range(len(sel)))[::-1]
        for yi, t in zip(y, sel):
            p = t["paired"]
            ax.plot([p["ratio_lo"], p["ratio_hi"]], [yi, yi], color=SERIES[0], linewidth=2, solid_capstyle="round", zorder=2)
            ax.plot([p["ratio_gmean"]], [yi], "o", color=SERIES[0], markersize=5, markeredgecolor=SURFACE, zorder=3)
        ax.axvline(1.0, color=INK2, linewidth=0.8, linestyle=(0, (3, 3)), zorder=1)
        ax.set_yticks(y)
        ax.set_yticklabels([cond_label(t["sweep"], t["level"]).replace(" (100 moving, 100 static, 2 players)", "") for t in sel],
                           fontsize=7, color=INK2)
        ax.tick_params(axis="x", colors=INK2, labelsize=7)
        ax.set_title(f"{comp}: {sel[0]['B']} / {sel[0]['A']}", fontsize=8, color=INK)
        ax.grid(axis="x", color=GRID, linewidth=0.6, zorder=0)
        for s in ("top", "right", "left"):
            ax.spines[s].set_visible(False)
        ax.spines["bottom"].set_color(GRID)
    fig.suptitle(title, color=INK, fontsize=10, x=0.01, ha="left")
    fig.tight_layout()
    path = adir / fname
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return path.name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    a = ap.parse_args()
    cdir, manifest, rows, ledger = load(a.name)
    adir = cdir / "analysis"
    adir.mkdir(exist_ok=True)

    report = {"audit": audit(manifest, rows, ledger)}
    tables = {}
    for suite in ("primary", "native-topology", "bidirectional-delta", "density", "game"):
        tables[suite] = comparison_tables(manifest, rows, suite)
        write_csv(adir / f"{suite}.csv", tables[suite])
    tables["keyframe"] = set_tables(rows, "keyframe", "keyframe 60")
    write_csv(adir / "keyframe.csv", tables["keyframe"])
    with open(adir / "runs.csv", "w", newline="") as f:
        keys = sorted({k for r in rows for k in r})
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    md = []
    sections = [
        ("primary", ["information", "representation", "update", "topology"]),
        ("native-topology", ["native-topology"]),
        ("bidirectional-delta", ["bidirectional-delta"]),
        ("density", ["density"]),
        ("game", ["game-encoding", "game-snapshots"]),
        ("keyframe", ["keyframe"]),
    ]
    for suite, comps in sections:
        for comp in comps:
            t = [x for x in tables[suite] if x["comparison"] == comp]
            if not t:
                continue
            md.append(f"## {suite} / {comp}\n")
            for metric, title in [("e2e_s", "End-to-end completion time, 100,000 ticks"), ("server_wall_s", "Server loop wall time"),
                                  ("peer_wall_max_s", "Slowest peer loop wall time"), ("cpu_total_s", "CPU time, all processes"),
                                  ("busy_total_s", "Busy time, all endpoints"), ("rss_total_mb", "Peak memory, sum of processes"),
                                  ("down_payload_per_pub", "Downstream payload per publication"),
                                  ("up_payload_per_msg", "Upstream / peer payload per message"),
                                  ("received_frame_bytes", "Bytes received, all endpoints"),
                                  ("server_encode_s", "Server encode time"), ("client_decode_total_s", "Client decode time, all clients"),
                                  ("down_encode_us", "Server encode per publication"),
                                  ("client_decode_us_per_msg", "Client decode per message")]:
                s = md_table(t, metric, title)
                if s:
                    md.append(s)
    (adir / "tables.md").write_text("\n".join(md))

    figs = {}
    try:
        figs["primary_e2e"] = charts(adir, tables["primary"], ["information", "representation", "update", "topology"], "e2e_s",
                                     "seconds", "primary_e2e.png",
                                     "Time to complete 100,000 ticks, end to end (mean ± SD over 10 runs per bar)")
        figs["primary_ratio"] = ratio_chart(adir, tables["primary"], ["information", "representation", "update", "topology"],
                                            "e2e_s", "primary_ratio.png",
                                            "Paired ratio B / A of end-to-end time (geometric mean, 95% CI; 1 = no difference)")
        figs["primary_cpu"] = charts(adir, tables["primary"], ["information", "representation", "update", "topology"],
                                     "cpu_total_s", "CPU seconds", "primary_cpu.png",
                                     "CPU time summed over every process (mean ± SD)")
        figs["primary_bytes"] = charts(adir, tables["primary"], ["information", "representation", "update", "topology"],
                                       "received_frame_bytes", "MB", "primary_bytes.png",
                                       "Bytes received by all endpoints during the measurement (each copy counted once)")
        figs["native_e2e"] = charts(adir, tables["native-topology"], ["native-topology"], "e2e_s", "seconds", "native_topology_e2e.png",
                                    "Native architectures: end-to-end time (mean ± SD)")
        figs["game_e2e"] = charts(adir, tables["game"], ["game-encoding", "game-snapshots"], "e2e_s", "seconds", "game_e2e.png",
                                  "Clockwork Dash: end-to-end time (mean ± SD)")
        figs["game_payload"] = charts(adir, tables["game"], ["game-encoding", "game-snapshots"], "down_payload_per_pub", "bytes",
                                      "game_payload.png", "Clockwork Dash: downstream payload per publication")
        figs["density_payload"] = charts(adir, tables["density"], ["density"], "down_payload_per_pub", "bytes", "density_payload.png",
                                         "Change density: downstream payload per publication")
        figs["density_e2e"] = charts(adir, tables["density"], ["density"], "e2e_s", "seconds", "density_e2e.png",
                                     "Change density: end-to-end time")
    except ImportError as e:
        report["charts_error"] = str(e)
    report["figures"] = figs
    report["runs"] = len(rows)
    (adir / "analysis.json").write_text(json.dumps({"audit": report["audit"], "figures": figs, "runs": len(rows),
                                                     "tables": {k: v for k, v in tables.items()}}, indent=1, default=str))
    print(json.dumps(report["audit"], indent=1))
    print("figures:", figs)


if __name__ == "__main__":
    main()
