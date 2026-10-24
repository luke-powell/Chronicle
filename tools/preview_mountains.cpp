#include <cstdio>
#include <cstdlib>
#include <fstream>
#ifdef MOUNTAIN_BASELINE
#include "mountain-before.hpp"
#else
#include "terrain_kernel.hpp"
#endif
int main(int argc, char **argv) {
    double        cx = argc > 1 ? std::atof(argv[1]) : 90000, cz = argc > 2 ? std::atof(argv[2]) : 30000, span = argc > 3 ? std::atof(argv[3]) : 180000;
    int           n = argc > 4 ? std::atoi(argv[4]) : 300;
    std::ofstream f(argc > 5 ? argv[5] : "mountains.csv");
    f << "x,z,height,slope,forest,snow,rock\n";
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            double wx = cx + (x - n / 2.) * span / n, wz = cz + (z - n / 2.) * span / n;
            double h = terrain::MountainHeight(wx, wz);
            double dx = (terrain::MountainHeight(wx + 32, wz) - terrain::MountainHeight(wx - 32, wz)) / 64;
            double dz = (terrain::MountainHeight(wx, wz + 32) - terrain::MountainHeight(wx, wz - 32)) / 64;
            double gradient = std::hypot(dx, dz), ny = 1 / std::sqrt(1 + gradient * gradient);
            auto   c = terrain::FinishClimate(terrain::BaseClimateAt(wx, wz, h), h);
#ifdef MOUNTAIN_BASELINE
            double rock = std::clamp(std::max((h - 900) / 2200, (1 - ny - .18) * 3), 0., 1.);
#else
            double rock = terrain::RockExposure(wx, wz, h, ny);
#endif
            f << wx << ',' << wz << ',' << h << ',' << std::atan(gradient) * 180 / 3.141592653589793 << ',' << c.forest << ',' << c.snow << ',' << rock << '\n';
        }
    }
}
