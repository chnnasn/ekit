// ekit vs EnTT - overall performance, 1..4 cores, controlled variables.
//
// The SAME fused boids algorithm (separation + alignment + cohesion + bounds,
// then combine + integrate) runs on both libraries with the same spatial grid,
// config and seed. Both sides use the same work-stealing chunking (ekit's
// detail::ParallelFor and EnTT's EnTTParallel::RunIndices are identical), so the
// only difference is the library's native per-entity component access:
//
//   ekit : world.Query<...>().ForEachParallel(pool, fn)  (archetype SoA columns)
//   EnTT : RunStorage(pool, storage, fn)                  (sparse-set storage.get)
//
// States are bit-identical; timings isolate the library-native storage + query
// overhead. EnTT is cloned externally (-DENTT_ROOT=E:/Github/entt).

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

using boids::Config, boids::Position, boids::Velocity, boids::BoidTag;
using boids::Separation, boids::Alignment, boids::Cohesion, boids::BoundsSteer;

Config MakeConfig(int boids, int width, int height) {
    Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.boid_count = boids;
    return cfg;
}

// ---------------------------------------------------------------------------
// ekit - native ForEachParallel
// ---------------------------------------------------------------------------
struct EkitNative {
    ekit::World world;
    boids::SpatialGrid grid;
    Config cfg;

    EkitNative(const Config& c, unsigned seed) : cfg(c) {
        world.RegisterComponents<Position, Velocity, BoidTag, Separation, Alignment, Cohesion,
                                 BoundsSteer, boids::MouseSteer>();
        std::mt19937 rng(seed);
        boids::SpawnBoids(world, cfg, rng);
        grid.Configure(cfg.neighbor_radius, float(cfg.width), float(cfg.height));
    }

    void Step(ekit::ThreadPool& pool) {
        grid.Build(world);

        world.Query<Position, Velocity, Separation, Alignment, Cohesion, BoidTag>().ForEachParallel(
            pool,
            [&](ekit::Entity e, const Position& p, const Velocity&, Separation& s, Alignment& a,
                Cohesion& c, const BoidTag&) {
                float sx = 0, sy = 0, ax = 0, ay = 0, cx = 0, cy = 0;
                int avg_n = 0, coh_n = 0;
                grid.ForEachNeighbor(world, p, cfg.neighbor_radius, [&](ekit::Entity n) {
                    if (n == e) return;
                    const Position* np = world.TryGet<Position>(n);
                    if (!np) return;
                    const float dx = p.x - np->x, dy = p.y - np->y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < cfg.separation_radius * cfg.separation_radius && d2 > 1e-4f) {
                        const float d = std::sqrt(d2), strength = 1.f - d / cfg.separation_radius;
                        sx += (dx / d) * strength;
                        sy += (dy / d) * strength;
                    }
                    if (const Velocity* nv = world.TryGet<Velocity>(n)) { ax += nv->x; ay += nv->y; ++avg_n; }
                    cx += np->x; cy += np->y; ++coh_n;
                });
                s.x = sx; s.y = sy;
                a.x = avg_n ? ax / float(avg_n) : 0.f;
                a.y = avg_n ? ay / float(avg_n) : 0.f;
                c.x = coh_n ? (cx / float(coh_n)) - p.x : 0.f;
                c.y = coh_n ? (cy / float(coh_n)) - p.y : 0.f;
            });

        world.Query<Velocity, Separation, Alignment, Cohesion, BoundsSteer, BoidTag>().ForEachParallel(
            pool,
            [&](Velocity& v, const Separation& s, const Alignment& a, const Cohesion& c,
                const BoundsSteer& b, const BoidTag&) {
                v.x += s.x * cfg.separation_weight + a.x * cfg.alignment_weight + c.x * cfg.cohesion_weight + b.x * cfg.edge_steer;
                v.y += s.y * cfg.separation_weight + a.y * cfg.alignment_weight + c.y * cfg.cohesion_weight + b.y * cfg.edge_steer;
                const float speed = std::sqrt(v.x * v.x + v.y * v.y);
                if (speed > cfg.max_speed) { v.x *= cfg.max_speed / speed; v.y *= cfg.max_speed / speed; }
                else if (speed < cfg.min_speed && speed > 1e-4f) { v.x *= cfg.min_speed / speed; v.y *= cfg.min_speed / speed; }
            });

        world.Query<Position, Velocity, BoidTag>().ForEachParallel(
            pool, [&](Position& p, const Velocity& v, const BoidTag&) {
                p.x += v.x * cfg.dt;
                p.y += v.y * cfg.dt;
            });
    }

    double Checksum() {
        double s = 0;
        world.Query<Position>().ForEach([&](const Position& p) { s += p.x + p.y; });
        return s;
    }
};

// ---------------------------------------------------------------------------
// EnTT - native RunStorage (same work-stealing chunking, storage.get access)
// ---------------------------------------------------------------------------
struct EnttNative {
    entt::registry reg;
    boids::EnTTGrid grid;
    Config cfg;

    EnttNative(const Config& c, unsigned seed) : cfg(c) {
        std::mt19937 rng(seed);
        std::vector<entt::entity> boids;
        boids::SpawnEnTTBoids(reg, boids, cfg, rng);
        grid.Configure(cfg.neighbor_radius, float(cfg.width), float(cfg.height));
    }

    void Step(boids::EnTTParallel& pool) {
        grid.Build(reg);

        auto& pos = reg.storage<Position>();
        auto& vel = reg.storage<Velocity>();
        auto& sep = reg.storage<Separation>();
        auto& ali = reg.storage<Alignment>();
        auto& coh = reg.storage<Cohesion>();
        auto& bnd = reg.storage<BoundsSteer>();

        // fused rules
        boids::RunStorage(pool, pos, [&](entt::entity e) {
            const Position& p = pos.get(e);
            const Velocity& v = vel.get(e);
            Separation& s = sep.get(e);
            Alignment& a = ali.get(e);
            Cohesion& c = coh.get(e);
            float sx = 0, sy = 0, ax = 0, ay = 0, cx = 0, cy = 0;
            int avg_n = 0, coh_n = 0;
            grid.ForEachNeighbor(reg, p, cfg.neighbor_radius, [&](entt::entity n) {
                if (n == e) return;
                const Position* np = reg.try_get<const Position>(n);
                if (!np) return;
                const float dx = p.x - np->x, dy = p.y - np->y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < cfg.separation_radius * cfg.separation_radius && d2 > 1e-4f) {
                    const float d = std::sqrt(d2), strength = 1.f - d / cfg.separation_radius;
                    sx += (dx / d) * strength;
                    sy += (dy / d) * strength;
                }
                if (const Velocity* nv = reg.try_get<const Velocity>(n)) { ax += nv->x; ay += nv->y; ++avg_n; }
                cx += np->x; cy += np->y; ++coh_n;
            });
            s.x = sx; s.y = sy;
            a.x = avg_n ? ax / float(avg_n) : 0.f;
            a.y = avg_n ? ay / float(avg_n) : 0.f;
            c.x = coh_n ? (cx / float(coh_n)) - p.x : 0.f;
            c.y = coh_n ? (cy / float(coh_n)) - p.y : 0.f;
        });

        // combine
        boids::RunStorage(pool, pos, [&](entt::entity e) {
            Velocity& v = vel.get(e);
            const Separation& s = sep.get(e);
            const Alignment& a = ali.get(e);
            const Cohesion& c = coh.get(e);
            const BoundsSteer& b = bnd.get(e);
            v.x += s.x * cfg.separation_weight + a.x * cfg.alignment_weight + c.x * cfg.cohesion_weight + b.x * cfg.edge_steer;
            v.y += s.y * cfg.separation_weight + a.y * cfg.alignment_weight + c.y * cfg.cohesion_weight + b.y * cfg.edge_steer;
            const float speed = std::sqrt(v.x * v.x + v.y * v.y);
            if (speed > cfg.max_speed) { v.x *= cfg.max_speed / speed; v.y *= cfg.max_speed / speed; }
            else if (speed < cfg.min_speed && speed > 1e-4f) { v.x *= cfg.min_speed / speed; v.y *= cfg.min_speed / speed; }
        });

        // integrate
        boids::RunStorage(pool, pos, [&](entt::entity e) {
            Position& p = pos.get(e);
            const Velocity& v = vel.get(e);
            p.x += v.x * cfg.dt;
            p.y += v.y * cfg.dt;
        });
    }

    double Checksum() {
        double s = 0;
        reg.view<const Position>().each([&](const Position& p) { s += p.x + p.y; });
        return s;
    }
};

template<typename F>
double Bench(F&& run, int steps) {
    for (int i = 0; i < 3; ++i) run();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < steps; ++i) run();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / steps;
}

} // namespace

int main(int argc, char** argv) {
    int boids = 10000, steps = 30, width = 800, height = 600;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--boids") boids = std::stoi(argv[++i]);
        else if (a == "--steps") steps = std::stoi(argv[++i]);
        else if (a == "--width") width = std::stoi(argv[++i]);
        else if (a == "--height") height = std::stoi(argv[++i]);
    }
    const Config cfg = MakeConfig(boids, width, height);
    const unsigned seed = 20260810u;

    std::printf("=== ekit vs EnTT: overall performance, same fused boids algorithm ===\n");
    std::printf("boids=%d, %d timed steps, world %dx%d, identical work-stealing chunking\n\n",
                boids, steps, width, height);

    EkitNative ek(cfg, seed);
    EnttNative en(cfg, seed);
    std::printf("state checksum: ekit=%.6f  entt=%.6f  (must match)\n\n",
                ek.Checksum(), en.Checksum());

    std::printf("%-8s %-12s %-12s %-10s\n", "threads", "entt ms/step", "ekit ms/step", "ekit/entt");
    std::printf("%-8s %-12s %-12s %-10s\n", "-------", "-----------", "-----------", "---------");

    for (unsigned threads : {1u, 2u, 3u, 4u}) {
        ekit::ThreadPool epool(threads);
        boids::EnTTParallel npool(threads);
        const double entt_ms = Bench([&] { en.Step(npool); }, steps);
        const double ekit_ms = Bench([&] { ek.Step(epool); }, steps);
        std::printf("%-8u %-12.3f %-12.3f %-10.3f\n", threads, entt_ms, ekit_ms,
                    ekit_ms / entt_ms);
    }
    return 0;
}
