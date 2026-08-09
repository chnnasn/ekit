# Ekit

A **friendly, header-only Entity Component System (ECS)** library for C++17+.

`entt` is extremely capable but notoriously hard to approach. **Ekit** keeps the
performance benefits of a sparse-set ECS while making the API read like ordinary
C++ containers ? no template gymnastics, no steep learning curve.

## Why Ekit?

- `entt` style: `registry.view<T1, T2>().each([](auto& a, auto& b) { ... })`
  is powerful, but the template/metaprogramming noise and cryptic errors are a barrier.
- Ekit's goal: **the code you write should look like the problem you're solving.**

```cpp
// target API (design)
ekit::World world;
ekit::Entity e = world.create("player");

e.add<Position>(0.f, 0.f);
e.add<Velocity>(1.f, 0.f);

for (auto [pos, vel] : world.view<Position, Velocity>()) {
    pos.x += vel.vx * dt;          // structured bindings, container-like
}
```

## Features

- Header-only, **zero dependencies** ? drop `include/` into your project
- Sparse-set component storage (cache-friendly iteration, swap-and-pop removal)
- Entity handle with **versioning** ? dangling-handle safety for free
- Intuitive API: `create / add / get / has / remove / view`
- Structured-binding iteration over multiple components
- No macros, no `#define` magic, plain C++17

> Status: early design/implementation phase. API above is the target; expect
> incremental progress toward it.

## Quick start

1. Copy the `include/` folder into your project (or add it as a submodule).
2. `#include <ekit/World.hpp>`
3. Write components as plain structs:

```cpp
#include <ekit/World.hpp>

struct Position { float x = 0.f, y = 0.f; };
struct Velocity { float vx = 0.f, vy = 0.f; };

int main() {
    ekit::World world;

    auto e = world.create("ship");
    e.add<Position>(10.f, 5.f);
    e.add<Velocity>(2.f, 0.f);

    float dt = 1.f / 60.f;
    for (auto [pos, vel] : world.view<Position, Velocity>()) {
        pos.x += vel.vx * dt;
        pos.y += vel.vy * dt;
    }
}
```

## Integration

- **Header-only**: just add `include/` to your compiler include path.
- **CMake**: `add_subdirectory(ekit)` then `target_link_libraries(app PRIVATE ekit)` (planned).
- **premake5**: `includedirs { "vendor/ekit/include" }`.

## Roadmap

- [x] Repository & docs
- [ ] Core: `World` / `Entity` / `SparseSet` / `View`
- [ ] Component registration & `add/emplace/get/has/remove`
- [ ] Structured-binding iteration (`view<Ts...>()`)
- [ ] Named entity lookup + component tags
- [ ] Unit tests (doctest/catch2) + benchmark vs `entt`
- [ ] CMake package config (`find_package(ekit)`)

## License

[MIT](LICENSE) ? 2026 chnnasn
