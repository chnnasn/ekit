#pragma once
// Shared command-line parsing for the boids demo and live viewer.

#include "boids.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace boids {

inline void PrintUsage(const char* program) {
    std::printf(
        "Usage: %s [options]\n"
        "  --boids N       number of boids (default 220)\n"
        "  --frames N      frames to run, 0 = infinite (headless: writes N frames;\n"
        "                  live: auto-quit after N steps) (default 180 / 0)\n"
        "  --width N       world/render width (default 800)\n"
        "  --height N      world/render height (default 600)\n"
        "  --seed N        random seed (default 20260810)\n"
        "  --threads N     scheduler threads, 0 = hardware concurrency (default 0)\n"
        "  --vsync 0|1     live viewer: 1 = match monitor refresh, 0 = uncapped (default 1)\n"
        "  --out DIR       output directory for PPM frames (headless only, default frames)\n"
        "  --help          show this help\n",
        program);
}

inline Config ParseConfig(int argc, char** argv) {
    Config cfg;
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
            cfg.boid_count = std::stoi(next());
        } else if (arg == "--frames") {
            cfg.frames = std::stoi(next());
            cfg.frames_set = 1;
        } else if (arg == "--width") {
            cfg.width = std::stoi(next());
        } else if (arg == "--height") {
            cfg.height = std::stoi(next());
        } else if (arg == "--seed") {
            cfg.seed = std::stoul(next());
        } else if (arg == "--threads") {
            cfg.threads = std::stoul(next());
        } else if (arg == "--vsync") {
            cfg.vsync = std::stoi(next());
        } else if (arg == "--out") {
            cfg.out_dir = next();
        } else if (arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "error: unknown option %s\n", arg.c_str());
            PrintUsage(argv[0]);
            std::exit(2);
        }
    }
    return cfg;
}

} // namespace boids
