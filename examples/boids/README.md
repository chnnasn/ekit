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

Measured on this machine (Apple Silicon, clang, -O2, 120 timed steps + 20
warmup, world 800x600, seed 20260810; ratio < 1 means ekit is faster):

```
threads = 1
boids    entt ms/step   ekit ms/step   ekit/entt  ekit-dp ms/step  ekit-dp/entt
200      0.0885         0.0856         0.967      0.0752          0.850
1000     1.5510         1.3593         0.876      1.3138          0.847
5000     20.7945        15.9294        0.766      15.2530         0.734
10000    70.1903        54.5702        0.777      52.1881         0.744

threads = 2
200      0.0917         0.0650         0.708      0.0902          0.983
1000     0.8197         0.8588         1.048      0.7278          0.888
5000     11.0227        9.9391         0.902      8.5136          0.772
10000    38.5296        34.7373        0.902      29.1513         0.757

threads = 3
200      0.0971         0.0615         0.634      0.0983          1.013
1000     0.6598         0.6413         0.972      0.5998          0.909
5000     8.2491         7.4971         0.909      6.0826          0.737
10000    26.3406        24.8070        0.942      19.7936         0.751

threads = 4
200      0.0932         0.0623         0.668      0.0837          0.898
1000     0.5460         0.6415         1.175      0.4941          0.905
5000     6.7052         7.4500         1.111      4.6617          0.695
10000    20.9687        25.8639        1.233      17.4223         0.831
```

Takeaways:

- **Controlled comparison (`ekit-dp`):** with identical algorithm, chunking,
  storage access and component set, ekit's ECS layer is ~17-31% faster than EnTT
  v4 at 5000/10000 boids across 1-4 threads (`ekit-dp/entt` ~0.69-0.78), and
  ~5-15% faster at 1000 boids. Small counts (200) are roughly equal.
- **Scheduler comparison (`ekit`):** the original scheduler wins at 1 thread but
  loses to EnTT at 4 threads on dense workloads - a parallelism-method
  difference (whole-system parallelism vs data parallelism), not a storage
  difference.
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

Measured on this machine (24 hardware threads, world 800x600, 120 steps):

```
boids    threads   ms/step     steps/s    speedup   k boids/s   us/boid
------   -------   --------    -------    -------   ---------   --------
200      1         0.1300      7692.4     1.00      1538.5      0.650
200      4         0.0717      13946.8    1.81      2789.4      0.359

500      1         0.6234      1604.1     1.00      802.0       1.247
500      4         0.2789      3585.0     2.23      1792.5      0.558

1000     1         1.8703      534.7      1.00      534.7       1.870
1000     4         0.8176      1223.1     2.29      1223.1      0.818

2000     1         5.6782      176.1      1.00      352.2       2.839
2000     4         2.4551      407.3      2.31      814.6       1.228

5000     1         37.6368     26.6       1.00      132.8       7.527
5000     4         15.7870     63.3       2.38      316.7       3.157

10000    1         113.4400    8.8        1.00      88.2        11.344
10000    4         51.6270     19.4       2.20      193.7       5.163

single-boid throughput (best): 2.789 boids/us (0.359 us/boid) at 200 boids, 4 threads
```

Key takeaways:

- **`us/boid`** is the per-boid cost per step in microseconds (inverse of the
  single-boid throughput `boids/us`). It grows with density: in a fixed-size
  world, more boids means more neighbors per boid, so the per-boid cost rises
  from ~0.36 us (200 boids) to ~5.2 us (10k boids) at 4 threads.
- **Parallel speedup plateaus around 2.3x at 4 threads.** The dependency graph has
  exactly 4 parallel rule systems in phase 1, and the spatial grid rebuild plus
  the 2-system phase 2 are serial, so more than ~4 worker threads cannot help.
  This result is consistent with the amount of serial work in this workload.
- **Per-step cost grows super-linearly with boid count** in a fixed-size world:
  doubling the boids doubles the density, so every boid finds more neighbors in
  its radius (neighbor queries are O(n * avg-neighbors)).
- 10k boids still runs at ~20 steps/s with 4 threads (~200k boids/s throughput);
  200 boids run at ~14.5k steps/s.

## Files

- `boids.hpp` — components, config, spatial grid, six systems, `SpawnBoids` / `CountFlocks` (the core)
- `canvas.hpp` — minimal software rasterizer (triangles/circles + PPM output, used by the headless mode)
- `render_helpers.hpp` — boid coloring and triangle drawing
- `cli.hpp` — command-line parsing (shared by headless and live)
- `main.cpp` — headless mode: simulation + PPM frame export + stats
- `main_live.cpp` — live mode: GLFW + OpenGL window + interactive controls
- `render.ps1` — PPM → PNG / GIF conversion script
- GLFW (external dependency, **not in this repo**): `git clone https://github.com/chnnasn/glfw.git` outside the repository
