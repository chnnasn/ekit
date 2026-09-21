"""Build bilingual Boids reports from the analyzed measurements."""
import csv
from pathlib import Path

HERE = Path(__file__).resolve().parent
with (HERE / "boids_retest.csv").open(encoding="utf-8") as stream:
    data = {(r["mode"], int(r["boids"]), int(r["threads"])): float(r["median_ms"])
            for r in csv.DictReader(stream)}

for lang in ("en", "zh"):
    zh = lang == "zh"
    filename = "boids_retest.zh-CN.md" if zh else "boids_retest.md"
    title = "Boids 当前版本重测" if zh else "Current-version Boids retest"
    text = f"# {title}\n\n[English](boids_retest.md) | [简体中文](boids_retest.zh-CN.md)\n\n"
    text += ("2026-09-21 实测，版本 `055e1c9`（核心优化 `4d2d014`）。这次重新编译并运行当前源码，"
             "没有运行旧版或 EnTT，不是跨版本提速或 EnTT 对比。\n\n"
             "Intel Core i7-14650HX，Windows 11 x64 10.0.26200，MSVC 19.50.35724 Release /O2。"
             "800×600 世界、种子 20260810，20 步预热 + 120 步计时，六轮舍弃首轮，取其余五轮中位数。"
             "所有 24 个逻辑 CPU 可调度，不固定单核；不同程序串行运行，每轮交替先后顺序。"
             "3 线程为后续单独补跑的六轮，可能受机器状态变化影响；CSV 保留每组最小/最大值供判断波动。\n\n"
             "主图使用 1、2、3、4 线程，24 线程为额外扩展点。加速比按各路径自身单线程中位数计算，"
             "并非原始 parallel 输出中的 orig 单线程基准。\n\n") if zh else (
             "Measured on 2026-09-21 at `055e1c9` (core optimization `4d2d014`). Current sources were rebuilt and run; "
             "neither the historical revision nor EnTT was rerun, so this is not a version speedup or EnTT comparison.\n\n"
             "Intel Core i7-14650HX, Windows 11 x64 10.0.26200, MSVC 19.50.35724 Release /O2. "
             "800×600 world, seed 20260810, 20 warmup + 120 timed steps. Six rounds, discard round 0, "
             "report five-round medians. All 24 logical CPUs are available to the OS; no single-core pinning. "
             "Programs run serially and their order alternates each round. Three-thread samples were collected "
             "in a later six-round supplement and may reflect changed machine conditions. CSV includes min/max values.\n\n"
             "Primary charts use 1, 2, 3 and 4 threads; 24 threads is an extra scaling point. "
             "Speedups divide each path's own single-thread median by its multithread median, unlike the "
             "parallel executable's original orig-baseline speedup column.\n\n")
    text += ("## 每步耗时\n\n" if zh else "## Per-step time\n\n")
    for mode in ("scheduler", "parallel"):
        name = (("系统调度器" if mode == "scheduler" else "查询数据并行 + 并行网格") if zh
                else ("System scheduler" if mode == "scheduler" else "Data-parallel queries + grid"))
        text += f"### {name}\n\n| boids | 1 (ms) | 2 (ms) | 3 (ms) | 4 (ms) | 24 (ms) |\n| --- | ---: | ---: | ---: | ---: | ---: |\n"
        for b in (200, 500, 1000, 2000, 5000, 10000):
            text += f"| {b} | " + " | ".join(f"{data[mode,b,t]:.4f}" for t in (1,2,3,4,24)) + " |\n"
        speed4 = data[mode,10000,1] / data[mode,10000,4]
        speed24 = data[mode,10000,1] / data[mode,10000,24]
        text += (f"\n10,000 boids：4 线程相对本路径单线程加速 **{speed4:.2f}×**；24 线程为 **{speed24:.2f}×**。\n\n"
                 if zh else f"\n10,000 boids: **{speed4:.2f}×** at 4 threads and **{speed24:.2f}×** at 24 threads versus this path's single thread.\n\n")
    text += f"![{'规模曲线' if zh else 'Cost scaling'}](chart_boids_current_cost.{lang}.png)\n\n"
    text += f"![{'线程扩展' if zh else 'Thread scaling'}](chart_boids_current_speedup.{lang}.png)\n\n"
    text += ("## 解释与验证范围\n\n"
             "- 10,000 boids 的 3 线程中位数慢于 2 线程，且补测波动明显：调度器 40.52–72.35 ms，数据并行 35.33–59.15 ms。保留原始结果，不将其解释为 3 线程必然更慢。\n"
             "- 调度器路径并行执行不同规则系统；数据并行路径按依赖顺序执行系统，但在每个系统和网格重建内部并行。两者不能共用一个扩展上限结论。\n"
             "- Boids 显式注册密集组件；本轮分页稀疏存储的收益不能直接套用。固定世界下，邻居扫描成本仍会随密度增加。\n"
             "- 程序只计模拟步，不含创建、渲染、GLFW 或编辑器。GIF 中旧 FPS 不代表本次结果。\n"
             "- 两个 Release 基准编译通过。所有 parallel 程序运行均未出现 DIFF：并行路径及 orig/24 的最终位置 FNV 校验和与 orig/1 相同。"
             "这不是全部组件状态或所有场景的严格相等证明。\n"
             "- 历史数据保留，不根据跨日期差异宣称版本提速；共享机器与异构核心调度会造成波动。\n\n"
             "## 原始数据与复现\n\n") if zh else (
             "## Interpretation and validation scope\n\n"
             "- At 10,000 boids, the 3-thread median is slower than 2 threads and supplemental samples vary widely: scheduler 40.52–72.35 ms, data-parallel 35.33–59.15 ms. These results do not establish that 3 threads are inherently slower.\n"
             "- The scheduler runs independent rule systems concurrently; the data-parallel path runs systems in dependency order but parallelizes queries and grid rebuilding. Their scaling limits differ.\n"
             "- Boids registers dense components. Sparse paging gains do not directly transfer; neighbor scans still grow with density in a fixed world.\n"
             "- Timings cover simulation steps, excluding creation, rendering, GLFW and editor work. Historical GIF FPS does not represent these measurements.\n"
             "- Both Release benchmarks built successfully. Every parallel executable run reported no DIFF: final-position FNV checksums for parallel modes and orig/24 matched orig/1. This is not a full-component-state or all-scenarios equivalence proof.\n"
             "- Historical results remain unchanged. Cross-date differences are not claimed as version speedups; shared-machine load and heterogeneous-core scheduling introduce variation.\n\n"
             "## Raw data and reproduction\n\n")
    text += "[CSV](boids_retest.csv) · [Environment / source hashes](boids_retest_environment.json) · [Raw runs](boids_retest_raw/)\n\n"
    text += "```powershell\ncmake -S . -B build_boids_retest -DEKIT_BUILD_BOIDS=ON -DEKIT_BUILD_BOIDS_LIVE=OFF -DEKIT_BUILD_ENTT_COMPARE=OFF\ncmake --build build_boids_retest --config Release --target ekit_boids_bench ekit_boids_bench_parallel\npython benchmarks/run_boids_retest.py build_boids_retest/examples/boids/Release --threads 1,2,4,24\npython benchmarks/run_boids_retest.py build_boids_retest/examples/boids/Release --threads 3 --tag _3\npython benchmarks/analyze_boids_retest.py\npython benchmarks/write_boids_report.py\n```\n\n"
    text += ("分析与绘图需要 Python 和 matplotlib。历史报告见[基准索引](README.zh-CN.md)。\n" if zh else
             "Analysis and plotting require Python and matplotlib. See the [benchmark index](README.md) for historical reports.\n")
    (HERE / filename).write_text(text, encoding="utf-8")
