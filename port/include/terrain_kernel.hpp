#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

// Pure, global-coordinate terrain. No renderer, saves, or game assets required.
namespace terrain {
constexpr int    Resolution = 16;
constexpr double BaseSize = 512;
// MorrowCraft's default 1,000-mile experiment: 112,640 units per mile.
constexpr double Radius = 112640000;
constexpr int    MaxLevel = 18;

struct Tile {
    int64_t x, z;
    int     level;
    auto    operator<=>(const Tile &) const = default;

    double size() const { return std::ldexp(BaseSize, level); }

    double left() const { return x * size(); }

    double top() const { return z * size(); }
};

inline uint64_t Mix(uint64_t v) {
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

inline double Noise(double x, double z) {
    auto   ix = int64_t(std::floor(x)), iz = int64_t(std::floor(z));
    double u = x - ix, v = z - iz;
    auto   smooth = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    u = smooth(u);
    v = smooth(v);
    auto h = [](int64_t a, int64_t b) { return double(Mix(uint64_t(a) ^ Mix(uint64_t(b) + 4271)) >> 11) / 9007199254740992.0 * 2 - 1; };
    return std::lerp(std::lerp(h(ix, iz), h(ix + 1, iz), u), std::lerp(h(ix, iz + 1), h(ix + 1, iz + 1), u), v);
}

inline double NaturalHeight(double x, double z) {
    double fade = std::clamp((std::hypot(x, z) - 1250) / 900, 0.0, 1.0);
    fade = fade * fade * (3 - 2 * fade);
    double hills = fade * (95 * Noise(x / 2400, z / 2400) + 22 * Noise(x / 850, z / 850) + 5 * Noise(x / 280, z / 280));
    double mx = x / 6, mz = z / 6;
    double basin = std::clamp((std::hypot(mx, mz) - 4500) / 8500, 0.0, 1.0);
    basin = basin * basin * (3 - 2 * basin);
    if (basin == 0) {
        return hills;
    }
    // Warped, correlated ridges form ranges with branching spurs, not isolated noise domes.
    double wx = mx + 6500 * Noise(mx / 27000 + 17, mz / 27000 - 31);
    double wz = mz + 6500 * Noise(mx / 27000 - 48, mz / 27000 + 9);
    double mass = std::clamp((Noise(wx / 62000 + 4, wz / 62000 + 7) + .45) / .8, 0.0, 1.0);
    double ridges = 0, weight = 1, amplitude = .64, scale = 24000;
    for (int octave = 0; octave < 6; ++octave) {
        double ridge = 1 - std::abs(Noise(wx / scale + 11, wz / scale - 8));
        ridge *= ridge;
        ridges += amplitude * ridge * weight;
        weight = std::clamp(ridge * 1.7, 0.0, 1.0);
        amplitude *= .47;
        scale *= .48;
    }
    return hills + basin * mass * (24000 * ridges + 900 * Noise(wx / 9000, wz / 9000));
}

struct Foundation {
    double x, z, halfX, halfZ, height;
};

inline std::vector<Foundation> foundations;

struct Road {
    double x, z, endX, endZ, height, endHeight;
};

inline std::vector<Road> roads;

inline double Height(double x, double z) {
    double weighted = 0, weights = 0, influence = 0;
    for (const auto &p : foundations) {
        double d = std::hypot(std::max(0.0, std::abs(x - p.x) - p.halfX), std::max(0.0, std::abs(z - p.z) - p.halfZ));
        // A building's footprint is a hard constraint. Nearby placements may
        // grade the surroundings, but must never lift or erode its foundation.
        if (d < .00001) {
            return p.height;
        }
        if (d >= 400) {
            continue;
        }
        double t = d / 400;
        t = t * t * (3 - 2 * t);
        double w = (1 - t) / (d * d);
        weighted += w * p.height;
        weights += w;
        influence = std::max(influence, 1 - t);
    }
    double base = weights > 0 ? std::lerp(NaturalHeight(x, z), weighted / weights, influence) : NaturalHeight(x, z);
    double roadSum = 0, roadWeight = 0, nearest = 1e30;
    for (const auto &r : roads) {
        double dx = r.endX - r.x, dz = r.endZ - r.z, length = dx * dx + dz * dz;
        double t = length ? std::clamp(((x - r.x) * dx + (z - r.z) * dz) / length, 0.0, 1.0) : 0;
        double d = std::hypot(x - r.x - t * dx, z - r.z - t * dz);
        if (d >= 200) {
            continue;
        }
        double w = std::pow(1 - d / 200, 2) / (1 + d * d);
        roadSum += w * std::lerp(r.height, r.endHeight, t);
        roadWeight += w;
        nearest = std::min(nearest, d);
    }
    if (roadWeight) {
        double t = std::clamp((nearest - 55) / 145, 0.0, 1.0);
        t = t * t * (3 - 2 * t);
        base = std::lerp(roadSum / roadWeight, base, t);
    }
    return base;
}

// Surface of the actual finest terrain triangles, also used by draped roads.
inline double SurfaceHeight(double x, double z) {
    double sx = std::floor(x / 32) * 32, sz = std::floor(z / 32) * 32, u = (x - sx) / 32, v = (z - sz) / 32;
    double b = Height(sx + 32, sz), c = Height(sx, sz + 32);
    return u + v <= 1 ? Height(sx, sz) * (1 - u - v) + b * u + c * v : b * (1 - v) + c * (1 - u) + Height(sx + 32, sz + 32) * (u + v - 1);
}

inline double Distance(const Tile &t, double x, double z) {
    return std::hypot(std::max({t.left() - x, 0.0, x - t.left() - t.size()}),
                      std::max({t.top() - z, 0.0, z - t.top() - t.size()}));
}

inline void SelectNode(std::vector<Tile> &out, Tile t, double x, double z, int minimumLevel = 0) {
    double distance = Distance(t, x, z);
    if (distance > Radius) {
        return;
    }
    if (t.level > minimumLevel && distance < t.size() * 2.5) {
        for (int dz = 0; dz < 2; dz++) {
            for (int dx = 0; dx < 2; dx++) {
                SelectNode(out, {t.x * 2 + dx, t.z * 2 + dz, t.level - 1}, x, z, minimumLevel);
            }
        }
    } else {
        out.push_back(t);
    }
}

inline std::vector<Tile> Select(double x, double z, int minimumLevel = 0) {
    std::vector<Tile> out;
    double            size = std::ldexp(BaseSize, MaxLevel);
    int64_t           cx = int64_t(std::floor(x / size)), cz = int64_t(std::floor(z / size));
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            SelectNode(out, {cx + dx, cz + dz, MaxLevel}, x, z, minimumLevel);
        }
    }
    return out;
}

using Selection = std::map<Tile, int>;

inline int LevelAt(const Selection &selection, double x, double z) {
    for (int l = 0; l <= MaxLevel; l++) {
        double s = std::ldexp(BaseSize, l);
        if (selection.contains({int64_t(std::floor(x / s)), int64_t(std::floor(z / s)), l})) {
            return l;
        }
    }
    return -1;
}

inline double EdgeHeight(double x, double z, int level, bool alongX) {
    double step = std::ldexp(BaseSize, level) / Resolution;
    double p = alongX ? x : z, lo = std::floor(p / step) * step;
    return std::lerp(Height(alongX ? lo : x, alongX ? z : lo),
                     Height(alongX ? lo + step : x, alongX ? z : lo + step), (p - lo) / step);
}
} // namespace terrain
