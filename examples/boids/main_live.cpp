// ekit Boids - real-time live viewer (GLFW + OpenGL).
//
// Opens a window and renders the flock in real time with OpenGL (rendering is
// GPU accelerated, vsync matches the monitor refresh rate). Controls:
//   SPACE  pause / resume      R  reset flock
//   UP/DN  simulation speed    ESC  quit
//
// GLFW is NOT part of this repository: configure with
//   -DEKIT_GLFW_ROOT=<path to a local GLFW checkout>  (e.g. E:/Github/glfw)
// Run with --frames N to auto-quit after N simulation steps (useful for tests).

#include <GLFW/glfw3.h>

#include "boids.hpp"
#include "canvas.hpp"
#include "cli.hpp"
#include "render_helpers.hpp"

#include <cstdio>
#include <random>
#include <string>

namespace {

void DrawBoidGL(const boids::Position& p, const boids::Velocity& v) {
    const boids::Color color = boids::BoidColor(v, 3.6f);
    const boids::Vec2 vel(v.x, v.y);
    const float speed = vel.Length();
    if (speed < 0.05f) {
        const float r = 3.f;
        glColor3ub(color.r, color.g, color.b);
        glBegin(GL_TRIANGLE_FAN);
        for (int i = 0; i <= 16; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / 16.f;
            glVertex2f(p.x + std::cos(a) * r, p.y + std::sin(a) * r);
        }
        glEnd();
        return;
    }
    const boids::Vec2 dir = vel.Normalized();
    const boids::Vec2 perp(-dir.y, dir.x);
    const float size = 6.f;
    const boids::Vec2 tip = boids::Vec2(p.x, p.y) + dir * size;
    const boids::Vec2 base = boids::Vec2(p.x, p.y) - dir * (size * 0.45f);
    const boids::Vec2 b1 = base + perp * (size * 0.45f);
    const boids::Vec2 b2 = base - perp * (size * 0.45f);

    glColor3ub(color.r, color.g, color.b);
    glBegin(GL_TRIANGLES);
    glVertex2f(tip.x, tip.y);
    glVertex2f(b1.x, b1.y);
    glVertex2f(b2.x, b2.y);
    glEnd();
}

} // namespace

int main(int argc, char** argv) {
    boids::Config cfg = boids::ParseConfig(argc, argv);
    std::mt19937 rng(cfg.seed);

    // ------------------------------------------------------------------
    // GLFW window (legacy OpenGL context; immediate mode - no loader needed).
    // ------------------------------------------------------------------
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "error: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

    GLFWwindow* window =
        glfwCreateWindow(cfg.width, cfg.height, "ekit Boids - live", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "error: could not create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(cfg.vsync ? 1 : 0); // 1 = match monitor refresh, 0 = uncapped

    // Track the current window/framebuffer size so the world can follow it.
    int win_w = cfg.width, win_h = cfg.height; // window content size
    int fb_w = cfg.width, fb_h = cfg.height;   // framebuffer (pixel) size
    glfwGetWindowSize(window, &win_w, &win_h);
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) {
        fb_w = cfg.width;
        fb_h = cfg.height;
    }
    std::printf("window size: %dx%d | framebuffer size: %dx%d\n", win_w, win_h, fb_w, fb_h);

    glViewport(0, 0, fb_w, fb_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(fb_w), static_cast<double>(fb_h), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(14.f / 255.f, 18.f / 255.f, 30.f / 255.f, 1.f);


    // ------------------------------------------------------------------
    // World + systems (same setup as the headless demo).
    // ------------------------------------------------------------------
    ekit::World world;
    world.RegisterComponents<boids::Position, boids::Velocity, boids::BoidTag,
                             boids::Separation, boids::Alignment, boids::Cohesion,
                             boids::BoundsSteer, boids::MouseSteer>();
    boids::SpawnBoids(world, cfg, rng);

    // The world follows the window: bounds + grid are updated on resize below.
    boids::WorldBounds bounds{static_cast<float>(fb_w), static_cast<float>(fb_h)};
    boids::SpatialGrid grid;
    grid.Configure(cfg.neighbor_radius, bounds.width, bounds.height);

    boids::MouseState mouse;
    ekit::Scheduler steering_scheduler(cfg.threads);
    steering_scheduler.AddSystem(boids::SeparationSystem{&grid, cfg.separation_radius})
                      .AddSystem(boids::AlignmentSystem{&grid, cfg.neighbor_radius})
                      .AddSystem(boids::CohesionSystem{&grid, cfg.neighbor_radius})
                      .AddSystem(boids::BoundsSystem{&bounds, cfg})
                      .AddSystem(boids::MouseSystem{&mouse, cfg});

    ekit::Scheduler integrate_scheduler(cfg.threads);
    integrate_scheduler.AddSystem(boids::UpdateVelocitySystem{cfg})
                       .AddSystem(boids::IntegrateSystem{cfg});

    // ------------------------------------------------------------------
    // Real-time loop.
    // ------------------------------------------------------------------
    bool paused = false;
    bool reset_requested = false;
    float speed_mult = 1.f;
    bool prev_space = false, prev_r = false, prev_up = false, prev_down = false;

    // The window stays open until the player closes it (ESC / window X).
    // Passing --frames N explicitly opts into auto-quit after N steps.
    if (cfg.frames_set == 0) {
        cfg.frames = 0;
    }
    int steps_run = 0;
    const bool auto_quit = cfg.frames > 0;

    double last_title = glfwGetTime();
    unsigned long long frame_count = 0;
    double fps = 0.0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const bool space = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        const bool r = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        const bool up = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
        const bool down = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;

        if (space && !prev_space) paused = !paused;
        if (r && !prev_r) reset_requested = true;
        if (up && !prev_up) speed_mult = speed_mult * 1.25f > 4.f ? 4.f : speed_mult * 1.25f;
        if (down && !prev_down) speed_mult = speed_mult * 0.8f < 0.25f ? 0.25f : speed_mult * 0.8f;
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        prev_space = space;
        prev_r = r;
        prev_up = up;
        prev_down = down;

        if (reset_requested) {
            world.ClearAll();
            boids::SpawnBoids(world, cfg, rng);
            reset_requested = false;
        }

        // Handle window resizes: scale the flock to fill the whole window,
        // rebuild the spatial grid and update the GL projection.
        {
            int new_win_w = 0, new_win_h = 0, new_fb_w = 0, new_fb_h = 0;
            glfwGetWindowSize(window, &new_win_w, &new_win_h);
            glfwGetFramebufferSize(window, &new_fb_w, &new_fb_h);
            if (new_fb_w > 0 && new_fb_h > 0 &&
                (new_fb_w != fb_w || new_fb_h != fb_h)) {
                const float scale_x = static_cast<float>(new_fb_w) / static_cast<float>(fb_w);
                const float scale_y = static_cast<float>(new_fb_h) / static_cast<float>(fb_h);
                world.Query<boids::Position, boids::BoidTag>().ForEach(
                    [&](ekit::Entity, boids::Position& p, const boids::BoidTag&) {
                        p.x *= scale_x;
                        p.y *= scale_y;
                    });
                win_w = new_win_w;
                win_h = new_win_h;
                fb_w = new_fb_w;
                fb_h = new_fb_h;
                bounds.width = static_cast<float>(fb_w);
                bounds.height = static_cast<float>(fb_h);
                grid.Configure(cfg.neighbor_radius, bounds.width, bounds.height);
                glViewport(0, 0, fb_w, fb_h);
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                glOrtho(0.0, static_cast<double>(fb_w), static_cast<double>(fb_h), 0.0, -1.0, 1.0);
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();
            }
        }

        // Track the cursor: boids are attracted to it, repelled while the
        // left button is held. Cursor coords are in window units, so scale to
        // framebuffer units to stay consistent with the world.
        {
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(window, &mx, &my);
            if (win_w > 0 && win_h > 0) {
                mouse.x = static_cast<float>(mx) * (static_cast<float>(fb_w) / static_cast<float>(win_w));
                mouse.y = static_cast<float>(my) * (static_cast<float>(fb_h) / static_cast<float>(win_h));
            } else {
                mouse.x = static_cast<float>(mx);
                mouse.y = static_cast<float>(my);
            }
            mouse.button_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        }

        if (!paused) {
            // Speed multiplier = number of simulation steps per rendered frame.
            const int steps = static_cast<int>(speed_mult + 0.5f);
            for (int s = 0; s < steps; ++s) {
                grid.Build(world);
                steering_scheduler.Run(world);
                integrate_scheduler.Run(world);
                ++steps_run;
                if (auto_quit && steps_run >= cfg.frames) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
        }

        // Render (GPU).
        glClear(GL_COLOR_BUFFER_BIT);
        world.Query<boids::Position, boids::Velocity, boids::BoidTag>().ForEach(
            [&](ekit::Entity, const boids::Position& p, const boids::Velocity& v, const boids::BoidTag&) {
                DrawBoidGL(p, v);
            });
        glfwSwapBuffers(window);

        // HUD in the window title (boids / flocks / fps / speed).
        ++frame_count;
        const double now = glfwGetTime();
        if (now - last_title >= 0.25) {
            fps = static_cast<double>(frame_count) / (now - last_title);
            frame_count = 0;
            last_title = now;

            char title[192];
            std::snprintf(title, sizeof(title),
                          "ekit Boids | boids: %zu | flocks: %d | fps: %.0f | speed x%.2f%s",
                          world.GetAliveEntityCount(),
                          boids::CountFlocks(world, grid, cfg.neighbor_radius), fps, speed_mult,
                          paused ? " | PAUSED" : "");
            glfwSetWindowTitle(window, title);
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    std::printf("done: %d simulation steps, %zu boids, %d flocks\n", steps_run,
                world.GetAliveEntityCount(),
                boids::CountFlocks(world, grid, cfg.neighbor_radius));
    return 0;
}
