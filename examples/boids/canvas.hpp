#pragma once
// Minimal software rasterizer used to render boids as oriented triangles.
// Output is written as PPM (P6) images, which the included render.ps1 script
// converts to PNG / animated GIF.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace boids {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;

    Vec2() = default;
    Vec2(float X, float Y) : x(X), y(Y) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2& operator+=(const Vec2& o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    Vec2& operator-=(const Vec2& o) {
        x -= o.x;
        y -= o.y;
        return *this;
    }
    Vec2& operator*=(float s) {
        x *= s;
        y *= s;
        return *this;
    }

    float Length() const { return std::sqrt(x * x + y * y); }
    Vec2 Normalized() const {
        const float len = Length();
        return len > 1e-6f ? Vec2(x / len, y / len) : Vec2{};
    }
    static float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
};

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    static Color Lerp(const Color& a, const Color& b, float t) {
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        return {
            static_cast<uint8_t>(a.r + (b.r - a.r) * t),
            static_cast<uint8_t>(a.g + (b.g - a.g) * t),
            static_cast<uint8_t>(a.b + (b.b - a.b) * t),
        };
    }
};

class Canvas {
public:
    Canvas(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 3, 0) {}

    int Width() const { return width_; }
    int Height() const { return height_; }

    // Raw RGB pixel data (width*height*3 bytes, row-major, top-down).
    const uint8_t* RawData() const { return pixels_.data(); }

    void Clear(const Color& c) {
        for (int i = 0; i < width_ * height_; ++i) {
            pixels_[static_cast<std::size_t>(i) * 3 + 0] = c.r;
            pixels_[static_cast<std::size_t>(i) * 3 + 1] = c.g;
            pixels_[static_cast<std::size_t>(i) * 3 + 2] = c.b;
        }
    }

    void SetPixel(int x, int y, const Color& c) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) {
            return;
        }
        std::size_t offset = (static_cast<std::size_t>(y) * width_ + x) * 3;
        pixels_[offset + 0] = c.r;
        pixels_[offset + 1] = c.g;
        pixels_[offset + 2] = c.b;
    }

    void FillCircle(float cx, float cy, float radius, const Color& c) {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int x1 = std::min(width_ - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int y1 = std::min(height_ - 1, static_cast<int>(std::ceil(cy + radius)));
        const float r2 = radius * radius;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                if (dx * dx + dy * dy <= r2) {
                    SetPixel(x, y, c);
                }
            }
        }
    }

    // Flat-top / flat-bottom scanline triangle fill.
    void FillTriangle(Vec2 a, Vec2 b, Vec2 c, const Color& col) {
        if (a.y > b.y) std::swap(a, b);
        if (a.y > c.y) std::swap(a, c);
        if (b.y > c.y) std::swap(b, c);

        const int y_start = std::max(0, static_cast<int>(std::ceil(a.y)));
        const int y_end = std::min(height_ - 1, static_cast<int>(std::floor(c.y)));

        auto x_at = [](const Vec2& p0, const Vec2& p1, float y) -> float {
            if (std::abs(p1.y - p0.y) < 1e-6f) {
                return p0.x;
            }
            const float t = (y - p0.y) / (p1.y - p0.y);
            return p0.x + t * (p1.x - p0.x);
        };

        for (int y = y_start; y <= y_end; ++y) {
            const float fy = static_cast<float>(y) + 0.5f;
            float x1, x2;
            if (fy < b.y) {
                x1 = x_at(a, b, fy);
                x2 = x_at(a, c, fy);
            } else {
                x1 = x_at(b, c, fy);
                x2 = x_at(a, c, fy);
            }
            if (x1 > x2) {
                std::swap(x1, x2);
            }
            const int ix0 = std::max(0, static_cast<int>(std::ceil(x1)));
            const int ix1 = std::min(width_ - 1, static_cast<int>(std::floor(x2)));
            for (int x = ix0; x <= ix1; ++x) {
                SetPixel(x, y, col);
            }
        }
    }

    bool WritePPM(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return false;
        }
        out << "P6\n" << width_ << ' ' << height_ << "\n255\n";
        out.write(reinterpret_cast<const char*>(pixels_.data()),
                  static_cast<std::streamsize>(pixels_.size()));
        return static_cast<bool>(out);
    }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> pixels_;
};

} // namespace boids

