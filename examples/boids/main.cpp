// ekit Boids case study - headless demo (renders PPM frames + stats).
//
// Runs the flocking simulation with the ekit ECS + Scheduler and writes every
// frame to a PPM image in the output directory. Convert to PNG / animated GIF
// with the bundled render.ps1 script. For a real-time window instead, build
// and run ekit_boids_live (main_live.cpp).

#include "boids.hpp"
#include "canvas.hpp"
#include "cli.hpp"
#include "render_helpers.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

int main(int argc, char** argv) {
    boids::Config cfg = boids::ParseConfig(argc, argv);

    // ------------------------------------------------------------------
    // 1. World setup: explicit component registration.
    // ------------------------------------------------------------------
    ekit::World world;
    world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                             boids::Separation, boids::Alignment, boids::Cohesion,
                             boids::BoundsSteer, boids::MouseSteer>();

    // ------------------------------------------------------------------
    // 2. Spawn the flock with random positions / headings.
    // ------------------------------------------------------------------
    std::mt19937 rng(cfg.seed);
    boids::SpawnBoids(world, cfg, rng);

    // ------------------------------------------------------------------
    // 3. Systems + Scheduler. The four rule systems write disjoint
    //    components, so the scheduler runs them in parallel.
    // ------------------------------------------------------------------
    boids::SpatialGrid grid;
    grid.Configure(cfg.neighbor_radius, static_cast<float>(cfg.width), static_cast<float>(cfg.height));

    // Phase 1 (rules): separation / alignment / cohesion / bounds, parallel.
    ekit::Scheduler steering_scheduler(cfg.threads);
    steering_scheduler.AddSystem(boids::SeparationSystem{&grid, cfg.separation_radius})
                      .AddSystem(boids::AlignmentSystem{&grid, cfg.neighbor_radius})
                      .AddSystem(boids::CohesionSystem{&grid, cfg.neighbor_radius})
                      .AddSystem(boids::BoundsSystem{nullptr, cfg});

    // Phase 2 (apply + integrate): combine accumulators into Velocity, then
    // advance Position. Kept in a separate phase so phase 1 reads the
    // frame-start state (no circular read/write dataflow).
    ekit::Scheduler integrate_scheduler(cfg.threads);
    integrate_scheduler.AddSystem(boids::UpdateVelocitySystem{cfg})
                       .AddSystem(boids::IntegrateSystem{cfg});

    // ------------------------------------------------------------------
    // 4. Simulation + rendering loop.
    // ------------------------------------------------------------------
    std::error_code ec;
    std::filesystem::create_directories(cfg.out_dir, ec);

    boids::Canvas canvas(cfg.width, cfg.height);
    const boids::Color background{14, 18, 30};

    const auto t_start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < cfg.frames; ++frame) {
        grid.Build(world); // synchronous; read-only during the scheduler phase
        steering_scheduler.Run(world);
        integrate_scheduler.Run(world);

        canvas.Clear(background);
        world.Query<boids::Position, boids::Velocity, boids::BoidTag>().ForEach(
            [&](ekit::Entity, const boids::Position& p, const boids::Velocity& v, const boids::BoidTag&) {
                boids::DrawBoid(canvas, p, v);
            });

        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04d.ppm", frame);
        const std::string path = cfg.out_dir + "/" + name;
        canvas.WritePPM(path);
    }

    const auto t_end = std::chrono::steady_clock::now();
    const double elapsed_s = std::chrono::duration<double>(t_end - t_start).count();

    // ------------------------------------------------------------------
    // 5. Final stats.
    // ------------------------------------------------------------------
    double avg_speed = 0.0;
    std::size_t count = 0;
    world.Query<boids::Velocity, boids::BoidTag>().ForEach(
        [&](const boids::Velocity& v, const boids::BoidTag&) {
            avg_speed += std::sqrt(v.x * v.x + v.y * v.y);
            ++count;
        });
    avg_speed /= count > 0 ? static_cast<double>(count) : 1.0;

    const int flocks = boids::CountFlocks(world, grid, cfg.neighbor_radius);

    std::printf("boids       : %zu\n", count);
    std::printf("avg speed   : %.3f px/frame\n", avg_speed);
    std::printf("flocks      : %d\n", flocks);
    std::printf("frames      : %d -> %s/\n", cfg.frames, cfg.out_dir.c_str());
    std::printf("threads     : %u\n", cfg.threads);
    std::printf("avg fps     : %.1f (%d frames in %.2f s)\n",
                static_cast<double>(cfg.frames) / elapsed_s, cfg.frames, elapsed_s);
    std::printf("phases      : steering (rules, parallel) -> integrate\n");
    return 0;
}
