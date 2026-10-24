#include <cassert>
#include <cstdio>
#include <set>

#include "terrain_kernel.hpp"

int main() {
    using namespace terrain;
    assert(Height(0, 0) == 0 && Height(0, 800) == 0);
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
}
