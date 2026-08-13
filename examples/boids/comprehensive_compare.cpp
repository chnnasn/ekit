// ekit vs EnTT - comprehensive, multi-dimensional comparison.
//
// Same algorithm / data / config everywhere; only the library's native API
// differs. Dimensions:
//   1. scalar iteration   (per-entity integrate, 1 thread)
//   2. batch iteration    (SoA batch integrate, 1 thread)
//   3. parallel iteration (per-entity integrate, 1..4 threads)
//   4. structural churn   (create + add 2 comps + destroy)
//   5. random access      (fused boids rules, 1..4 threads)
//
// EnTT is cloned externally (-DENTT_ROOT=E:/Github/entt).

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

double Now() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- common: build N entities with Position + Velocity ----------------------

struct EkitData {
    ekit::World world;
    explicit EkitData(int n) {
        world.RegisterComponents<Position, Velocity>();
        for (int i = 0; i < n; ++i) {
            ekit::Entity e = world.Create();
            world.Add<Position>(e, static_cast<float>(i), 0.f);
            world.Add<Velocity>(e, 0.25f, 0.5f);
        }
    }
};

struct EnttData {
    entt::registry reg;
    explicit EnttData(int n) {
        for (int i = 0; i < n; ++i) {
            entt::entity e = reg.create();
            reg.emplace<Position>(e, static_cast<float>(i), 0.f);
            reg.emplace<Velocity>(e, 0.25f, 0.5f);
        }
    }
};

// --- dimension 1: scalar iteration (1 thread) --------------------------------

double EkitIterateScalar(EkitData& d, int reps) {
    double t0 = Now();
    for (int r = 0; r < reps; ++r)
        d.world.Query<Position, Velocity>().ForEach(
            [](Position& p, Velocity& v) { p.x += v.x; p.y += v.y; });
    return (Now() - t0) / reps;
}

double EnttIterateScalar(EnttData& d, int reps) {
    double t0 = Now();
    for (int r = 0; r < reps; ++r)
        d.reg.view<Position, Velocity>().each(
            [](Position& p, Velocity& v) { p.x += v.x; p.y += v.y; });
    return (Now() - t0) / reps;
}

// --- dimension 2: batch iteration (1 thread) --------------------------------

double EkitIterateBatch(EkitData& d, int reps) {
    double t0 = Now();
    for (int r = 0; r < reps; ++r)
        d.world.Query<Position, Velocity>().ForEachBatch(
            [](Position* p, Velocity* v, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) { p[i].x += v[i].x; p[i].y += v[i].y; }
            });
    return (Now() - t0) / reps;
}

// EnTT has no flat SoA batch primitive (storage.raw() is paged), so the native
// batch equivalent is the packed-array loop with storage access.
double EnttIterateBatch(EnttData& d, int reps) {
    double t0 = Now();
    auto& ps = d.reg.storage<Position>();
    auto& vs = d.reg.storage<Velocity>();
    for (int r = 0; r < reps; ++r) {
        const std::size_t n = ps.size();
        for (std::size_t i = 0; i < n; ++i) {
            Position& p = ps.get(ps[i]);
            const Velocity& v = vs.get(ps[i]);
            p.x += v.x; p.y += v.y;
        }
    }
    return (Now() - t0) / reps;
}

// --- dimension 3: parallel iteration (T threads) -----------------------------

double EkitIterateParallel(EkitData& d, int reps, unsigned threads) {
    ekit::ThreadPool pool(threads);
    double t0 = Now();
    for (int r = 0; r < reps; ++r)
        d.world.Query<Position, Velocity>().ForEachParallel(
            pool, [](Position& p, Velocity& v) { p.x += v.x; p.y += v.y; });
    return (Now() - t0) / reps;
}

double EnttIterateParallel(EnttData& d, int reps, unsigned threads) {
    boids::EnTTParallel pool(threads);
    auto& ps = d.reg.storage<Position>();
    auto& vs = d.reg.storage<Velocity>();
    double t0 = Now();
    for (int r = 0; r < reps; ++r)
        boids::RunStorage(pool, ps, [&](entt::entity e) {
            Position& p = ps.get(e);
            const Velocity& v = vs.get(e);
            p.x += v.x; p.y += v.y;
        });
    return (Now() - t0) / reps;
}

// --- dimension 4: structural churn ------------------------------------------

double EkitStructural(int n, int reps) {
    double t0 = Now();
    for (int r = 0; r < reps; ++r) {
        ekit::World world;
        world.RegisterComponents<Position, Velocity>();
        std::vector<ekit::Entity> es;
        es.reserve(n);
        for (int i = 0; i < n; ++i) {
            ekit::Entity e = world.Create();
            world.Add<Position>(e, 1.f, 1.f);
            world.Add<Velocity>(e, 1.f, 1.f);
            es.push_back(e);
        }
        for (ekit::Entity e : es) world.Destroy(e);
    }
    return (Now() - t0) / reps;
}

double EnttStructural(int n, int reps) {
    double t0 = Now();
    for (int r = 0; r < reps; ++r) {
        entt::registry reg;
        std::vector<entt::entity> es;
        es.reserve(n);
        for (int i = 0; i < n; ++i) {
            entt::entity e = reg.create();
            reg.emplace<Position>(e, 1.f, 1.f);
            reg.emplace<Velocity>(e, 1.f, 1.f);
            es.push_back(e);
        }
        for (entt::entity e : es) reg.destroy(e);
    }
    return (Now() - t0) / reps;
}

// --- dimension 5: random access (fused boids rules) -------------------------

struct EkitBoids {
    ekit::World world;
    boids::SpatialGrid grid;
    Config cfg;
    EkitBoids(const Config& c, unsigned seed) : cfg(c) {
        world.RegisterComponents<Position, Velocity, BoidTag, Separation, Alignment, Cohesion,
                                 BoundsSteer, boids::MouseSteer>();
        std::mt19937 rng(seed);
        boids::SpawnBoids(world, cfg, rng);
        grid.Configure(cfg.neighbor_radius, float(cfg.width), float(cfg.height));
    }
    void Step(ekit::ThreadPool& pool) {
        grid.Build(world);
        world.Query<Position, Velocity, Separation, Alignment, Cohesion, BoidTag>().ForEachParallel(
            pool, [&](ekit::Entity e, const Position& p, const Velocity&, Separation& s,
                      Alignment& a, Cohesion& c, const BoidTag&) {
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
    }
};

struct EnttBoids {
    entt::registry reg;
    boids::EnTTGrid grid;
    Config cfg;
    EnttBoids(const Config& c, unsigned seed) : cfg(c) {
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
    }
};

} // namespace

int main(int argc, char** argv) {
    int n = 1000000, reps = 50, boids = 10000, steps = 20, width = 800, height = 600;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n") n = std::stoi(argv[++i]);
        else if (a == "--reps") reps = std::stoi(argv[++i]);
        else if (a == "--boids") boids = std::stoi(argv[++i]);
        else if (a == "--steps") steps = std::stoi(argv[++i]);
    }

    std::printf("=== ekit vs EnTT - comprehensive comparison ===\n\n");

    // dimensions 1-4
    EkitData ed(n);
    EnttData nd(n);
    std::printf("-- iterate (integrate %d entities, %d reps) --\n", n, reps);
    std::printf("  %-16s %-12s %-12s %-10s\n", "mode", "entt ms", "ekit ms", "ekit/entt");
    {
        double a = EnttIterateScalar(nd, reps);
        double b = EkitIterateScalar(ed, reps);
        std::printf("  %-16s %-12.3f %-12.3f %-10.3f\n", "scalar", a, b, b / a);
    }
    {
        double a = EnttIterateBatch(nd, reps);
        double b = EkitIterateBatch(ed, reps);
        std::printf("  %-16s %-12.3f %-12.3f %-10.3f\n", "batch(SoA)", a, b, b / a);
    }
    for (unsigned t : {1u, 2u, 3u, 4u}) {
        double a = EnttIterateParallel(nd, reps / 5, t);
        double b = EkitIterateParallel(ed, reps / 5, t);
        std::printf("  %-16s %-12.3f %-12.3f %-10.3f\n",
                    ("parallel t=" + std::to_string(t)).c_str(), a, b, b / a);
    }

    std::printf("\n-- structural churn (create + 2 comps + destroy, %d entities) --\n", n / 10);
    {
        double a = EnttStructural(n / 10, reps / 10);
        double b = EkitStructural(n / 10, reps / 10);
        std::printf("  %-16s %-12.3f %-12.3f %-10.3f\n", "churn", a, b, b / a);
    }

    // dimension 5: random access
    Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.boid_count = boids;
    EkitBoids eb(cfg, 20260810u);
    EnttBoids nb(cfg, 20260810u);
    std::printf("\n-- random access (fused boids rules, %d boids) --\n", boids);
    for (unsigned t : {1u, 2u, 3u, 4u}) {
        ekit::ThreadPool ep(t);
        boids::EnTTParallel np(t);
        double t0 = Now();
        for (int i = 0; i < steps; ++i) nb.Step(np);
        double a = (Now() - t0) / steps;
        t0 = Now();
        for (int i = 0; i < steps; ++i) eb.Step(ep);
        double b = (Now() - t0) / steps;
        std::printf("  %-16s %-12.3f %-12.3f %-10.3f\n",
                    ("rules t=" + std::to_string(t)).c_str(), a, b, b / a);
    }
    return 0;
}
