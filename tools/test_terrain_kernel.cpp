#include <cassert>
#include <cstdio>
#include <set>

#include "terrain_kernel.hpp"

int main() {
    using namespace terrain;
    assert(Height(0, 0) == 0 && Height(0, 800) == 0);
    foundations={{3000,3000,100,100,40},{3300,3000,100,100,100}};
    for(int i=2900;i<=3100;i++)assert(Height(i,3000)==40);
    for(int i=3200;i<=3400;i++)assert(Height(i,3000)==100);
    std::vector<double> samples;
    for(int x=2700;x<3700;x++)samples.push_back(Height(x,3000));
    std::reverse(foundations.begin(),foundations.end());
    for(int x=2700;x<3700;x++)assert(std::abs(samples[x-2700]-Height(x,3000))<1e-9);
    roads={{3100,3000,3200,3000,40,100}};
    for(int x=3101;x<3200;x++)assert(std::abs(Height(x,3000)-(40+.6*(x-3100)))<1e-8);
    assert(Height(3000,3000)==40 && Height(3300,3000)==100);
    for(int x=3000;x<3400;x+=32)for(int z=2900;z<3100;z+=32)
        assert(std::isfinite(SurfaceHeight(x+.1,z+.2)));
    foundations.clear();roads.clear();
    std::puts("PASS foundations: hard footprints, order-independent blending, graded road and finite triangle surface");
    for (auto focus : {
             std::pair{0.,        0.        },
             std::pair{-513.,     769.      },
             std::pair{10000000., -20000000.}
    }) {
        auto tiles = Select(focus.first, focus.second);
        assert(tiles.size() < 2000);
        Selection     s;
        std::set<int> levels;
        for (auto t : tiles) {
            assert(s.emplace(t, 0).second);
            levels.insert(t.level);
        }
        assert(levels.size() >= 15);
        assert(LevelAt(s, focus.first, focus.second) == 0);
        assert(LevelAt(s, focus.first + Radius * .99, focus.second) >= 15);
        for (auto t : tiles) {
            double x = t.left(), z = t.top(), w = t.size();
            for (int edge = 0; edge < 4; edge++) {
                bool   alongX = edge >= 2;
                double px = x + (edge == 0 ? -1 : edge == 1 ? w + 1
                                                            : w / 2);
                double pz = z + (edge == 2 ? -1 : edge == 3 ? w + 1
                                                            : w / 2);
                int    neighbor = LevelAt(s, px, pz);
                assert(neighbor < 0 || std::abs(neighbor - t.level) <= 1);
                if (neighbor <= t.level) {
                    continue;
                }
                for (int i = 0; i <= Resolution; i++) {
                    double vx = alongX ? x + w * i / Resolution : x + (edge == 1 ? w : 0);
                    double vz = alongX ? z + (edge == 3 ? w : 0) : z + w * i / Resolution;
                    double h = EdgeHeight(vx, vz, neighbor, alongX);
                    assert(std::isfinite(h));
                    if (i % 2 == 0) {
                        assert(std::abs(h - Height(vx, vz)) < 1e-8);
                    } else {
                        double half = w / Resolution;
                        double a = Height(vx - (alongX ? half : 0), vz - (alongX ? 0 : half));
                        double b = Height(vx + (alongX ? half : 0), vz + (alongX ? 0 : half));
                        assert(std::abs(h - (a + b) / 2) < 1e-8);
                    }
                }
            }
        }
        std::printf("PASS focus %.0f %.0f: %zu tiles, %zu LOD levels, bounded cache and matching edges\n", focus.first, focus.second, tiles.size(), levels.size());
    }
    for(int minimum : {1,4,9}) {
        auto tiles=Select(1234567,-7654321,minimum);
        Selection selection;
        for(auto tile:tiles){assert(tile.level>=minimum);selection.emplace(tile,0);}
        assert(LevelAt(selection,1234567,-7654321)==minimum);
        for(auto tile:tiles)for(auto delta: {std::pair{-.001,.5},std::pair{1.001,.5},std::pair{.5,-.001},std::pair{.5,1.001}}) {
            int neighbor=LevelAt(selection,tile.left()+delta.first*tile.size(),tile.top()+delta.second*tile.size());
            assert(neighbor<0 || std::abs(neighbor-tile.level)<=1);
        }
    }
    std::puts("PASS altitude LOD coverage and balanced seams");

}
