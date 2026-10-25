#include <cassert>
#include <cstdio>
#include <set>

#include "forest_kernel.hpp"

int main() {
    for (auto origin : {
             std::pair{15360.,         -2048.        },
             std::pair{-4096.,         -4096.        },
             std::pair{99999744.,      -99999744.    },
             std::pair{999999997952.,  -999999997952.},
             std::pair{-999999997952., 999999997952. }
    }) {
        // A far coordinate can now be open ocean. Exercise actual populated
        // land near it as well, rather than passing the partition test with zero trees.
        if(std::abs(origin.first)>1e7) {
            bool found=false;
            for(int z=-100;z<=100&&!found;++z)for(int x=-100;x<=100&&!found;++x) {
                double wx=origin.first+x*16000,wz=origin.second+z*16000;
                double h=terrain::LandHeight(wx,wz);auto c=terrain::FinishClimate(terrain::BaseClimateAt(wx,wz,h),h);
                if(h>0 && c.forest>.7 && terrain::VegetationDensity(wx,wz,c)>.35) {
                    origin={std::floor(wx/512)*512,std::floor(wz/512)*512};found=true;
                }
            }
            assert(found);
        }
        auto all = forest::Generate<false>(origin.first, origin.second, 2048);
        auto cached=forest::GenerateCached(origin.first,origin.second,2048);
        std::map<uint64_t,forest::Tree> cachedById;for(auto t:cached)assert(cachedById.emplace(t.id,t).second);
        assert(cached.size()==all.size());for(auto t:all){auto c=cachedById.at(t.id);assert(t.x==c.x&&t.y==c.y&&t.z==c.z&&t.yaw==c.yaw&&t.scale==c.scale&&t.species==c.species);}
        assert(!all.empty());
        std::map<uint64_t, forest::Tree> split;
        for (int z = 0; z < 4; z++) {
            for (int x = 0; x < 4; x++) {
                for (auto t : forest::GenerateCached(origin.first + x * 512, origin.second + z * 512, 512)) {
                    assert(split.emplace(t.id, t).second);
                }
            }
        }
        assert(all.size() == split.size());
        for (auto t : all) {
            auto other = split.at(t.id);
            assert(t.x == other.x && t.y == other.y && t.z == other.z && t.scale == other.scale && t.yaw == other.yaw && t.species == other.species);
            assert(std::abs(t.y - terrain::SurfaceHeight(t.x, t.z)) < 1e-9);
        }
        std::printf("PASS identical population across chunk partitions at %.0f %.0f: %zu trees\n", origin.first, origin.second, all.size());
    }
    for (double d = 0; d < 20000; d += .5) {
        auto w = forest::Weights(d);
        assert(std::abs(w[0] + w[1] + w[2] - 1) < 1e-12);
        for (int k = 0; k < 3; k++) {
            assert(w[k] >= 0 && w[k] <= 1);
        }
        auto next = forest::Weights(d + .01);
        for (int k = 0; k < 3; k++) {
            assert(std::abs(next[k] - w[k]) < .0001);
        }
    }
    std::puts("PASS continuous complementary tree LOD weights: no density loss");
}
