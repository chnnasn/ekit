// ekit Boids - simulation benchmark (headless, no rendering).
//
// Measures the full per-step cost of the flocking simulation:
//   grid rebuild (sync) -> phase 1 rules (parallel) -> phase 2 apply+integrate
// across a matrix of boid counts x thread counts, and reports the parallel
// speedup versus a single-threaded run.
//
// Usage: ekit_boids_bench [--boids 200,500,1000,...] [--threads 1,2,4,0]
//                         [--steps N] [--warmup N] [--seed N]
//                         [--width W] [--height H]

#include "boids.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Opts {
    std::vector<int> boid_counts = {200, 500, 1000, 2000, 5000, 10000};
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
                "  --boids N,N,..  boid counts (default 200,500,1000,2000,5000,10000)\n"
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

// Runs `steps` full simulation steps for a fresh world of `boid_count` boids
// and returns the average time per step in milliseconds.
double RunOne(int boid_count, unsigned threads, const Opts& o) {
    boids::Config cfg;
    cfg.width = o.width;
    cfg.height = o.height;
    cfg.boid_count = boid_count;
    cfg.threads = threads;

    ekit::World world;
    world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                             boids::Separation, boids::Alignment, boids::Cohesion,
                             boids::BoundsSteer, boids::MouseSteer>();
    std::mt19937 rng(o.seed);
    boids::SpawnBoids(world, cfg, rng);

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

    const double total_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    return total_ms / static_cast<double>(o.steps);
}

} // namespace

int main(int argc, char** argv) {
    const Opts o = Parse(argc, argv);
    const unsigned hw = std::thread::hardware_concurrency();

    std::printf("=== ekit Boids benchmark ===\n");
    std::printf("world %dx%d | seed %u | %d timed steps (+%d warmup) | hw threads = %u\n\n",
                o.width, o.height, o.seed, o.steps, o.warmup, hw);
    std::printf("%-8s %-9s %-11s %-10s %-9s %-11s %-10s\n", "boids", "threads", "ms/step",
                "steps/s", "speedup", "k boids/s", "us/boid");
    std::printf("%-8s %-9s %-11s %-10s %-9s %-11s %-10s\n", "------", "-------", "--------",
                "-------", "-------", "---------", "--------");

    double best_boids_per_us = 0.0;
    int best_count = 0;
    unsigned best_threads = 0;

    for (int count : o.boid_counts) {
        double baseline_ms = 0.0; // threads == 1
        bool first = true;
        for (unsigned t : o.thread_counts) {
            const unsigned eff = t == 0 ? hw : t;
            const double ms = RunOne(count, t, o);
            const double steps_s = 1000.0 / ms;
            const double speedup = first ? 1.0 : baseline_ms / ms;
            const double k_boids_s = static_cast<double>(count) * steps_s / 1000.0;
            const double us_per_boid = ms * 1000.0 / static_cast<double>(count);
            const double boids_per_us = 1.0 / us_per_boid;
            std::printf("%-8d %-9u %-11.4f %-10.1f %-9.2f %-11.1f %-10.3f\n", count, eff, ms,
                        steps_s, speedup, k_boids_s, us_per_boid);
            if (boids_per_us > best_boids_per_us) {
                best_boids_per_us = boids_per_us;
                best_count = count;
                best_threads = eff;
            }
            if (first) {
                baseline_ms = ms;
                first = false;
            }
        }
        std::printf("\n");
    }

    std::printf("single-boid throughput (best): %.3f boids/us (%.3f us/boid) "
                "at %d boids, %u threads\n",
                best_boids_per_us, 1.0 / best_boids_per_us, best_count, best_threads);
    return 0;
}
