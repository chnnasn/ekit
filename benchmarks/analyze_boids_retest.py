"""Analyze six current-version Boids runs without replacing historical data."""
import csv
import json
import math
from pathlib import Path
from statistics import median

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
groups = {}
for mode in ("scheduler", "parallel"):
    for run in range(6):
        text = (HERE / "boids_retest_raw" / f"{mode}_{run}.txt").read_text(encoding="utf-8")
        supplement = HERE / "boids_retest_raw" / f"{mode}_{run}_3.txt"
        if supplement.exists():
            text += "\n" + supplement.read_text(encoding="utf-8")
        assert "DIFF" not in text
        count = None
        seen = set()
        for line in text.splitlines():
            fields = line.split()
            if mode == "scheduler" and len(fields) == 7 and fields[0].isdigit():
                count, threads, ms = int(fields[0]), int(fields[1]), float(fields[2])
            elif mode == "parallel" and line.startswith("boids = "):
                count = int(fields[-1])
                continue
            elif mode == "parallel" and len(fields) == 6 and fields[0] == "parallel":
                threads, ms = int(fields[1]), float(fields[2])
                assert fields[-1] == "ok", line
            else:
                continue
            assert (count, threads) not in seen
            seen.add((count, threads))
            if run != 0:
                groups.setdefault((mode, count, threads), []).append(ms)
        assert len(seen) == 30, (mode, run, seen)

assert len(groups) == 60
assert all(len(samples) == 5 for samples in groups.values())
values = {key: median(samples) for key, samples in groups.items()}
with (HERE / "boids_retest.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.writer(stream)
    writer.writerow(["mode", "boids", "threads", "median_ms", "min_ms", "max_ms", "speedup_vs_same_mode_1_thread"])
    for (mode, count, threads), ms in sorted(values.items()):
        samples = groups[mode, count, threads]
        writer.writerow([mode, count, threads, ms, min(samples), max(samples), values[mode, count, 1] / ms])

counts = [200, 500, 1000, 2000, 5000, 10000]
threads_list = [1, 2, 3, 4]
colors = ["#555555", "#2879ad", "#16866c", "#ba622d"]
for language in ("en", "zh"):
    chinese = language == "zh"
    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "DejaVu Sans"] if chinese else ["DejaVu Sans"]
    for kind in ("cost", "speedup"):
        fig, axes = plt.subplots(1, 2, figsize=(12, 5), layout="constrained")
        for ax, mode in zip(axes, ("scheduler", "parallel")):
            if kind == "cost":
                for t, color in zip(threads_list, colors):
                    ax.plot(counts, [values[mode, b, t] for b in counts], marker="o", color=color,
                            label=f"{t} " + ("线程" if chinese else "threads"))
                ax.set_xscale("log")
                ax.set_yscale("log")
                ax.set_xlabel("boid 数量" if chinese else "Boid count")
                ax.set_ylabel("毫秒 / 模拟步（越低越好）" if chinese else "ms / step (lower is better)")
            else:
                for b, color in zip((200, 1000, 10000), colors):
                    ax.plot(threads_list, [values[mode, b, 1] / values[mode, b, t] for t in threads_list],
                            marker="o", color=color, label=f"{b} boids")
                ax.axhline(1, color="#888888", linestyle="--", linewidth=1)
                ax.set_xticks(threads_list)
                ax.set_xlabel("线程数" if chinese else "Threads")
                ax.set_ylabel("相对本路径单线程加速比" if chinese else "Speedup vs same-path single thread")
            ax.set_title(("系统调度器" if mode == "scheduler" else "查询数据并行 + 并行网格") if chinese
                         else ("System scheduler" if mode == "scheduler" else "Data-parallel queries + grid"))
            ax.grid(True, which="both", linestyle=":", alpha=0.4)
            ax.legend()
        fig.suptitle(("当前 Boids 重测 · 055e1c9（核心优化 4d2d014）\n"
                      "800×600 · MSVC Release · 120 步 + 20 步预热 · 五轮中位数\n3 线程为单独补测；波动范围见 CSV") if chinese else
                     "Current Boids retest · 055e1c9 (core optimization 4d2d014)\n"
                     "800×600 · MSVC Release · 120 steps + 20 warmup · five-round medians\n3 threads tested separately; see CSV for sample ranges", fontsize=12)
        fig.savefig(HERE / f"chart_boids_current_{kind}.{language}.png", dpi=150)
        plt.close(fig)

summary = {}
for mode in ("scheduler", "parallel"):
    summary[mode] = {
        "10000_ms": {t: values[mode, 10000, t] for t in [1, 2, 3, 4, 24]},
        "10000_speedup": {t: values[mode, 10000, 1] / values[mode, 10000, t] for t in [1, 2, 3, 4, 24]},
        "200_ms": {t: values[mode, 200, t] for t in threads_list},
        "exponent_5000_to_10000_at_4": math.log(values[mode, 10000, 4] / values[mode, 5000, 4], 2),
    }
print(json.dumps(summary, indent=2))
