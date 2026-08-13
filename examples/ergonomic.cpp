// ekit example: the "write it like C#" tour.
//
// This file shows the everyday ergonomic surface of ekit:
//   - explicit component registration
//   - world.ForEach<T...>(...), world.Count<T...>()
//   - the full fluent Query (Where / With / Without / Optional)
//   - ForEachParallel / ForEachBatch / ForEachBatchParallel
//   - ScratchSoa (collect-then-batch stream processing)
//   - systems + scheduler, and events
#include <ekit/ekit.hpp>

#include <cstdio>

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

struct Health {
    int hp = 100;
    EKIT_COMPONENT(Health);
};

struct Renderable {
    int mesh = 0;
    EKIT_COMPONENT(Renderable);
};

struct Disabled {
    EKIT_COMPONENT(Disabled);
};

struct DamageEvent {
    ekit::Entity target;
    int amount = 0;
};

struct GravitySystem {
    using Writes = ekit::TypeList<Velocity>;

    void Execute(ekit::World& world) {
        world.ForEach<Velocity>([](Velocity& v) { v.vy -= 9.8f; });
    }
};

struct MoveSystem {
    using Reads = ekit::TypeList<Position, Velocity>;
    using Writes = ekit::TypeList<Position>;

    void Execute(ekit::World& world) {
        const float dt = 1.f / 60.f;
        world.ForEach<Position, Velocity>(
            [dt](Position& p, Velocity& v) {
                p.x += v.vx * dt;
                p.y += v.vy * dt;
            });
    }
};

int main() {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health, Renderable, Disabled>();

    ekit::Entity player = world.Create("player");
    world.Add<Position>(player, 0.f, 0.f);
    world.Add<Velocity>(player, 3.f, 5.f);
    world.Add<Health>(player, 50);
    world.Add<Renderable>(player, 42);

    for (int i = 0; i < 100; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, static_cast<float>(i), 0.f);
        world.Add<Velocity>(e, 0.5f, 0.25f);
        world.Add<Health>(e, 100 - i % 20);
        if (i % 5 == 0) {
            world.Add<Disabled>(e);
        }
    }

    // --- C#-style shortcuts ------------------------------------------------
    const std::size_t movers = world.Count<Position, Velocity>();
    std::printf("movers (Position+Velocity): %zu\n", movers);

    world.ForEach<Position, Velocity>([](Position& p, Velocity& v) {
        p.x += v.vx;
        p.y += v.vy;
    });

    // --- full fluent query -------------------------------------------------
    std::size_t living = 0;
    world.Query<Position, Velocity>()
         .With<Health>()
         .Without<Disabled>()
         .Where([](Position&, Velocity&, Health& h) { return h.hp > 0; })
         .ForEach([&](ekit::Entity, Position&, Velocity&, Health&) { ++living; });
    std::printf("living movers (With/Without/Where): %zu\n", living);

    // --- data-parallel ------------------------------------------------------
    ekit::ThreadPool pool(0); // 0 == hardware concurrency
    world.ForEachParallel<Position, Velocity>(pool, [](Position& p, Velocity& v) {
        p.x += v.vx;
        p.y += v.vy;
    });

    // --- SoA batch ----------------------------------------------------------
    world.ForEachBatch<Position, Velocity>([](Position* p, Velocity* v, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            p[i].x += v[i].vx;
            p[i].y += v[i].vy;
        }
    });

    // --- stream processing: collect scattered data, then batch-consume ------
    ekit::ScratchSoa<ekit::Entity, Position, Velocity> stream;
    stream.Reserve(world.Count<Position, Velocity>());
    world.ForEach<Position, Velocity>([&](ekit::Entity e, Position& p, Velocity& v) {
        stream.Append(e, p, v);
    });
    stream.ForEachBatchParallel(
        pool,
        [](ekit::Entity* ids, Position* p, Velocity* v, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                (void)ids[i];
                p[i].x += v[i].vx;
                p[i].y += v[i].vy;
            }
        },
        64);

    // --- systems + scheduler ------------------------------------------------
    ekit::Scheduler scheduler(4);
    scheduler.AddSystem(GravitySystem{}).AddSystem(MoveSystem{});
    for (int frame = 0; frame < 60; ++frame) {
        scheduler.Run(world);
    }

    // --- events -------------------------------------------------------------
    auto sub = world.Subscribe<DamageEvent>([](const DamageEvent& evt) {
        std::printf("damage: entity slot %u took %d\n",
                    static_cast<unsigned>(evt.target.GetIndex()), evt.amount);
    });
    world.Emit<DamageEvent>(player, 12);
    sub.Unsubscribe();

    std::printf("player pos=(%.2f, %.2f) hp=%d alive=%zu\n",
                world.Get<Position>(player).x, world.Get<Position>(player).y,
                world.Get<Health>(player).hp, world.GetAliveEntityCount());
    return 0;
}