// ekit vs EnTT - same boids algorithm, same thread counts.
//
// Both implementations run the EXACT same flocking algorithm (identical rule
// math, same spatial grid, same phase order). Only the ECS layer differs:
// EnTT (implicit registration, views, manual parallel_for) vs ekit
// (explicit registration, fluent queries, dependency-graph scheduler).
//
// Output is organized in four groups (threads = 1..4); within each group the
// EnTT column is on the left and the ekit column on the right.
//
// EnTT is NOT part of this repository: configure with
//   -DENTT_ROOT=<path to an EnTT checkout>  (e.g. E:/Github/entt)

#include <entt/entt.hpp>

#include "boids.hpp"
#include "entt_impl.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

struct Opts {
    std::vector<int> boid_counts = {200, 1000, 5000, 10000};
    std::vector<unsigned> thread_counts = {1, 2, 3, 4};
    int steps = 30;
    int warmup = 10;
    unsigned seed = 20260810u;
    int width = 800;
    int height = 600;
};

std::vector<int> ParseIntList(const std::string& s) {
    std::vector<int> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            out.push_back(std::stoi(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        out.push_back(std::stoi(cur));
    }
    return out;
}

Opts Parse(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: missing value for %s\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--boids") {
            o.boid_counts = ParseIntList(next());
        } else if (arg == "--threads") {
            std::vector<int> ts = ParseIntList(next());
            o.thread_counts.clear();
            for (int t : ts) {
                o.thread_counts.push_back(static_cast<unsigned>(t));
            }
        } else if (arg == "--steps") {
            o.steps = std::stoi(next());
        } else if (arg == "--warmup") {
            o.warmup = std::stoi(next());
        } else if (arg == "--seed") {
            o.seed = std::stoul(next());
        } else if (arg == "--width") {
            o.width = std::stoi(next());
        } else if (arg == "--height") {
            o.height = std::stoi(next());
        } else if (arg == "--help") {
            std::printf(
                "Usage: %s [options]\n"
                "  --boids N,N,..  boid counts (default 200,1000,5000,10000)\n"
                "  --threads T,..  thread counts (default 1,2,3,4)\n"
                "  --steps N       timed steps per run (default 30)\n"
                "  --warmup N      warmup steps (default 10)\n"
                "  --seed N        random seed (default 20260810)\n"
                "  --width W       world width (default 800)\n"
                "  --height H      world height (default 600)\n",
                argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "error: unknown option %s\n", arg.c_str());
            std::exit(2);
        }
    }
    return o;
}

boids::Config MakeConfig(const Opts& o, int boid_count, unsigned threads) {
    boids::Config cfg;
    cfg.width = o.width;
    cfg.height = o.height;
    cfg.boid_count = boid_count;
    cfg.threads = threads;
    return cfg;
}

// --- ekit side -------------------------------------------------------------

double RunEkit(const Opts& o, int boid_count, unsigned threads) {
    boids::Config cfg = MakeConfig(o, boid_count, threads);

    ekit::World world;
    world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                             boids::Separation, boids::Alignment, boids::Cohesion,
                             boids::BoundsSteer, boids::MouseSteer>();
    std::mt19937 rng(o.seed);
    boids::SpawnBoids(world, cfg, rng);

    boids::SpatialGrid grid;
    grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));

    ekit::Scheduler steering(threads);
    steering.AddSystem(boids::SeparationSystem{&grid, cfg.separation_radius})
            .AddSystem(boids::AlignmentSystem{&grid, cfg.neighbor_radius})
            .AddSystem(boids::CohesionSystem{&grid, cfg.neighbor_radius})
            .AddSystem(boids::BoundsSystem{nullptr, cfg});

    ekit::Scheduler integrate(threads);
    integrate.AddSystem(boids::UpdateVelocitySystem{cfg})
             .AddSystem(boids::IntegrateSystem{cfg});

    auto step = [&] {
        grid.Build(world);
        steering.Run(world);
        integrate.Run(world);
    };
    for (int i = 0; i < o.warmup; ++i) step();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < o.steps; ++i) step();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / o.steps;
}

// --- EnTT side -------------------------------------------------------------

double RunEnTT(const Opts& o, int boid_count, unsigned threads) {
    boids::Config cfg = MakeConfig(o, boid_count, threads);

    entt::registry reg;
    std::vector<entt::entity> boids;
    std::mt19937 rng(o.seed);
    boids::SpawnEnTTBoids(reg, boids, cfg, rng);

    boids::EnTTGrid grid;
    grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));
    boids::EnTTParallel pool(threads);

    auto step = [&] { boids::StepEnTT(reg, grid, cfg, pool, boids); };
    for (int i = 0; i < o.warmup; ++i) step();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < o.steps; ++i) step();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / o.steps;
}

} // namespace

int main(int argc, char** argv) {
    const Opts o = Parse(argc, argv);

    // Sanity: both implementations must produce the exact same state.
    {
        const int count = o.boid_counts.front();
        const boids::Config cfg = MakeConfig(o, count, 1);

        ekit::World world;
        world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                                 boids::Separation, boids::Alignment, boids::Cohesion,
                                 boids::BoundsSteer, boids::MouseSteer>();
        std::mt19937 rng(o.seed);
        boids::SpawnBoids(world, cfg, rng);
        boids::SpatialGrid grid;
        grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));
        ekit::Scheduler steering(1);
        steering.AddSystem(boids::SeparationSystem{&grid, cfg.separation_radius})
                .AddSystem(boids::AlignmentSystem{&grid, cfg.neighbor_radius})
                .AddSystem(boids::CohesionSystem{&grid, cfg.neighbor_radius})
                .AddSystem(boids::BoundsSystem{nullptr, cfg});
        ekit::Scheduler integrate(1);
        integrate.AddSystem(boids::UpdateVelocitySystem{cfg}).AddSystem(boids::IntegrateSystem{cfg});
        double ekit_checksum = 0.0;
        for (int i = 0; i < o.steps + o.warmup; ++i) {
            grid.Build(world);
            steering.Run(world);
            integrate.Run(world);
        }
        world.Query<boids::Position>().ForEach(
            [&](const boids::Position& p) { ekit_checksum += p.x + p.y; });

        entt::registry reg;
        std::vector<entt::entity> boids;
        std::mt19937 rng2(o.seed);
        boids::SpawnEnTTBoids(reg, boids, cfg, rng2);
        boids::EnTTGrid egrid;
        egrid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));
        boids::EnTTParallel pool(1);
        for (int i = 0; i < o.steps + o.warmup; ++i) {
            boids::StepEnTT(reg, egrid, cfg, pool, boids);
        }
        const double entt_checksum = boids::StateChecksum(reg);
        const bool identical = ekit_checksum == entt_checksum;
        std::printf("state identical (ekit vs EnTT): %s  (sum=%.6f)\n",
                    identical ? "YES" : "NO", ekit_checksum);
        std::printf("(if NO, the two implementations diverge - check the algorithm)\n\n");
    }

    std::printf("=== ekit vs EnTT: same boids algorithm, same threads ===\n");
    std::printf("world %dx%d | seed %u | %d timed steps (+%d warmup)\n\n", o.width, o.height,
                o.seed, o.steps, o.warmup);

    for (unsigned threads : o.thread_counts) {
        std::printf("============================================================\n");
        std::printf("threads = %u\n", threads);
        std::printf("------------------------------------------------------------\n");
        std::printf("%-8s %-15s %-15s %-10s\n", "boids", "entt ms/step", "ekit ms/step",
                    "ekit/entt");
        std::printf("%-8s %-15s %-15s %-10s\n", "------", "-----------", "-----------", "--------");
        for (int count : o.boid_counts) {
            const double entt_ms = RunEnTT(o, count, threads);
            const double ekit_ms = RunEkit(o, count, threads);
            std::printf("%-8d %-15.4f %-15.4f %-10.3f\n", count, entt_ms, ekit_ms,
                        ekit_ms / entt_ms);
        }
        std::printf("\n");
    }
    return 0;
}
