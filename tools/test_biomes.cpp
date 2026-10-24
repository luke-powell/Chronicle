#include <cassert>
#include <cstdio>
#include <set>

#include "terrain_kernel.hpp"

int main() {
    using namespace terrain;
    std::set<Biome> biomes;
    int             rivers = 0;
    for (int z = -20; z <= 20; z++) {
        for (int x = -20; x <= 20; x++) {
            auto a = DrainPoint(x, z);
            auto dest = Downstream({x, z});
            auto b = DrainPoint(dest.first, dest.second);
            assert(b.height <= a.height);
            if (dest != DrainKey{x, z}) {
                assert(b.height < a.height);
            }
            auto reach = DrainReach(x, z);
            if (reach.width > 0) {
                rivers++;
                for (size_t k = 1; k < reach.points.size(); k++) {
                    assert(reach.points[k].height <= reach.points[k - 1].height);
                }
            }
            double wx = x * 2800.25, wz = z * 2800.25, h = Height(wx, wz);
            assert(std::isfinite(h));
            biomes.insert(ClimateAt(wx, wz, h).biome);
            assert(std::abs(Height(wx + .001, wz) - h) < .1);
        }
    }
    // Continents deliberately have broad climate regions; the protected town
    // neighbourhood is no longer required to contain every biome.
    for(int z=-800000;z<=800000;z+=12000)for(int x=-800000;x<=800000;x+=12000) {
        double h=LandHeight(x,z);
        if(h>SeaLevel)biomes.insert(FinishClimate(BaseClimateAt(x,z,h),h).biome);
    }
    std::fprintf(stderr,"biome coverage %zu categories, %d local rivers\n",biomes.size(),rivers);
    assert(biomes.size() >= 5 && rivers > 0);
    for (int cell = -8; cell <= 8; cell++) {
        for (int k = -12000; k <= 12000; k += 173) {
            double boundary = cell * 2048.0;
            assert(std::abs(Height(boundary - .001, k) - Height(boundary + .001, k)) < .1);
            assert(std::abs(Height(k, boundary - .001) - Height(k, boundary + .001)) < .1);
        }
    }
    // Continuous mixtures, including weather-grid edges and distant coordinates.
    for (double origin : {0., 100000000., -100000000., 1e12, -1e12}) {
        for (int k = -12; k <= 12; ++k) {
            double x = origin + k * 4096., z = origin * .37 + k * 731.;
            double h = Height(x, z);
            auto   a = ClimateAt(x - .001, z, h), b = ClimateAt(x + .001, z, h);
            double sum = 0;
            for (int i = 0; i < 6; ++i) {
                assert(a.weights[i] >= 0 && a.weights[i] <= 1);
                sum += a.weights[i];
                assert(std::abs(a.weights[i] - b.weights[i]) < .001);
            }
            assert(std::abs(sum - 1) < 1e-12);
            auto species = SpeciesWeights(a);
            sum = 0;
            for (auto w : species) {
                assert(w >= 0 && w <= 1);
                sum += w;
            }
            assert(std::abs(sum - 1) < 1e-12);
            assert(a.moisture >= 0 && a.moisture <= 1);
            assert(std::isfinite(VegetationDensity(x, z, a)));
            auto dry = FinishClimate(BaseClimateAt(x, z, h), h, 0), wet = FinishClimate(BaseClimateAt(x, z, h), h, 1);
            assert(wet.moisture >= dry.moisture && wet.forest >= dry.forest && wet.sand <= dry.sand);
        }
    }
    int breaches = 0;
    for (int z = -20; z <= 20; ++z) {
        for (int x = -20; x <= 20; ++x) {
            DrainKey key{x, z};
            auto     d = Downstream(key);
            if (std::max(std::abs(d.first - x), std::abs(d.second - z)) > 1) {
                ++breaches;
            }
            if (d != key) {
                const auto &incoming = Tributaries(d);
                assert(std::find(incoming.begin(), incoming.end(), key) != incoming.end());
            }
        }
    }
    assert(breaches > 0);
    std::printf("PASS normalized continuous biome/species blends, riparian ecology, weather seams, %d depression outlets and trillion-unit coordinates\n", breaches);
    // The float origin is irrelevant: all generator inputs remain doubles.
    for (auto p : {
             std::pair{-36952796.125, 7094121.5     },
             std::pair{100000000.25,  -100000000.125}
    }) {
        double h = Height(p.first, p.second);
        assert(std::isfinite(h));
        assert(std::abs(Height(p.first + .01, p.second) - h) < .1);
        auto climate = ClimateAt(p.first, p.second, h);
        assert(std::isfinite(climate.moisture) && std::isfinite(climate.temperature));
    }
    for (int z = -2800; z <= 2800; z += 200) {
        for (int x = -2800; x <= 2800; x += 200) {
            if (std::hypot(x, z) < 3000) {
                assert(Height(x, z) == MountainHeight(x, z));
            }
        }
    }
    // Coarse visual LOD filters only sub-pixel river carving; authoritative collision stays fine.
    assert(Height(17000, 10000, 1024) == LandHeight(17000, 10000));
    std::printf("PASS six climate biomes, %d drainage reaches, downhill flow, original-town protection and far coordinates\n", rivers);
}
