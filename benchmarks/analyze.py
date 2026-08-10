#!/usr/bin/env python3
# Parses the raw benchmark outputs in this directory, prints the decline-curve
# and stall-point analysis, writes CSVs and renders charts.
import io, math, os

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- parse ekit_boids_bench_raw.txt ----
rows = []  # (boids, threads, ms)
with io.open(os.path.join(HERE, "ekit_boids_bench_raw.txt"), encoding="utf-8") as f:
    for line in f:
        parts = line.split()
        if len(parts) == 7 and parts[0].isdigit():
            rows.append((int(parts[0]), int(parts[1]), float(parts[2])))

boids = sorted({r[0] for r in rows})
threads = sorted({r[1] for r in rows})
ms = {(b, t): m for b, t, m in rows}

def expo(a, b, t):
    x = b / a
    y = ms[(b, t)] / ms[(a, t)]
    return x, y, math.log(y) / math.log(x)

print("== decline curve: per-step cost scaling (4 threads) ==")
print(f"{'from':>6} {'to':>6} {'x boids':>8} {'x time':>8} {'exponent':>9}")
prev = None
for b in boids:
    if prev is not None:
        x, y, e = expo(prev, b, 4)
        print(f"{prev:>6} {b:>6} {x:>8.1f} {y:>8.2f} {e:>9.2f}")
    prev = b

print("\n== decline curve: per-step cost scaling (1 thread) ==")
prev = None
for b in boids:
    if prev is not None:
        x, y, e = expo(prev, b, 1)
        print(f"{prev:>6} {b:>6} {x:>8.1f} {y:>8.2f} {e:>9.2f}")
    prev = b

print("\n== stall point: speedup vs threads ==")
print(f"{'boids':>7} {'t1':>6} {'t2':>6} {'t4':>6} {'t24':>6}")
for b in boids:
    base = ms[(b, 1)]
    print(f"{b:>7} {1.0:>6.2f} {base/ms[(b,2)]:>6.2f} {base/ms[(b,4)]:>6.2f} {base/ms[(b,24)]:>6.2f}")

# ---- CSV ----
with io.open(os.path.join(HERE, "ekit_boids_bench.csv"), "w", encoding="utf-8", newline="\n") as f:
    f.write("boids,threads,ms_per_step,steps_per_sec,speedup,k_boids_per_sec,us_per_boid\n")
    for b, t, m in sorted(rows):
        steps_s = 1000.0 / m
        base = ms[(b, 1)]
        f.write(f"{b},{t},{m:.4f},{steps_s:.1f},{base/m:.2f},{b*steps_s/1000:.1f},{m*1000/b:.3f}\n")

# ---- charts ----
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["figure.dpi"] = 120

# 1) ms/step vs boid count (log-log): the decline curve
fig, ax = plt.subplots(figsize=(7, 5))
for t in (1, 2, 4, 24):
    ax.plot(boids, [ms[(b, t)] for b in boids], marker="o", label=f"{t} threads")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("boids"); ax.set_ylabel("ms / step")
ax.set_title("Per-step cost vs boid count (log-log, ekit Boids)")
ax.grid(True, which="both", ls=":", alpha=0.5)
ax.legend()
fig.tight_layout(); fig.savefig(os.path.join(HERE, "chart_cost_vs_boids.png")); plt.close(fig)

# 2) speedup vs threads: the stall point
fig, ax = plt.subplots(figsize=(7, 5))
for b in (200, 1000, 10000):
    base = ms[(b, 1)]
    ax.plot(threads, [base / ms[(b, t)] for t in threads], marker="o", label=f"{b} boids")
ax.set_xlabel("threads"); ax.set_ylabel("speedup vs 1 thread")
ax.set_title("Parallel speedup vs thread count (ekit Boids)")
ax.axvline(4, color="gray", ls="--", alpha=0.6, label="stall point ~4 threads")
ax.grid(True, ls=":", alpha=0.5)
ax.legend()
fig.tight_layout(); fig.savefig(os.path.join(HERE, "chart_speedup_vs_threads.png")); plt.close(fig)

# 3) throughput decline
fig, ax = plt.subplots(figsize=(7, 5))
for t in (1, 4, 24):
    kb = [b * 1000.0 / ms[(b, t)] / 1000.0 for b in boids]
    ax.plot(boids, kb, marker="o", label=f"{t} threads")
ax.set_xscale("log")
ax.set_xlabel("boids"); ax.set_ylabel("k boids / s")
ax.set_title("Throughput vs boid count (ekit Boids)")
ax.grid(True, which="both", ls=":", alpha=0.5)
ax.legend()
fig.tight_layout(); fig.savefig(os.path.join(HERE, "chart_throughput.png")); plt.close(fig)

print("\nwrote CSV + 3 charts")
