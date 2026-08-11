# ekit benchmarks

[English](README.md) | [简体中文](README.zh-CN.md)

Raw data, test conditions and analysis for the ekit Boids benchmark and the
ekit vs EnTT comparison. Generated on **2026-08-11**.

## Code version

- Git commit: `62aab16` (`62aab165fa21030c09528cecece1ecd3560e559a`)
  "Serialize writer pairs by registration order instead of reporting a cycle"
- Benchmark sources: `examples/boids/bench.cpp` (ekit Boids),
  `examples/boids/compare_bench.cpp` + `examples/boids/entt_impl.hpp`
  (ekit vs EnTT), `examples/boids/boids.hpp` (simulation core)

## Test conditions

| item | value |
| --- | --- |
| CPU | Intel Core i7-14650HX (16 cores / 24 threads) |
| OS | Windows 10/11 x64 |
| Compiler | MSVC 19.50 (VS 2026), `/O2`, C++20, Release x64 |
| World | 800 x 600 |
| Seed | 20260810 |
| Algorithm | separation / alignment / cohesion / bounds rules, uniform spatial grid (cell = neighbor radius 48), **cells sorted by entity id** so neighbor accumulation is deterministic |

`ekit_boids_bench` (single-library):

| | |
| --- | --- |
| boid counts | 200, 500, 1000, 2000, 5000, 10000 |
| thread counts | 1, 2, 4, 24 |
| timed steps | 120 (+ 20 warmup) |

`ekit_entt_compare` (EnTT v4 vs ekit, same algorithm):

| | |
| --- | --- |
| boid counts | 200, 1000, 5000, 10000 |
| thread counts | 1, 2, 3, 4 |
| timed steps | 30 (+ 10 warmup) |
| verification | both implementations produce bit-identical state (`state identical: YES`) |

> Variance: single-run measurements on a shared machine; expect +/-10-30% run
> to run. Compare relative ratios, not absolute values.

## Files

| file | description |
| --- | --- |
| `ekit_boids_bench_raw.txt` | raw console output of the ekit Boids benchmark |
| `ekit_boids_bench.csv` | same data, machine readable |
| `entt_vs_ekit_raw.txt` | raw console output of the ekit vs EnTT comparison |
| `chart_cost_vs_boids.png` | ms/step vs boid count (log-log) |
| `chart_speedup_vs_threads.png` | speedup vs thread count |
| `chart_throughput.png` | throughput (k boids/s) vs boid count |
| `analyze.py` | script that parses the raw data and regenerates the CSV/charts |

## Analysis

### Per-step cost vs boid count

Measured at 4 threads (ms/step):

| from | to | x boids | x time | exponent |
| --- | --- | --- | --- | --- |
| 200 | 500 | 2.5x | 4.05x | 1.53 |
| 500 | 1000 | 2.0x | 2.80x | 1.49 |
| 1000 | 2000 | 2.0x | 3.07x | 1.62 |
| 2000 | 5000 | 2.5x | 3.97x | 1.51 |
| 5000 | 10000 | 2.0x | 3.41x | **1.77** |

The cost grows with exponent ~1.5-1.8 (between linear and quadratic) and the
exponent **rises toward 2 as density increases**. Cause: the world size is
fixed, so doubling the boids doubles the density, which doubles the *number of
neighbors per boid*; the neighbor search is O(n x neighbors), i.e. O(n^2) in
the uniform-density limit. The curve is a decline of throughput: 200 boids run
at ~2.7M boids/s (4 threads) but only ~285k boids/s at 10000.

### Parallel scaling by thread count

| boids | t2 | t4 | t24 |
| --- | --- | --- | --- |
| 200 | 1.32x | 1.94x | 1.96x |
| 1000 | 1.62x | 2.41x | 2.28x |
| 10000 | 1.69x | 2.23x | 2.44x |

In these measurements, speedup changes little beyond **4 threads**. The
dependency graph has 4 parallel rule systems in phase 1, while the spatial grid
rebuild and the 2-system phase-2 chain are serial. The observed range of
~2.2-2.9x is consistent with that serial work and load imbalance among the 4
rules (alignment and cohesion scan more neighbors than separation).

### ekit vs EnTT (same algorithm, EnTT v4)

- 1 thread: ekit measures ~15-25% faster. One possible contributor is its
  dense-array query iteration.
- 4 threads: EnTT measures ~25-30% faster. Its entity-chunked `parallel_for`
  distributes each pass across workers, while ekit's scheduler parallelizes
  whole systems and includes per-step scheduling overhead.

## Reproduce

```powershell
cmake -S . -B build -DENTT_ROOT=E:/Github/entt   # EnTT cloned outside the repo
cmake --build build --config Release --target ekit_boids_bench ekit_entt_compare
.\build\examples\boids\Release\ekit_boids_bench.exe     *> benchmarks\ekit_boids_bench_raw.txt
.\build\examples\boids\Release\ekit_entt_compare.exe    *> benchmarks\entt_vs_ekit_raw.txt
python benchmarks\analyze.py
```
