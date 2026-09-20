#include <ekit/ekit.hpp>
#include "test_framework.hpp"
#include <atomic>
#include <vector>

namespace {
struct Common { int value = 0; EKIT_COMPONENT(Common); };
struct Rare { int value = 0; EKIT_COMPONENT(Rare); };
struct Dense { int value = 0; EKIT_COMPONENT(Dense); };
struct Optional { int value = 0; EKIT_COMPONENT(Optional); };
struct Excluded { EKIT_COMPONENT(Excluded); };
}

TEST(SparseQueryCoverageAndParallel) {
    constexpr std::size_t total = 100000;
    ekit::ThreadPool pool(2);
    for (std::size_t percent : {1u, 10u, 100u}) {
        ekit::World world;
        world.RegisterSparseComponent<Common>();
        world.RegisterSparseComponent<Rare>();
        world.ReserveEntities(total);
        world.ReserveSparseComponent<Common>(total);
        world.ReserveSparseComponent<Rare>(total * percent / 100);
        for (std::size_t i = 0; i < total; ++i) {
            auto e = world.Create();
            world.Add<Common>(e, static_cast<int>(i));
            if (i % 100 < percent) world.Add<Rare>(e, static_cast<int>(i));
        }
        auto query = world.Query<Common>().With<Rare>();
        CHECK_EQ(query.CandidateCount(), total * percent / 100);
        std::size_t visits = 0;
        query.Where([&](Common& c, Rare& r) {
            CHECK_EQ(c.value, r.value);
            ++visits;
            return true;
        }).ForEach([](Common& c, Rare& r) { ++c.value; ++r.value; });
        CHECK_EQ(visits, query.CandidateCount());
        std::atomic<std::size_t> parallel_visits{0};
        query.ForEachParallel(pool, [&](Common& c, Rare& r) {
            if (c.value == r.value) ++parallel_visits;
        });
        CHECK_EQ(parallel_visits.load(), visits);
        CHECK_EQ(query.Count(), visits);
        CHECK_EQ((world.Query<Rare, Common>().CandidateCount()), visits);
    }
}

TEST(SparseQueryCacheMutationAndReferenceStability) {
    ekit::World world;
    world.RegisterSparseComponent<Common>();
    world.RegisterSparseComponent<Rare>();
    auto query = world.Query<Common, Rare>();
    CHECK_EQ(query.Count(), 0u); // cache before any archetype exists
    auto first = world.Create();
    auto* stable = &world.Add<Common>(first, 7);
    world.Add<Rare>(first, 9);
    CHECK_EQ(query.Count(), 1u);
    world.RegisterComponent<Dense>(); // invalidate after registration
    world.ReserveEntities(4096);
    world.ReserveSparseComponent<Common>(4096);
    world.ReserveArchetype<Dense>(4096); // new archetype invalidates cache
    world.Add<Dense>(first, 11);
    for (int i = 0; i < 2048; ++i) {
        auto e = world.Create();
        world.Add<Common>(e, i);
        world.Add<Dense>(e, i);
    }
    CHECK(&world.Get<Common>(first) == stable);
    CHECK_EQ(query.CandidateCount(), 1u);
    query.ForEach([&](Common& c, Rare&) { CHECK(&c == stable); });
    world.Remove<Dense>(first);
    CHECK_EQ(query.Count(), 1u);
    auto last = world.Create();
    world.Add<Common>(last, 23);
    world.Add<Rare>(last, 42);
    world.Remove<Rare>(first); // swap-and-pop must update the driver entity array
    query.ForEach([&](ekit::Entity e, Common& c, Rare& r) {
        CHECK(e == last); CHECK_EQ(c.value, 23); CHECK_EQ(r.value, 42);
    });
    world.ClearComponent<Common>(); // switch to a new smallest pool without a version change
    CHECK_EQ(query.CandidateCount(), 0u);
    CHECK_EQ(query.Count(), 0u);
    world.Add<Common>(last, 24);
    CHECK_EQ(query.Count(), 1u);
    world.Destroy(last);
    auto reused = world.Create();
    CHECK(reused.GetIndex() == last.GetIndex());
    CHECK(reused.GetGeneration() != last.GetGeneration());
    CHECK_EQ(query.Count(), 0u);
    world.Add<Common>(reused, 25);
    world.Add<Rare>(reused, 43);
    CHECK_EQ(query.Count(), 1u);
    world.ClearAll();
    CHECK_EQ(query.Count(), 0u);
    auto fresh = world.Create();
    world.Add<Common>(fresh, 1);
    world.Add<Rare>(fresh, 2);
    CHECK_EQ(query.Count(), 1u);
}

TEST(MixedQueryBindingsAndFilters) {
    ekit::World world;
    world.RegisterSparseComponent<Common>();
    world.RegisterComponent<Dense>();
    world.RegisterSparseComponent<Optional>();
    world.RegisterSparseComponent<Excluded>();
    auto query = world.Query<Common, Dense>().Optional<Optional>().Without<Excluded>()
        .Where([](Common& c, Dense& d, Optional* o) { return c.value == d.value && (!o || o->value > 0); });
    CHECK_EQ(query.Count(), 0u);
    std::vector<ekit::Entity> entities;
    for (int i = 0; i < 100; ++i) {
        auto e = world.Create(); entities.push_back(e);
        world.Add<Common>(e, i);
        if (i % 2 == 0) world.Add<Dense>(e, i);
        if (i % 4 == 0) world.Add<Optional>(e, 1);
        if (i % 10 == 0) world.Add<Excluded>(e);
    }
    CHECK_EQ(query.Count(), 40u);
    world.ReserveArchetype<Dense>(10000); // reallocation must not stale cached pointers
    CHECK_EQ(query.Count(), 40u);
    world.Remove<Dense>(entities[2]);
    world.Add<Dense>(entities[3], 3); // existing archetype move, no cache invalidation
    CHECK_EQ(query.Count(), 40u);
    ekit::ThreadPool pool(2);
    std::atomic<std::size_t> count{0};
    query.ForEachParallel(pool, [&](Common&, Dense&, Optional*) { ++count; });
    CHECK_EQ(count.load(), 40u);
    // No required sparse pool: optional/excluded pools must not drive traversal.
    CHECK_EQ(world.Query<Dense>().Optional<Optional>().Without<Excluded>().Count(), 40u);
    CHECK_EQ(world.Query<Common>().Without<Dense>().Count(), 50u);
    CHECK_THROWS_AS(world.ReserveSparseComponent<Dense>(10), ekit::EkitException);
    CHECK_THROWS_AS(world.ReserveArchetype<Common>(10), ekit::EkitException);
}

TEST(QueryRegistrationIsStillExplicit) {
    ekit::World world;
    auto query = world.Query<Common>().Optional<Optional>();
    CHECK_THROWS_AS(query.Count(), ekit::EkitException);
    world.RegisterSparseComponent<Common>();
    CHECK_THROWS_AS(query.Count(), ekit::EkitException);
    world.RegisterSparseComponent<Optional>();
    CHECK_EQ(query.Count(), 0u);
}

TEST(SparseTypedDriverSwitchAndAliasing) {
    ekit::World world;
    world.RegisterSparseComponent<Common>();
    world.RegisterSparseComponent<Rare>();
    world.RegisterSparseComponent<Optional>();
    world.RegisterSparseComponent<Excluded>();
    std::vector<ekit::Entity> entities;
    for (int i = 0; i < 8; ++i) {
        auto e = world.Create(); entities.push_back(e);
        world.Add<Common>(e, i);
        if (i < 4) world.Add<Rare>(e, i);
        if (i % 2 == 0) world.Add<Optional>(e, i);
    }
    auto query = world.Query<Common, Rare>().Optional<Rare, Optional>().Without<Excluded>();
    auto check = [&] {
        std::size_t count = 0;
        query.ForEach([&](ekit::Entity e, Common& c, Rare& r, Rare* alias, Optional* o) {
            CHECK(&r == alias);
            CHECK_EQ(c.value, r.value);
            CHECK(o == world.TryGet<Optional>(e));
            ++count;
        });
        return count;
    };
    CHECK_EQ(check(), 4u); // Rare drives
    for (int i = 1; i < 8; ++i) world.Remove<Common>(entities[i]);
    CHECK_EQ(check(), 1u); // Common now drives, no storage version change
    world.Add<Excluded>(entities[0]);
    CHECK_EQ(check(), 0u);
    world.Remove<Excluded>(entities[0]);
    CHECK_EQ(check(), 1u);
    CHECK_EQ(world.Query<Common>().Without<Common>().Count(), 0u);
    std::size_t duplicates = 0;
    world.Query<Common, Common>().ForEach([&](Common& a, Common& b) {
        CHECK(&a == &b); ++duplicates;
    });
    CHECK_EQ(duplicates, 1u); // typed dispatch must not execute twice
    query.Visit<true>([&](ekit::Entity e, ekit::Archetype& a, std::size_t row) {
        CHECK_EQ(a.entities[row], e.GetIndex());
    });
}

TEST(DenseFastPathOptionalColumnsAndReallocation) {
    ekit::World world;
    world.RegisterComponents<Common, Rare, Optional, Excluded>();
    auto query = world.Query<Common, Rare>().Optional<Optional>().Without<Excluded>()
        .Where([](Common& c, Rare&, Optional* o) { return c.value > 0 && (!o || o->value > 0); });
    std::vector<ekit::Entity> entities;
    for (int i = 0; i < 1000; ++i) {
        auto e = world.Create(); entities.push_back(e);
        world.Add<Common>(e, i);
        world.Add<Rare>(e, i);
        if (i % 2 == 0) world.Add<Optional>(e, i);
        if (i % 5 == 0) world.Add<Excluded>(e);
    }
    CHECK_EQ(query.Count(), 800u);
    world.ReserveArchetype<Common, Rare>(10000);
    world.ReserveArchetype<Common, Rare, Optional>(10000);
    std::size_t count = 0;
    query.ForEach([&](ekit::Entity e, Common& c, Rare& r, Optional* o) {
        CHECK(&c == &world.Get<Common>(e));
        CHECK(o == world.TryGet<Optional>(e));
        CHECK_EQ(c.value, r.value); ++count;
    });
    CHECK_EQ(count, 800u);
    ekit::ThreadPool pool(2);
    std::atomic<std::size_t> parallel_count{0};
    query.ForEachParallel(pool, [&](Common&, Rare&, Optional*) { ++parallel_count; });
    CHECK_EQ(parallel_count.load(), count);
    world.Remove<Rare>(entities[1]);
    CHECK_EQ(query.Count(), 799u);
    world.Destroy(entities[2]);
    CHECK_EQ(query.Count(), 798u);
}

TEST(SparseAccessStillChecksHandlesAndRegistration) {
    ekit::World world;
    world.RegisterSparseComponent<Common>();
    const auto& read_only = world;
    auto e = world.Create();
    CHECK(world.TryGet<Common>(e) == nullptr);
    CHECK(read_only.TryGet<Common>(e) == nullptr);
    CHECK_THROWS_AS(world.Get<Common>(e), ekit::EkitException);
    CHECK_THROWS_AS(read_only.Get<Common>(e), ekit::EkitException);
    world.Set<Common>(e, 42);
    CHECK_EQ(read_only.Get<Common>(e).value, 42);
    CHECK(read_only.TryGet<Common>(e) == world.TryGet<Common>(e));
    world.Destroy(e);
    auto reused = world.Create();
    world.Add<Common>(reused, 43);
    CHECK(!world.Has<Common>(e));
    CHECK(world.TryGet<Common>(e) == nullptr);
    CHECK(read_only.TryGet<Common>(e) == nullptr);
    CHECK_THROWS_AS(world.Get<Common>(e), ekit::EkitException);
    CHECK_THROWS_AS(read_only.Get<Common>(e), ekit::EkitException);
    CHECK_THROWS_AS(world.Get<Optional>(reused), ekit::EkitException);
    CHECK_THROWS_AS(read_only.TryGet<Optional>(reused), ekit::EkitException);
}

TEST(SparseRequiredWithDenseOptionalUsesEntityLocation) {
    ekit::World world;
    world.RegisterSparseComponent<Common>();
    world.RegisterComponent<Optional>();
    auto query = world.Query<Common>().Optional<Optional>();
    CHECK_EQ(query.Count(), 0u);
    for (int i = 0; i < 10; ++i) {
        auto e = world.Create();
        world.Add<Common>(e, i);
        if (i % 2 == 0) world.Add<Optional>(e, i);
    }
    std::size_t with_optional = 0;
    query.ForEach([&](Common& c, Optional* o) {
        if (o) { CHECK_EQ(o->value, c.value); ++with_optional; }
        else CHECK(c.value % 2 != 0);
    });
    CHECK_EQ(with_optional, 5u);
}
