#include <ekit/ekit.hpp>
#include "test_framework.hpp"
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Tracked {
    inline static int live = 0;
    inline static int copies_before_throw = -1;
    inline static bool fail = false;
    int value = 0;
    std::string text;
    Tracked() { ++live; }
    explicit Tracked(int v) : value(v), text(std::to_string(v)) {
        if (fail) throw std::runtime_error("construction failure");
        ++live;
    }
    Tracked(const Tracked& other) : value(other.value), text(other.text) {
        if (copies_before_throw == 0) throw std::runtime_error("copy failure");
        if (copies_before_throw > 0) --copies_before_throw;
        ++live;
    }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --live; }
    EKIT_COMPONENT(Tracked);
};
struct Large {
    std::array<std::byte, 5000> bytes{};
    std::unique_ptr<int> resource;
    EKIT_COMPONENT(Large);
};
struct alignas(std::max_align_t) Aligned {
    std::uint64_t value = 0;
    EKIT_COMPONENT(Aligned);
};
}

TEST(PagedStorageReserveGrowthAndFailure) {
    CHECK_EQ(Tracked::live, 0);
    {
        ekit::ComponentStorage<Tracked> storage;
        storage.Reserve(3000, 6000);
        CHECK_EQ(Tracked::live, 0); // raw pages must not default-construct objects
        std::vector<Tracked*> addresses;
        for (std::uint32_t i = 1; i <= 3000; ++i)
            addresses.push_back(&storage.Emplace(i, static_cast<int>(i)));
        storage.Reserve(7000, 10000);
        for (std::uint32_t i = 3001; i <= 7000; ++i)
            storage.Emplace(i, static_cast<int>(i));
        for (std::uint32_t i = 1; i <= addresses.size(); ++i) {
            CHECK(storage.TryGet(i) == addresses[i - 1]);
            CHECK_EQ(storage.Get(i).value, static_cast<int>(i));
        }
        const auto before = storage.Size();
        Tracked::fail = true;
        CHECK_THROWS_AS(storage.Emplace(9000, 9000), std::runtime_error);
        Tracked::fail = false;
        CHECK_EQ(storage.Size(), before);
        CHECK(!storage.Contains(9000));
        CHECK_EQ(Tracked::live, static_cast<int>(before));
        storage.Emplace(9000, 9000);
        CHECK(storage.TryRemove(1));
        CHECK_EQ(storage.Get(9000).value, 9000); // moved last entry must update sparse index
        CHECK(!storage.Contains(1));
        CHECK_EQ(storage.EntityAt(0), 9000u);
        storage.Clear();
        CHECK_EQ(Tracked::live, 0);
        CHECK_EQ(storage.Size(), 0u);
        CHECK(!storage.Contains(9000));
        storage.Emplace(1, 42);
        CHECK_EQ(storage.Get(1).value, 42);
    }
    CHECK_EQ(Tracked::live, 0);
    // Cross multiple unreserved page boundaries; failure leaves no live object.
    {
        ekit::ComponentStorage<Tracked> storage;
        for (std::uint32_t i = 1; i <= 300; ++i) {
            Tracked::fail = true;
            CHECK_THROWS_AS(storage.Emplace(i, static_cast<int>(i)), std::runtime_error);
            Tracked::fail = false;
            CHECK_EQ(storage.Size(), i - 1u);
            storage.Emplace(i, static_cast<int>(i));
        }
    }
    CHECK_EQ(Tracked::live, 0);
}

TEST(PagedStorageCopyMoveAndCopyFailure) {
    {
        ekit::ComponentStorage<Tracked> original;
        for (std::uint32_t i = 1; i <= 300; ++i) original.Emplace(i, static_cast<int>(i));
        auto copy = original;
        CHECK_EQ(Tracked::live, 600);
        CHECK(&copy.Get(1) != &original.Get(1));
        copy.Get(1).value = 99;
        CHECK_EQ(original.Get(1).value, 1);
        auto moved = std::move(copy);
        CHECK_EQ(moved.Get(1).value, 99);
        CHECK_EQ(copy.Size(), 0u);
        ekit::ComponentStorage<Tracked> assigned;
        assigned.Emplace(1, 7);
        Tracked::copies_before_throw = 5;
        CHECK_THROWS_AS(assigned = original, std::runtime_error);
        Tracked::copies_before_throw = -1;
        CHECK_EQ(assigned.Get(1).value, 7);
        CHECK_EQ(Tracked::live, 601);
        assigned = original;
        CHECK_EQ(assigned.Size(), 300u);
        assigned = std::move(moved);
        CHECK_EQ(assigned.Get(1).value, 99);
        CHECK_EQ(Tracked::live, 600);
    }
    CHECK_EQ(Tracked::live, 0);
}

TEST(PagedStorageLargeOwningAndAlignedComponents) {
    ekit::World world;
    world.RegisterSparseComponent<Large>();
    world.RegisterSparseComponent<Aligned>();
    world.ReserveEntities(200);
    world.ReserveSparseComponent<Large>(200);
    std::vector<ekit::Entity> entities;
    for (std::size_t i = 0; i < 200; ++i) {
        auto e = world.Create(); entities.push_back(e);
        auto& large = world.Add<Large>(e);
        large.resource = std::make_unique<int>(static_cast<int>(i));
        auto& aligned = world.Add<Aligned>(e, static_cast<std::uint64_t>(i));
        CHECK(reinterpret_cast<std::uintptr_t>(&aligned) % alignof(Aligned) == 0);
    }
    world.Remove<Large>(entities[0]);
    CHECK_EQ(*world.Get<Large>(entities.back()).resource, 199);
    world.Query<Large, Aligned>().ForEach([](Large& large, Aligned& aligned) {
        CHECK_EQ(*large.resource, static_cast<int>(aligned.value));
    });
    world.ClearAll();
    CHECK_EQ(world.GetAliveEntityCount(), 0u);
    auto fresh = world.Create(); // cached empty archetype survives ClearAll
    world.Add<Aligned>(fresh, std::uint64_t{42});
    CHECK_EQ(world.Get<Aligned>(fresh).value, 42u);
}

TEST(DeadSlotsRejectPredictedGenerationHandles) {
    ekit::World world;
    world.RegisterSparseComponent<Aligned>();
    auto e = world.Create();
    world.Add<Aligned>(e, std::uint64_t{9});
    world.Destroy(e);
    ekit::Entity future(e.GetIndex(), e.GetGeneration() + 1);
    ekit::Entity marker(0, e.GetGeneration() + 1);
    for (auto invalid : {e, future, marker, ekit::Entity::Null}) {
        CHECK(!world.IsAlive(invalid));
        CHECK(world.TryGet<Aligned>(invalid) == nullptr);
        CHECK_THROWS_AS(world.Get<Aligned>(invalid), ekit::EkitException);
        world.Destroy(invalid);
    }
    CHECK(world.GetEntity(e.GetIndex()) == ekit::Entity::Null);
    auto reused = world.Create();
    CHECK(reused == future);
    CHECK(world.IsAlive(reused));
    CHECK(!world.IsAlive(e));
    world.Add<Aligned>(reused, std::uint64_t{10});
    CHECK_EQ(world.Get<Aligned>(reused).value, 10u);
}
