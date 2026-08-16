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

// --- ekit data-parallel side ------------------------------------------------
// Same algorithm and same phase structure as EnTT (grid build serial, the four
// rule passes parallel, apply+integrate serial); only the ECS parallel
// mechanism differs: ekit uses Query::ForEachParallel instead of a hand-rolled
// pool + views.

double RunEkitDataParallel(const Opts& o, int boid_count, unsigned threads) {
    boids::Config cfg = MakeConfig(o, boid_count, threads);

    ekit::World world;
    world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                             boids::Separation, boids::Alignment, boids::Cohesion,
                             boids::BoundsSteer, boids::MouseSteer>();
    std::mt19937 rng(o.seed);
    boids::SpawnBoids(world, cfg, rng);

    boids::SpatialGrid grid;
    grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));

    ekit::ThreadPool pool(threads);

    auto step = [&] {
        grid.Build(world); // serial, same as the EnTT side

        world.Query<boids::Position, boids::Separation>().ForEachParallel(
            pool, [&](ekit::Entity e, boids::Position& p, boids::Separation& s) {
                float sx = 0.f;
                float sy = 0.f;
                grid.ForEachNeighbor(world, p, cfg.separation_radius, [&](ekit::Entity n) {
                    if (n == e) {
                        return;
                    }
                    const boids::Position* np = world.TryGet<boids::Position>(n);
                    if (np == nullptr) {
                        return;
                    }
                    const float dx = p.x - np->x;
                    const float dy = p.y - np->y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < cfg.separation_radius * cfg.separation_radius && d2 > 1e-4f) {
                        const float d = std::sqrt(d2);
                        const float strength = 1.f - d / cfg.separation_radius;
                        sx += (dx / d) * strength;
                        sy += (dy / d) * strength;
                    }
                });
                s.x = sx;
                s.y = sy;
            });

        world.Query<boids::Position, boids::Velocity, boids::Alignment>().ForEachParallel(
            pool, [&](ekit::Entity e, boids::Position& p, boids::Velocity&, boids::Alignment& a) {
                float ax = 0.f;
                float ay = 0.f;
                int count = 0;
                grid.ForEachNeighbor(world, p, cfg.neighbor_radius, [&](ekit::Entity n) {
                    if (n == e) {
                        return;
                    }
                    const boids::Velocity* nv = world.TryGet<boids::Velocity>(n);
                    if (nv == nullptr) {
                        return;
                    }
                    ax += nv->x;
                    ay += nv->y;
                    ++count;
                });
                if (count > 0) {
                    a.x = ax / static_cast<float>(count);
                    a.y = ay / static_cast<float>(count);
                } else {
                    a.x = 0.f;
                    a.y = 0.f;
                }
            });

        world.Query<boids::Position, boids::Cohesion>().ForEachParallel(
            pool, [&](ekit::Entity e, boids::Position& p, boids::Cohesion& c) {
                float cx = 0.f;
                float cy = 0.f;
                int count = 0;
                grid.ForEachNeighbor(world, p, cfg.neighbor_radius, [&](ekit::Entity n) {
                    if (n == e) {
                        return;
                    }
                    const boids::Position* np = world.TryGet<boids::Position>(n);
                    if (np == nullptr) {
                        return;
                    }
                    cx += np->x;
                    cy += np->y;
                    ++count;
                });
                if (count > 0) {
                    c.x = (cx / static_cast<float>(count)) - p.x;
                    c.y = (cy / static_cast<float>(count)) - p.y;
                } else {
                    c.x = 0.f;
                    c.y = 0.f;
                }
            });

        world.Query<boids::Position, boids::BoundsSteer>().ForEachParallel(
            pool, [&](ekit::Entity, boids::Position& p, boids::BoundsSteer& b) {
                const float world_w = static_cast<float>(cfg.width);
                const float world_h = static_cast<float>(cfg.height);
                float sx = 0.f;
                float sy = 0.f;
                if (p.x < cfg.edge_margin) {
                    sx = (cfg.edge_margin - p.x) / cfg.edge_margin;
                } else if (p.x > world_w - cfg.edge_margin) {
                    sx = (world_w - cfg.edge_margin - p.x) / cfg.edge_margin;
                }
                if (p.y < cfg.edge_margin) {
                    sy = (cfg.edge_margin - p.y) / cfg.edge_margin;
                } else if (p.y > world_h - cfg.edge_margin) {
                    sy = (world_h - cfg.edge_margin - p.y) / cfg.edge_margin;
                }
                b.x = sx;
                b.y = sy;
            });

        world.Query<boids::Velocity, boids::Separation, boids::Alignment, boids::Cohesion,
                    boids::BoundsSteer>()
            .ForEach([&](ekit::Entity, boids::Velocity& v, const boids::Separation& s,
                         const boids::Alignment& a, const boids::Cohesion& c,
                         const boids::BoundsSteer& b) {
                v.x += s.x * cfg.separation_weight + a.x * cfg.alignment_weight +
                       c.x * cfg.cohesion_weight + b.x * cfg.edge_steer;
                v.y += s.y * cfg.separation_weight + a.y * cfg.alignment_weight +
                       c.y * cfg.cohesion_weight + b.y * cfg.edge_steer;
                const float speed = std::sqrt(v.x * v.x + v.y * v.y);
                if (speed > cfg.max_speed) {
                    v.x *= cfg.max_speed / speed;
                    v.y *= cfg.max_speed / speed;
                } else if (speed < cfg.min_speed && speed > 1e-4f) {
                    v.x *= cfg.min_speed / speed;
                    v.y *= cfg.min_speed / speed;
                }
            });

        world.Query<boids::Position, boids::Velocity>().ForEach(
            [&](ekit::Entity, boids::Position& p, const boids::Velocity& v) {
                p.x += v.x * cfg.dt;
                p.y += v.y * cfg.dt;
            });
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

    auto step = [&] { boids::StepEnTT(reg, grid, cfg, pool); };
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
            boids::StepEnTT(reg, egrid, cfg, pool);
        }
        const double entt_checksum = boids::StateChecksum(reg);
        const bool identical = ekit_checksum == entt_checksum;
        std::printf("state identical (ekit vs EnTT): %s  (sum=%.6f)\n",
                    identical ? "YES" : "NO", ekit_checksum);
        std::printf("(if NO, the two implementations diverge - check the algorithm)\n");

        // The data-parallel ekit path must also reproduce the same state.
        ekit::World dpworld;
        dpworld.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                                   boids::Separation, boids::Alignment, boids::Cohesion,
                                   boids::BoundsSteer, boids::MouseSteer>();
        std::mt19937 rng3(o.seed);
        boids::SpawnBoids(dpworld, cfg, rng3);
        boids::SpatialGrid dpgrid;
        dpgrid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));
        ekit::ThreadPool dppool(1);
        boids::SeparationSystem dpsep{&dpgrid, cfg.separation_radius, &dppool};
        boids::AlignmentSystem dpalign{&dpgrid, cfg.neighbor_radius, &dppool};
        boids::CohesionSystem dpcoh{&dpgrid, cfg.neighbor_radius, &dppool};
        boids::BoundsSystem dpbounds{nullptr, cfg, &dppool};
        boids::UpdateVelocitySystem dpupdate{cfg};
        boids::IntegrateSystem dpinteg{cfg};
        for (int i = 0; i < o.steps + o.warmup; ++i) {
            dpgrid.Build(dpworld);
            dpsep.Execute(dpworld);
            dpalign.Execute(dpworld);
            dpcoh.Execute(dpworld);
            dpbounds.Execute(dpworld);
            dpupdate.Execute(dpworld);
            dpinteg.Execute(dpworld);
        }
        double ekdp_checksum = 0.0;
        dpworld.Query<boids::Position>().ForEach(
            [&](const boids::Position& p) { ekdp_checksum += p.x + p.y; });
        std::printf("state identical (ekit-dp vs EnTT): %s  (sum=%.6f)\n\n",
                    (ekdp_checksum == entt_checksum) ? "YES" : "NO", ekdp_checksum);
    }

    std::printf("=== ekit vs EnTT: same boids algorithm, same threads ===\n");
    std::printf("world %dx%d | seed %u | %d timed steps (+%d warmup)\n\n", o.width, o.height,
                o.seed, o.steps, o.warmup);

    for (unsigned threads : o.thread_counts) {
        std::printf("============================================================\n");
        std::printf("threads = %u\n", threads);
        std::printf("------------------------------------------------------------\n");
        std::printf("%-8s %-14s %-14s %-9s %-14s %-12s\n", "boids", "entt ms/step",
                    "ekit ms/step", "ekit/entt", "ekit-dp ms/step", "ekit-dp/entt");
        std::printf("%-8s %-14s %-14s %-9s %-14s %-12s\n", "------", "-----------",
                    "-----------", "---------", "---------------", "------------");
        for (int count : o.boid_counts) {
            const double entt_ms = RunEnTT(o, count, threads);
            const double ekit_ms = RunEkit(o, count, threads);
            const double ekit_dp_ms = RunEkitDataParallel(o, count, threads);
            std::printf("%-8d %-14.4f %-14.4f %-9.3f %-14.4f %-12.3f\n", count, entt_ms,
                        ekit_ms, ekit_ms / entt_ms, ekit_dp_ms, ekit_dp_ms / entt_ms);
        }
        std::printf("\n");
    }
    return 0;
}
