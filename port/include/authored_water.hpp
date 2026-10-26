#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace authoredwater {
// Immutable spatial index shared by rendering and collision workers. A narrow
// shoulder covers the supporting 32-unit terrain vertices beneath native banks.
struct Triangle {
    std::array<double, 3> x, z;
    double                bed;
};

struct SurfaceTriangle {
    std::array<double, 3> x, y, z;
    std::array<double, 4> bounds{};
};

struct Beds {
    std::map<std::pair<int, int>, std::vector<SurfaceTriangle>> shell;
    std::vector<std::array<double, 4>>                          protectedAreas;
    std::map<std::pair<int, int>, std::vector<SurfaceTriangle>> cliffs;

    void AddSurface(const SurfaceTriangle &t) {
        auto [ax, bx] = std::minmax_element(t.x.begin(), t.x.end());
        auto [az, bz] = std::minmax_element(t.z.begin(), t.z.end());
        auto indexed = t;
        indexed.bounds = {*ax, *az, *bx, *bz};
        double ux = t.x[1] - t.x[0], uy = t.y[1] - t.y[0], uz = t.z[1] - t.z[0], vx = t.x[2] - t.x[0], vy = t.y[2] - t.y[0], vz = t.z[2] - t.z[0];
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        if (std::abs(ny) < .65 * std::sqrt(nx * nx + ny * ny + nz * nz)) {
            for (int z = int(std::floor((*az - 96) / 256)); z <= int(std::floor((*bz + 96) / 256)); ++z) {
                for (int x = int(std::floor((*ax - 96) / 256)); x <= int(std::floor((*bx + 96) / 256)); ++x) {
                    cliffs[{x, z}].push_back(indexed);
                }
            }
        }

        for (int z = int(std::floor((*az - 400) / 256)); z <= int(std::floor((*bz + 400) / 256)); ++z) {
            for (int x = int(std::floor((*ax - 400) / 256)); x <= int(std::floor((*bx + 400) / 256)); ++x) {
                shell[{x, z}].push_back(indexed);
            }
        }
    }

    double FitShell(double x, double z, double height) const {
        for (auto r : protectedAreas) {
            if (x >= r[0] && z >= r[1] && x <= r[2] && z <= r[3]) {
                return height;
            }
        }
        auto it = shell.find({int(std::floor(x / 256)), int(std::floor(z / 256))});
        if (it == shell.end()) {
            return height;
        }
        double nearest = 400, surface = height;
        for (const auto &t : it->second) {
            double bx = std::max({t.bounds[0] - x, 0., x - t.bounds[2]}), bz = std::max({t.bounds[1] - z, 0., z - t.bounds[3]});
            if (bx * bx + bz * bz > (nearest + 1e-6) * (nearest + 1e-6)) {
                continue;
            }

            double den = (t.z[1] - t.z[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.z[0] - t.z[2]);
            double d = 1e30, y = 0;
            if (std::abs(den) > 1e-6) {
                double a = ((t.z[1] - t.z[2]) * (x - t.x[2]) + (t.x[2] - t.x[1]) * (z - t.z[2])) / den, b = ((t.z[2] - t.z[0]) * (x - t.x[2]) + (t.x[0] - t.x[2]) * (z - t.z[2])) / den;
                if (a >= 0 && b >= 0 && a + b <= 1) {
                    d = 0;
                    y = a * t.y[0] + b * t.y[1] + (1 - a - b) * t.y[2];
                }
            }
            if (d > 0 && nearest == 0) {
                continue;
            }
            if (d > 0) {
                for (int i = 0; i < 3; ++i) {
                    int    j = (i + 1) % 3;
                    double dx = t.x[j] - t.x[i], dz = t.z[j] - t.z[i], len = dx * dx + dz * dz;
                    double a = len ? std::clamp(((x - t.x[i]) * dx + (z - t.z[i]) * dz) / len, 0., 1.) : 0;
                    double dist = std::hypot(x - t.x[i] - a * dx, z - t.z[i] - a * dz);
                    if (dist < d) {
                        d = dist;
                        y = std::lerp(t.y[i], t.y[j], a);
                    }
                }
            }
            if (d < nearest - 1e-6 || (std::abs(d - nearest) < 1e-6 && y > surface)) {
                nearest = d;
                surface = y;
            }
        }
        double u = std::clamp(nearest / 400., 0., 1.);
        u = u * u * (3 - 2 * u);
        return std::lerp(surface - 2, height, u);
    }

    std::map<std::pair<int, int>, std::vector<Triangle>> cells;
    double                                               minX = 1e30, minZ = 1e30, maxX = -1e30, maxZ = -1e30;

    void Add(const Triangle &t) {
        auto [xmin, xmax] = std::minmax_element(t.x.begin(), t.x.end());
        auto [zmin, zmax] = std::minmax_element(t.z.begin(), t.z.end());
        minX = std::min(minX, *xmin);
        maxX = std::max(maxX, *xmax);
        minZ = std::min(minZ, *zmin);
        maxZ = std::max(maxZ, *zmax);
        for (int z = int(std::floor((*zmin - 48) / 256)); z <= int(std::floor((*zmax + 48) / 256)); ++z) {
            for (int x = int(std::floor((*xmin - 48) / 256)); x <= int(std::floor((*xmax + 48) / 256)); ++x) {
                cells[{x, z}].push_back(t);
            }
        }
    }

    double Support(double x, double z, double height) const {
        auto it = shell.find({int(std::floor(x / 256)), int(std::floor(z / 256))});
        if (it == shell.end()) {
            return height;
        }
        for (const auto &t : it->second) {
            if (x < t.bounds[0] || x > t.bounds[2] || z < t.bounds[1] || z > t.bounds[3]) {
                continue;
            }
            double den = (t.z[1] - t.z[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.z[0] - t.z[2]);
            if (std::abs(den) < 1e-6) {
                continue;
            }
            double a = ((t.z[1] - t.z[2]) * (x - t.x[2]) + (t.x[2] - t.x[1]) * (z - t.z[2])) / den, b = ((t.z[2] - t.z[0]) * (x - t.x[2]) + (t.x[0] - t.x[2]) * (z - t.z[2])) / den;
            if (a >= 0 && b >= 0 && a + b <= 1) {
                height = std::max(height, a * t.y[0] + b * t.y[1] + (1 - a - b) * t.y[2]);
            }
        }
        return height;
    }

    double ClearCliffs(double x, double z, double height, double spacing) const {
        auto found = cliffs.find({int(std::floor(x / 256)), int(std::floor(z / 256))});
        if (found == cliffs.end()) {
            return height;
        }
        double radius = std::clamp(spacing * 1.25, 24., 96.);
        for (const auto &t : found->second) {
            double bx = std::max({t.bounds[0] - x, 0., x - t.bounds[2]}), bz = std::max({t.bounds[1] - z, 0., z - t.bounds[3]});
            if (bx * bx + bz * bz > radius * radius) {
                continue;
            }
            double distance = 1e20;
            bool   pos = false, neg = false;
            for (int i = 0; i < 3; ++i) {
                int    j = (i + 1) % 3;
                double dx = t.x[j] - t.x[i], dz = t.z[j] - t.z[i], len = dx * dx + dz * dz;
                double cross = dx * (z - t.z[i]) - dz * (x - t.x[i]);
                pos |= cross > 0;
                neg |= cross < 0;
                double u = len ? std::clamp(((x - t.x[i]) * dx + (z - t.z[i]) * dz) / len, 0., 1.) : 0;
                distance = std::min(distance, std::hypot(x - t.x[i] - u * dx, z - t.z[i] - u * dz));
            }
            if (!(pos && neg)) {
                distance = 0;
            }
            if (distance < radius) {
                height = std::min(height, *std::min_element(t.y.begin(), t.y.end()) - 3);
            }
        }
        return height;
    }

    double Carve(double x, double z, double height, double spacing = 32) const {

        double shoulder = std::clamp(spacing * .75, 6., 256.), original = height;
        if (x < minX - 2 * shoulder || x > maxX + 2 * shoulder || z < minZ - 2 * shoulder || z > maxZ + 2 * shoulder) {
            return height;
        }
        int cellX = int(std::floor(x / 256)), cellZ = int(std::floor(z / 256));
        int reach = spacing <= 32 ? 0 : int(std::ceil(2 * shoulder / 256));
        for (int iz = cellZ - reach; iz <= cellZ + reach; ++iz) {
            for (int ix = cellX - reach; ix <= cellX + reach; ++ix) {
                auto found = cells.find({ix, iz});
                if (found == cells.end()) {
                    continue;
                }
                for (const auto &t : found->second) {
                    double d = 1e30;
                    bool   positive = false, negative = false;
                    for (int i = 0; i < 3; ++i) {
                        int    j = (i + 1) % 3;
                        double dx = t.x[j] - t.x[i], dz = t.z[j] - t.z[i];
                        double cross = dx * (z - t.z[i]) - dz * (x - t.x[i]);
                        positive |= cross > 0;
                        negative |= cross < 0;
                        double length = dx * dx + dz * dz, u = length ? std::clamp(((x - t.x[i]) * dx + (z - t.z[i]) * dz) / length, 0., 1.) : 0;
                        d = std::min(d, std::hypot(x - t.x[i] - u * dx, z - t.z[i] - u * dz));
                    }
                    if (!(positive && negative)) {
                        d = 0;
                    }
                    double u = std::clamp((d - shoulder) / shoulder, 0., 1.);
                    u = u * u * (3 - 2 * u);
                    height = std::min(height, std::lerp(t.bed, original, u));
                }
            }
        }
        return height;
    }
};

inline std::shared_ptr<const Beds> &Current() {
    static thread_local std::shared_ptr<const Beds> beds;
    return beds;
}

inline double FitShell(double x, double z, double height) { return Current() ? Current()->FitShell(x, z, height) : height; }

inline double Carve(double x, double z, double height, double spacing = 32) { return Current() ? Current()->Carve(x, z, height, spacing) : height; }
} // namespace authoredwater
