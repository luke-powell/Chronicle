#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Stable global anchors: every renderer, collision query and worker uses these.
namespace foresttown {
struct Site {
    double x, z, radius;
};

inline constexpr Site       Village{-14000, -10000, 1450};
inline constexpr Site       Treehouse{Village.x - 1850, Village.z + 100, 780};
inline constexpr Site       Treant{Village.x - 1050, Village.z + 2300, 480};
inline constexpr double     Elevation = 200;
inline constexpr std::array Sites{Village, Treehouse, Treant};

struct Path {
    double x, z, endX, endZ, height, endHeight;
};

inline constexpr std::array Paths{
    Path{-2000,            -150,             -4500,          -1600,            0,   200},
    Path{-4500,            -1600,            -8500,          -5000,            200, 200},
    Path{-8500,            -5000,            Village.x + 40, Village.z - 1000, 200, 200},
    Path{Village.x + 40,   Village.z - 1000, Village.x + 40, Village.z - 700,  200, 200},
    // s01's south entrance faces Matataki's west exit after a quarter turn.
    Path{Village.x - 1100, Village.z + 100,  Treehouse.x,    Treehouse.z,      200, 200},
    // A dry bank alongside the fixed northern outlet leads to Treant.
    Path{Village.x - 900,  Village.z + 1550, Treant.x + 150, Treant.z - 100,   200, 200}
};

inline double Smooth(double a, double b, double x) {
    x = std::clamp((x - a) / (b - a), 0., 1.);
    return x * x * (3 - 2 * x);
}

inline double PathDistance(double x, double z) {
    double distance = 1e30;
    for (auto p : Paths) {
        double dx = p.endX - p.x, dz = p.endZ - p.z;
        double t = std::clamp(((x - p.x) * dx + (z - p.z) * dz) / (dx * dx + dz * dz), 0., 1.);
        distance = std::min(distance, std::hypot(x - p.x - t * dx, z - p.z - t * dz));
    }
    return distance;
}

// The three original EDITAREA_RECT footprints, plus the fixed northern
// reservoir/outlet. Keep the U-shaped village, rather than a circular clearing.
inline double VillageDistance(double x, double z) {
    x -= Village.x;
    z -= Village.z;
    constexpr std::array<std::array<double, 4>, 4> areas{
        {{{-1100, -500, -450, 900}}, {{-900, -800, 720, -130}}, {{500, -350, 1200, 1000}}, {{-1100, 900, 1200, 1600}}}
    };
    double d = 1e30;
    for (auto r : areas) {
        d = std::min(d, std::hypot(std::max({r[0] - x, 0., x - r[2]}), std::max({r[1] - z, 0., z - r[3]})));
    }
    return d;
}

inline double TreehouseDistance(double x, double z) {
    // Original s01 floor and surrounding cliffs, turned toward the village.
    double lx = Treehouse.z - z, lz = x - Treehouse.x;
    return std::hypot(std::max({-210 - lx, 0., lx - 280}), std::max({-150 - lz, 0., lz - 750}));
}

inline double Clearing(double x, double z) {
    double weight = 1 - Smooth(0, 180, VillageDistance(x, z));
    weight = std::max(weight, 1 - Smooth(0, 180, TreehouseDistance(x, z)));
    for (auto s : {Treant}) {
        weight = std::max(weight, 1 - Smooth(s.radius, s.radius + 250, std::hypot(x - s.x, z - s.z)));
    }
    return weight;
}

inline double Grade(double x, double z, double natural) {
    double graded = std::lerp(natural, Elevation, Clearing(x, z));
    double distance = 1e30, target = 0;
    for (auto p : Paths) {
        double dx = p.endX - p.x, dz = p.endZ - p.z;
        double t = std::clamp(((x - p.x) * dx + (z - p.z) * dz) / (dx * dx + dz * dz), 0., 1.);
        double d = std::hypot(x - p.x - t * dx, z - p.z - t * dz);
        if (d < distance) {
            distance = d;
            target = std::lerp(p.height, p.endHeight, t);
        }
    }
    graded = std::lerp(graded, target, 1 - Smooth(70, 250, distance));
    // Keep the original reservoir waterfall supplied by a forest ridge. Its
    // lip is at local z=1450, 250 units above the village; the back slopes
    // return to the surrounding biome instead of ending at a map wall.
    double localZ = z - Village.z, across = std::abs(x - Village.x - 850);
    double ridge = 250 * Smooth(1410, 1450, localZ) * (1 - Smooth(1550, 2200, localZ)) * (1 - Smooth(110, 550, across));
    return graded + ridge;
}

inline bool Occupied(double x, double z) {
    if (VillageDistance(x, z) < 100) {
        return true;
    }
    if (TreehouseDistance(x, z) < 60) {
        return true;
    }
    for (auto s : {Treant}) {
        if (std::hypot(x - s.x, z - s.z) < s.radius + 60) {
            return true;
        }
    }
    return PathDistance(x, z) < 110;
}

inline double Forest(double x, double z) {
    return (1 - Smooth(3200, 5500, std::hypot(x - Village.x, z - Village.z))) * Smooth(2000, 3000, std::hypot(x, z));
}

inline bool &RiverConnected() {
    static thread_local bool connected = false;
    return connected;
}

struct StreamSample {
    double distance = 1e30, width = 50, water = 187, flowX = 0, flowZ = 0;
    bool   lake = false;
};

inline StreamSample Stream(double x, double z) {
    // The original western creek ends at local (-1050,1600), y=-13.
    // Continue from that exact cross-section into the surrounding forest.
    constexpr std::array<std::array<double, 3>, 4> points{
        {{{-1050, 1600, 187}}, {{-1050, 1800, 187}}, {{-1050, 2100, 186}}, {{-1050, 2140, 185}}}
    };
    x -= Village.x;
    z -= Village.z;
    StreamSample out;
    for (size_t i = 1; i < points.size(); ++i) {
        auto   a = points[i - 1], b = points[i];
        double dx = b[0] - a[0], dz = b[1] - a[1], length = std::hypot(dx, dz);
        double t = std::clamp(((x - a[0]) * dx + (z - a[1]) * dz) / (length * length), 0., 1.);
        double d = std::hypot(x - a[0] - t * dx, z - a[1] - t * dz);
        if (d < out.distance) {
            out = {d, 50, std::lerp(a[2], b[2], t), 6 * dx / length, 6 * dz / length, false};
        }
    }
    double lake = std::hypot(x + 1050, z - 2100);
    // Round the new stream/pond union instead of joining two silhouettes with
    // a hard corner and a discontinuous water level.
    double channel = out.distance - out.width, pool = lake - 100;
    double mix = std::clamp(.5 + .5 * (pool - channel) / 40., 0., 1.);
    double edge = std::lerp(pool, channel, mix) - 40 * mix * (1 - mix);
    out.distance = out.width + edge;
    out.water = std::lerp(185., out.water, mix);
    out.flowX *= mix;
    out.flowZ *= mix;
    out.lake = mix < .001;
    return out;
}
} // namespace foresttown
