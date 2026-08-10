#pragma once
// ekit Boids case study - simulation core.
//
// This file showcases the ekit ECS API:
//   * components are POD structs declared with EKIT_COMPONENT(T);
//   * components are explicitly registered with world.RegisterComponent<T>();
//   * systems are plain classes with an Execute(World&) method;
//   * data dependencies are declared with using Reads / Writes = TypeList<...>;
//   * the Scheduler analyzes Reads/Writes, so the three boid rule systems
//     (Separation / Alignment / Cohesion) run in parallel automatically.
//
// The neighbor search uses a uniform spatial grid rebuilt once per frame
// (synchronously, before the scheduler runs). During the scheduler phase the
// grid is only read, so the parallel rule systems are race-free.
//
// The frame is split into TWO scheduler phases to keep the dependency graph
// acyclic (a classic frame-pipeline pattern):
//   phase 1 (rules, parallel): separation / alignment / cohesion / bounds
//                              rules only READ Position/Velocity and each write
//                              their own accumulator;
//   phase 2 (apply):           UpdateVelocitySystem combines the accumulators
//                              into Velocity, then IntegrateSystem advances
//                              Position.
// Phase 1 reads the frame-start state; phase 2 writes the new state. A system
// must not read, in the same phase, a component that another system of that
// phase writes - that would be circular dataflow and the scheduler correctly
// reports it as a dependency cycle.

#include <ekit/ekit.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace boids {

// ---------------------------------------------------------------------------
// Components (POD structs)
// ---------------------------------------------------------------------------

struct Position {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Position);
};

struct Velocity {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Velocity);
};

// Marker component: every flock member carries one. Demonstrates the
// With<T>() / marker-component idiom.
struct BoidTag {
    EKIT_COMPONENT(BoidTag);
};

// Per-frame steering accumulators, one per rule. Each rule system writes its
// own accumulator (so they never conflict and can be parallelized); a final
// system combines them into the velocity.
struct Separation {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Separation);
};

struct Alignment {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Alignment);
};

struct Cohesion {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(Cohesion);
};

struct BoundsSteer {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(BoundsSteer);
};

// Steering accumulator produced by the mouse-follow system (live viewer).
struct MouseSteer {
    float x = 0.f;
    float y = 0.f;
    EKIT_COMPONENT(MouseSteer);
};

// Shared cursor state updated by the live viewer each frame.
struct MouseState {
    float x = 0.f;
    float y = 0.f;
    bool button_down = false;
};

// Current world bounds. The live viewer updates these when the window is
// resized, so the flock always roams the whole window. Systems fall back to
// the Config size when no bounds are provided (headless mode).
struct WorldBounds {
    float width = 0.f;
    float height = 0.f;
};

// ---------------------------------------------------------------------------
// Simulation configuration
// ---------------------------------------------------------------------------

struct Config {
    int width = 800;
    int height = 600;
    int boid_count = 220;
    int frames = 180;

    float dt = 1.f;

    float neighbor_radius = 48.f;
    float separation_radius = 26.f;

    float separation_weight = 1.6f;
    float alignment_weight = 0.12f;
    float cohesion_weight = 0.06f;

    float max_speed = 1.6f;
    float min_speed = 0.5f;

    // Soft bounds: boids steer back when they approach the world edge.
    float edge_margin = 60.f;
    float edge_steer = 1.2f;

    // Mouse interaction (live viewer): boids are attracted to the cursor;
    // while the left button is held they are repelled (shooed away).
    float mouse_radius = 220.f;
    float mouse_attract = 0.8f;
    float mouse_repel = 2.2f;

    unsigned seed = 20260810u;
    std::string out_dir = "frames";
    unsigned frames_set = 0; // set by cli when --frames was passed explicitly
    unsigned threads = 0;    // 0 == hardware concurrency
    int vsync = 1;           // live viewer: 1 = match monitor refresh, 0 = uncapped
};

// ---------------------------------------------------------------------------
// Spatial grid (uniform hash grid for O(1) neighbor lookup)
// ---------------------------------------------------------------------------

class SpatialGrid {
public:
    void Configure(float cell_size, float world_width, float world_height) {
        cell_size_ = cell_size;
        world_width_ = world_width;
        world_height_ = world_height;
        cols_ = std::max(1, int(std::ceil(world_width / cell_size)));
        rows_ = std::max(1, int(std::ceil(world_height / cell_size)));
        cells_.assign(static_cast<std::size_t>(cols_) * rows_, {});
    }

    void Build(ekit::World& world) {
        cells_.assign(static_cast<std::size_t>(cols_) * rows_, {});
        world.Query<Position, BoidTag>().ForEach(
            [&](ekit::Entity e, const Position& p, const BoidTag&) {
                const int cx = Clamp(int(p.x / cell_size_), 0, cols_ - 1);
                const int cy = Clamp(int(p.y / cell_size_), 0, rows_ - 1);
                cells_[static_cast<std::size_t>(cy) * cols_ + cx].push_back(e);
            });
        // Sort each cell by entity id so the neighbor iteration order (and
        // therefore the floating-point accumulation) is fully deterministic.
        for (auto& cell : cells_) {
            if (cell.size() > 1) {
                std::sort(cell.begin(), cell.end());
            }
        }
    }

    // Calls fn(entity) for every entity within `radius` of `center`.
    template<typename F>
    void ForEachNeighbor(ekit::World& world, const Position& center, float radius, F&& fn) const {
        const int x0 = std::max(0, int((center.x - radius) / cell_size_));
        const int x1 = std::min(cols_ - 1, int((center.x + radius) / cell_size_));
        const int y0 = std::max(0, int((center.y - radius) / cell_size_));
        const int y1 = std::min(rows_ - 1, int((center.y + radius) / cell_size_));
        const float r2 = radius * radius;
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                for (ekit::Entity n : cells_[static_cast<std::size_t>(cy) * cols_ + cx]) {
                    const Position* np = world.TryGet<Position>(n);
                    if (np == nullptr) {
                        continue;
                    }
                    const float dx = np->x - center.x;
                    const float dy = np->y - center.y;
                    if (dx * dx + dy * dy <= r2) {
                        fn(n);
                    }
                }
            }
        }
    }

private:
    static int Clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    float cell_size_ = 0.f;
    float world_width_ = 0.f;
    float world_height_ = 0.f;
    int cols_ = 0;
    int rows_ = 0;
    std::vector<std::vector<ekit::Entity>> cells_;
};

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

// Rule 1: steer away from nearby neighbors (writes Separation).
struct SeparationSystem {
    using Reads = ekit::TypeList<Position>;
    using Writes = ekit::TypeList<Separation>;

    const SpatialGrid* grid = nullptr;
    float radius = 26.f;

    void Execute(ekit::World& world) {
        world.Query<Position, Separation, BoidTag>().ForEach(
            [&](ekit::Entity e, const Position& p, Separation& s, const BoidTag&) {
                float sx = 0.f;
                float sy = 0.f;
                grid->ForEachNeighbor(world, p, radius, [&](ekit::Entity n) {
                    if (n == e) {
                        return;
                    }
                    const Position* np = world.TryGet<Position>(n);
                    if (np == nullptr) {
                        return;
                    }
                    const float dx = p.x - np->x;
                    const float dy = p.y - np->y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < radius * radius && d2 > 1e-4f) {
                        const float d = std::sqrt(d2);
                        const float strength = 1.f - d / radius; // closer => stronger push
                        sx += (dx / d) * strength;
                        sy += (dy / d) * strength;
                    }
                });
                s.x = sx;
                s.y = sy;
            });
    }
};

// Rule 2: align with the average heading of nearby neighbors (writes Alignment).
struct AlignmentSystem {
    using Reads = ekit::TypeList<Position, Velocity>;
    using Writes = ekit::TypeList<Alignment>;

    const SpatialGrid* grid = nullptr;
    float radius = 48.f;

    void Execute(ekit::World& world) {
        world.Query<Position, Velocity, Alignment, BoidTag>().ForEach(
            [&](ekit::Entity e, const Position& p, const Velocity&, Alignment& a, const BoidTag&) {
                float ax = 0.f;
                float ay = 0.f;
                int count = 0;
                grid->ForEachNeighbor(world, p, radius, [&](ekit::Entity n) {
                    if (n == e) {
                        return;
                    }
                    const Velocity* nv = world.TryGet<Velocity>(n);
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
    }
};

// Rule 3: steer toward the center of mass of nearby neighbors (writes Cohesion).
struct CohesionSystem {
    using Reads = ekit::TypeList<Position>;
    using Writes = ekit::TypeList<Cohesion>;

    const SpatialGrid* grid = nullptr;
    float radius = 48.f;

    void Execute(ekit::World& world) {
        world.Query<Position, Cohesion, BoidTag>().ForEach(
            [&](ekit::Entity e, const Position& p, Cohesion& c, const BoidTag&) {
                float cx = 0.f;
                float cy = 0.f;
                int count = 0;
                grid->ForEachNeighbor(world, p, radius, [&](ekit::Entity n) {
                    if (n == e) {
                        return;
                    }
                    const Position* np = world.TryGet<Position>(n);
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
    }
};

// Combines the rule accumulators (separation / alignment / cohesion / bounds)
// into the velocity and clamps speed. Runs after all four rule systems.
struct UpdateVelocitySystem {
    using Reads = ekit::TypeList<Separation, Alignment, Cohesion, BoundsSteer, MouseSteer>;
    using Writes = ekit::TypeList<Velocity>;

    Config cfg;

    void Execute(ekit::World& world) {
        world.Query<Velocity, Separation, Alignment, Cohesion, BoundsSteer, MouseSteer, BoidTag>()
            .ForEach([&](ekit::Entity, Velocity& v, const Separation& s, const Alignment& a,
                         const Cohesion& c, const BoundsSteer& b, const MouseSteer& m,
                         const BoidTag&) {
                v.x += s.x * cfg.separation_weight + a.x * cfg.alignment_weight +
                       c.x * cfg.cohesion_weight + b.x * cfg.edge_steer + m.x;
                v.y += s.y * cfg.separation_weight + a.y * cfg.alignment_weight +
                       c.y * cfg.cohesion_weight + b.y * cfg.edge_steer + m.y;

                // Clamp speed to [min_speed, max_speed].
                const float speed = std::sqrt(v.x * v.x + v.y * v.y);
                if (speed > cfg.max_speed) {
                    v.x *= cfg.max_speed / speed;
                    v.y *= cfg.max_speed / speed;
                } else if (speed < cfg.min_speed && speed > 1e-4f) {
                    v.x *= cfg.min_speed / speed;
                    v.y *= cfg.min_speed / speed;
                }
            });
    }
};

// Rule 5 (live viewer): follow the mouse cursor. Attracted while the button is
// up, repelled while it is held. Writes its own accumulator, so it runs in
// parallel with the other rules; in headless mode the cursor is idle and this
// system is simply not registered.
struct MouseSystem {
    using Reads = ekit::TypeList<Position>;
    using Writes = ekit::TypeList<MouseSteer>;

    const MouseState* mouse = nullptr;
    Config cfg;

    void Execute(ekit::World& world) {
        if (mouse == nullptr) {
            return;
        }
        const float mx = mouse->x;
        const float my = mouse->y;
        const bool repel = mouse->button_down;

        world.Query<Position, MouseSteer, BoidTag>().ForEach(
            [&](ekit::Entity, const Position& p, MouseSteer& m, const BoidTag&) {
                const float dx = mx - p.x;
                const float dy = my - p.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < 1e-4f) {
                    m.x = 0.f;
                    m.y = 0.f;
                    return;
                }
                const float d = std::sqrt(d2);
                // Strong near the cursor, fades to zero beyond mouse_radius.
                const float falloff = d < cfg.mouse_radius ? (1.f - d / cfg.mouse_radius) : 0.f;
                const float strength = (repel ? -cfg.mouse_repel : cfg.mouse_attract) * falloff;
                m.x = (dx / d) * strength;
                m.y = (dy / d) * strength;
            });
    }
};

// Rule 4: soft bounds - steer back towards the world center near the edges
// (writes its own accumulator, so it runs in parallel with the other rules).
struct BoundsSystem {
    using Reads = ekit::TypeList<Position>;
    using Writes = ekit::TypeList<BoundsSteer>;

    const WorldBounds* bounds = nullptr; // live viewer: follows the window size
    Config cfg;

    void Execute(ekit::World& world) {
        const float world_w =
            (bounds != nullptr && bounds->width > 0.f) ? bounds->width : static_cast<float>(cfg.width);
        const float world_h =
            (bounds != nullptr && bounds->height > 0.f) ? bounds->height : static_cast<float>(cfg.height);
        world.Query<Position, BoundsSteer, BoidTag>().ForEach(
            [&](ekit::Entity, const Position& p, BoundsSteer& b, const BoidTag&) {
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
    }
};

// Integrates velocity into position (writes Position, so the scheduler orders
// it after every system that reads positions).
struct IntegrateSystem {
    using Reads = ekit::TypeList<Velocity>;
    using Writes = ekit::TypeList<Position>;

    Config cfg;

    void Execute(ekit::World& world) {
        world.Query<Position, Velocity, BoidTag>().ForEach(
            [&](ekit::Entity, Position& p, const Velocity& v, const BoidTag&) {
                p.x += v.x * cfg.dt;
                p.y += v.y * cfg.dt;
            });
    }
};


// ---------------------------------------------------------------------------
// Shared helpers (used by both the headless demo and the live viewer)
// ---------------------------------------------------------------------------

// Spawns `cfg.boid_count` boids with random positions and headings.
inline void SpawnBoids(ekit::World& world, const Config& cfg, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist_x(0.f, static_cast<float>(cfg.width));
    std::uniform_real_distribution<float> dist_y(0.f, static_cast<float>(cfg.height));
    std::uniform_real_distribution<float> dist_v(-1.5f, 1.5f);

    for (int i = 0; i < cfg.boid_count; ++i) {
        ekit::Entity e = world.Create();
        world.Add<Position>(e, dist_x(rng), dist_y(rng));
        world.Add<Velocity>(e, dist_v(rng), dist_v(rng));
        world.Add<BoidTag>(e);
        world.Add<Separation>(e, 0.f, 0.f);
        world.Add<Alignment>(e, 0.f, 0.f);
        world.Add<Cohesion>(e, 0.f, 0.f);
        world.Add<BoundsSteer>(e, 0.f, 0.f);
        world.Add<MouseSteer>(e, 0.f, 0.f);
    }
}

// Counts connected flocks (union-find over the neighbor graph) - a nice
// query + grid showcase.
inline int CountFlocks(ekit::World& world, const SpatialGrid& grid, float radius) {
    std::vector<ekit::Entity> boids;
    world.Query<Position, BoidTag>().ForEach(
        [&](ekit::Entity e, const Position&, const BoidTag&) { boids.push_back(e); });
    if (boids.empty()) {
        return 0;
    }

    std::unordered_map<ekit::Entity, int> index;
    index.reserve(boids.size());
    for (std::size_t i = 0; i < boids.size(); ++i) {
        index[boids[i]] = static_cast<int>(i);
    }

    std::vector<int> parent(boids.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        const int ra = find(a);
        const int rb = find(b);
        if (ra != rb) {
            parent[ra] = rb;
        }
    };

    for (std::size_t i = 0; i < boids.size(); ++i) {
        const Position* p = world.TryGet<Position>(boids[i]);
        if (p == nullptr) {
            continue;
        }
        grid.ForEachNeighbor(world, *p, radius, [&](ekit::Entity n) {
            auto it = index.find(n);
            if (it != index.end()) {
                unite(static_cast<int>(i), it->second);
            }
        });
    }

    std::unordered_set<int> roots;
    for (std::size_t i = 0; i < boids.size(); ++i) {
        roots.insert(find(static_cast<int>(i)));
    }
    return static_cast<int>(roots.size());
}

} // namespace boids
