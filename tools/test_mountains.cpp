#include <cassert>
#include <cstdio>

#include "terrain_kernel.hpp"

int main() {
    using namespace terrain;
    int    upland = 0, gentle = 0, cliffs = 0, benches = 0, valleys = 0, soil = 0;
    double peak = 0;
    for (int z = -350000; z < 450000; z += 2000) {
        for (int x = -400000; x < 600000; x += 2000) {
            auto   f = MountainAt(x, z);
            double dx = (MountainHeight(x + 32, z) - MountainHeight(x - 32, z)) / 64;
            double dz = (MountainHeight(x, z + 32) - MountainHeight(x, z - 32)) / 64;
            double gradient = std::hypot(dx, dz), normalY = 1 / std::sqrt(1 + gradient * gradient);
            assert(std::isfinite(f.height));
            assert(f.valley >= 0 && f.valley <= 1 && f.bench >= 0 && f.bench <= 1);
            peak = std::max(peak, f.height);
            if (f.height > 2000) {
                ++upland;
                gentle += gradient < .268; // 15 degrees
                cliffs += gradient > .839; // 40 degrees
                soil += RockExposure(x, z, f.height, normalY) < .4;
                valleys += f.valley > .9;
                benches += f.bench > .5;
            }
        }
    }
    // Preserve towering mountains while also providing traversable uplands.
    std::fprintf(stderr,"morphology peak %.0f upland %d gentle %d cliffs %d soil %d valleys %d benches %d\n",peak,upland,gentle,cliffs,soil,valleys,benches);
    assert(peak > 20000 && peak < 35000);
    assert(gentle > upland * .35 && cliffs > upland * .02 && cliffs < upland * .3);
    assert(soil > upland * .5 && valleys > 100 && benches > 100);
    assert(RockExposure(100000, 30000, 4000, 1) < .01);
    assert(RockExposure(100000, 30000, 4000, .5) > .99);
    assert(SlopeVegetation(1) == 1 && SlopeVegetation(.5) == 0);
    // Global coordinates, town protection, approach boundaries and local continuity.
    for (double origin : {0., 1e8, -1e8, 1e12, -1e12}) {
        for (int k = -30; k <= 30; ++k) {
            double x = origin + 24000 + k * 2048., z = -origin * .37 + k * 1337.;
            assert(std::isfinite(MountainHeight(x, z)));
            for (double spacing : {32., 1024., 32.}) {
                assert(NaturalHeight(x,z,spacing)==NaturalHeightUncached(x,z,spacing));
            }
            assert(std::abs(MountainHeight(x + .001, z) - MountainHeight(x - .001, z)) < .1);
        }
    }
    for (int z = -2800; z <= 2800; z += 100) {
        for (int x = -2800; x <= 2800; x += 100) {
            if (std::hypot(x, z) > 3000) {
                continue;
            }
            double expected = Smooth(1250, 2150, std::hypot(x, z)) * (95 * Noise(x / 2400., z / 2400.) + 22 * Noise(x / 850., z / 850.) + 5 * Noise(x / 280., z / 280.));
            assert(std::abs(MountainHeight(x, z) - expected) < 1e-10);
        }
    }
    std::printf("PASS mountain morphology: peak %.0f, uplands %d, gentle %d, cliffs %d, valleys %d, benches %d; town and trillion-unit continuity\n", peak, upland, gentle, cliffs, valleys, benches);
}
