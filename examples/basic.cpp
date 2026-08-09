// ekit example: a tiny moving-entity simulation using queries and the scheduler.
#include <ekit/ekit.hpp>

#include <cstdio>

struct Position { float x = 0.f, y = 0.f; };
EKIT_COMPONENT(Position);

struct Velocity { float vx = 0.f, vy = 0.f; };
EKIT_COMPONENT(Velocity);

struct Health { int hp = 100; };
EKIT_COMPONENT(Health);

// Reads nothing, writes Velocity (applies gravity).
struct GravitySystem {
    using Writes = ekit::TypeList<Velocity>;

    void Execute(ekit::World& world) {
        world.Query<Velocity>().ForEach([](Velocity& v) { v.vy -= 9.8f; });
    }
};

// Reads Position + Velocity, writes Position (integrates motion).
struct MoveSystem {
    using Reads = ekit::TypeList<Position, Velocity>;
    using Writes = ekit::TypeList<Position>;

    void Execute(ekit::World& world) {
        const float dt = 1.f / 60.f;
        world.Query<Position, Velocity>().ForEach(
            [dt](Position& p, Velocity& v) {
                p.x += v.vx * dt;
                p.y += v.vy * dt;
            });
    }
};

// Reads/writes nothing declared; boosts health of tagged entities.
struct HealSystem {
    void Execute(ekit::World& world) {
        world.ForEachEntity([&](ekit::Entity e) {
            if (Health* h = world.TryGet<Health>(e)) {
                h->hp = h->hp < 100 ? h->hp + 1 : h->hp;
            }
        });
    }
};

int main() {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health>();

    ekit::Entity player = world.Create("player");
    world.Add<Position>(player, 0.f, 0.f);
    world.Add<Velocity>(player, 3.f, 5.f);
    world.Add<Health>(player, 50);

    for (int i = 0; i < 100; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        world.Add<Velocity>(e, 0.5f, 0.25f);
    }

    ekit::Scheduler scheduler(4);
    scheduler.AddSystem(GravitySystem{})
             .AddSystem(MoveSystem{})
             .AddSystem(HealSystem{});

    for (int frame = 0; frame < 60; ++frame) {
        scheduler.Run(world);
    }

    const Position& p = world.Get<Position>(player);
    const Velocity& v = world.Get<Velocity>(player);
    const Health& h = world.Get<Health>(player);
    std::printf("player: pos=(%.2f, %.2f) vel=(%.2f, %.2f) hp=%d alive=%zu\n",
                p.x, p.y, v.vx, v.vy, h.hp, world.GetAliveEntityCount());
    std::printf("entities with Position+Velocity: %zu\n",
                world.Query<Position, Velocity>().Count());
    return 0;
}
