#include <cassert>
#include <cstdio>
#include <set>
#include <string>

#include "../port/src/biome_music.hpp"
#include "audio/sq.hpp"
#include "terrain_kernel.hpp"

int           bank = 1, volume = 100, plays = 0, cropChecks = 0;
std::set<int> heard;

int SndGetBgmNo() { return bank; }

int SndGetBgmVol() { return volume; }

int SndGetDefaultBgmVol() { return 100; }

void SndSetBgmVol(int v) {
    assert(v >= 0 && v <= 127);
    volume = v;
}

void SndBgmStop() {}

int SndBgmLoad(int b) {
    bank = b;
    return 1;
}

void SndBgmPlay(int) {
    ++plays;
    heard.insert(bank);
    auto original = audio::SqFile::CreateFromJson(R"({"schema_version":1,"kind":"sequence","songs":[{"division":480,"events":[{"delta":0,"kind":"channel","status":192,"data":[1,0]},{"delta":480,"kind":"channel","status":144,"data":[20,100]},{"delta":11520,"kind":"channel","status":148,"data":[51,100]},{"delta":480,"kind":"end"}]}]})");
    auto cropped = BiomeMusicSequence("o04a_a.sq", original);
    if (bank == 13) {
        assert(cropped != original);
        auto events = cropped->Events(cropped->Songs()[0]);
        assert(events.size() == 3 && events[0].status == 192);
        assert(events[1].delta == 0 && events[1].data[0] == 51);
        assert(events[2].delta == 480);
        ++cropChecks;
    } else {
        assert(cropped == original);
    }
}

int main() {
    // Same production controller, fake audio device. All tracks rotate without
    // audio hardware or time delays; boundary chatter and events cannot hijack it.
    std::array<std::pair<double, double>, 4> places{};
    std::array<bool, 4>                      found{};
    for (double z = -200000; z < 200000; z += 5000) {
        for (double x = -200000; x < 200000; x += 5000) {
            auto c = terrain::ClimateAt(x, z, terrain::LandHeight(x, z));
            int  g = c.biome == terrain::Biome::Alpine ? 3 : c.biome == terrain::Biome::Dryland                                                                                 ? 2
                                                         : (c.biome == terrain::Biome::Woodland || c.biome == terrain::Biome::Rainforest || c.biome == terrain::Biome::Wetland) ? 1
                                                                                                                                                                                : 0;
            if (!found[g]) {
                places[g] = {x, z};
                found[g] = true;
            }
        }
    }
    for (int g = 0; g < 5; g++) {
        assert(g == 4 || found[g]);
        BiomeMusicTick(false, false);
        heard.clear();
        auto [x, z] = places[g % 4];
        for (int i = 0; i < 120000; i++) {
            BiomeMusicTick(g == 4, true, x, z);
        }
        const std::set<int> expected[] = {
            {0, 1, 13, 29, 34},
            {2, 6, 26, 29, 101},
            {3, 4, 18, 29},
            {7, 14, 15, 29},
            {16}
        };
        assert(heard == expected[g]);
    }
    assert(cropChecks > 0);
    int before = plays;
    for (int i = 0; i < 1000; i++) {
        BiomeMusicTick(false, false);
    }
    assert(plays == before);
    auto [x, z] = places[0];
    for (int i = 0; i < 300; i++) {
        BiomeMusicTick(false, true, x, z);
    }
    before = plays;
    for (int i = 0; i < 2000; i++) {
        auto p = places[(i / 60) % 2];
        BiomeMusicTick(false, true, p.first, p.second);
    }
    assert(plays == before);
    std::puts("PASS all biome playlists, both forest themes, indoor music, cropped intro, event control and boundary hysteresis");
}
