#include <cstdio>
#include <fstream>

#include "terrain_kernel.hpp"

// Preview the actual generator, not an independently approximated noise map.
int main(int argc, char **argv) {
    double        cx = argc > 1 ? std::atof(argv[1]) : 0, cz = argc > 2 ? std::atof(argv[2]) : 0;
    double        span = argc > 3 ? std::atof(argv[3]) : 90000;
    const int     n = argc > 4 ? std::max(32, std::atoi(argv[4])) : 600;
    std::ofstream fields;
    if (argc > 5) {
        fields.open(argv[5]);
        fields << "x,z,height,temperature,rainfall,moisture,forest,sand,snow,seasonality,soil\n";
    }
    std::ofstream f("biome-preview.ppm", std::ios::binary);
    f << "P6\n"
      << n << " " << n << "\n255\n";
    int    counts[6]{};
    double best[6] = {1e30, 1e30, 1e30, 1e30, 1e30, 1e30}, sites[6][2]{};
    double bestRiver = 1e30, riverX = 0, riverZ = 0;
    for (int z = 0; z < n; z++) {
        for (int x = 0; x < n; x++) {
            double wx = cx + (x - n / 2) * span / n, wz = cz + (z - n / 2) * span / n;
            double h = terrain::Height(wx, wz);
            auto   c = terrain::ClimateAt(wx, wz, h);
            auto   r = terrain::RiverAt(wx, wz);
            counts[int(c.biome)]++;
            if (fields) {
                fields << wx << ',' << wz << ',' << h << ',' << c.temperature << ',' << c.rainfall << ',' << c.moisture << ',' << c.forest << ',' << c.sand << ',' << c.snow << ',' << c.seasonality << ',' << c.soil << '\n';
            }
            double distance = std::hypot(wx, wz);
            if (distance < best[int(c.biome)] && distance > 5000) {
                best[int(c.biome)] = distance;
                sites[int(c.biome)][0] = wx;
                sites[int(c.biome)][1] = wz;
            }
            if (c.forest > .65 && r.distance > r.width + 20 && r.distance < r.width + 80 && distance < bestRiver) {
                bestRiver = distance;
                riverX = wx;
                riverZ = wz;
            }
            double colors[6][3] = {
                {146, 161, 96 },
                {63,  111, 67 },
                {36,  91,  62 },
                {80,  120, 100},
                {201, 173, 108},
                {214, 223, 225}
            };
            double        shade = std::clamp(.85 + (terrain::Height(wx - 50, wz - 50) - h) / 180, .45, 1.2);
            unsigned char pixel[3];
            for (int a = 0; a < 3; a++) {
                double color = 0;
                for (int biome = 0; biome < 6; ++biome) {
                    color += c.weights[biome] * colors[biome][a];
                }
                pixel[a] = std::clamp(int(color * shade), 0, 255);
            }
            if (r.distance < r.width && h < r.water) {
                pixel[0] = 40;
                pixel[1] = 135;
                pixel[2] = 177;
            }
            if (std::hypot(wx, wz) < 800) {
                pixel[0] = 235;
                pixel[1] = 220;
                pixel[2] = 190;
            }
            f.write(reinterpret_cast<char *>(pixel), 3);
        }
    }
    for (int i = 0; i < 6; i++) {
        std::printf("biome %d: %d pixels, site %.0f,%.0f\n", i, counts[i], sites[i][0], sites[i][1]);
    }
    std::printf("river bank %.0f,%.0f\n", riverX, riverZ);
}
