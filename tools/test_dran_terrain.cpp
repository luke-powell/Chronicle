#include "terrain_kernel.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>

int main() {
    using namespace terrain;
    Foundations() = {{12000, 12000, 100, 100, 1500}};
    Roads() = {{12100, 12000, 12500, 12000, 1500, 1000}};
    double flightMs = 0, exactMs = 0;
    for (int i = 0; i < 64; ++i) {
        double x = i < 16 ? 11900 + i * 43.7 : (i - 32) * 7111.3;
        double z = i < 16 ? 12001.3 : (i - 48) * 3781.9;
        auto start = std::chrono::steady_clock::now();
        double upper = FlightSurfaceHeight(x, z);
        auto middle = std::chrono::steady_clock::now();
        double actual = SurfaceHeight(x, z);
        auto end = std::chrono::steady_clock::now();
        flightMs += std::chrono::duration<double, std::milli>(middle - start).count();
        exactMs += std::chrono::duration<double, std::milli>(end - middle).count();
        assert(std::isfinite(upper) && upper + 1e-8 >= actual);
    }
    assert(FlightSurfaceHeight(12000, 12000) == 1500);
    assert(Select(0, 0, 2).size() < Select(0, 0, 0).size());
    std::printf("PASS flight terrain: conservative across 64 foundation, road and wilderness samples; flight %.3f ms, detailed %.3f ms\n", flightMs, exactMs);
}
