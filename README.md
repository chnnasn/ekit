# Ekit

[English](README.md) | [简体中文](README.zh-CN.md)

**Ekit** is a header-only **C++20 ECS** (Entity-Component-System) library for game
engines. It combines sparse-set storage with an explicit, fluent API influenced by
C# LINQ and Unity DOTS. The project focuses on keeping common ECS operations readable
while avoiding type erasure and per-entity virtual dispatch in query iteration.

[EnTT](https://github.com/skypjack/entt) is a mature, feature-rich ECS library. Ekit is
smaller and covers a narrower feature set, with more emphasis on explicit registration
and fluent queries. The appropriate choice depends on the requirements of the project.

## Design philosophy

1. **Explicit over implicit**
   - Components must be declared by adding `EKIT_COMPONENT(T)` inside the struct and explicitly registered:
     `world.RegisterComponent<T>()`. No magic implicit registration.
   - Using an undeclared component produces a readable `static_assert`; using an
     unregistered one throws a clear `EkitException` telling you exactly what to call.
   - Systems declare their data dependencies (`Reads` / `Writes`) inside the class.

2. **Fluent & modern API (C# LINQ + Unity DOTS)**
   - PascalCase methods: `world.Create()`, `world.Query<Ts...>().ForEach(...)`.
   - Systems are classes with an `Execute(World&)` method, like Unity DOTS.

3. **Readable diagnostics and typed interfaces**
   - No template error storms: `static_assert` and `if constexpr` produce precise errors.
   - Entities are strong-typed, generation-based handles (never bare `uint32_t`).
   - The fluent query chain is composed at compile time, without type erasure or
     per-entity virtual calls.

4. **Core architecture**
   - Sparse-set storage, a dependency-aware parallel scheduler, named entities, and an
     event system provide building blocks for declarative auto-parallelization, editor
     integration, and network synchronization.

## Quick start

```cpp
#include <ekit/ekit.hpp>

struct Position {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Position);
};

struct Velocity {
    float vx = 0.f;
    float vy = 0.f;
    EKIT_COMPONENT(Velocity);
};

int main() {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();

    auto ship = world.Create("ship");
    world.Add<Position>(ship, 10.f, 5.f);
    world.Add<Velocity>(ship, 2.f, 0.f);

    const float dt = 1.f / 60.f;
    world.Query<Position, Velocity>()
         .ForEach([dt](Position& p, Velocity& v) {
             p.x += v.vx * dt;
             p.y += v.vy * dt;
         });
}
```

## Write it like C#

For the 90% case, `World` exposes C#-style shortcuts so you do not have to spell
out a full fluent query:

```cpp
world.RegisterComponents<Position, Velocity>();

// Count entities that have all of these components
std::size_t movers = world.Count<Position, Velocity>();

// Update them in one call (C#: foreach (var e in view))
world.ForEach<Position, Velocity>([](Position& p, Velocity& v) {
    p.x += v.vx;
    p.y += v.vy;
});

// Parallel scalar update
ekit::ThreadPool pool(0); // 0 == hardware concurrency
world.ForEachParallel<Position, Velocity>(pool, [](Position& p, Velocity& v) {
    p.x += v.vx;
});

// SoA batch update (dense components only, raw aligned pointers)
world.ForEachBatch<Position, Velocity>([](Position* p, Velocity* v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        p[i].x += v[i].vx;
    }
});
```

Every shortcut is literally `Query<Ts...>().Method(...)`, so you can always drop
back to the full fluent chain (`Where` / `With` / `Without` / `Optional`) when you
need filters:

```cpp
world.Query<Position, Velocity>()
     .With<Renderable>()
     .Without<Disabled>()
     .Where([](Position&, Velocity&, Renderable&) { return true; })
     .ForEach([](ekit::Entity e, Position& p, Velocity& v, Renderable&) {
         // ...
     });
```

A runnable tour of the ergonomic surface lives in
[`examples/ergonomic.cpp`](examples/ergonomic.cpp); build it with the
`ekit_ergonomic` target.
## Features

- **Entity** — strong-typed, generation-based handle with dangling-handle safety
  (`Entity::Null`, `IsAlive`, automatic slot recycling with generation bumps).
- **Component** — POD structs declaring `EKIT_COMPONENT(T)` inside the class body; explicit
  `world.RegisterComponent<T>()` for dense archetype SoA storage, or
  `world.RegisterSparseComponent<T>()` for per-type sparse sets (cache-friendly
  dense arrays, swap-and-pop removal).
- **World** — entity create/destroy, component `Add / Emplace / Set / Get / TryGet /
  Has / Remove / Patch / Clear`, named entities, batch registration, `ClearAll`.
- **Query** — fluent queries with `Where / With / Without / Optional / ForEach / Count`,
  iterating the smallest matching storage:
  ```cpp
  world.Query<Position, Velocity>()
       .With<Renderable>()
       .Without<Disabled>()
       .Optional<Health>()
       .Where([](Position& p, Velocity& v, Renderable&, Health* hp) {
           return hp == nullptr || hp->hp > 0;
       })
       .ForEach([](ekit::Entity e, Position& p, Velocity& v, Renderable&, Health* hp) {
           // ...
       });
  ```
  Required components are passed by reference; optional components as pointers
  (`nullptr` when absent); the `Entity` handle is optional and comes first.
- **Data-parallel query & thread pool** — `ekit::ThreadPool` plus
  `Query::ForEachParallel(pool, fn)` split the smallest storage into chunks and run
  them concurrently (dynamic atomic work stealing, so per-entity cost stays balanced).
  The callback must only touch the entity's own components:
  ```cpp
  ekit::ThreadPool pool(0);                 // 0 == hardware concurrency
  world.Query<Position, Velocity>()
       .ForEachParallel(pool, [](Position& p, Velocity& v) { p.x += v.vx; });
  ```
- **System & Scheduler** — systems declare `Reads` / `Writes`; the scheduler builds a
  dependency DAG and executes independent systems in parallel on an internal thread pool:
  ```cpp
  struct GravitySystem {
      using Writes = ekit::TypeList<Velocity>;
      void Execute(ekit::World& world) {
          world.Query<Velocity>().ForEach([](Velocity& v) { v.vy -= 9.8f; });
      }
  };

  ekit::Scheduler scheduler(4);           // 0 == hardware concurrency
  scheduler.AddSystem(GravitySystem{})
           .AddSystem(MoveSystem{});
  scheduler.Run(world);                   // or RunSingleThreaded(world)
  ```
  A writer is ordered before every reader of the same component. Two writers of the
  same component do not form a cycle: they are serialized in registration order.
  A cycle is only reported when the declared dependencies genuinely contradict
  each other (e.g. A writes X / reads Y while B writes Y / reads X).
- **Event** — `world.Subscribe<T>(handler)` / `world.Emit<T>(args...)`:
  ```cpp
  struct HitEvent { int damage; ekit::Entity target; };
  ekit::EventSubscription sub = world.Subscribe<HitEvent>(
      [](const HitEvent& ev) { /* ... */ });
  world.Emit<HitEvent>(10, target);
  sub.Unsubscribe();
  ```

## Integration

Header-only, zero runtime dependencies:

- **CMake**
  ```cmake
  add_subdirectory(ekit)
  target_link_libraries(app PRIVATE ekit::ekit)
  ```
- **Manual**: add `include/` to your include path and `#include <ekit/ekit.hpp>`.

Requires C++20 (MSVC 19.29+, GCC 11+, Clang 14+).

## Case study: Boids

`examples/boids/` is a flocking simulation built on ekit. It demonstrates
explicit component registration, fluent queries, systems with `Reads/Writes`
declarations, the parallel scheduler (four boid-rule systems run concurrently),
spatial-hash neighbor queries, and a two-phase frame pipeline:

```bash
# Real-time window (GLFW + OpenGL, GPU rendering). GLFW is NOT part of this
# repo: clone https://github.com/chnnasn/glfw outside the repo, then:
cmake -S . -B build -DEKIT_GLFW_ROOT=E:/Github/glfw
cmake --build build --config Release --target ekit_boids_live
./build/examples/boids/Release/ekit_boids_live.exe --boids 220    # SPACE pause, R reset, ESC quit

# Headless: PPM frames -> animated GIF
cmake --build build --config Release --target ekit_boids
./build/examples/boids/Release/ekit_boids.exe --boids 220 --frames 180
powershell -ExecutionPolicy Bypass -File examples/boids/render.ps1 -Fps 30   # -> boids.gif
```

See [examples/boids/README.md](examples/boids/README.md) for details.

## Benchmarks

The [latest benchmark report](benchmarks/ecs_comparison.md) records the native ECS
and SceneWorld results below. The [benchmark index](benchmarks/README.md) separates
this EnTT comparison from earlier version-to-version and Boids measurements.

### ekit vs EnTT: native ECS and engine integration

The native ECS test uses **ekit `3fcda56`**; the engine adapter is **`01f923f` on
`dev_ekit`**. The comparison uses **EnTT 3.15.0**, as used by TomCat's `main` branch.
Test environment and integration code:
[TomCat_Engine / dev_ekit](https://github.com/chnnasn/TomCat_Engine/tree/dev_ekit).

Hardware/software: **Intel Core i7-14650HX, Windows x64, MSVC Release**.
Tests run **single-threaded for six rounds, discarding the first and reporting
the median of the remaining five**.

Native ECS results for **100,000 entities**, in milliseconds; lower is better:

| Operation | EnTT | ekit sparse | ekit relative time |
| --- | ---: | ---: | --- |
| Create entities and add two components | 4.07 | 8.80 | 2.17× |
| Two-component traversal/update, 200 passes | 35.85 | 49.19 | 37% more |
| Random reads, 10 passes | 3.72 | 13.97 | 3.75× |
| Component add/remove, 10 rounds | 24.93 | 12.86 | 48% less |
| Destroy all entities | 5.80 | 1.69 | 71% less |

Across three successive benchmark reports, sparse traversal time relative to EnTT narrowed from
**7.28× → 3.26× → 1.37×**. Query optimization has substantially improved traversal;
random reads and entity creation still trail EnTT.

**Dense storage** completes the same 200 traversal passes in **13.74 ms**, about
**62% less time** than EnTT's 35.85 ms. Its component add/remove time is **6.55×**
EnTT's, however. It suits frequently traversed data with infrequent structural
changes; these results do not make dense storage universally preferable.

The new **`SceneWorld::ForEach`** adapter was separately tested using the dependency
actually pinned by `dev_ekit`, rather than assuming it uses the native test's ekit
revision:

| Second-component coverage | EnTT | SceneWorld | Relative time |
| --- | ---: | ---: | --- |
| 1% | 0.455 ms | 0.575 ms | About 26% more |
| 10% | 3.991 ms | 5.786 ms | About 45% more |
| 100% | 35.687 ms | 41.959 ms | About 18% more |

All adapter measurements use **100,000 entities and 200 updates**, excluding
creation and editor picking management.

In this workload, sparse component churn and entity destruction outperform the
EnTT comparison, sparse traversal is much closer, and dense traversal outperforms
EnTT for two simple data components. The next priorities are random access and
creation overhead while preserving reference validity, lifetimes and query
semantics. **These results do not generalize directly to complex components,
multithreaded systems or whole-engine frame rates.**

### Historical Boids benchmark

The separate Boids workload below uses different test conditions from the
single-threaded ECS measurements above.

Full conditions, raw data and the analysis scripts live in
[`benchmarks/`](benchmarks/README.md). Headline results (Intel i7-14650HX, 24
threads, MSVC Release /O2, world 800x600, seed 20260810, 120 timed steps + 20
warmup):

### Per-step cost as boid count increases

| from | to | x boids | x time | exponent |
| --- | --- | --- | --- | --- |
| 200 | 500 | 2.5x | 4.82x | 1.72 |
| 1000 | 2000 | 2.0x | 3.14x | 1.65 |
| 5000 | 10000 | 2.0x | 3.26x | 1.71 |

Per-step cost scales as n^1.5..n^1.7 and the exponent **rises toward 2 with
density**: the world is fixed, so doubling the boids doubles the density and
the number of neighbors per boid - the neighbor search is O(n x neighbors),
i.e. O(n^2) in the uniform-density limit. Throughput falls from ~2.65M boids/s
(200 boids) to ~238k boids/s (10000 boids).

![per-step cost vs boids](benchmarks/chart_cost_vs_boids.png)

### Parallel scaling by thread count

| boids | t2 | t4 | t24 |
| --- | --- | --- | --- |
| 200 | 1.26x | 1.97x | 1.92x |
| 10000 | 1.69x | 2.50x | 2.51x |

In these measurements, speedup changes little beyond **4 threads**. The dependency
graph has 4 parallel rule systems, while the grid rebuild and phase-2 chain are
serial; the observed speedup is therefore ~2.0-2.5x rather than 4x.

![speedup vs threads](benchmarks/chart_speedup_vs_threads.png)

### ekit vs EnTT (same algorithm, EnTT v4)

On this Windows/MSVC build, the ekit scheduler is ~20% slower than EnTT at 1
thread and ~1.7-1.9x slower at 4 threads on dense workloads. The controlled
data-parallel path `ekit-dp` (same chunking, same storage access, same component
set) narrows the gap to ~1.1-1.13x at 4 threads. Both implementations produce
bit-identical simulation state for the tested workload.
## Building & testing

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

## Unit tests

`tests/tests.cpp` ships with the library and is run via `ctest`:
**49 test cases / 307 assertions, all passing.** Coverage:

| area | tests |
| --- | --- |
| Entity - generation, stale-handle safety, slot recycling | 5 |
| Components - registration, CRUD, error paths, clear | 6 |
| Queries - ForEach, Where, With/Without/Optional, const refs | 6 |
| Parallel queries - ForEachParallel correctness, filters, deterministic writes | 3 |
| SoA batch queries - ForEachBatch / ForEachBatchParallel | 3 |
| World shortcuts - ForEach / ForEachParallel / ForEachBatch(Parallel) / Count | 3 |
| Named entities | 1 |
| Events - subscribe/emit, multiple handlers | 3 |
| Systems & Scheduler - dependency ordering, parallelism, writer serialization, genuine-cycle detection | 6 |
| Component declarations - traits, manual specialization | 2 |
| Sparse components - basic CRUD, parallel queries | 2 |
| Stream processing - ScratchSoa collect-then-batch | 3 |
| Regression - destroy-then-iterate, free-slot access, mutation during iteration, generation overflow, self-unsubscribe, scheduler recovery after a task exception | 6 |
| **Total** | **49 / 307** |

Run them with:

```bash
cmake -S . -B build
cmake --build build --config Release --target ekit_tests
./build/Release/ekit_tests.exe        # or: ctest --test-dir build -C Release
```

## Project layout

```
include/ekit/
  core.hpp        exceptions, TypeList, type ids
  entity.hpp      Entity (generation-based handle)
  component.hpp   EKIT_COMPONENT, Archetype (SoA) + ComponentStorage (sparse set)
  query.hpp       fluent Query (Where / With / Without / Optional / ForEach / ForEachBatch / ForEachParallel)
  stream.hpp      ScratchSoa<Ts...> collect-then-batch staging buffer
  parallel.hpp    reusable ThreadPool + chunked ParallelFor
  world.hpp       World, component CRUD, named entities, events, C#-style shortcuts
  system.hpp      system interface + Reads/Writes extraction
  scheduler.hpp   dependency-graph scheduler + thread pool
  ekit.hpp        unified entry point

examples/
  basic.cpp       classic system/query simulation
  ergonomic.cpp   the "write it like C#" tour
  boids/          GLFW boids case study + EnTT comparison
```
## Roadmap

- [x] Entity / Component / World core (dense archetype SoA + sparse-set storage)
- [x] Fluent Query with `Where / With / Without / Optional`
- [x] System `Reads/Writes` + parallel Scheduler
- [x] Event system (`Subscribe` / `Emit`)
- [x] Archetype chunks (SoA) + stream/batch processing
- [x] Unit tests + benchmark vs `entt`
- [ ] CMake package config (`find_package(ekit)`)

## License

[MIT](LICENSE) © 2026 chnnasn

### Owning sparse components

Dense components must be trivially copyable. Register components containing
`std::string`, containers, or resource owners with `RegisterSparseComponent<T>()`.
Sparse components are default constructible, move assignable, and not over-aligned.
Sparse storage uses paged storage: appending preserves existing component references.
Swap-and-pop removal invalidates references to the removed component and the last
component moved into its slot; clearing or destroying the world invalidates all
references. Structural mutation during query iteration is not supported.
A type cannot switch between dense and sparse storage within the same world.

### Choosing storage and reserving capacity

Keep frequently added/removed components and resource owners sparse. Dense storage
is appropriate for trivially copyable components that are iterated often and rarely
added or removed. Audit retained pointers/references before moving a component
(including Transform) to dense storage: column growth, reserve, and archetype moves
can invalidate them. Sparse storage preserves references on append and reserve,
but its swap-and-pop removal is **not** fully reference-stable.

```cpp
world.RegisterSparseComponent<Position>();
world.RegisterSparseComponent<Velocity>();
world.ReserveEntities(100000);
world.ReserveSparseComponent<Position>(100000);
world.ReserveSparseComponent<Velocity>(1000);
auto moving = world.Query<Position, Velocity>();
moving.ForEach([](Position& p, Velocity& v) { /* update */ });
```

`ReserveEntities` reserves total entity capacity and the empty archetype.
`ReserveSparseComponent<T>` reserves the pool's entity and sparse-index arrays;
the reference-stable component deque still allocates pages as needed. Call
`ReserveEntities` first to size the sparse index reservation for the entity range.
`ReserveArchetype<Ts...>(capacity)` reserves one exact dense signature; reserve
intermediate signatures as well when using successive `Add` calls.

Queries with required sparse components (including `With`) start from the smallest
required sparse pool. `Optional` and `Without` never select the driving pool.
`CandidateCount()` reports the candidates before filters. Without required sparse
components, queries scan matching archetypes. Required/optional component pointers
are resolved once per candidate and shared by `Where` and the scalar callback.

Reuse a query object to retain its pool bindings and matching archetype/column
metadata. Registration and newly created archetypes invalidate that metadata;
pool sizes and entity locations are read anew on each execution. Cached bindings
do not retain column data pointers, so reservation and relocation between calls
are supported. Do not structurally mutate the world inside a query callback or
concurrently with execution. A query must not outlive its world; concurrent calls
on the same query object require external synchronization. Iteration order is not
guaranteed. Scalar parallel queries use the same candidate selection as serial ones.

Scalar execution specializes by storage kind: dense-only queries bind column
bases once per archetype and execution, while all-sparse queries skip entity
archetype/row lookup and access the driving component directly by pool position.
Mixed queries keep their membership checks. World component access reuses the
validated type ID internally; entity generation and registration checks remain.
See the [access-path comparison](benchmarks/access_paths.md) against `3e0daa8`.

See [sparse query measurements](benchmarks/sparse_query.md) for the 1%, 10%, and
100% coverage checks and their limits.
