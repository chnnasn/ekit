# Current-version Boids retest

[English](boids_retest.md) | [简体中文](boids_retest.zh-CN.md)

Measured on 2026-09-21 at `055e1c9` (core optimization `4d2d014`). Current sources were rebuilt and run; neither the historical revision nor EnTT was rerun, so this is not a version speedup or EnTT comparison.

Intel Core i7-14650HX, Windows 11 x64 10.0.26200, MSVC 19.50.35724 Release /O2. 800×600 world, seed 20260810, 20 warmup + 120 timed steps. Six rounds, discard round 0, report five-round medians. All 24 logical CPUs are available to the OS; no single-core pinning. Programs run serially and their order alternates each round. Three-thread samples were collected in a later six-round supplement and may reflect changed machine conditions. CSV includes min/max values.

Primary charts use 1, 2, 3 and 4 threads; 24 threads is an extra scaling point. Speedups divide each path's own single-thread median by its multithread median, unlike the parallel executable's original orig-baseline speedup column.

## Per-step time

### System scheduler

| boids | 1 (ms) | 2 (ms) | 3 (ms) | 4 (ms) | 24 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 200 | 0.1408 | 0.1184 | 0.0778 | 0.0770 | 0.0775 |
| 500 | 0.7452 | 0.4979 | 0.3268 | 0.3275 | 0.3260 |
| 1000 | 2.3783 | 1.4703 | 1.0227 | 1.0142 | 1.0323 |
| 2000 | 7.7274 | 4.5938 | 3.1555 | 3.1947 | 3.1698 |
| 5000 | 31.2551 | 18.2715 | 12.3555 | 12.5305 | 12.4350 |
| 10000 | 102.1213 | 59.6865 | 67.7146 | 40.3539 | 40.3134 |

10,000 boids: **2.53×** at 4 threads and **2.53×** at 24 threads versus this path's single thread.

### Data-parallel queries + grid

| boids | 1 (ms) | 2 (ms) | 3 (ms) | 4 (ms) | 24 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 200 | 0.1437 | 0.1634 | 0.1227 | 0.1424 | 0.1417 |
| 500 | 0.7483 | 0.4981 | 0.3941 | 0.3193 | 0.7458 |
| 1000 | 2.3630 | 1.3972 | 0.9906 | 0.7614 | 2.3535 |
| 2000 | 7.6955 | 4.0827 | 2.8418 | 2.2126 | 1.3341 |
| 5000 | 30.4961 | 15.8150 | 19.7172 | 8.2029 | 3.1622 |
| 10000 | 100.8996 | 51.0233 | 57.9652 | 25.6267 | 8.4852 |

10,000 boids: **3.94×** at 4 threads and **11.89×** at 24 threads versus this path's single thread.

![Cost scaling](chart_boids_current_cost.en.png)

![Thread scaling](chart_boids_current_speedup.en.png)

## Interpretation and validation scope

- At 10,000 boids, the 3-thread median is slower than 2 threads and supplemental samples vary widely: scheduler 40.52–72.35 ms, data-parallel 35.33–59.15 ms. These results do not establish that 3 threads are inherently slower.
- The scheduler runs independent rule systems concurrently; the data-parallel path runs systems in dependency order but parallelizes queries and grid rebuilding. Their scaling limits differ.
- Boids registers dense components. Sparse paging gains do not directly transfer; neighbor scans still grow with density in a fixed world.
- Timings cover simulation steps, excluding creation, rendering, GLFW and editor work. Historical GIF FPS does not represent these measurements.
- Both Release benchmarks built successfully. Every parallel executable run reported no DIFF: final-position FNV checksums for parallel modes and orig/24 matched orig/1. This is not a full-component-state or all-scenarios equivalence proof.
- Historical results remain unchanged. Cross-date differences are not claimed as version speedups; shared-machine load and heterogeneous-core scheduling introduce variation.

## Raw data and reproduction

[CSV](boids_retest.csv) · [Environment / source hashes](boids_retest_environment.json) · [Raw runs](boids_retest_raw/)

```powershell
cmake -S . -B build_boids_retest -DEKIT_BUILD_BOIDS=ON -DEKIT_BUILD_BOIDS_LIVE=OFF -DEKIT_BUILD_ENTT_COMPARE=OFF
cmake --build build_boids_retest --config Release --target ekit_boids_bench ekit_boids_bench_parallel
python benchmarks/run_boids_retest.py build_boids_retest/examples/boids/Release --threads 1,2,4,24
python benchmarks/run_boids_retest.py build_boids_retest/examples/boids/Release --threads 3 --tag _3
python benchmarks/analyze_boids_retest.py
python benchmarks/write_boids_report.py
```

Analysis and plotting require Python and matplotlib. See the [benchmark index](README.md) for historical reports.
