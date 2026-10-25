#include "biome_music.hpp"
#include "dran_flight.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "audio/sq.hpp"
#include "snd.hpp"
#include "terrain_kernel.hpp"

namespace {
struct Track {
    int         bank;
    int         seconds;
    const char *name;
};

constexpr Track tracks[] = {
    {1,   91,  "Norune Village"         },
    {13,  34,  "Open Your Eyes"         },
    {34,  188, "Reminiscence"           },
    {0,   197, "Departure"              },
    {2,   86,  "Matataki Village"       },
    {6,   124, "Brownboo"               },
    {26,  44,  "Legend of Hunter"       },
    {101, 201, "Wise Owl Forest"        },
    {3,   123, "Queens"                 },
    {4,   156, "Muska Lacka"            },
    {18,  139, "Listen Carefully"       },
    {14,  204, "Factory"                },
    {15,  181, "Yellow Drops"           },
    {7,   87,  "Land of Hope"           },
    {29,  240, "Main Theme (Bossa Nova)"},
    {16,  94,  "Daily Life"             }
};
const std::vector<int> pools[] = {
    {0, 1, 2, 3, 14},
    {4, 5, 6, 7, 14},
    {8, 9, 10, 14},
    {11, 12, 13, 14},
    {15}
};
int              group = -1, candidate = -1, stable = 0, age = 0, current = -1, pending = -1, fade = 0, baseVolume = 0;
bool             cropIntro = false;
std::vector<int> bag;
std::mt19937     random{std::random_device{}()};

int Choose(int g) {
    if (bag.empty()) {
        bag = pools[g];
        std::shuffle(bag.begin(), bag.end(), random);
    }
    if (bag.size() > 1 && bag.back() == current) {
        std::swap(bag.front(), bag.back());
    }
    int t = bag.back();
    bag.pop_back();
    return t;
}

void Reset() {
    group = candidate = current = pending = -1;
    stable = age = fade = 0;
    bag.clear();
}
} // namespace

void BiomeMusicTick(bool interior, bool allowed, double x, double z) {
    // Authored events retain complete control of their music and fades.
    if (!allowed) {
        Reset();
        return;
    }
    int desired = 4;
    if (!interior) {
        // In flight choose the regional biome, rather than constructing a
        // drainage network on the game thread just to identify a riverbank.
        auto c = terrain::ClimateAt(x, z, terrain::LandHeight(x, z), DranFlightActive() ? 2048 : 0);
        desired = c.biome == terrain::Biome::Alpine ? 3 : c.biome == terrain::Biome::Dryland                                                                                 ? 2
                                                      : (c.biome == terrain::Biome::Woodland || c.biome == terrain::Biome::Rainforest || c.biome == terrain::Biome::Wetland) ? 1
                                                                                                                                                                             : 0;
    }
    if (desired != candidate) {
        candidate = desired;
        stable = 0;
    }
    ++stable;
    ++age;
    // An external music change is authoritative. Wait before taking ownership again.
    if (current >= 0 && SndGetBgmNo() != tracks[current].bank) {
        Reset();
        return;
    }
    if (!fade && ((desired != group && stable >= (group < 0 || interior ? 120 : 480)) ||
                  (current >= 0 && age >= tracks[current].seconds * 60))) {
        if (desired != group) {
            group = desired;
            bag.clear();
        }
        pending = Choose(group);
        baseVolume = SndGetBgmVol();
        fade = -90;
    }
    if (fade < 0) {
        SndSetBgmVol(baseVolume * (-fade) / 90);
        if (++fade == 0) {
            SndBgmStop();
            SndBgmLoad(tracks[pending].bank);
            if (SndGetBgmNo() != tracks[pending].bank) {
                Reset();
                return;
            }
            cropIntro = tracks[pending].bank == 13;
            SndBgmPlay(0);
            cropIntro = false;
            current = pending;
            pending = -1;
            age = 0;
            baseVolume = SndGetDefaultBgmVol();
            SndSetBgmVol(0);
            fade = 90;
            if (std::getenv("DC_BIOME_MUSIC_TRACE")) {
                std::fprintf(stderr, "biome music: group %d bank %d %s\n", group, tracks[current].bank, tracks[current].name);
            }
        }
    } else if (fade > 0) {
        SndSetBgmVol(baseVolume * (91 - fade) / 90);
        --fade;
    }
}

std::shared_ptr<audio::SqFile> BiomeMusicSequence(std::string_view name, std::shared_ptr<audio::SqFile> sequence) {
    if (!cropIntro || name != "o04a_a.sq" || !sequence) {
        return sequence;
    }
    // The first melody chord is at tick 12000 (25.862 s). Carry program,
    // controller and tempo setup forward, but omit the introductory note events.
    // This is only applied to the outdoor playlist; story playback stays intact.
    using nlohmann::json;
    json root = {
        {"schema_version", 1            },
        {"kind",           "sequence"   },
        {"songs",          json::array()}
    };
    for (const auto &song : sequence->Songs()) {
        json     events = json::array();
        uint32_t tick = 0, previous = 0;
        for (const auto &e : sequence->Events(song)) {
            tick += e.delta;
            if (tick < 12000 && e.kind == audio::SqEventKind::Channel &&
                ((e.status & 0xf0) == 0x90 || (e.status & 0xf0) == 0x80)) {
                continue;
            }
            uint32_t at = tick > 12000 ? tick - 12000 : 0;
            json     out = {
                {"delta", at - previous}
            };
            previous = at;
            if (e.kind == audio::SqEventKind::Channel) {
                out["kind"] = "channel";
                out["status"] = e.status;
                out["data"] = {e.data[0], e.data[1]};
            } else {
                out["kind"] = e.kind == audio::SqEventKind::Meta ? "meta" : e.kind == audio::SqEventKind::SysEx ? "sysex"
                                                                                                                : "end";
                out["meta"] = e.meta;
                out["payload"] = std::vector<uint8_t>(e.payload.begin(), e.payload.end());
            }
            events.push_back(std::move(out));
        }
        root["songs"].push_back({
            {"division", song.division    },
            {"events",   std::move(events)}
        });
    }
    auto cropped = audio::SqFile::CreateFromJson(root.dump());
    return cropped ? cropped : sequence;
}
