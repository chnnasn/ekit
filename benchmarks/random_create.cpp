#include <ekit/ekit.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

struct Position { std::uint64_t value = 0; EKIT_COMPONENT(Position); };
struct Velocity { std::uint64_t value = 1; EKIT_COMPONENT(Velocity); };

template<typename F> double Time(F&& fn) {
    const auto begin = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

int main(int argc, char** argv) {
    const int first_run = argc > 1 ? std::stoi(argv[1]) : 0;
    const int end_run = argc > 1 ? first_run + 1 : 6;
    if (first_run < 0 || first_run > 5) return 1;
    constexpr std::size_t total = 100000;
    std::vector<std::size_t> order(total);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 random(20260921);
    std::shuffle(order.begin(), order.end(), random);
    std::cout << "run,reserved,operation,ms,checksum\n";
    for (int run = first_run; run < end_run; ++run) {
        for (bool reserved : {false, true}) {
            ekit::World world;
            world.RegisterSparseComponent<Position>();
            world.RegisterSparseComponent<Velocity>();
            std::vector<ekit::Entity> entities;
            entities.reserve(total);
            auto report = [&](const char* op, double ms, std::uint64_t checksum) {
                std::cout << run << ',' << reserved << ',' << op << ',' << ms << ',' << checksum << '\n';
            };
            auto reserve_ms = Time([&] {
                if (reserved) {
                    world.ReserveEntities(total);
                    world.ReserveSparseComponent<Position>(total);
                    world.ReserveSparseComponent<Velocity>(total);
                }
            });
            report("reserve", reserve_ms, 0);
            auto create_ms = Time([&] { for (std::size_t i = 0; i < total; ++i) entities.push_back(world.Create()); });
            report("create_only", create_ms, world.GetAliveEntityCount());
            auto add_ms = Time([&] { for (std::size_t i = 0; i < total; ++i) world.Add<Position>(entities[i], i); });
            report("add_first", add_ms, world.Count<Position>());
            auto second_ms = Time([&] { for (auto e : entities) world.Add<Velocity>(e, std::uint64_t{1}); });
            report("add_second", second_ms, world.Count<Velocity>());
            report("setup_including_reserve", reserve_ms + create_ms + add_ms + second_ms, total);
            auto& storage = world.GetSparseStorage<Position>();
            for (int mode = 0; mode < 4; ++mode) {
                std::uint64_t checksum = 0;
                auto ms = Time([&] {
                    for (int pass = 0; pass < 10; ++pass) {
                        for (auto i : order) {
                            if (mode == 0) checksum += world.IsAlive(entities[i]);
                            if (mode == 1) checksum += world.Get<Position>(entities[i]).value;
                            if (mode == 2) checksum += storage.Get(entities[i].GetIndex()).value;
                            if (mode == 3) checksum += storage.ComponentAt(i).value;
                        }
                    }
                });
                const char* names[] = {"random_alive_10", "random_world_get_10", "random_pool_get_10", "random_component_at_10"};
                report(names[mode], ms, checksum);
            }
        }
    }
}
