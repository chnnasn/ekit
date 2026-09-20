#include <ekit/ekit.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>

struct Common { std::uint64_t value = 0; EKIT_COMPONENT(Common); };
struct Rare { std::uint64_t value = 0; EKIT_COMPONENT(Rare); };

int main() {
    constexpr std::size_t total = 100000;
    constexpr std::size_t iterations = 200;
    std::cout << "coverage_percent,entities,candidates,query_us,checksum\n";
    for (std::size_t percent : {1u, 10u, 100u}) {
        ekit::World world;
        world.RegisterSparseComponent<Common>();
        world.RegisterSparseComponent<Rare>();
#ifndef EKIT_QUERY_BASELINE
        world.ReserveEntities(total);
        world.ReserveSparseComponent<Common>(total);
        world.ReserveSparseComponent<Rare>(total * percent / 100);
#endif
        for (std::size_t i = 0; i < total; ++i) {
            auto e = world.Create();
            world.Add<Common>(e, i);
            if (i % 100 < percent) world.Add<Rare>(e, i);
        }
        auto query = world.Query<Common, Rare>().Where([](Common& c, Rare& r) {
            return c.value == r.value;
        });
        std::uint64_t checksum = 0;
        auto run = [&] { query.ForEach([&](Common& c, Rare& r) { checksum += c.value + r.value; }); };
        for (int i = 0; i < 10; ++i) run();
        checksum = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < iterations; ++i) run();
        const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
#ifdef EKIT_QUERY_BASELINE
        const auto candidates = total;
#else
        const auto candidates = query.CandidateCount();
#endif
        std::cout << percent << ',' << total << ',' << candidates << ',' << elapsed / iterations << ',' << checksum << '\n';
    }
}
