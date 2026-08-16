// ekit Boids - data-parallel benchmark (headless, no rendering).
//
// Compares the original scheduler-based execution against the new data-parallel
// execution (Query::ForEachParallel + parallel spatial-grid rebuild), and
// verifies that both produce bit-identical simulation state.
//
// Usage: ekit_boids_bench_parallel [--boids 200,1000,5000] [--threads 1,2,4,0]
//                                  [--steps N] [--warmup N] [--seed N]
//                                  [--width W] [--height H]

#include "boids.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Opts {
    std::vector<int> boid_counts = {200, 1000, 5000};
    std::vector<unsigned> thread_counts = {1, 2, 4, 0}; // 0 == hardware concurrency
    int steps = 120;
    int warmup = 20;
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
                "  --boids N,N,..  boid counts (default 200,1000,5000)\n"
                "  --threads T,..  thread counts, 0 = hardware (default 1,2,4,0)\n"
                "  --steps N       timed steps per run (default 120)\n"
                "  --warmup N      warmup steps (default 20)\n"
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

struct RunResult {
    double ms = 0.0;
    std::uint64_t checksum = 0;
};

std::uint64_t PositionChecksum(ekit::World& world) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    world.Query<boids::Position, boids::BoidTag>().ForEach(
        [&](const boids::Position& p, const boids::BoidTag&) {
            auto mix = [&](float f) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                h ^= bits;
                h *= 1099511628211ull;
            };
            mix(p.x);
            mix(p.y);
        });
    return h;
}

void SetupWorld(ekit::World& world, const boids::Config& cfg, unsigned seed) {
    world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                             boids::Separation, boids::Alignment, boids::Cohesion,
                             boids::BoundsSteer, boids::MouseSteer>();
    std::mt19937 rng(seed);
    boids::SpawnBoids(world, cfg, rng);
}

boids::Config MakeConfig(int boid_count, const Opts& o) {
    boids::Config cfg;
    cfg.width = o.width;
    cfg.height = o.height;
    cfg.boid_count = boid_count;
    return cfg;
}

// Original scheduler-based execution (as shipped).
RunResult RunOriginal(int boid_count, unsigned threads, const Opts& o) {
    const boids::Config cfg = MakeConfig(boid_count, o);

    ekit::World world;
    SetupWorld(world, cfg, o.seed);

    boids::SpatialGrid grid;
    grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width),
                   static_cast<float>(cfg.height));

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

    for (int i = 0; i < o.warmup; ++i) {
        step();
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < o.steps; ++i) {
        step();
    }
    const auto t1 = std::chrono::steady_clock::now();

    RunResult r;
    r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count() /
           static_cast<double>(o.steps);
    r.checksum = PositionChecksum(world);
    return r;
}

// New data-parallel execution: systems run in dependency order, each system
// parallelizes its own query over all worker threads, and the spatial grid is
// rebuilt in parallel.
RunResult RunDataParallel(int boid_count, unsigned threads, const Opts& o) {
    const boids::Config cfg = MakeConfig(boid_count, o);
    const unsigned eff = threads == 0 ? std::thread::hardware_concurrency() : threads;

    ekit::World world;
    SetupWorld(world, cfg, o.seed);

    boids::SpatialGrid grid;
    grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width),
                   static_cast<float>(cfg.height));

    ekit::ThreadPool pool(eff);

    boids::SeparationSystem sep{&grid, cfg.separation_radius, &pool};
    boids::AlignmentSystem align{&grid, cfg.neighbor_radius, &pool};
    boids::CohesionSystem coh{&grid, cfg.neighbor_radius, &pool};
    boids::BoundsSystem bounds{nullptr, cfg, &pool};
    boids::UpdateVelocitySystem update{cfg, &pool};
    boids::IntegrateSystem integ{cfg, &pool};

    auto step = [&] {
        grid.BuildParallel(pool, world);
        sep.Execute(world);
        align.Execute(world);
        coh.Execute(world);
        bounds.Execute(world);
        update.Execute(world);
        integ.Execute(world);
    };

    for (int i = 0; i < o.warmup; ++i) {
        step();
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < o.steps; ++i) {
        step();
    }
    const auto t1 = std::chrono::steady_clock::now();

    RunResult r;
    r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count() /
           static_cast<double>(o.steps);
    r.checksum = PositionChecksum(world);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    const Opts o = Parse(argc, argv);
    const unsigned hw = std::thread::hardware_concurrency();

    std::printf("=== ekit Boids: original scheduler vs data-parallel ===\n");
    std::printf("world %dx%d | seed %u | %d timed steps (+%d warmup) | hw threads = %u\n\n",
                o.width, o.height, o.seed, o.steps, o.warmup, hw);

    for (int count : o.boid_counts) {
        // Original single-threaded baseline.
        const RunResult orig1 = RunOriginal(count, 1, o);

        std::printf("boids = %d\n", count);
        std::printf("%-9s %-9s %-11s %-10s %-9s %-10s\n", "mode", "threads", "ms/step",
                    "speedup", "k boids/s", "determin");
        std::printf("%-9s %-9s %-11s %-10s %-9s %-10s\n", "-----", "-------", "--------",
                    "-------", "---------", "--------");

        std::printf("%-9s %-9d %-11.4f %-10.2f %-9.1f %-10s\n", "orig", 1, orig1.ms,
                    1.0, static_cast<double>(count) * 1000.0 / orig1.ms / 1000.0, "-");

        const RunResult origHW = RunOriginal(count, 0, o);
        std::printf("%-9s %-9u %-11.4f %-10.2f %-9.1f %-10s\n", "orig", hw, origHW.ms,
                    orig1.ms / origHW.ms,
                    static_cast<double>(count) * 1000.0 / origHW.ms / 1000.0,
                    origHW.checksum == orig1.checksum ? "ok" : "DIFF");

        for (unsigned t : o.thread_counts) {
            const unsigned eff = t == 0 ? hw : t;
            const RunResult dp = RunDataParallel(count, t, o);
            std::printf("%-9s %-9u %-11.4f %-10.2f %-9.1f %-10s\n", "parallel", eff, dp.ms,
                        orig1.ms / dp.ms,
                        static_cast<double>(count) * 1000.0 / dp.ms / 1000.0,
                        dp.checksum == orig1.checksum ? "ok" : "DIFF");
        }
        std::printf("\n");
    }
    return 0;
}
