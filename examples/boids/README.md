# ekit Boids — a flocking (Boids) case study built on ekit

[English](README.md) | [简体中文](README.zh-CN.md)

An implementation of Craig Reynolds' Boids flocking algorithm built on **ekit**,
serving as a case study for the library. It demonstrates
ekit's core capabilities: **explicit component registration, fluent queries,
declarative `Reads/Writes` system dependencies, automatic parallel scheduling**,
plus a spatial hash grid for neighbor queries.

![boids](boids.gif)

## 10,000-boid recordings (no GLFW needed)

The animated GIFs below are rendered by the **headless** mode (`ekit_boids`) -
**GLFW is only needed for the interactive live viewer**, so anyone can watch the
simulation without installing anything. Each recording: 10,000 boids, 90 frames
@ 30 fps, 800x600 world (downscaled to 400x300), fixed seed.

The simulation is **deterministic**: the same seed and boid count produce
identical frames regardless of the thread count, so the four clips look the same
except for the **runtime FPS**, which is measured inside the recorder and stamped
on every frame. For 90 frames at 800x600 (10000 boids):

| threads | runtime FPS |
| --- | --- |
| 1 | ~12 fps |
| 2 | ~20 fps |
| 3 | ~29 fps |
| 4 | ~29 fps |

The stamp reads `threads=N | ~X fps | frame i / 90`.

| threads = 1 | threads = 2 |
| --- | --- |
| ![10k boids, 1 thread](boids_t1.gif) | ![10k boids, 2 threads](boids_t2.gif) |
| threads = 3 | threads = 4 |
| ![10k boids, 3 threads](boids_t3.gif) | ![10k boids, 4 threads](boids_t4.gif) |

Reproduce them with:

```powershell
cmake --build build --config Release --target ekit_boids
.\build\examples\boids\Release\ekit_boids.exe --boids 10000 --frames 90 --threads 1 --out frames_t1
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Dir frames_t1 -Fps 30 -Out boids_t1.gif -Scale 0.5 -Label -Title "threads=1 | ~12 fps"
```

## ekit vs EnTT (same algorithm)

The same boids algorithm (identical rule math, spatial grid, phase order) is
implemented on both [EnTT](https://github.com/skypjack/entt) (v4, cloned
externally - **not part of this repo**) and ekit. The two implementations
produce **bit-identical states** (verified by checksum), so the timings below
compare the ECS layers only.

```powershell
# clone EnTT outside the repo once
git clone --depth 1 https://github.com/skypjack/entt.git E:\Github\entt
cmake -S . -B build -DENTT_ROOT=E:/Github/entt
cmake --build build --config Release --target ekit_entt_compare
.\build\examples\boids\Release\ekit_entt_compare.exe   # 4 groups, EnTT left / ekit right
```

The comparison has three columns:

- `ekit` — the dependency-graph scheduler: whole systems run in parallel, one
  thread each.
- `ekit-dp` — the data-parallel query path. It uses the **same** dynamic
  chunking and the **same** storage-driven component access as the EnTT side
  (dense-array drive + per-component sparse lookup, identical component set), so
  this column isolates the ECS layer itself.

Measured on this machine (Windows 11, Intel i7-14650HX 24 threads, MSVC Release
/O2, 30 timed steps + 10 warmup, world 800x600, seed 20260810; ratio < 1 means
ekit is faster):

```
threads = 1
boids    entt ms/step   ekit ms/step   ekit/entt ekit-dp ms/step ekit-dp/entt
200      0.0930         0.1061         1.140     0.1042         1.120
1000     1.4037         1.7053         1.215     1.6756         1.194
5000     25.8781        31.2067        1.206     31.4271        1.214
10000    88.0296        106.9959       1.215     105.9340       1.203

threads = 2
200      0.1056         0.0934         0.884     0.1181         1.118
1000     0.8267         1.1056         1.337     0.9474         1.146
5000     13.2493        18.8909        1.426     15.8054        1.193
10000    45.1254        63.0978        1.398     51.2334        1.135

threads = 3
200      0.0683         0.0589         0.862     0.0862         1.262
1000     0.5583         0.7473         1.339     0.6700         1.200
5000     8.8551         12.6786        1.432     10.7378        1.213
10000    29.5412        42.3230        1.433     36.0372        1.220

threads = 4
200      0.0873         0.0637         0.730     0.0976         1.118
1000     0.4492         0.7703         1.715     0.5407         1.204
5000     7.4944         13.0885        1.746     8.4492         1.127
10000    23.6035        45.1274        1.912     26.1160        1.106
```
Takeaways:

- **Scheduler comparison (`ekit`):** on this Windows/MSVC build, ekit's
  whole-system scheduler is slower than EnTT at 1000+ boids (~1.2-1.9x on
  dense workloads). The cause is the parallelism method: ekit runs each of the
  4 phase-1 systems as one whole-system thread, while EnTT data-parallelizes
  each query across threads.
- **Controlled comparison (`ekit-dp`):** with identical algorithm, chunking,
  storage access and component set, `ekit-dp` closes most of the gap and stays
  ~1.1-1.26x slower than EnTT on this machine; at 200 boids it is roughly equal.
- Both `ekit` and `ekit-dp` produce **bit-identical** states to EnTT.
- The spatial grid sorts each cell by entity id so neighbor accumulation is
  order-independent and bit-deterministic on both sides.
## Algorithm

Each boid evaluates rules against its neighbors every frame and steers accordingly:

1. **Separation** — avoid nearby neighbors
2. **Alignment** — match the average heading of nearby neighbors
3. **Cohesion** — move toward the center of mass of nearby neighbors
4. **Bounds** — steer back toward the world center near the edges

Each rule writes its own **accumulator component**
(`Separation / Alignment / Cohesion / BoundsSteer`); a combine system applies them
to the velocity, and an integrate system advances the position.

## ekit feature map

| ekit feature | How it is used here |
|---|---|
| `EKIT_COMPONENT(T)` + `RegisterComponents<Ts...>()` | `Position / Velocity / BoidTag / Separation / Alignment / Cohesion / BoundsSteer` explicitly registered |
| Fluent queries `Query<Ts...>()` | every system iterates boids with `Query<...>().ForEach(...)` |
| `With<T>()` / marker components | `BoidTag` participates in every query as a marker |
| `TryGet<T>(entity)` | safe access to neighbor `Position / Velocity` during neighbor queries |
| system `Reads / Writes` | each system declares `using Reads/Writes = ekit::TypeList<...>` in-class |
| scheduler dependency analysis + parallelism | the four rule systems write **disjoint** accumulators, so the scheduler runs them in parallel; combine/integrate are ordered by the dependency chain |
| generation-based entity handles | `world.Create()` returns a generation-tagged `Entity`; recycling is stale-handle safe |

## How the scheduler works: two phases

Each frame is split into **two scheduler phases**:

```
Phase 1 (rules, fully parallel)
  SeparationSystem ──(writes Separation)───┐
  AlignmentSystem  ──(writes Alignment)────┼──┐
  CohesionSystem   ──(writes Cohesion)─────┼──┼──> Phase 2
  BoundsSystem     ──(writes BoundsSteer)──┘  │
                                              │
Phase 2 (apply + integrate)                   │
  UpdateVelocitySystem ──(writes Velocity)────┘
        |
        v
  IntegrateSystem ──(writes Position)──> next frame
```

Phase 1 rule systems only **read** `Position/Velocity` (the frame-start state) and
each write their own accumulator, so the scheduler runs them **fully in parallel**.
In phase 2, `UpdateVelocitySystem` reads the four accumulators and writes
`Velocity`; `IntegrateSystem` reads `Velocity` and writes `Position` — ordered by
the dependency chain.

**Why two phases?** This is the most instructive part of the case study. If a
system reads `Position` (rules) while another system in the same phase writes
`Position` (integrate), the scheduler detects circular dataflow and throws a
"dependency cycle" error — because `Reads/Writes` only declares access, it cannot
express "the rules want to read **last frame's** Position". Splitting "read the
frame-start state" and "write the new state" into two phases keeps the DAG
acyclic. This is a common frame-pipeline constraint in real ECS engines.

> The spatial grid is rebuilt **synchronously** at the start of each frame and is
> read-only during the scheduler phase, so the parallel rule systems are race-free.

## Real-time viewer

**`ekit_boids_live`** opens a real-time window using **GLFW + OpenGL** (GPU
rendering; hundreds of FPS with `--vsync 0`, or locked to the monitor refresh rate
with the default `--vsync 1`).

GLFW is your fork (https://github.com/chnnasn/glfw) and is **not part of this
repository** — clone it outside the repo (e.g. `E:\Github\glfw`) and point CMake
at it with `-DEKIT_GLFW_ROOT`:

```powershell
# 1) clone GLFW once (outside the repo)
git clone --depth 1 https://github.com/chnnasn/glfw.git E:\Github\glfw

# 2) configure + build
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DEKIT_GLFW_ROOT=E:/Github/glfw
cmake --build build --config Release --target ekit_boids_live

# 3) run
.\build\examples\boids\Release\ekit_boids_live.exe --boids 220
```

The window stays open until **you** close it (ESC or the window X button);
it never auto-quits by default. Pass `--frames N` to make it close after N
simulation steps (useful for automated testing).

**Resize / maximize the window and the flock follows**: the world bounds, the
spatial grid and the GL projection all track the current window size, and the
boids' positions are scaled proportionally, so the flock always roams the whole
window instead of a fixed 800x600 region.

In-window controls:

| Input | Action |
|---|---|
| mouse move | the flock follows the cursor |
| left button held | shoo the flock away from the cursor (repel) |
| `SPACE` | pause / resume |
| `R` | reset the flock (random respawn) |
| `UP` / `DOWN` | speed up / slow down (simulation steps per rendered frame) |
| `ESC` | quit |

The window title bar shows **boid count / flock count / FPS / speed multiplier**
in real time. Default movement speed is intentionally calm (max ~1.6 px/frame);
speed up with the `UP` key if you want more action.

## Headless mode (frames / animation)

```bash
cmake --build build --config Release --target ekit_boids
./build/Release/ekit_boids.exe --boids 220 --frames 180 --width 800 --height 600
```

Writes `frames/frame_0000.ppm ... frame_0179.ppm` and prints final stats (boid
count, average speed, flock count).

### Command-line options

```
--boids N       number of boids (default 220)
--frames N      frames to run (default 180; live mode: steps before auto-quit, 0 = infinite)
--width N       world/render width (default 800)
--height N      world/render height (default 600)
--seed N        random seed (default 20260810)
--threads N     scheduler threads, 0 = hardware concurrency (default 0)
--vsync 0|1     live mode: 1 = match monitor refresh, 0 = uncapped (default 1)
--out DIR       output directory for PPM frames (headless only, default frames)
--help          show help
```

### Rendering an animation

Windows (uses built-in .NET drawing, no extra dependencies):

```powershell
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Dir frames -Fps 30 -Out boids.gif -PngDir png
```

Produces per-frame PNGs in `png/` and a `boids.gif` animation. Any PPM-capable
tool works too (ImageMagick / ffmpeg):

```bash
ffmpeg -framerate 30 -i frames/frame_%04d.ppm boids.gif
```

## Benchmark

`ekit_boids_bench` measures the full per-step cost of the simulation
(grid rebuild + parallel rules + apply/integrate) across a matrix of boid
counts and thread counts:

```powershell
cmake --build build --config Release --target ekit_boids_bench
.\build\examples\boids\Release\ekit_boids_bench.exe
# custom matrix: .\...\ekit_boids_bench.exe --boids 200,500,1000 --threads 1,2,4,0
```

Measured on this machine (Windows 11, Intel i7-14650HX 24 threads, MSVC Release
/O2, world 800x600, seed 20260810, 120 timed steps + 20 warmup):

```
boids    threads   ms/step     steps/s    speedup   k boids/s   us/boid
------   -------   --------    -------    -------   ---------   --------
200      1         0.1488      6718.7     1.00      1343.7      0.744
200      2         0.1184      8446.7     1.26      1689.3      0.592
200      4         0.0755      13241.7    1.97      2648.3      0.378
200      24        0.0775      12906.0    1.92      2581.2      0.387

500      1         0.7771      1286.8     1.00      643.4       1.554
500      2         0.5361      1865.3     1.45      932.7       1.072
500      4         0.3636      2750.2     2.14      1375.1      0.727
500      24        0.3503      2854.9     2.22      1427.4      0.701

1000     1         2.5356      394.4      1.00      394.4       2.536
1000     2         1.5476      646.1      1.64      646.1       1.548
1000     4         1.0596      943.8      2.39      943.8       1.060
1000     24        1.0607      942.7      2.39      942.7       1.061

2000     1         8.0494      124.2      1.00      248.5       4.025
2000     2         4.8508      206.1      1.66      412.3       2.425
2000     4         3.3307      300.2      2.42      600.5       1.665
2000     24        3.3322      300.1      2.42      600.2       1.666

5000     1         32.6087     30.7       1.00      153.3       6.522
5000     2         18.9152     52.9       1.72      264.3       3.783
5000     4         12.8591     77.8       2.54      388.8       2.572
5000     24        12.8450     77.9       2.54      389.3       2.569

10000    1         104.7395    9.5        1.00      95.5        10.474
10000    2         62.1217     16.1       1.69      161.0       6.212
10000    4         41.9782     23.8       2.50      238.2       4.198
10000    24        41.7498     24.0       2.51      239.5       4.175

single-boid throughput (best): 2.648 boids/us (0.378 us/boid) at 200 boids, 4 threads
```
Key takeaways:

- **`us/boid`** is the per-boid cost per step in microseconds (inverse of the
  single-boid throughput `boids/us`). It grows with density: ~0.38 us (200
  boids) to ~4.2 us (10k boids) at 4 threads.
- **Parallel speedup plateaus around 2.5x at 4 threads; 24 threads add nothing.**
  The dependency graph has exactly 4 parallel rule systems in phase 1, while the
  spatial grid rebuild and the 2-system phase 2 are serial. This matches the
  serial fraction of the workload.
- **Per-step cost grows super-linearly with boid count** in a fixed-size world:
  doubling the boids doubles the density, so every boid finds more neighbors
  (neighbor queries are O(n * avg-neighbors)).
- 10k boids runs at ~24 steps/s with 4 threads (~238k boids/s throughput); 200
  boids run at ~13.2k steps/s.
## Files

- `boids.hpp` — components, config, spatial grid, six systems, `SpawnBoids` / `CountFlocks` (the core)
- `canvas.hpp` — minimal software rasterizer (triangles/circles + PPM output, used by the headless mode)
- `render_helpers.hpp` — boid coloring and triangle drawing
- `cli.hpp` — command-line parsing (shared by headless and live)
- `main.cpp` — headless mode: simulation + PPM frame export + stats
- `main_live.cpp` — live mode: GLFW + OpenGL window + interactive controls
- `render.ps1` — PPM → PNG / GIF conversion script
- GLFW (external dependency, **not in this repo**): `git clone https://github.com/chnnasn/glfw.git` outside the repository
