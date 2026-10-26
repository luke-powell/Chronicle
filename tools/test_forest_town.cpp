#include <cassert>
#include <cstdio>

#include "forest_kernel.hpp"

int main() {
    using namespace foresttown;
    assert(std::hypot(Village.x, Village.z) > 15000);
    assert(VillageDistance(Village.x, Village.z + 400) > 300); // original wooded central island
    for (auto p : std::array<std::array<double, 2>, 3>{
             {{{-800, 500}}, {{-300, -500}}, {{850, 600}}}
    }) {
        double x = Village.x + p[0], z = Village.z + p[1];
        assert(VillageDistance(x, z) == 0);
        assert(forest::Occupied(x, z));
        auto climate = terrain::FinishClimate(terrain::BaseClimateAt(x, z, 200), 200);
        assert(climate.forest > .8); // this site already belongs to natural forest
    }
    assert(std::abs(Treehouse.x + 745 - (Village.x - 1105)) < 1);
    assert(Treant.x == Village.x - 1050 && Treant.z > Village.z + 1600);
    for (auto path : Paths) {
        for (int i = 0; i <= 100; ++i) {
            double t = i / 100.;
            double x = std::lerp(path.x, path.endX, t), z = std::lerp(path.z, path.endZ, t);
            assert(PathDistance(x, z) < 1e-6);
            assert(forest::Occupied(x, z));
        }
    }
    assert(std::abs(terrain::Height(0, 0)) < 1e-6);
    auto beds = std::make_shared<authoredwater::Beds>();
    beds->Add({
        {-50, 50,  -50},
        {-50, -50, 50 },
        167
    });
    assert(beds->Carve(0, 0, 200) == 167);
    assert(beds->Carve(-65, -50, 200, 8) == 200); // fine banks do not excavate the neighbouring floor
    assert(beds->Carve(-65, -50, 200, 32) == 167);
    assert(beds->Carve(100, 0, 200, 128) < 190);
    auto duplicate = *beds;
    duplicate.Add({
        {-50, 50,  -50},
        {-50, -50, 50 },
        167
    });
    assert(duplicate.Carve(-85, -50, 200) == beds->Carve(-85, -50, 200));
    beds->AddSurface({
        {1000, 1100, 1000},
        {450,  450,  450 },
        {1000, 1000, 1100}
    });
    assert(beds->FitShell(1020, 1020, 200) == 448);
    assert(beds->FitShell(1600, 1600, 200) == 200);
    beds->protectedAreas.push_back({1000, 1000, 1100, 1100});
    assert(beds->FitShell(1020, 1020, 200) == 200);
    authoredwater::Beds cliff;
    cliff.AddSurface({
        {100, 100, 100},
        {200, 400, 200},
        {100, 100, 200}
    });
    cliff.AddSurface({
        {100, 200, 100},
        {400, 400, 400},
        {100, 100, 200}
    });
    assert(cliff.ClearCliffs(120, 150, 400, 32) < 200);
    assert(cliff.Support(120, 150, 197) == 400); // foliage stays on the visible native crown
    assert(cliff.ClearCliffs(500, 500, 200, 32) == 200);
    for (int z = 1970; z < 2160; ++z) {
        for (int x = -1150; x < -950; ++x) {
            auto a = Stream(Village.x + x, Village.z + z), b = Stream(Village.x + x, Village.z + z + 1);
            assert(std::abs(a.water - b.water) < .1); // no height step at the new pond mouth
        }
    }
    RiverConnected() = true;
    auto outlet = terrain::RiverAt(Village.x - 1050, Village.z + 1600);
    assert(outlet.water == 187 && outlet.width == 50 && outlet.distance == outlet.width && outlet.flowZ > 0);
    auto upstream = terrain::RiverAt(Village.x - 1050, Village.z + 1590);
    auto downstream = terrain::RiverAt(Village.x - 1050, Village.z + 1610);
    assert(upstream.distance > upstream.width); // no overlapping procedural cap on native water
    assert(downstream.distance < downstream.width && downstream.water == outlet.water);
    auto lake = terrain::RiverAt(Village.x - 1050, Village.z + 2100);
    assert(lake.lake && lake.flowX == 0 && lake.flowZ == 0);
    assert(terrain::Height(Treant.x, Treant.z) >= Elevation - 1); // roots remain on solid ground
    RiverConnected() = false;
    assert(terrain::RiverAt(Village.x - 1050, Village.z + 1600).distance > 1000);
    assert(terrain::RiverAt(Village.x - 1050, Village.z + 2100).distance > 1000);
    puts("PASS original clearing footprint, natural forest location, exit alignment, terrain protection, water beds and dry/live channel");
}
