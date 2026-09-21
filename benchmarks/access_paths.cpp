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
struct Tag { std::uint64_t value = 0; EKIT_COMPONENT(Tag); };

template<typename F>
double Time(F&& fn) {
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
    std::cout << "run,storage,operation,ms,checksum\n";
    // Run 0 is warmup. Report medians of runs 1 through 5 externally.
    for (int run = first_run; run < end_run; ++run) {
        for (bool sparse : {true, false}) {
            if (argc > 2 && (std::string(argv[2]) == "sparse") != sparse) continue;
            ekit::World world;
            if (sparse) {
                world.RegisterSparseComponent<Position>();
                world.RegisterSparseComponent<Velocity>();
                world.RegisterSparseComponent<Tag>();
            } else {
                world.RegisterComponents<Position, Velocity, Tag>();
            }
            std::vector<ekit::Entity> entities;
            entities.reserve(total);
            auto report = [&](const char* operation, double ms, std::uint64_t checksum) {
                std::cout << run << ',' << (sparse ? "sparse" : "dense") << ','
                          << operation << ',' << ms << ',' << checksum << '\n';
            };
            auto elapsed = Time([&] {
                for (std::size_t i = 0; i < total; ++i) {
                    auto e = world.Create();
                    entities.push_back(e);
                    world.Add<Position>(e, i);
                    world.Add<Velocity>(e, std::uint64_t{1});
                }
            });
            report("create_two", elapsed, world.GetAliveEntityCount());
            auto query = world.Query<Position, Velocity>();
            auto sum = [&] {
                std::uint64_t result = 0;
                query.ForEach([&](Position& p, Velocity&) { result += p.value; });
                return result;
            };
            elapsed = Time([&] {
                for (int i = 0; i < 200; ++i)
                    query.ForEach([](Position& p, Velocity& v) { p.value += v.value; });
            });
            report("foreach_200", elapsed, sum());
            elapsed = Time([&] {
                for (int i = 0; i < 200; ++i)
                    world.Query<Position, Velocity>().ForEach([](Position& p, Velocity& v) { p.value += v.value; });
            });
            report("fresh_foreach_200", elapsed, sum());
            if (!sparse) {
                elapsed = Time([&] {
                    for (int i = 0; i < 200; ++i)
                        query.ForEachBatch([](Position* p, Velocity* v, std::size_t n) {
                            for (std::size_t j = 0; j < n; ++j) p[j].value += v[j].value;
                        });
                });
                report("batch_200", elapsed, sum());
            }
            std::uint64_t checksum = 0;
            elapsed = Time([&] {
                for (int i = 0; i < 10; ++i)
                    for (auto index : order) checksum += world.Get<Position>(entities[index]).value;
            });
            report("random_get_10", elapsed, checksum);
            elapsed = Time([&] {
                for (int i = 0; i < 10; ++i) {
                    for (auto e : entities) world.Add<Tag>(e, std::uint64_t{7});
                    for (auto e : entities) world.Remove<Tag>(e);
                }
            });
            report("add_remove_10", elapsed, world.Count<Tag>());
            elapsed = Time([&] { for (auto e : entities) world.Destroy(e); });
            report("destroy", elapsed, world.GetAliveEntityCount());
        }
    }
}
