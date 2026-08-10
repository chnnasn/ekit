#pragma once
// EnTT implementation of the SAME boids simulation as boids.hpp (ekit).
// It reuses the exact same component types and rule math, so the comparison is
// algorithm-for-algorithm: only the ECS layer differs (EnTT vs ekit).
//
// EnTT is header-only and NOT part of this repository; include it via
// -I<E:\Github\entt\single_include> (set ENTT_ROOT at configure time).

#include <entt/entt.hpp>

#include "boids.hpp" // same components: Position / Velocity / BoidTag / ...

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace boids {

// ---------------------------------------------------------------------------
// Uniform spatial grid (same as the ekit SpatialGrid, but EnTT entities)
// ---------------------------------------------------------------------------

class EnTTGrid {
public:
    void Configure(float cell_size, float world_width, float world_height) {
        cell_size_ = cell_size;
        cols_ = std::max(1, int(std::ceil(world_width / cell_size)));
        rows_ = std::max(1, int(std::ceil(world_height / cell_size)));
        cells_.assign(static_cast<std::size_t>(cols_) * rows_, {});
    }

    void Build(entt::registry& reg) {
        cells_.assign(static_cast<std::size_t>(cols_) * rows_, {});
        // Note: EnTT skips empty types (BoidTag) in view iteration, so iterate
        // Position only - every boid owns one.
        auto view = reg.view<const Position>();
        view.each([&](entt::entity e, const Position& p) {
            const int cx = Clamp(int(p.x / cell_size_), 0, cols_ - 1);
            const int cy = Clamp(int(p.y / cell_size_), 0, rows_ - 1);
            cells_[static_cast<std::size_t>(cy) * cols_ + cx].push_back(e);
        });
        // Sort each cell by entity id so the neighbor iteration order (and
        // therefore the floating-point accumulation) matches the ekit side.
        for (auto& cell : cells_) {
            if (cell.size() > 1) {
                std::sort(cell.begin(), cell.end(),
                          [](entt::entity a, entt::entity b) {
                              return static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b);
                          });
            }
        }
    }

    template<typename F>
    void ForEachNeighbor(entt::registry& reg, const Position& center, float radius, F&& fn) const {
        const int x0 = std::max(0, int((center.x - radius) / cell_size_));
        const int x1 = std::min(cols_ - 1, int((center.x + radius) / cell_size_));
        const int y0 = std::max(0, int((center.y - radius) / cell_size_));
        const int y1 = std::min(rows_ - 1, int((center.y + radius) / cell_size_));
        const float r2 = radius * radius;
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                for (entt::entity n : cells_[static_cast<std::size_t>(cy) * cols_ + cx]) {
                    const Position* np = reg.try_get<const Position>(n);
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
    int cols_ = 0;
    int rows_ = 0;
    std::vector<std::vector<entt::entity>> cells_;
};

// ---------------------------------------------------------------------------
// Persistent thread pool used to parallelize the four rule passes (the ekit
// scheduler uses an equivalent pool; same thread counts on both sides).
// ---------------------------------------------------------------------------

class EnTTParallel {
public:
    explicit EnTTParallel(unsigned threads) : threads_(threads) {
        for (unsigned i = 0; i < threads_; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~EnTTParallel() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    EnTTParallel(const EnTTParallel&) = delete;
    EnTTParallel& operator=(const EnTTParallel&) = delete;

    // Runs f over the given entities using `threads_` workers.
    void Run(const std::vector<entt::entity>& entities,
             const std::function<void(entt::entity)>& f) {
        const std::size_t n = entities.size();
        if (threads_ <= 1 || n <= 1) {
            for (entt::entity e : entities) {
                f(e);
            }
            return;
        }
        const std::size_t chunk = (n + threads_ - 1) / threads_;
        for (unsigned t = 0; t < threads_; ++t) {
            const std::size_t begin = t * chunk;
            const std::size_t end = std::min(n, begin + chunk);
            if (begin >= end) {
                break;
            }
            Submit([&, begin, end] {
                for (std::size_t i = begin; i < end; ++i) {
                    f(entities[i]);
                }
            });
        }
        WaitAll();
    }

private:
    void Submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++pending_;
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void WaitAll() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return pending_ == 0; });
    }

    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --pending_;
                if (pending_ == 0) {
                    done_cv_.notify_all();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;
    std::size_t pending_ = 0;
    bool stop_ = false;
    unsigned threads_;
};

// ---------------------------------------------------------------------------
// Spawn + one simulation step (identical math to the ekit systems)
// ---------------------------------------------------------------------------

inline void SpawnEnTTBoids(entt::registry& reg, std::vector<entt::entity>& out,
                           const Config& cfg, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist_x(0.f, static_cast<float>(cfg.width));
    std::uniform_real_distribution<float> dist_y(0.f, static_cast<float>(cfg.height));
    std::uniform_real_distribution<float> dist_v(-1.5f, 1.5f);

    out.clear();
    out.reserve(static_cast<std::size_t>(cfg.boid_count));
    for (int i = 0; i < cfg.boid_count; ++i) {
        entt::entity e = reg.create();
        reg.emplace<Position>(e, dist_x(rng), dist_y(rng));
        reg.emplace<Velocity>(e, dist_v(rng), dist_v(rng));
        reg.emplace<BoidTag>(e);
        reg.emplace<Separation>(e, 0.f, 0.f);
        reg.emplace<Alignment>(e, 0.f, 0.f);
        reg.emplace<Cohesion>(e, 0.f, 0.f);
        reg.emplace<BoundsSteer>(e, 0.f, 0.f);
        out.push_back(e);
    }
}

inline void StepEnTT(entt::registry& reg, EnTTGrid& grid, const Config& cfg,
                     EnTTParallel& pool, const std::vector<entt::entity>& boids) {
    grid.Build(reg);

    // --- Rule 1: separation (parallel) ----------------------------------
    pool.Run(boids, [&](entt::entity e) {
        const Position* p = reg.try_get<const Position>(e);
        Separation* s = reg.try_get<Separation>(e);
        if (p == nullptr || s == nullptr) {
            return;
        }
        float sx = 0.f;
        float sy = 0.f;
        grid.ForEachNeighbor(reg, *p, cfg.separation_radius, [&](entt::entity n) {
            if (n == e) {
                return;
            }
            const Position* np = reg.try_get<const Position>(n);
            if (np == nullptr) {
                return;
            }
            const float dx = p->x - np->x;
            const float dy = p->y - np->y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < cfg.separation_radius * cfg.separation_radius && d2 > 1e-4f) {
                const float d = std::sqrt(d2);
                const float strength = 1.f - d / cfg.separation_radius;
                sx += (dx / d) * strength;
                sy += (dy / d) * strength;
            }
        });
        s->x = sx;
        s->y = sy;
    });

    // --- Rule 2: alignment (parallel) -----------------------------------
    pool.Run(boids, [&](entt::entity e) {
        const Position* p = reg.try_get<const Position>(e);
        Alignment* a = reg.try_get<Alignment>(e);
        if (p == nullptr || a == nullptr) {
            return;
        }
        float ax = 0.f;
        float ay = 0.f;
        int count = 0;
        grid.ForEachNeighbor(reg, *p, cfg.neighbor_radius, [&](entt::entity n) {
            if (n == e) {
                return;
            }
            const Velocity* nv = reg.try_get<const Velocity>(n);
            if (nv == nullptr) {
                return;
            }
            ax += nv->x;
            ay += nv->y;
            ++count;
        });
        if (count > 0) {
            a->x = ax / static_cast<float>(count);
            a->y = ay / static_cast<float>(count);
        } else {
            a->x = 0.f;
            a->y = 0.f;
        }
    });

    // --- Rule 3: cohesion (parallel) ------------------------------------
    pool.Run(boids, [&](entt::entity e) {
        const Position* p = reg.try_get<const Position>(e);
        Cohesion* c = reg.try_get<Cohesion>(e);
        if (p == nullptr || c == nullptr) {
            return;
        }
        float cx = 0.f;
        float cy = 0.f;
        int count = 0;
        grid.ForEachNeighbor(reg, *p, cfg.neighbor_radius, [&](entt::entity n) {
            if (n == e) {
                return;
            }
            const Position* np = reg.try_get<const Position>(n);
            if (np == nullptr) {
                return;
            }
            cx += np->x;
            cy += np->y;
            ++count;
        });
        if (count > 0) {
            c->x = (cx / static_cast<float>(count)) - p->x;
            c->y = (cy / static_cast<float>(count)) - p->y;
        } else {
            c->x = 0.f;
            c->y = 0.f;
        }
    });

    // --- Rule 4: soft bounds (parallel) ---------------------------------
    pool.Run(boids, [&](entt::entity e) {
        const Position* p = reg.try_get<const Position>(e);
        BoundsSteer* b = reg.try_get<BoundsSteer>(e);
        if (p == nullptr || b == nullptr) {
            return;
        }
        const float world_w = static_cast<float>(cfg.width);
        const float world_h = static_cast<float>(cfg.height);
        float sx = 0.f;
        float sy = 0.f;
        if (p->x < cfg.edge_margin) {
            sx = (cfg.edge_margin - p->x) / cfg.edge_margin;
        } else if (p->x > world_w - cfg.edge_margin) {
            sx = (world_w - cfg.edge_margin - p->x) / cfg.edge_margin;
        }
        if (p->y < cfg.edge_margin) {
            sy = (cfg.edge_margin - p->y) / cfg.edge_margin;
        } else if (p->y > world_h - cfg.edge_margin) {
            sy = (world_h - cfg.edge_margin - p->y) / cfg.edge_margin;
        }
        b->x = sx;
        b->y = sy;
    });

    // --- Combine rules into velocity (serial, single system) ------------
    for (entt::entity e : boids) {
        Velocity* v = reg.try_get<Velocity>(e);
        const Separation* s = reg.try_get<const Separation>(e);
        const Alignment* a = reg.try_get<const Alignment>(e);
        const Cohesion* c = reg.try_get<const Cohesion>(e);
        const BoundsSteer* b = reg.try_get<const BoundsSteer>(e);
        if (v == nullptr || s == nullptr || a == nullptr || c == nullptr || b == nullptr) {
            continue;
        }
        v->x += s->x * cfg.separation_weight + a->x * cfg.alignment_weight +
                c->x * cfg.cohesion_weight + b->x * cfg.edge_steer;
        v->y += s->y * cfg.separation_weight + a->y * cfg.alignment_weight +
                c->y * cfg.cohesion_weight + b->y * cfg.edge_steer;
        const float speed = std::sqrt(v->x * v->x + v->y * v->y);
        if (speed > cfg.max_speed) {
            v->x *= cfg.max_speed / speed;
            v->y *= cfg.max_speed / speed;
        } else if (speed < cfg.min_speed && speed > 1e-4f) {
            v->x *= cfg.min_speed / speed;
            v->y *= cfg.min_speed / speed;
        }
    }

    // --- Integrate position (serial, single system) ---------------------
    for (entt::entity e : boids) {
        Position* p = reg.try_get<Position>(e);
        const Velocity* v = reg.try_get<const Velocity>(e);
        if (p == nullptr || v == nullptr) {
            continue;
        }
        p->x += v->x * cfg.dt;
        p->y += v->y * cfg.dt;
    }
}

// Total position checksum - used to prove both implementations produce the
// exact same simulation state.
inline double StateChecksum(entt::registry& reg) {
    double sum = 0.0;
    auto view = reg.view<const Position>();
    view.each([&](const Position& p) { sum += p.x + p.y; });
    return sum;
}

} // namespace boids
