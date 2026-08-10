# Ekit

[English](README.md) | [简体中文](README.zh-CN.md)

**Ekit** is a friendly, header-only, **C++20 ECS** (Entity-Component-System) library
for game engines. Most ECS libraries fall into two camps: powerful but hard to use
(deep template metaprogramming, cryptic compile errors), or simple but slow. Ekit is
designed to escape that trade-off - sparse-set performance with an API that reads like
C# LINQ + Unity DOTS: explicit, fluent, and zero-overhead. So you spend your time on
game logic, not plumbing.

[EnTT](https://github.com/skypjack/entt) is a battle-tested, feature-rich ECS library.
Ekit is a smaller, deliberately ergonomic alternative focused on a friendly API and a
lean feature set - pick whichever fits your project.

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

3. **Extremely caller-friendly**
   - No template error storms: `static_assert` and `if constexpr` produce precise errors.
   - Entities are strong-typed, generation-based handles (never bare `uint32_t`).
   - Zero-overhead: the fluent query chain is composed at compile time (no type erasure,
     no per-entity virtual calls).

4. **Architecture cornerstone**
   - Sparse-set storage, a dependency-aware parallel scheduler, named entities, and an
     event system provide a solid base for declarative auto-parallelization, editor
     integration, and network sync.

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

## Features

- **Entity** — strong-typed, generation-based handle with dangling-handle safety
  (`Entity::Null`, `IsAlive`, automatic slot recycling with generation bumps).
- **Component** — POD structs declaring `EKIT_COMPONENT(T)` inside the class body; explicit
  `world.RegisterComponent<T>()`; sparse-set storage (cache-friendly dense arrays,
  swap-and-pop removal).
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
  A writer is ordered before every reader/writer of the same component; two writers of
  the same component are a genuine conflict and are reported as a dependency cycle.
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

`examples/boids/` is a complete flocking simulation built on ekit. It showcases
explicit component registration, fluent queries, systems with `Reads/Writes`
declarations, the parallel scheduler (four boid-rule systems run concurrently),
spatial-hash neighbor queries, and a classic two-phase frame pipeline:

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

## Building & testing

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

## Project layout

```
include/ekit/
  core.hpp        exceptions, TypeList, type ids
  entity.hpp      Entity (generation-based handle)
  component.hpp   EKIT_COMPONENT, ComponentStorage (sparse set)
  query.hpp       fluent Query (Where / With / Without / Optional / ForEach)
  world.hpp       World, component CRUD, named entities, events
  system.hpp      system interface + Reads/Writes extraction
  scheduler.hpp   dependency-graph scheduler + thread pool
  ekit.hpp        unified entry point
```

## Roadmap

- [x] Entity / Component / World core (sparse-set storage)
- [x] Fluent Query with `Where / With / Without / Optional`
- [x] System `Reads/Writes` + parallel Scheduler
- [x] Event system (`Subscribe` / `Emit`)
- [ ] Archetype chunks (SoA) as the next storage tier
- [ ] Unit-test / benchmark vs `entt`
- [ ] CMake package config (`find_package(ekit)`)

## License

[MIT](LICENSE) © 2026 chnnasn
