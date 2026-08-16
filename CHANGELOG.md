# Changelog

All notable changes to ekit are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Data-parallel query** - `ekit::ThreadPool` and `Query::ForEachParallel(pool, fn)` split the
  smallest storage into chunks and run them concurrently with dynamic atomic work stealing.
- **Parallel spatial grid** - the Boids `SpatialGrid` now has a `BuildParallel` path (count ->
  resize -> scatter -> sort) that is bit-identical to the serial `Build`.
- **Comparison benchmark** - `ekit_boids_bench_parallel` compares the original scheduler-based
  execution against the data-parallel path and verifies deterministic state.
- **EnTT comparison with matched parallelism** - the EnTT side of `ekit_entt_compare` now uses the
  same dynamic-chunking parallel loop as `Query::ForEachParallel`, so the benchmark isolates the
  ECS layer instead of the parallelization scheme.
- **EnTT comparison with matched storage access** - the EnTT side now drives the same storage as the
  ekit query and resolves every component via `storage->get(entity)` (a sparse lookup), mirroring
  ekit's dense-array drive + per-component lookup and identical component set; `ekit-dp` measures
  ~22-31% faster than EnTT v4 at 5000-10000 boids.

### Fixed

- `Query::Where` now supports capturing lambdas and composes chained predicates with
  logical AND (previously only the last filter was kept and capturing predicates did not compile).
- `ForEachBatch` / `ForEachBatchParallel` now throw when an excluded component is sparse,
  instead of silently ignoring the exclusion.
- `ScratchSoa` columns now use `std::vector<T>` storage, guaranteeing correct alignment for
  every column type.
- `Scheduler::SetThreadCount` now recreates the worker pool so the new thread count takes
  effect on the next run.
- Documentation updated to match the dense-archetype + sparse-set dual storage backend.

## [0.1.0] - 2026-08-12

First public release.

### Added

- **Core ECS**
  - Strong-typed, generation-based `Entity` handle (`Entity::Null`, `IsAlive`, automatic slot recycling with generation bumps).
  - Explicit component declaration via `EKIT_COMPONENT(T)` and registration via `world.RegisterComponent<T>()`; undeclared/unregistered usage produces readable compile-time or runtime errors.
  - Dense archetype storage (SoA columns) plus a `ComponentStorage<T>` sparse-set backend (cache-friendly dense arrays, swap-and-pop removal).
- **World** - entity create/destroy, component `Add / Emplace / Set / Get / TryGet / Has / Remove / Patch / Clear`, named entities, batch registration, `ClearAll`.
- **Query** - fluent `Query<Ts...>()` with `Where / With / Without / Optional / ForEach / Each / Count`, composed entirely at compile time (no type erasure, no per-entity virtual calls); iterates the smallest matching storage.
- **System & Scheduler** - systems declare `Reads / Writes` in-class; the scheduler builds a dependency DAG and runs independent systems in parallel on an internal thread pool. Two writers of the same component are serialized in registration order; a cycle is reported only for genuine dependency contradictions.
- **Event** - `world.Subscribe<T>(handler)` / `world.Emit<T>(args...)` with stable subscription handles (safe to unsubscribe from inside a callback).
- **Boids case study** (`examples/boids/`) - a complete flocking simulation on ekit:
  - GLFW + OpenGL live viewer (mouse-follow flock, window-following world, SPACE / R / arrows / ESC controls).
  - Headless PPM renderer + `render.ps1` (PNG / animated GIF, with optional frame/FPS stamping).
  - `ekit_boids_bench` (boid-count x thread-count matrix) and `ekit_entt_compare` (same algorithm on EnTT v4, bit-identical state).
  - 10,000-boid recordings with runtime-FPS labels.
- **Benchmarks** (`benchmarks/`) - test conditions, raw data (txt/csv), charts, `analyze.py`, and analysis of the decline curve (super-linear per-step cost, exponent ~1.5-1.8) and the parallel stall point (speedup plateaus at 4 threads); EN + zh-CN docs.
- Unit tests (33 cases / 268 assertions) and a README in English and Simplified Chinese.

### Changed

- `EKIT_COMPONENT(T)` is now an in-class marker, so components can be declared in any namespace (previously the namespace-scope specialization failed outside the global namespace).

### Fixed

- Entity slot liveness: destroyed slots were reported as alive by `ForEachEntity` / `GetEntity` / `IsAlive`; now tracked by an explicit liveness flag.
- Scheduler: two writers of the same component no longer form a dependency cycle (serialized by registration order instead).
- Boids spatial grid sorts each cell by entity id so neighbor accumulation (and therefore the whole simulation) is fully deterministic.

[Unreleased]: https://github.com/chnnasn/ekit/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/chnnasn/ekit/releases/tag/v0.1.0