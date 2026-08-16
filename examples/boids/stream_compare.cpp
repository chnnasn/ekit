// ekit vs EnTT: the SAME fused boids algorithm (separation + alignment +
// cohesion + bounds, then combine + integrate), expressed with each library's
// NATIVE single-threaded iteration primitives only:
//
//   ekit scalar : Query<...>().ForEach(...)      (per-entity callback)
//   ekit batch  : Query<...>().ForEachBatch(...) (SoA pointers + count)
//   EnTT scalar : registry.view<...>().each(...) (per-entity callback)
//
// All three run single-threaded; the neighbor gather is identical scalar code,
// so only the per-entity SoA passes differ. The three paths must produce
// bit-identical state. EnTT is cloned externally (-DENTT_ROOT=...).

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
// ekit - scalar (ForEach for rules, combine and integrate)
// ---------------------------------------------------------------------------
struct EkitScalar {
    ekit::World world;
    boids::SpatialGrid grid;
    Config cfg;

    EkitScalar(const Config& c, unsigned seed) : cfg(c) {
        world.RegisterComponents<Position, Velocity, BoidTag, Separation, Alignment, Cohesion,
                                 BoundsSteer, boids::MouseSteer>();
        std::mt19937 rng(seed);
        boids::SpawnBoids(world, cfg, rng);
        grid.Configure(cfg.neighbor_radius, float(cfg.width), float(cfg.height));
    }

    void Step() {
        grid.Build(world);

        // fused rules (scalar neighbor gather -> accumulators)
        world.Query<Position, Velocity, Separation, Alignment, Cohesion, BoidTag>().ForEach(
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

        // combine (scalar)
        world.Query<Velocity, Separation, Alignment, Cohesion, BoundsSteer, BoidTag>().ForEach(
            [&](Velocity& v, const Separation& s, const Alignment& a, const Cohesion& c,
                const BoundsSteer& b, const BoidTag&) {
                v.x += s.x * cfg.separation_weight + a.x * cfg.alignment_weight + c.x * cfg.cohesion_weight + b.x * cfg.edge_steer;
                v.y += s.y * cfg.separation_weight + a.y * cfg.alignment_weight + c.y * cfg.cohesion_weight + b.y * cfg.edge_steer;
                const float speed = std::sqrt(v.x * v.x + v.y * v.y);
                if (speed > cfg.max_speed) { v.x *= cfg.max_speed / speed; v.y *= cfg.max_speed / speed; }
                else if (speed < cfg.min_speed && speed > 1e-4f) { v.x *= cfg.min_speed / speed; v.y *= cfg.min_speed / speed; }
            });

        // integrate (scalar)
        world.Query<Position, Velocity, BoidTag>().ForEach(
            [&](Position& p, const Velocity& v, const BoidTag&) {
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
// ekit - batch (ForEach for rules, ForEachBatch for combine + integrate)
// ---------------------------------------------------------------------------
struct EkitBatch {
    ekit::World world;
    boids::SpatialGrid grid;
    Config cfg;

    EkitBatch(const Config& c, unsigned seed) : cfg(c) {
        world.RegisterComponents<Position, Velocity, BoidTag, Separation, Alignment, Cohesion,
                                 BoundsSteer, boids::MouseSteer>();
        std::mt19937 rng(seed);
        boids::SpawnBoids(world, cfg, rng);
        grid.Configure(cfg.neighbor_radius, float(cfg.width), float(cfg.height));
    }

    void Step() {
        grid.Build(world);

        // fused rules (identical scalar gather as EkitScalar)
        world.Query<Position, Velocity, Separation, Alignment, Cohesion, BoidTag>().ForEach(
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

        // combine (SoA batch)
        world.Query<Velocity, Separation, Alignment, Cohesion, BoundsSteer, BoidTag>().ForEachBatch(
            [&](Velocity* v, Separation* s, Alignment* a, Cohesion* c, BoundsSteer* b, BoidTag*,
                std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) {
                    v[i].x += s[i].x * cfg.separation_weight + a[i].x * cfg.alignment_weight +
                              c[i].x * cfg.cohesion_weight + b[i].x * cfg.edge_steer;
                    v[i].y += s[i].y * cfg.separation_weight + a[i].y * cfg.alignment_weight +
                              c[i].y * cfg.cohesion_weight + b[i].y * cfg.edge_steer;
                    const float speed = std::sqrt(v[i].x * v[i].x + v[i].y * v[i].y);
                    if (speed > cfg.max_speed) { v[i].x *= cfg.max_speed / speed; v[i].y *= cfg.max_speed / speed; }
                    else if (speed < cfg.min_speed && speed > 1e-4f) { v[i].x *= cfg.min_speed / speed; v[i].y *= cfg.min_speed / speed; }
                }
            });

        // integrate (SoA batch)
        world.Query<Position, Velocity, BoidTag>().ForEachBatch(
            [&](Position* p, Velocity* v, BoidTag*, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) {
                    p[i].x += v[i].x * cfg.dt;
                    p[i].y += v[i].y * cfg.dt;
                }
            });
    }

    double Checksum() {
        double s = 0;
        world.Query<Position>().ForEach([&](const Position& p) { s += p.x + p.y; });
        return s;
    }
};

// ---------------------------------------------------------------------------
// EnTT - scalar (registry.view.each)
// ---------------------------------------------------------------------------
struct EnttScalar {
    entt::registry reg;
    boids::EnTTGrid grid;
    std::vector<entt::entity> boids;
    Config cfg;

    EnttScalar(const Config& c, unsigned seed) : cfg(c) {
        std::mt19937 rng(seed);
        boids::SpawnEnTTBoids(reg, boids, cfg, rng);
        grid.Configure(cfg.neighbor_radius, float(cfg.width), float(cfg.height));
    }

    void Step() {
        grid.Build(reg);

        auto view = reg.view<Position, Velocity, Separation, Alignment, Cohesion>();
        view.each([&](entt::entity e, Position& p, const Velocity&, Separation& s, Alignment& a,
                      Cohesion& c) {
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

        for (entt::entity e : boids) {
            Velocity* v = reg.try_get<Velocity>(e);
            const Separation* s = reg.try_get<const Separation>(e);
            const Alignment* a = reg.try_get<const Alignment>(e);
            const Cohesion* c = reg.try_get<const Cohesion>(e);
            const BoundsSteer* b = reg.try_get<const BoundsSteer>(e);
            if (!v || !s || !a || !c || !b) continue;
            v->x += s->x * cfg.separation_weight + a->x * cfg.alignment_weight + c->x * cfg.cohesion_weight + b->x * cfg.edge_steer;
            v->y += s->y * cfg.separation_weight + a->y * cfg.alignment_weight + c->y * cfg.cohesion_weight + b->y * cfg.edge_steer;
            const float speed = std::sqrt(v->x * v->x + v->y * v->y);
            if (speed > cfg.max_speed) { v->x *= cfg.max_speed / speed; v->y *= cfg.max_speed / speed; }
            else if (speed < cfg.min_speed && speed > 1e-4f) { v->x *= cfg.min_speed / speed; v->y *= cfg.min_speed / speed; }
        }
        for (entt::entity e : boids) {
            Position* p = reg.try_get<Position>(e);
            const Velocity* v = reg.try_get<const Velocity>(e);
            if (!p || !v) continue;
            p->x += v->x * cfg.dt;
            p->y += v->y * cfg.dt;
        }
    }

    double Checksum() {
        double s = 0;
        reg.view<const Position>().each([&](const Position& p) { s += p.x + p.y; });
        return s;
    }
};

template<typename Impl>
double Bench(Impl& impl, int steps) {
    for (int i = 0; i < 5; ++i) impl.Step();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < steps; ++i) impl.Step();
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

    std::printf("=== ekit vs EnTT: same fused boids algorithm, native single-thread primitives ===\n");
    std::printf("boids=%d, %d timed steps, world %dx%d\n\n", boids, steps, width, height);

    EkitScalar e_s(cfg, seed);
    EkitBatch e_b(cfg, seed);
    EnttScalar n_s(cfg, seed);

    std::printf("state checks (must be equal):\n");
    std::printf("  %-14s %.6f\n", "ekit scalar", e_s.Checksum());
    std::printf("  %-14s %.6f\n", "ekit batch", e_b.Checksum());
    std::printf("  %-14s %.6f\n", "entt scalar", n_s.Checksum());
    std::printf("\n");

    const double base = Bench(n_s, steps);
    const double e_s_ms = Bench(e_s, steps);
    const double e_b_ms = Bench(e_b, steps);

    std::printf("%-14s %-12s %-12s\n", "impl", "ms/step", "vs entt-scalar");
    std::printf("%-14s %-12s %-12s\n", "-----", "-------", "-------------");
    std::printf("%-14s %-12.3f %-12.3f\n", "entt scalar", base, 1.000);
    std::printf("%-14s %-12.3f %-12.3f\n", "ekit scalar", e_s_ms, e_s_ms / base);
    std::printf("%-14s %-12.3f %-12.3f\n", "ekit batch", e_b_ms, e_b_ms / base);
    return 0;
}
