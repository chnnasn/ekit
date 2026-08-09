#pragma once
// Shared boid drawing helpers (speed-based coloring + oriented triangles).

#include "boids.hpp"
#include "canvas.hpp"

namespace boids {

inline Color BoidColor(const Velocity& v, float max_speed) {
    const float speed = std::sqrt(v.x * v.x + v.y * v.y);
    const float t = speed / max_speed;
    // slow = teal, fast = orange
    const Color slow{70, 200, 220};
    const Color fast{255, 150, 60};
    return Color::Lerp(slow, fast, t < 0.f ? 0.f : (t > 1.f ? 1.f : t));
}

inline void DrawBoid(Canvas& canvas, const Position& p, const Velocity& v) {
    const Color color = BoidColor(v, 3.6f);
    const Vec2 vel(v.x, v.y);
    const float speed = vel.Length();
    if (speed < 0.05f) {
        canvas.FillCircle(p.x, p.y, 3.f, color);
        return;
    }
    const Vec2 dir = vel.Normalized();
    const Vec2 perp(-dir.y, dir.x);
    const float size = 6.f;
    const Vec2 tip = Vec2(p.x, p.y) + dir * size;
    const Vec2 base = Vec2(p.x, p.y) - dir * (size * 0.45f);
    canvas.FillTriangle(tip, base + perp * (size * 0.45f), base - perp * (size * 0.45f), color);
}

} // namespace boids
