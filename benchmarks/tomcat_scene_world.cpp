// Compile against an unmodified SceneWorld.h exported from TomCat dev_ekit.
#include "SceneWorld.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

struct Transform { std::uint64_t value = 0; EKIT_COMPONENT(Transform); };
struct Rigidbody { std::uint64_t value = 1; EKIT_COMPONENT(Rigidbody); };

int main() {
    constexpr std::size_t total = 100000;
    constexpr std::size_t iterations = 200;
    std::cout << "run,coverage_percent,candidates,view_get_ms,checksum\n";
    for (int run = 0; run < 6; ++run) {
        for (std::size_t percent : {1u, 10u, 100u}) {
            TomCat::SceneWorld scene;
            scene.RegisterSparseComponent<Transform>();
            scene.RegisterSparseComponent<Rigidbody>();
            std::uint64_t expected = 0;
            for (std::size_t i = 0; i < total; ++i) {
                auto e = scene.Create();
                scene.Add<Transform>(e, i);
                if (i % 100 < percent) {
                    scene.Add<Rigidbody>(e, std::uint64_t{1});
                    expected += i + iterations;
                }
            }
            auto view = scene.View<Transform, Rigidbody>();
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < iterations; ++i) {
                for (auto e : view) {
                    auto [transform, body] = view.Get<Transform, Rigidbody>(e);
                    transform.value += body.value;
                }
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            const auto& read_only = scene;
            auto const_view = read_only.View<Transform, Rigidbody>();
            std::uint64_t checksum = 0;
            std::size_t count = 0;
            for (auto e : const_view) {
                checksum += const_view.Get<Transform>(e).value;
                ++count;
            }
            if (checksum != expected || count != total * percent / 100)
                throw std::runtime_error("TomCat View/Get result mismatch");
            std::cout << run << ',' << percent << ',' << view.SizeHint() << ','
                      << elapsed << ',' << checksum << '\n';
        }
    }
}
