#!/usr/bin/env python3
"""Milestone 3, Part 1C: run the performance experiments and chart them.

    python3 engine/tools/net_experiments.py --compare encoding
    python3 engine/tools/net_experiments.py --compare delta --base "--encoding binary"
    python3 engine/tools/net_experiments.py --compare upstream --reps 5 --iterations 20000
    python3 engine/tools/net_experiments.py --compare topology --experiments moving,clients

Run it from anywhere: it builds the project that contains the engine folder.

It builds an optimized net_bench (build-release/), then, for the two sides of
the chosen comparison, runs three experiments:

    moving    10, 100, 500 moving platforms   (100 static, 2 clients)
    clients   1, 2, 4 clients                 (100 moving, 100 static)
    static    10, 100, 500 static platforms   (10 moving, 2 clients)

Every condition runs --reps times (default 5). One run = a fresh server (or
peers) and clients; the server - or peer 0, with no server - times
--iterations loop iterations after --warmup untimed ones. Each iteration
simulates the world, builds the snapshot and sends it.

Comparisons (side A vs side B; everything else as --base says):

    encoding   --encoding text   vs  --encoding binary
    delta      --delta off       vs  --delta on
    upstream   --upstream state  vs  --upstream input
    topology   --topology cs     vs  --topology p2p

Output in --out (default results/):

    <compare>_runs.csv      every run, every number net_bench printed
    <compare>_summary.csv   per condition and side: mean, variance, standard
                            deviation, min and max of the loop time, and the
                            means of the other measurements
    <compare>_<exp>.svg     the loop time, mean with one standard deviation
    <compare>_<exp>_bytes.svg  the size of each message

Standard library only, so it runs anywhere Python 3 does.
"""
import argparse
import csv
import math
import pathlib
import shlex
import statistics
import subprocess
import sys
import time

# engine/tools/ -> engine/ -> the project that contains the engine folder.
REPO = pathlib.Path(__file__).resolve().parent.parent.parent
BENCH = REPO / "build-release" / "net_bench"

COMPARISONS = {
    "encoding": ("text", ["--encoding", "text"], "binary", ["--encoding", "binary"]),
    "delta": ("full", ["--delta", "off"], "delta", ["--delta", "on"]),
    "upstream": ("state", ["--upstream", "state"], "input", ["--upstream", "input"]),
    "topology": ("client-server", ["--topology", "cs"], "peer-to-peer", ["--topology", "p2p"]),
}

EXPERIMENTS = {
    "moving": ("moving platforms", "--moving", [10, 100, 500], {"--static": 100, "--clients": 2}),
    "clients": ("clients", "--clients", [1, 2, 4], {"--moving": 100, "--static": 100}),
    "static": ("static platforms", "--static", [10, 100, 500], {"--moving": 10, "--clients": 2}),
}


def build():
    print("building an optimized net_bench ...", flush=True)
    subprocess.run(["cmake", "-S", str(REPO), "-B", str(REPO / "build-release"), "-DCMAKE_BUILD_TYPE=Release"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(REPO / "build-release"), "--target", "net_bench", "-j", "8"],
                   check=True, stdout=subprocess.DEVNULL)


def parse_result(line):
    fields = {}
    for part in line.split()[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            try:
                fields[key] = float(value) if any(c in value for c in ".e") or value.lstrip("-").isdigit() else value
            except ValueError:
                fields[key] = value
    return fields


def run_once(args, run_index, timeout):
    """One run: the timing process plus the others. Returns the timing
    process's RESULT and the mean of the clients' (or other peers')."""
    port = 8500 + 10 * (run_index % 10)
    common = args + ["--port-base", str(port)]
    p2p = "p2p" in common
    clients = int(common[common.index("--clients") + 1])
    procs = []
    try:
        if p2p:
            timer = subprocess.Popen([str(BENCH), "peer", "--id", "0"] + common, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True)
            for i in range(1, clients):
                procs.append(subprocess.Popen([str(BENCH), "peer", "--id", str(i)] + common, stdout=subprocess.PIPE,
                                              stderr=subprocess.DEVNULL, text=True))
        else:
            timer = subprocess.Popen([str(BENCH), "server"] + common, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True)
            time.sleep(0.3)
            for _ in range(clients):
                procs.append(subprocess.Popen([str(BENCH), "client"] + common, stdout=subprocess.PIPE,
                                              stderr=subprocess.DEVNULL, text=True))
        out, _ = timer.communicate(timeout=timeout)
        others = []
        for p in procs:
            o, _ = p.communicate(timeout=30)
            others += [parse_result(l) for l in o.splitlines() if l.startswith("RESULT")]
        mine = [parse_result(l) for l in out.splitlines() if l.startswith("RESULT")]
        if not mine:
            return None, []
        return mine[0], others
    except subprocess.TimeoutExpired:
        return None, []
    finally:
        for p in [*procs] + ([timer] if "timer" in locals() else []):
            if p.poll() is None:
                p.kill()
                p.wait()


def mean_of(rows, key):
    values = [r[key] for r in rows if isinstance(r.get(key), float)]
    return statistics.fmean(values) if values else float("nan")


# --- SVG charts (no plotting library needed) -----------------------------------

COLORS = ["#534AB7", "#0F6E56", "#993C1D", "#5F5E5A"]


def value_label(v):
    """37,041 rather than 3.7e+04; three significant figures below 100."""
    return f"{v:,.0f}" if v >= 100 else f"{v:.3g}"


def bar_chart(path, title, ylabel, categories, series, xlabel):
    """series: [(name, [(mean, sd), ...])] - grouped bars with error bars."""
    w, h, left, right, top, bottom = 720, 420, 80, 20, 50, 70
    pw, ph = w - left - right, h - top - bottom
    top_value = max((m + (sd if sd == sd else 0)) for _, vals in series for m, sd in vals if m == m) or 1.0
    exp = 10 ** math.floor(math.log10(top_value))
    step = next(s * exp for s in (0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5, 10) if top_value / (s * exp) <= 6)
    ymax = step * math.ceil(top_value / step)
    y = lambda v: top + ph - ph * v / ymax
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
           f'font-family="Helvetica Neue, Helvetica, Arial, sans-serif">',
           f'<rect width="{w}" height="{h}" fill="#ffffff"/>',
           f'<text x="{w / 2}" y="26" font-size="15" font-weight="700" text-anchor="middle" fill="#222">{title}</text>']
    tick = 0.0
    while tick <= ymax + 1e-9:
        ty = y(tick)
        out.append(f'<line x1="{left}" y1="{ty:.1f}" x2="{w - right}" y2="{ty:.1f}" stroke="#e4e4e4"/>')
        label = f"{tick:g}" if tick < 1000 else f"{tick:,.0f}"
        out.append(f'<text x="{left - 8}" y="{ty + 4:.1f}" font-size="11" text-anchor="end" fill="#555">{label}</text>')
        tick += step
    out.append(f'<line x1="{left}" y1="{top + ph}" x2="{w - right}" y2="{top + ph}" stroke="#888"/>')
    out.append(f'<text x="18" y="{top + ph / 2}" font-size="12" fill="#333" text-anchor="middle" '
               f'transform="rotate(-90 18 {top + ph / 2})">{ylabel}</text>')
    out.append(f'<text x="{left + pw / 2}" y="{h - 14}" font-size="12" fill="#333" text-anchor="middle">{xlabel}</text>')
    group = pw / len(categories)
    bar = min(48, group * 0.7 / len(series))
    for ci, cat in enumerate(categories):
        cx = left + group * (ci + 0.5)
        out.append(f'<text x="{cx:.1f}" y="{top + ph + 18}" font-size="12" text-anchor="middle" fill="#333">{cat}</text>')
        for si, (name, vals) in enumerate(series):
            m, sd = vals[ci]
            if m != m:
                continue
            bx = cx - bar * len(series) / 2 + bar * si
            out.append(f'<rect x="{bx:.1f}" y="{y(m):.1f}" width="{bar - 4:.1f}" height="{top + ph - y(m):.1f}" '
                       f'fill="{COLORS[si % len(COLORS)]}"/>')
            if sd == sd and sd > 0:
                mx = bx + (bar - 4) / 2
                out.append(f'<line x1="{mx:.1f}" y1="{y(m + sd):.1f}" x2="{mx:.1f}" y2="{y(max(0, m - sd)):.1f}" stroke="#111"/>')
                out.append(f'<line x1="{mx - 5:.1f}" y1="{y(m + sd):.1f}" x2="{mx + 5:.1f}" y2="{y(m + sd):.1f}" stroke="#111"/>')
            out.append(f'<text x="{bx + (bar - 4) / 2:.1f}" y="{y(m) - 6:.1f}" font-size="10" text-anchor="middle" '
                       f'fill="#333">{value_label(m)}</text>')
    for si, (name, _) in enumerate(series):
        lx = left + 10 + si * 150
        out.append(f'<rect x="{lx}" y="{top - 14}" width="12" height="12" fill="{COLORS[si % len(COLORS)]}"/>')
        out.append(f'<text x="{lx + 18}" y="{top - 4}" font-size="12" fill="#333">{name}</text>')
    out.append("</svg>")
    path.write_text("\n".join(out))


def write_csv(path, rows):
    keys = []
    for r in rows:
        keys += [k for k in r if k not in keys]
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)


def merge(out_dir, compare):
    """<compare>_runs.csv and <compare>_summary.csv from every experiment saved
    so far, in experiment order."""
    for kind in ("runs", "summary"):
        rows = []
        for exp in EXPERIMENTS:
            part = out_dir / f"{compare}_{exp}_{kind}.csv"
            if part.exists():
                with part.open(newline="") as f:
                    rows += list(csv.DictReader(f))
        if rows:
            write_csv(out_dir / f"{compare}_{kind}.csv", rows)


def draw_charts(out_dir, compare, exp_name, rows, base_text):
    """Both charts for one experiment, from its summary rows (fresh dicts or
    rows read back from the CSV)."""
    title, _, values, _ = EXPERIMENTS[exp_name]
    name_a, _, name_b, _ = COMPARISONS[compare]
    on = f" ({base_text})" if base_text else ""
    iterations = int(float(rows[0]["iterations"])) if rows else 0
    size_key = "client_upstream_bytes" if compare == "upstream" else "message_bytes"

    def column(side, key, sd_key=None):
        out = []
        for v in values:
            row = next((r for r in rows if r["side"] == side and int(float(r["value"])) == v), None)
            out.append((float(row[key]), float(row[sd_key]) if sd_key else 0.0) if row else (float("nan"), 0.0))
        return out

    bar_chart(out_dir / f"{compare}_{exp_name}.svg",
              f"Time for {iterations:,} loop iterations: {name_a} vs {name_b}{on}",
              "seconds (mean, error bar = 1 standard deviation)", values,
              [(name_a, column(name_a, "mean_s", "stdev_s")), (name_b, column(name_b, "mean_s", "stdev_s"))], title)
    bar_chart(out_dir / f"{compare}_{exp_name}_bytes.svg",
              f"Size of each {'client message' if compare == 'upstream' else 'message'}: {name_a} vs {name_b}{on}",
              "bytes", values,
              [(name_a, column(name_a, size_key)), (name_b, column(name_b, size_key))], title)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--compare", choices=sorted(COMPARISONS), default="encoding")
    ap.add_argument("--experiments", default="moving,clients,static")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--iterations", type=int, default=10000)
    ap.add_argument("--warmup", type=int, default=500)
    ap.add_argument("--base", default="", help="extra net_bench flags for every run, e.g. \"--encoding binary\"")
    ap.add_argument("--out", default=str(REPO / "results"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--charts-only", action="store_true",
                    help="redraw the charts from the saved summary CSVs; runs nothing (pass the same --base)")
    a = ap.parse_args()
    out_dir = pathlib.Path(a.out)
    base_text = a.base.replace("--", "").strip()

    if a.charts_only:
        for exp_name in a.experiments.split(","):
            part = out_dir / f"{a.compare}_{exp_name}_summary.csv"
            if part.exists():
                with part.open(newline="") as f:
                    draw_charts(out_dir, a.compare, exp_name, list(csv.DictReader(f)), base_text)
                print(f"redrew {a.compare}_{exp_name}.svg and {a.compare}_{exp_name}_bytes.svg")
        return 0

    if not a.no_build:
        build()
    out_dir.mkdir(parents=True, exist_ok=True)
    name_a, flags_a, name_b, flags_b = COMPARISONS[a.compare]
    base = shlex.split(a.base) + ["--iterations", str(a.iterations), "--warmup", str(a.warmup)]
    timeout = 120 + a.iterations / 200

    runs, summary = [], []
    run_index = 0
    for exp_name in a.experiments.split(","):
        title, flag, values, fixed = EXPERIMENTS[exp_name]
        first_run, first_summary = len(runs), len(summary)
        for value in values:
            for side, side_flags in ((name_a, flags_a), (name_b, flags_b)):
                cond = dict(fixed)
                cond[flag] = value
                args = base + side_flags + [x for k, v in cond.items() for x in (k, str(v))]
                times, mains, others = [], [], []
                for rep in range(a.reps):
                    run_index += 1
                    main_result, other_results = run_once(args, run_index, timeout)
                    if not main_result:
                        print(f"  {exp_name}={value} {side} rep {rep + 1}: FAILED", flush=True)
                        continue
                    secs = main_result["seconds"]
                    times.append(secs)
                    mains.append(main_result)
                    others += other_results
                    row = {"experiment": exp_name, "variable": flag.lstrip("-"), "value": value, "side": side,
                           "rep": rep + 1, **main_result}
                    for key in ("decode_us", "latency_ms", "latency_p95_ms", "snapshots_in", "decoded", "skipped"):
                        row["clients_" + key] = mean_of(other_results, key)
                    runs.append(row)
                    print(f"  {exp_name}={value:<4} {side:<14} rep {rep + 1}: {secs:.4f} s "
                          f"({main_result['us_per_iter']:.1f} us/iter)", flush=True)
                mean = statistics.fmean(times) if times else float("nan")
                var = statistics.variance(times) if len(times) > 1 else 0.0
                sd = math.sqrt(var)
                msg_key = "snapshot_bytes" if "snapshot_bytes" in (mains[0] if mains else {}) else "msg_bytes"
                enc_key = "encode_us"
                summary.append({
                    "experiment": exp_name, "variable": flag.lstrip("-"), "value": value, "side": side,
                    "runs": len(times), "iterations": a.iterations,
                    "mean_s": mean, "variance_s2": var, "stdev_s": sd,
                    "min_s": min(times) if times else float("nan"), "max_s": max(times) if times else float("nan"),
                    "us_per_iter": mean * 1e6 / a.iterations if times else float("nan"),
                    "message_bytes": mean_of(mains, msg_key), "encode_us": mean_of(mains, enc_key),
                    "upstream_decode_us": mean_of(mains, "decode_us"),
                    "client_decode_us": mean_of(others, "decode_us"),
                    "client_upstream_bytes": mean_of(others, "upstream_bytes"),
                    "client_latency_ms": mean_of(others, "latency_ms"),
                })
        draw_charts(out_dir, a.compare, exp_name, summary[first_summary:], base_text)

        # Saved as soon as the experiment ends, so a run cut short keeps what
        # it finished, and experiments run in separate invocations add up.
        write_csv(out_dir / f"{a.compare}_{exp_name}_runs.csv", runs[first_run:])
        write_csv(out_dir / f"{a.compare}_{exp_name}_summary.csv", summary[first_summary:])
        merge(out_dir, a.compare)

    print(f"\n{'experiment':<9} {'value':>5}  {'side':<14} {'runs':>4} {'mean s':>9} {'stdev s':>9} {'us/iter':>9} {'msg bytes':>10}")
    for s in summary:
        print(f"{s['experiment']:<9} {s['value']:>5}  {s['side']:<14} {s['runs']:>4} {s['mean_s']:>9.4f} "
              f"{s['stdev_s']:>9.4f} {s['us_per_iter']:>9.1f} {s['message_bytes']:>10.0f}")
    print(f"\nwrote {out_dir}/{a.compare}_runs.csv, {a.compare}_summary.csv and the charts")
    return 0 if all(s["runs"] == a.reps for s in summary) else 1


if __name__ == "__main__":
    sys.exit(main())
