#pragma once
#include "terrain_kernel.hpp"

// One authoritative population for every render LOD; never reseeded or thinned.
namespace forest {
struct Tree {
    double   x, z, y, yaw, scale;
    uint64_t id;
    int      species;
};

inline bool Occupied(double x, double z) {
    for (const auto &p : terrain::Foundations()) {
        if (std::abs(x - p.x) < p.halfX + 90 && std::abs(z - p.z) < p.halfZ + 90) {
            return true;
        }
    }
    for (const auto &r : terrain::Roads()) {
        double dx = r.endX - r.x, dz = r.endZ - r.z, ll = dx * dx + dz * dz;
        double u = ll ? std::clamp(((x - r.x) * dx + (z - r.z) * dz) / ll, 0., 1.) : 0;
        if (std::hypot(x - r.x - u * dx, z - r.z - u * dz) < 120) {
            return true;
        }
    }
    return false;
}

inline std::vector<Tree> Generate(double left, double top, double size) {
    std::vector<Tree> result;
    for (int layer : {0, 2}) {
        double spacing = layer == 0 ? 100 : 72;
        for (auto iz = int64_t(std::floor(top / spacing)); iz * spacing < top + size; iz++) {
            for (auto ix = int64_t(std::floor(left / spacing)); ix * spacing < left + size; ix++) {
                uint64_t seed = terrain::Mix(uint64_t(ix) ^ terrain::Mix(uint64_t(iz) + 9181 + layer * 7193));
                double   x = (ix + .12 + .76 * double(seed & 65535) / 65535) * spacing;
                double   z = (iz + .12 + .76 * double((seed >> 16) & 65535) / 65535) * spacing;
                if (x < left || x >= left + size || z < top || z >= top + size || std::hypot(x, z) < 2200 || Occupied(x, z)) {
                    continue;
                }
                double y = terrain::SurfaceHeight(x, z);
                auto   c = terrain::ClimateAt(x, z, y);
                double density = terrain::VegetationDensity(x, z, c);
                if (layer == 2) {
                    density *= .42;
                }
                if (double((seed >> 32) & 65535) / 65535 > density) {
                    continue;
                }
                auto river = terrain::RiverAt(x, z);
                if (river.distance < river.width + 30) {
                    continue;
                }
                double gradient = std::hypot(terrain::Height(x + 32, z) - y, terrain::Height(x, z + 32) - y) / 32;
                double slopeCoverage = terrain::SlopeVegetation(1 / std::sqrt(1 + gradient * gradient));
                if (double(terrain::Mix(seed + 1181) >> 11) / 9007199254740992. > slopeCoverage) {
                    continue;
                }
                int    species = terrain::TreeSpecies(c, double(terrain::Mix(seed + 817) >> 11) / 9007199254740992.);
                double scale = .9 + double((seed >> 24) & 255) / 255 * 1.3;
                if (layer == 2) {
                    if (species == 3 && (seed & 1)) {
                        species = 4;
                        scale = .7 + double((seed >> 24) & 255) / 255;
                    } else {
                        scale = .25 + double((seed >> 24) & 255) / 255 * .4;
                    }
                }
                result.push_back({x, z, y, double(seed >> 48) / 65535 * 6.28318530718, scale, seed, species});
            }
        }
    }
    return result;
}

inline std::array<double, 3> Weights(double distance) {
    double a = terrain::Smooth(1800, 2600, distance), b = terrain::Smooth(5000, 7000, distance);
    return {1 - a, a * (1 - b), b};
}
} // namespace forest
