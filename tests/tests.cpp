// ekit test suite.
#include <ekit/ekit.hpp>

#include "test_framework.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test components
// ---------------------------------------------------------------------------

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

struct Tag {
    EKIT_COMPONENT(Tag);
};

struct NotAComponent {}; // deliberately NOT declared with EKIT_COMPONENT

// A component using the alternative declaration form (trait specialization).
struct ManualComponent {
    float value = 0.f;
    static constexpr const char* GetComponentName() noexcept { return "ManualComponent"; }
};
template<>
struct ::ekit::IsComponent<ManualComponent> : std::true_type {};

struct HitEvent {
    int damage = 0;
    ekit::Entity target;
    HitEvent() = default;
    HitEvent(int d, ekit::Entity t) : damage(d), target(t) {}
};

struct RespawnEvent {};

// ---------------------------------------------------------------------------
// Entity
// ---------------------------------------------------------------------------

TEST(entity_create_destroy_alive) {
    ekit::World world;
    ekit::Entity e = world.Create();
    CHECK(e.IsValid());
    CHECK(world.IsAlive(e));
    CHECK_EQ(world.GetAliveEntityCount(), 1u);

    world.Destroy(e);
    CHECK(!world.IsAlive(e));
    CHECK_EQ(world.GetAliveEntityCount(), 0u);
}

TEST(entity_stale_handle_detection) {
    ekit::World world;
    ekit::Entity first = world.Create();
    const ekit::EntityGeneration gen1 = first.GetGeneration();
    CHECK_EQ(gen1, 1u);

    world.Destroy(first);
    ekit::Entity second = world.Create(); // reuses the slot, bumps generation
    CHECK_EQ(second.GetGeneration(), gen1 + 1);

    CHECK(!world.IsAlive(first)); // stale handle is NOT aliased
    CHECK(world.IsAlive(second));
}

TEST(entity_destroyed_slot_is_not_alive) {
    ekit::World world;
    ekit::Entity first = world.Create();
    ekit::Entity destroyed = world.Create();
    ekit::Entity third = world.Create();

    world.Destroy(destroyed);

    CHECK(world.GetEntity(destroyed.GetIndex()) == ekit::Entity::Null);
    CHECK(!world.IsAlive(destroyed));
    CHECK_EQ(world.GetAliveEntityCount(), 2u);

    std::vector<ekit::Entity> visited;
    world.ForEachEntity([&](ekit::Entity e) { visited.push_back(e); });
    CHECK_EQ(visited.size(), 2u);
    CHECK(visited[0] == first);
    CHECK(visited[1] == third);

    ekit::Entity replacement = world.Create();
    CHECK_EQ(replacement.GetIndex(), destroyed.GetIndex());
    CHECK_EQ(replacement.GetGeneration(), destroyed.GetGeneration() + 1);
    CHECK(world.GetEntity(replacement.GetIndex()) == replacement);
    CHECK(world.IsAlive(replacement));
    CHECK(!world.IsAlive(destroyed));
}

TEST(entity_null) {
    ekit::Entity e;
    CHECK(!e.IsValid());
    CHECK(e == ekit::Entity::Null);
    CHECK(!static_cast<bool>(e));
}

TEST(entity_recycle_many) {
    ekit::World world;
    std::vector<ekit::Entity> handles;
    for (int i = 0; i < 100; ++i) {
        handles.push_back(world.Create());
    }
    for (auto h : handles) {
        world.Destroy(h);
    }
    CHECK_EQ(world.GetAliveEntityCount(), 0u);
    for (auto h : handles) {
        CHECK(!world.IsAlive(h));
    }
}

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

TEST(component_register_and_crud) {
    ekit::World world;
    const ekit::ComponentTypeId pos_id = world.RegisterComponent<Position>();
    world.RegisterComponent<Velocity>();
    world.RegisterComponent<Health>();

    CHECK(pos_id != ekit::kInvalidComponentTypeId);
    CHECK(world.IsComponentRegistered<Position>());
    CHECK_EQ(world.GetRegisteredComponentCount(), 3u);
    CHECK_EQ(world.GetComponentTypeId<Position>(), pos_id);

    ekit::Entity e = world.Create();
    world.Add<Position>(e, 1.f, 2.f);
    world.Add<Velocity>(e, 3.f, 4.f);

    CHECK(world.Has<Position>(e));
    CHECK(world.Has<Velocity>(e));
    CHECK(!world.Has<Health>(e));

    Position& p = world.Get<Position>(e);
    CHECK_EQ(p.x, 1.f);
    CHECK_EQ(p.y, 2.f);

    const Position* cp = world.TryGet<Position>(e);
    CHECK(cp != nullptr && cp->x == 1.f);
    CHECK(world.TryGet<Health>(e) == nullptr);

    world.Patch<Position>(e, [](Position& pos) { pos.x += 10.f; });
    CHECK_EQ(world.Get<Position>(e).x, 11.f);

    CHECK(world.Remove<Velocity>(e));
    CHECK(!world.Has<Velocity>(e));
    CHECK(!world.Remove<Velocity>(e));

    world.Set<Position>(e, 7.f, 8.f);
    CHECK_EQ(world.Get<Position>(e).x, 7.f);
    CHECK_EQ(world.Get<Position>(e).y, 8.f);
    world.Set<Health>(e, 50);
    CHECK(world.Has<Health>(e));
    world.Set<Health>(e, 25);
    CHECK_EQ(world.Get<Health>(e).hp, 25);
}

TEST(component_add_already_present_throws) {
    ekit::World world;
    world.RegisterComponent<Position>();
    ekit::Entity e = world.Create();
    world.Add<Position>(e, 0.f, 0.f);
    CHECK_THROWS_AS(world.Add<Position>(e, 1.f, 1.f), ekit::EkitException);
}

TEST(component_unregistered_throws) {
    ekit::World world;
    world.RegisterComponent<Position>();

    ekit::Entity e = world.Create();
    CHECK_THROWS_AS(world.Add<Velocity>(e, 0.f, 0.f), ekit::EkitException);
    CHECK_THROWS_AS(world.Get<Velocity>(e), ekit::EkitException);
    CHECK_THROWS_AS(world.GetComponentTypeId<Velocity>(), ekit::EkitException);
    CHECK(!world.IsComponentRegistered<Velocity>());
}

TEST(component_dead_entity_throws) {
    ekit::World world;
    world.RegisterComponent<Position>();
    ekit::Entity e = world.Create();
    world.Destroy(e);
    CHECK_THROWS_AS(world.Add<Position>(e, 0.f, 0.f), ekit::EkitException);
    CHECK_THROWS_AS(world.Get<Position>(e), ekit::EkitException);
    CHECK(world.TryGet<Position>(e) == nullptr);
    CHECK(!world.Has<Position>(e));
}

TEST(component_clear_and_clearall) {
    ekit::World world;
    world.RegisterComponents<Position, Health>();

    std::vector<ekit::Entity> entities;
    for (int i = 0; i < 10; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        entities.push_back(e);
    }

    world.ClearComponent<Position>();
    for (auto e : entities) {
        CHECK(!world.Has<Position>(e));
        CHECK(world.IsAlive(e));
    }

    world.ClearAll();
    CHECK_EQ(world.GetAliveEntityCount(), 0u);
    for (auto e : entities) {
        CHECK(!world.IsAlive(e));
    }
}

TEST(component_batch_registration) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health>();
    CHECK_EQ(world.GetRegisteredComponentCount(), 3u);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TEST(query_basic_for_each) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();

    for (int i = 0; i < 5; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        if (i % 2 == 0) {
            world.Add<Velocity>(e, 1.f, 0.f);
        }
    }

    int seen = 0;
    world.Query<Position, Velocity>().ForEach([&](ekit::Entity, Position& p, Velocity& v) {
        ++seen;
        p.x += v.vx;
    });
    CHECK_EQ(seen, 3); // only even indices have Velocity

    int seen2 = 0;
    world.Query<Position>().ForEach([&](Position& p) { ++seen2; });
    CHECK_EQ(seen2, 5);
}

TEST(query_where_filter) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();

    for (int i = 1; i <= 5; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        world.Add<Velocity>(e, float(i), 0.f);
    }

    int count = 0;
    float sum = 0.f;
    world.Query<Position, Velocity>()
        .Where([](Position&, Velocity& v) { return v.vx > 3.f; })
        .ForEach([&](Position& p, Velocity&) { ++count; sum += p.x; });
    CHECK_EQ(count, 2);
    CHECK_EQ(sum, 9.f); // 4 + 5

    int count2 = 0;
    world.Query<Position, Velocity>()
        .Where([](ekit::Entity e, Position&, Velocity&) { return e.GetIndex() % 2 == 0; })
        .ForEach([&](Position&, Velocity&) { ++count2; });
    CHECK_EQ(count2, 2); // even indices among 1..5

    CHECK_EQ((world.Query<Position, Velocity>().Count()), 5u);
    CHECK_EQ((world.Query<Position, Velocity>()
                 .Where([](Position&, Velocity& v) { return v.vx > 3.f; })
                 .Count()),
             2u);
}

TEST(query_with_without_optional) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health, Tag, ManualComponent>();

    for (int i = 0; i < 6; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        world.Add<Velocity>(e, 1.f, 0.f);
        if (i % 2 == 0) {
            world.Add<Health>(e, i);
        }
        if (i == 0 || i == 3) {
            world.Add<Tag>(e);
        }
    }

    int with_sum = 0;
    world.Query<Position, Velocity>()
        .With<Health>()
        .ForEach([&](Position&, Velocity&, Health& h) { with_sum += h.hp; });
    CHECK_EQ(with_sum, 0 + 2 + 4); // hp == index for even i

    int without_count = 0;
    world.Query<Position>()
        .Without<Tag>()
        .ForEach([&](Position&) { ++without_count; });
    CHECK_EQ(without_count, 4); // 6 entities minus indices 0 and 3

    int optional_count = 0;
    int optional_sum = 0;
    world.Query<Position>()
        .Optional<Health>()
        .ForEach([&](Position&, Health* h) {
            ++optional_count;
            optional_sum += h ? h->hp : -1;
        });
    CHECK_EQ(optional_count, 6);
    CHECK_EQ(optional_sum, 0 + 2 + 4 - 1 - 1 - 1);

    // Combo: With + Without + Where + Optional
    int combo = 0;
    world.Query<Position, Velocity>()
        .With<Health>()
        .Without<Tag>()
        .Optional<ManualComponent>()
        .Where([](Position& p, Velocity&, Health&, ManualComponent*) {
            return p.x > 1.f;
        })
        .ForEach([&](Position&, Velocity&, Health&, ManualComponent*) { ++combo; });
    CHECK_EQ(combo, 2); // even i (0,2,4) without Tag -> 2 and 4, both > 1
}

TEST(query_const_refs) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();

    ekit::Entity e = world.Create();
    world.Add<Position>(e, 5.f, 6.f);
    world.Add<Velocity>(e, 1.f, 1.f);

    float sum = 0.f;
    world.Query<Position, Velocity>().ForEach(
        [&](const Position& p, const Velocity& v) { sum = p.x + v.vx; });
    CHECK_EQ(sum, 6.f);
}

TEST(query_foreach_entity) {
    ekit::World world;
    world.RegisterComponent<Position>();
    for (int i = 0; i < 4; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, 0.f, 0.f);
    }
    int n = 0;
    world.ForEachEntity([&](ekit::Entity) { ++n; });
    CHECK_EQ(n, 4);
}

// ---------------------------------------------------------------------------
// Named entities
// ---------------------------------------------------------------------------

TEST(named_entities) {
    ekit::World world;
    ekit::Entity player = world.Create("player");
    ekit::Entity enemy = world.Create("enemy");

    CHECK(world.Find("player") == player);
    CHECK(world.Find("enemy") == enemy);
    CHECK(world.Find("missing") == ekit::Entity::Null);
    CHECK(world.GetName(player) == "player");

    world.SetName(player, "hero");
    CHECK(world.Find("hero") == player);
    CHECK(world.Find("player") == ekit::Entity::Null);
    CHECK(world.GetName(player) == "hero");

    world.Destroy(player);
    CHECK(world.Find("hero") == ekit::Entity::Null);
    CHECK(world.GetName(player).empty());
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

TEST(events_subscribe_emit) {
    ekit::World world;
    world.RegisterComponent<Position>();

    int hits = 0;
    int total_damage = 0;
    ekit::EventSubscription sub = world.Subscribe<HitEvent>(
        [&](const HitEvent& ev) {
            ++hits;
            total_damage += ev.damage;
        });

    ekit::Entity target = world.Create();
    world.Emit<HitEvent>(10, target);
    world.Emit<HitEvent>(5, target);
    CHECK_EQ(hits, 2);
    CHECK_EQ(total_damage, 15);

    HitEvent ev{7, target};
    world.Emit(ev);
    CHECK_EQ(hits, 3);
    CHECK_EQ(total_damage, 22);

    sub.Unsubscribe();
    world.Emit<HitEvent>(100, target);
    CHECK_EQ(hits, 3);
    CHECK(!sub.IsValid());
}

TEST(events_multiple_handlers) {
    ekit::World world;

    int first = 0;
    int second = 0;
    ekit::EventSubscription a = world.Subscribe<RespawnEvent>([&](const RespawnEvent&) { ++first; });
    ekit::EventSubscription b = world.Subscribe<RespawnEvent>([&](const RespawnEvent&) { ++second; });

    world.Emit<RespawnEvent>();
    CHECK_EQ(first, 1);
    CHECK_EQ(second, 1);

    // A handler unsubscribes another handler while an emit is in flight.
    ekit::EventSubscription c = world.Subscribe<RespawnEvent>([&](const RespawnEvent&) { a.Unsubscribe(); });
    world.Emit<RespawnEvent>();
    CHECK_EQ(first, 2); // A ran on this emit, then C unsubscribed it
    CHECK_EQ(second, 2);

    b.Unsubscribe();
    world.Emit<RespawnEvent>();
    CHECK_EQ(second, 2); // B was removed, C still alive
    (void)c;
}

TEST(events_no_subscribers_is_noop) {
    ekit::World world;
    world.Emit<RespawnEvent>();
}

// ---------------------------------------------------------------------------
// Systems & Scheduler
// ---------------------------------------------------------------------------

struct MoveSystem {
    using Reads = ekit::TypeList<Position, Velocity>;
    using Writes = ekit::TypeList<Position>;

    void Execute(ekit::World& world) {
        world.Query<Position, Velocity>().ForEach(
            [](Position& p, Velocity& v) {
                p.x += v.vx;
                p.y += v.vy;
            });
    }
};

struct GravitySystem {
    using Writes = ekit::TypeList<Velocity>;

    void Execute(ekit::World& world) {
        world.Query<Velocity>().ForEach([](Velocity& v) { v.vy -= 9.8f; });
    }
};

struct HealthBoostSystem {
    void Execute(ekit::World& world) {
        world.ForEachEntity([&](ekit::Entity e) {
            if (Health* h = world.TryGet<Health>(e)) {
                h->hp += 1;
            }
        });
    }
};

TEST(system_run_single) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();
    ekit::Entity e = world.Create();
    world.Add<Position>(e, 0.f, 0.f);
    world.Add<Velocity>(e, 2.f, 3.f);

    world.RunSystem(MoveSystem{});
    CHECK_EQ(world.Get<Position>(e).x, 2.f);
    CHECK_EQ(world.Get<Position>(e).y, 3.f);
}

TEST(scheduler_dependency_ordering) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health>();

    for (int i = 0; i < 50; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, 0.f, 0.f);
        world.Add<Velocity>(e, 1.f, 1.f);
        world.Add<Health>(e, 100);
    }

    ekit::Scheduler scheduler(2);
    scheduler.AddSystem(GravitySystem{}); // writes Velocity
    scheduler.AddSystem(MoveSystem{});    // reads Velocity, writes Position
    scheduler.AddSystem(HealthBoostSystem{});

    scheduler.Run(world);

    // Gravity MUST run before Move (Move reads what Gravity writes).
    // v after gravity: (1, -8.8); p after move: (1, -8.8).
    int correct = 0;
    world.Query<Position, Velocity>().ForEach([&](const Position& p, const Velocity& v) {
        if (p.x == v.vx && p.y == v.vy) {
            ++correct;
        }
    });
    CHECK_EQ(correct, 50);

    int boosted = 0;
    world.ForEachEntity([&](ekit::Entity e) {
        if (auto* h = world.TryGet<Health>(e)) {
            boosted += h->hp == 101 ? 1 : 0;
        }
    });
    CHECK_EQ(boosted, 50);
}

struct CounterA {
    using Writes = ekit::TypeList<Position>;
    std::atomic<int>* counter;
    void Execute(ekit::World& world) {
        world.Query<Position>().ForEach([&](Position&) { counter->fetch_add(1); });
    }
};
struct CounterB {
    using Writes = ekit::TypeList<Velocity>;
    std::atomic<int>* counter;
    void Execute(ekit::World& world) {
        world.Query<Velocity>().ForEach([&](Velocity&) { counter->fetch_add(1); });
    }
};
struct CounterC {
    using Writes = ekit::TypeList<Health>;
    std::atomic<int>* counter;
    void Execute(ekit::World& world) {
        world.Query<Health>().ForEach([&](Health&) { counter->fetch_add(1); });
    }
};

TEST(scheduler_parallel_execution) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health>();
    for (int i = 0; i < 1000; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, 0.f, 0.f);
        world.Add<Velocity>(e, 0.f, 0.f);
        world.Add<Health>(e, 1);
    }

    std::atomic<int> counter{0};
    ekit::Scheduler scheduler(4);
    scheduler.AddSystem(CounterA{&counter});
    scheduler.AddSystem(CounterB{&counter});
    scheduler.AddSystem(CounterC{&counter});

    scheduler.Run(world);
    CHECK_EQ(counter.load(), 3000);
}

// Two writers of the same component are NOT a cycle anymore: they are
// serialized in registration order. With several worker threads this would be
// nondeterministic without the dependency edges, so the recorded order proves
// the serialization.
struct WriterRecorderSystem {
    int id;
    std::vector<int>* order;
    std::mutex* mtx;
    using Writes = ekit::TypeList<Position>;
    void Execute(ekit::World&) {
        std::lock_guard<std::mutex> lock(*mtx);
        order->push_back(id);
    }
};

TEST(scheduler_two_writers_serialized_in_registration_order) {
    ekit::World world;
    world.RegisterComponent<Position>();
    for (int i = 0; i < 32; ++i) {
        world.Add<Position>(world.Create(), 0.f, 0.f);
    }

    std::vector<int> order;
    std::mutex mtx;
    ekit::Scheduler scheduler(4);
    scheduler.AddSystem(WriterRecorderSystem{0, &order, &mtx})
             .AddSystem(WriterRecorderSystem{1, &order, &mtx})
             .AddSystem(WriterRecorderSystem{2, &order, &mtx});

    scheduler.Run(world); // must NOT throw
    CHECK(order == std::vector<int>({0, 1, 2})); // registration order
}

// A genuine dependency contradiction (A writes X / reads Y, B writes Y / reads
// X) must still be reported as a cycle.
struct GenuineCycleA {
    using Reads = ekit::TypeList<Position>;
    using Writes = ekit::TypeList<Velocity>;
    void Execute(ekit::World&) {}
};
struct GenuineCycleB {
    using Reads = ekit::TypeList<Velocity>;
    using Writes = ekit::TypeList<Position>;
    void Execute(ekit::World&) {}
};

TEST(scheduler_genuine_cycle_detection) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();
    ekit::Scheduler scheduler;
    scheduler.AddSystem(GenuineCycleA{});
    scheduler.AddSystem(GenuineCycleB{});
    CHECK_THROWS_AS(scheduler.Run(world), ekit::EkitException);
}

TEST(scheduler_run_single_threaded) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();
    ekit::Entity e = world.Create();
    world.Add<Position>(e, 0.f, 0.f);
    world.Add<Velocity>(e, 2.f, 0.f);

    ekit::Scheduler scheduler;
    scheduler.AddSystem(MoveSystem{});
    scheduler.RunSingleThreaded(world);
    CHECK_EQ(world.Get<Position>(e).x, 2.f);
}

// ---------------------------------------------------------------------------
// Compile-time declarations
// ---------------------------------------------------------------------------

TEST(component_declaration_traits) {
    static_assert(!ekit::IsComponent<NotAComponent>::value, "NotAComponent must not be a component");
    static_assert(ekit::IsComponent<Position>::value, "Position must be a component");
    static_assert(ekit::IsComponent<ManualComponent>::value, "ManualComponent must be a component");
    static_assert(ekit::ComponentNameOf<Position>() == std::string_view("Position"));
}

TEST(manual_component_registration) {
    ekit::World world;
    world.RegisterComponent<ManualComponent>();
    ekit::Entity e = world.Create();
    world.Add<ManualComponent>(e, 3.5f);
    CHECK_EQ(world.Get<ManualComponent>(e).value, 3.5f);
    CHECK(world.GetComponentTypeId<ManualComponent>() != ekit::kInvalidComponentTypeId);
    CHECK(std::string_view(ekit::ComponentNameOf<ManualComponent>()) == "ManualComponent");
}

// ---------------------------------------------------------------------------
// Regression tests
// ---------------------------------------------------------------------------

// Iterating a query after entities were destroyed must only visit the alive
// ones, and the destroyed entities' components must be cleaned out of storage.
TEST(regression_query_after_destroy) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity>();

    std::vector<ekit::Entity> all;
    for (int i = 0; i < 10; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        world.Add<Velocity>(e, 1.f, 1.f);
        all.push_back(e);
    }
    for (int i = 0; i < 4; ++i) {
        world.Destroy(all[i]);
    }

    int seen = 0;
    float sum = 0.f;
    world.Query<Position, Velocity>().ForEach(
        [&](const Position& p, const Velocity&) { ++seen; sum += p.x; });
    CHECK_EQ(seen, 6);
    CHECK_EQ(sum, 4.f + 5.f + 6.f + 7.f + 8.f + 9.f);
    CHECK_EQ(world.GetStorage<Position>().Size(), 6u);
    CHECK_EQ(world.GetStorage<Velocity>().Size(), 6u);
    CHECK_EQ(world.GetAliveEntityCount(), 6u);
}

// A destroyed (freed) slot must be unreachable through every access path, and
// after the slot is recycled the stale handle must stay dead.
TEST(regression_free_slot_access) {
    ekit::World world;
    world.RegisterComponent<Position>();

    ekit::Entity e = world.Create();
    world.Add<Position>(e, 5.f, 5.f);
    const ekit::EntityId idx = e.GetIndex();
    const ekit::EntityGeneration gen = e.GetGeneration();

    world.Destroy(e);

    CHECK(world.GetEntity(idx) == ekit::Entity::Null);
    CHECK(!world.IsAlive(e));
    CHECK(!world.Has<Position>(e));
    CHECK(world.TryGet<Position>(e) == nullptr);
    CHECK(!world.Remove<Position>(e));
    CHECK_THROWS_AS(world.Get<Position>(e), ekit::EkitException);
    CHECK_THROWS_AS(world.Add<Position>(e, 1.f, 1.f), ekit::EkitException);

    // The slot is recycled with a bumped generation; the stale handle is dead.
    ekit::Entity fresh = world.Create();
    CHECK_EQ(fresh.GetIndex(), idx);
    CHECK_EQ(fresh.GetGeneration(), gen + 1);
    CHECK(world.IsAlive(fresh));
    CHECK(!world.IsAlive(e));

    world.Add<Position>(fresh, 9.f, 9.f);
    CHECK_EQ(world.Get<Position>(fresh).x, 9.f);
    CHECK(!world.Has<Position>(e));
    CHECK(world.TryGet<Position>(e) == nullptr);
    CHECK(world.GetStorage<Position>().Contains(idx));
}

// Adding/removing components during iteration:
//  * touching storages that are NOT the one being iterated is safe;
//  * removing the iterated component itself must be deferred until after the
//    loop (sparse-set swap-and-pop would invalidate the iteration).
TEST(regression_mutate_components_during_iteration) {
    ekit::World world;
    world.RegisterComponents<Position, Velocity, Health>();

    for (int i = 0; i < 5; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, float(i), 0.f);
        world.Add<Health>(e, 100);
    }

    std::vector<ekit::Entity> defer_remove;
    world.Query<Position>().ForEach([&](ekit::Entity e, Position& p) {
        // Mutating a different storage mid-iteration is safe.
        world.Add<Velocity>(e, p.x, p.x);
        if (e.GetIndex() % 2 == 0) {
            world.Remove<Health>(e);
        }
        // Removing the iterated component is deferred until after the loop.
        if (p.x > 2.f) {
            defer_remove.push_back(e);
        }
    });

    for (ekit::Entity e : defer_remove) {
        world.Remove<Position>(e);
    }

    CHECK_EQ(world.Query<Position>().Count(), 3u);
    CHECK_EQ(world.Query<Velocity>().Count(), 5u);
    CHECK_EQ(world.Query<Health>().Count(), 3u); // even indices 2 and 4 removed
}

// Wrapping the 16-bit generation counter: the wrapped slot must be permanently
// dead and new entities must be allocated at fresh indices.
TEST(regression_generation_overflow) {
    ekit::World world;

    ekit::Entity e = world.Create(); // index 1, generation 1
    const ekit::EntityId slot = e.GetIndex();
    const ekit::EntityGeneration first_gen = e.GetGeneration();

    // Cycle the same slot until its generation wraps (uint16 -> 0).
    for (int i = 0; i < 70000; ++i) {
        world.Destroy(e);
        e = world.Create();
    }

    CHECK(!world.IsAlive(ekit::Entity(slot, first_gen))); // pre-wrap handle dead
    CHECK(world.IsAlive(e));

    // The wrapped slot must never be handed out again.
    ekit::Entity a = world.Create();
    ekit::Entity b = world.Create();
    CHECK(a.GetIndex() != slot);
    CHECK(b.GetIndex() != slot);
    CHECK(world.IsAlive(a));
    CHECK(world.IsAlive(b));
}

// Unsubscribing a handler from inside its own callback must remove it for the
// next emit without disturbing other handlers or crashing mid-emit.
TEST(regression_event_unsubscribe_self_during_emit) {
    ekit::World world;

    int self_runs = 0;
    int other_runs = 0;
    ekit::EventSubscription self;
    self = world.Subscribe<RespawnEvent>([&](const RespawnEvent&) {
        ++self_runs;
        self.Unsubscribe();
    });
    ekit::EventSubscription other =
        world.Subscribe<RespawnEvent>([&](const RespawnEvent&) { ++other_runs; });

    world.Emit<RespawnEvent>();
    CHECK_EQ(self_runs, 1);
    CHECK_EQ(other_runs, 1);

    world.Emit<RespawnEvent>();
    CHECK_EQ(self_runs, 1);  // self removed itself
    CHECK_EQ(other_runs, 2); // the other handler is still active
    (void)other;
}

// A system throwing inside a parallel scheduler run must not poison the thread
// pool: the error is rethrown, and the scheduler can be run again afterwards.
struct ThrowingSystem {
    std::atomic<int>* calls;
    void Execute(ekit::World&) {
        calls->fetch_add(1);
        throw ekit::EkitException("boom");
    }
};
struct CountingSystem {
    std::atomic<int>* calls;
    void Execute(ekit::World&) { calls->fetch_add(1); }
};

TEST(regression_scheduler_recover_after_task_exception) {
    ekit::World world;
    world.RegisterComponent<Position>();
    for (int i = 0; i < 64; ++i) {
        world.Add<Position>(world.Create(), 0.f, 0.f);
    }

    std::atomic<int> throws{0};
    std::atomic<int> counts{0};

    {
        ekit::Scheduler sched(2);
        sched.AddSystem(ThrowingSystem{&throws});
        sched.AddSystem(CountingSystem{&counts});

        // Both systems run in parallel; the throwing one aborts the run.
        CHECK_THROWS_AS(sched.Run(world), ekit::EkitException);
        CHECK_EQ(throws.load(), 1);
        CHECK_EQ(counts.load(), 1);

        // The pool must remain usable: running again throws again, no deadlock.
        CHECK_THROWS_AS(sched.Run(world), ekit::EkitException);
        CHECK_EQ(throws.load(), 2);
        CHECK_EQ(counts.load(), 2);
    }

    // A fresh scheduler with only the counting system works normally.
    ekit::Scheduler sched2(2);
    sched2.AddSystem(CountingSystem{&counts});
    sched2.Run(world);
    CHECK_EQ(counts.load(), 3);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    return testfw::RunAll();
}






