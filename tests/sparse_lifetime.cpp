#include <ekit/ekit.hpp>
#include "test_framework.hpp"
#include <memory>
#include <string>
#include <vector>

struct OwnedComponent {
    std::string name;
    std::vector<int> values;
    std::shared_ptr<int> resource;
    EKIT_COMPONENT(OwnedComponent);
};
struct SparsePosition { int x = 0; EKIT_COMPONENT(SparsePosition); };

TEST(SparseOwnedLifetimeAndReferenceStability) {
    std::weak_ptr<int> weak;
    {
        ekit::World world;
        world.RegisterSparseComponent<OwnedComponent>();
        world.RegisterSparseComponent<SparsePosition>();
        auto first = world.Create();
        auto resource = std::make_shared<int>(42);
        weak = resource;
        auto& component = world.Add<OwnedComponent>(first, "first", std::vector<int>{1, 2}, resource);
        auto* address = &component;
        auto* position = &world.Add<SparsePosition>(first, 7);
        resource.reset();
        for (int i = 0; i < 4096; ++i) {
            auto e = world.Create();
            world.Add<OwnedComponent>(e, "other", std::vector<int>{i}, std::shared_ptr<int>{});
            world.Add<SparsePosition>(e, i);
        }
        CHECK(&world.Get<OwnedComponent>(first) == address);
        CHECK(&world.Get<SparsePosition>(first) == position);
        CHECK(component.name == "first");
        CHECK(component.values.size() == 2);
        CHECK(!weak.expired());
        size_t count = 0;
        world.Query<OwnedComponent, SparsePosition>().ForEach([&](OwnedComponent& c, SparsePosition&) {
            CHECK(!c.name.empty()); ++count;
        });
        CHECK(count == 4097);
        auto copy = world.Create();
        world.Set<OwnedComponent>(copy, component);
        world.Remove<OwnedComponent>(first);
        CHECK(!weak.expired());
        CHECK(world.Get<OwnedComponent>(copy).name == "first");
        world.Destroy(copy);
        CHECK(weak.expired());
        world.ClearComponent<OwnedComponent>();
        CHECK(world.Count<OwnedComponent>() == 0);
    }
    CHECK(weak.expired());
}

TEST(StorageKindCannotChange) {
    ekit::World world;
    world.RegisterSparseComponent<SparsePosition>();
    CHECK_THROWS_AS(world.RegisterComponent<SparsePosition>(), ekit::EkitException);
    ekit::World dense;
    dense.RegisterComponent<SparsePosition>();
    CHECK_THROWS_AS(dense.RegisterSparseComponent<SparsePosition>(), ekit::EkitException);
}

// Linked with tests.cpp: catches non-inline header definitions in real consumers.
const ekit::Entity* NullFromSecondTranslationUnit() { return &ekit::Entity::Null; }
