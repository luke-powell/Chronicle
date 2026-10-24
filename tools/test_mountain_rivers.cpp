#include <cassert>
#include <cstdio>

#include "terrain_kernel.hpp"

int main() {
    using namespace terrain;
    int channels = 0, segments = 0, bends = 0;
    for (auto origin : {
             DrainKey{0,         0         },
             DrainKey{-2,        -55       },
             DrainKey{50,        -100      },
             DrainKey{-40,       -40       },
             DrainKey{220,       -480      },
             DrainKey{305,       -350      },
             DrainKey{488281250, -488281250}
    }) {
        for (int z = -4; z <= 4; ++z) {
            for (int x = -4; x <= 4; ++x) {
                DrainKey key{origin.first + x, origin.second + z};
                auto     reach = DrainReach(key.first, key.second);
                if (reach.lake || reach.width == 0) {
                    continue;
                }
                ++channels;
                auto destination = Downstream(key);
                auto a = DrainPoint(key.first, key.second), b = DrainPoint(destination.first, destination.second);
                assert(reach.points.front().x == a.x && reach.points.front().z == a.z && reach.points.front().height == a.height);
                assert(reach.points.back().x == b.x && reach.points.back().z == b.z && reach.points.back().height == b.height);
                for (size_t k = 1; k < reach.points.size(); ++k) {
                    auto p = reach.points[k - 1], q = reach.points[k];
                    assert(q.height <= p.height + 1e-8);
                    assert(ChannelSegment(p, q));
                    // Independently sample denser than the routing grid: no airborne
                    // reaches and no deep ridge-cutting shortcuts through mountains.
                    int n = std::max(1, int(std::ceil(std::hypot(q.x - p.x, q.z - p.z) / 32)));
                    for (int i = 0; i <= n; ++i) {
                        double t = double(i) / n, wx = std::lerp(p.x, q.x, t), wz = std::lerp(p.z, q.z, t), water = std::lerp(p.height, q.height, t);
                        double ground = LandHeight(wx, wz);
                        assert(water < ground + 1);
                        assert(ground - water < RiverMaxCut + 3);
                    }
                    ++segments;
                }
                if (reach.points.size() > 3) {
                    ++bends;
                }
            }
        }
    }
    std::fprintf(stderr,"river coverage %d channels %d segments %d bends\n",channels,segments,bends);
    assert(channels > 40 && segments > 500 && bends > 20);
    std::printf("PASS %d connected terrain-following rivers, %d descending segments; no floating water or deep ridge shortcuts, including far coordinates\n", channels, segments);
}
