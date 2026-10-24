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

inline double Height(double x, double z) {
    double fade = std::clamp((std::hypot(x, z) - 1250) / 900, 0.0, 1.0);
    fade = fade * fade * (3 - 2 * fade);
    return fade * (95 * Noise(x / 2400, z / 2400) + 22 * Noise(x / 850, z / 850) + 5 * Noise(x / 280, z / 280));
}

inline double Distance(const Tile &t, double x, double z) {
    return std::hypot(std::max({t.left() - x, 0.0, x - t.left() - t.size()}),
                      std::max({t.top() - z, 0.0, z - t.top() - t.size()}));
}

inline void SelectNode(std::vector<Tile> &out, Tile t, double x, double z) {
    double distance = Distance(t, x, z);
    if (distance > Radius) {
        return;
    }
    if (t.level && distance < t.size() * 2.5) {
        for (int dz = 0; dz < 2; dz++) {
            for (int dx = 0; dx < 2; dx++) {
                SelectNode(out, {t.x * 2 + dx, t.z * 2 + dz, t.level - 1}, x, z);
            }
        }
    } else {
        out.push_back(t);
    }
}

inline std::vector<Tile> Select(double x, double z) {
    std::vector<Tile> out;
    double            size = std::ldexp(BaseSize, MaxLevel);
    int64_t           cx = int64_t(std::floor(x / size)), cz = int64_t(std::floor(z / size));
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            SelectNode(out, {cx + dx, cz + dz, MaxLevel}, x, z);
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
