#include "forest_georama.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <set>

#include "character.hpp"
#include "collision.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editloop3.hpp"
#include "editmapscript.hpp"
#include "editpartsinfo.hpp"
#include "eparts_port.hpp"
#include "forest_town.hpp"
#include "forest_town_layout.hpp"
#include "frame.hpp"
#include "platform/paths.hpp"
#include "town_terrain.hpp"
#include "world_georama.hpp"
#include "world_origin.hpp"

namespace {
std::vector<SV_GRD_PART>   placements;
std::array<CMapParts, 128> parked;
std::array<CMapParts, 24>  templates;
CMapParts                 *originalTemplates = nullptr, *originalObj = nullptr;
CEditPartsInfo             catalog{}, originalCatalog{};
bool                       initialized = false, editing = false;
unsigned                   revision = 0;
alignas(64) unsigned char storage[1024 * 1024];
CDataAlloc2<1>        arena;
constexpr const char *names[] = {"e02h01", "e02h02", "e02h03", "e02h04", "e02h05", "e02h06", "e02h07", "e02h08", "e02h09", "e02h09", "e02h09", "e02c09", "e02c15", "e02c16", "e02h10", "e02t01", nullptr, nullptr, "e02m01", "e02m02"};

std::vector<unsigned char> Read(const char *name) {
    std::ifstream f(PathsDataRoot() / name, std::ios::binary);
    return {(std::istreambuf_iterator<char>(f)), {}};
}

unsigned Word(const std::vector<unsigned char> &d, size_t n) {
    unsigned v = 0;
    if (n + 4 <= d.size()) {
        std::memcpy(&v, d.data() + n, 4);
    }
    return v;
}

void Initialize() {
    if (initialized || !SaveData) {
        return;
    }
    initialized = true;
    arena.base = arena.buffer = storage;
    arena.used = 0;
    arena.limit = sizeof(storage) / 16;
    catalog.Initialize(1);
    auto                      scene = Read("gedit/e02/scene.scn");
    static EPARTS_INFO_HEADER riverHeaders[2]{};
    static unsigned char      cell = 128;
    for (int i = 0; i < 24; ++i) {
        auto &p = templates[i];
        p.Initialize();
        p.handle = -1;
        if (i >= 20) {
            continue;
        }
        EPARTS_INFO_HEADER *header = nullptr;
        if (names[i]) {
            for (unsigned e = 0; e < Word(scene, 4); ++e) {
                size_t at = 16 + 48 * e;
                if (at + 48 > scene.size()) {
                    break;
                }
                if (std::strncmp(reinterpret_cast<char *>(scene.data() + at), names[i], 16)) {
                    continue;
                }
                size_t b = Word(scene, at + 16);
                size_t h = b + Word(scene, b + 4);
                if (h + 120 <= scene.size()) {
                    header = EPartsLoad(scene.data() + h, &arena);
                }
                break;
            }
        }
        if (!header) {
            auto &h = riverHeaders[i == 17];
            h.width = h.height = 1;
            h.cell = &cell;
            header = &h;
        }
        catalog.Initialize(i, header);
        catalog.parts[i].obtained = 1;
        catalog.parts[i].stock = std::max(catalog.parts[i].stock, 1);
        auto *sv = SaveData->GetEditPartsInfo(1, i);
        for (int j = 0; j < 6; ++j) {
            catalog.parts[i].elements[j].enabled = sv->element_enabled[j];
        }
        p.handle = p.parts_no = p.def_parts_no = i;
        p.info = &catalog.parts[i];
        p.area = -1;
        p.unit_size = 100;
        p.draw_on = 1;
        p.category_no = 1;
        p.subtype = (i == 16 ? MAP_PARTS_SUBTYPE_RIVER : i == 17            ? MAP_PARTS_SUBTYPE_BRIDGE
                                                     : (i >= 11 && i <= 13) ? MAP_PARTS_SUBTYPE_ON_RIVER
                                                                            : MAP_PARTS_SUBTYPE_NONE);
        p.bound.min[0] = -50 * header->width;
        p.bound.max[0] = 50 * header->width;
        p.bound.min[2] = -50 * header->height;
        p.bound.max[2] = 50 * header->height;
        p.bound.min[1] = -30;
        p.bound.max[1] = 300;
    }
    int   count = 0;
    auto *saved = SaveData->GetParts(1, &count);
    // Reserved catalog slot 23 records that the open-world town was initialized;
    // an intentionally emptied village must not repopulate on the next load.
    bool imported = SaveData->GetEditPartsInfo(1, 23)->obtained != 0;
    if (count > 0 || imported) {
        placements.assign(saved, saved + count);
    } else {
        for (auto &info : catalog.parts) {
            for (auto &e : info.elements) {
                if (e.id >= 0) {
                    e.enabled = 1;
                }
            }
        }
        auto           d = Read("gedit/e02/gdata0.edt");
        unsigned short n = 0, off = 0;
        if (d.size() >= 8) {
            std::memcpy(&n, d.data(), 2);
            std::memcpy(&off, d.data() + 4, 2);
            for (unsigned i = 0; i < n && off + (i + 1) * sizeof(SV_GRD_PART) <= d.size(); ++i) {
                SV_GRD_PART p;
                std::memcpy(&p, d.data() + off + i * sizeof(p), sizeof(p));
                placements.push_back(p);
            }
        }
    }
    if (std::getenv("DC_FOREST_DRY")) {
        placements.erase(std::remove_if(placements.begin(), placements.end(), [](auto p) { return p.part_id == 16 && std::abs(p.pos_x - 950) < 1 && std::abs(p.pos_z - 250) < 1; }), placements.end());
    }
    ++revision;
}

void ReadEditor() {
    if (!editing) {
        return;
    }
    std::vector<SV_GRD_PART> next;
    for (auto &p : pEditGround->parts) {
        if (p.handle >= 0) {
            auto w = world::PositionOf(p.pos);
            next.push_back({short(p.parts_no), short(p.rot_y), float(w.x - foresttown::Village.x), float(w.y - foresttown::Elevation), float(w.z - foresttown::Village.z)});
        }
    }
    if (next.size() != placements.size() || (!next.empty() && std::memcmp(next.data(), placements.data(), next.size() * sizeof(SV_GRD_PART)))) {
        placements = std::move(next);
        ++revision;
    }
}
} // namespace

const std::vector<SV_GRD_PART> &ForestGeoramaParts() {
    Initialize();
    ReadEditor();
    return placements;
}

bool ForestGeoramaActive() { return editing; }

unsigned ForestGeoramaRevision() {
    Initialize();
    ReadEditor();
    return revision;
}

void ForestGeoramaSave() {
    Initialize();
    ReadEditor();
    if (!initialized) {
        return;
    }
    int   count = 0;
    auto *dst = SaveData->GetParts(1, &count);
    int   i = 0;
    for (auto p : placements) {
        if (i == 128) {
            break;
        }
        dst[i++] = p;
    }
    dst[i].part_id = -1;
    if (editing) {
        EditPartsInfo.Save(1, SaveData);
    } else {
        catalog.Save(1, SaveData);
    }
    SaveData->GetEditPartsInfo(1, 23)->obtained = 1;
}

void ForestGeoramaEnter() {
    if (editing || !TownTerrainEnabled(pEditGround) || !Chara) {
        return;
    }
    auto w = world::PositionOf(Chara->pos);
    if (std::hypot(w.x - foresttown::Village.x, w.z - foresttown::Village.z) > 4500) {
        return;
    }
    Initialize();
    pEditGround->Save(0, SaveData);
    EditPartsInfo.Save(0, SaveData);
    originalTemplates = pEditGround->plot_parts;
    originalObj = ObjParts;
    originalCatalog = EditPartsInfo;
    for (int i = 0; i < 128; ++i) {
        auto global = world::PositionOf(pEditGround->parts[i].pos);
        parked[i] = pEditGround->parts[i];
        world::Assign(parked[i].pos, global);
        world::Forget(pEditGround->parts[i].pos);
        pEditGround->parts[i].Initialize();
        pEditGround->parts[i].handle = -1;
    }
    EditPartsInfo = catalog;
    for (auto &p : templates) {
        if (p.handle >= 0) {
            p.info = &EditPartsInfo.parts[p.parts_no];
        }
    }
    ObjParts = templates.data();
    pEditGround->plot_parts = ObjParts;
    pEditGround->parts_info = &EditPartsInfo;
    for (auto &p : EditPartsInfo.parts) {
        p.placed = 0;
    }
    int i = 0;
    for (auto r : placements) {
        if (i >= 128 || r.part_id < 0 || r.part_id >= 20) {
            continue;
        }
        auto &p = pEditGround->parts[i++];
        p = templates[r.part_id];
        p.SetRotY(r.rot_y);
        world::Assign(p.pos, {foresttown::Village.x + r.pos_x, foresttown::Elevation + r.pos_y, foresttown::Village.z + r.pos_z});
        p.SetPosition(p.pos);
        EditPartsInfo.parts[r.part_id].placed++;
    }
    NowEditMap = 1;
    editing = true;
    pEditGround->map_no = 1;
    pEditGround->MakePartsBox();
    std::fprintf(stderr, "forest georama: active catalog=1 placements=%zu\n", placements.size());
}

void ForestGeoramaTick() {
    static int test = 0;
    if (std::getenv("DC_FOREST_GEORAMA_TEST") && ++test == 160 && Chara) {
        auto player = world::PositionOf(Chara->pos);
        world::Assign(Chara->pos, {foresttown::Village.x, foresttown::Elevation, foresttown::Village.z});
        ForestGeoramaEnter();
        GameMode = ED_MODE_GEORAMA;
        auto check = [](bool ok, const char *message) {std::fprintf(stderr,"forest edit test: %s %s\n",ok?"PASS":"FAIL",message);if(!ok){std::abort();
} };
        check(editing && NowEditMap == 1, "original Matataki catalog selected");
        ForestTownPrepare();
        CCPoly  polys[64];
        CBoxVu0 box;
        box.min[0] = world::LocalX(foresttown::Treant.x) - 80;
        box.max[0] = box.min[0] + 160;
        box.min[2] = world::LocalZ(foresttown::Treant.z) - 80;
        box.max[2] = box.min[2] + 160;
        box.min[1] = foresttown::Elevation;
        box.max[1] = foresttown::Elevation + 180;
        check(ForestTownPolys(polys, box, 64) > 0, "Treant has solid trunk collision");

        int h = -1, r = -1;
        for (int i = 0; i < 128; ++i) {
            auto &p = pEditGround->parts[i];
            if (p.handle >= 0 && p.parts_no == 0) {
                h = i;
            }
            if (p.handle >= 0 && p.parts_no == 16 && std::abs(world::X(p.pos[0]) - foresttown::Village.x - 950) < 1 && std::abs(world::Z(p.pos[2]) - foresttown::Village.z - 250) < 1) {
                r = i;
            }
        }
        check(h >= 0 && r >= 0, "original house and river placements loaded");
        auto house = pEditGround->parts[h];
        int  plot, rot;
        check(pEditGround->DeleteMapParts(&plot, &rot, house.pos[0], house.pos[1], house.pos[2]) >= 0 && plot == 0, "native pickup selects house");
        check(pEditGround->SetMapParts(plot, house.pos[0], house.pos[1], house.pos[2], rot) >= 0, "native placement restores house");
        auto river = pEditGround->parts[r];
        bool connected = ForestGeoramaConnected();
        check(pEditGround->DeleteMapParts(&plot, &rot, river.pos[0], river.pos[1], river.pos[2]) >= 0 && plot == 16, "native pickup selects river");
        check(!ForestGeoramaConnected(), "breaking a required river reach dries Treant");
        check(pEditGround->SetMapParts(plot, river.pos[0], river.pos[1], river.pos[2], rot) >= 0, "native placement restores river");
        check(ForestGeoramaConnected() == connected, "river network restored");
        ForestGeoramaSave();
        int count = 0;
        SaveData->GetParts(1, &count);
        check(count == int(ForestGeoramaParts().size()), "placements saved to Matataki slot");
        GameMode = ED_MODE_WALK;
        world::Assign(Chara->pos, player);
    }
    if (!editing) {
        return;
    }
    ReadEditor();
    if (GameMode != ED_MODE_WALK) {
        return;
    }
    ForestGeoramaSave();
    catalog = EditPartsInfo;
    for (int i = 0; i < 128; ++i) {
        auto &p = pEditGround->parts[i];
        world::Forget(p.pos);
        p = parked[i];
        if (p.handle >= 0) {
            auto w = world::PositionOf(parked[i].pos);
            world::Assign(p.pos, w);
            p.SetPosition(p.pos);
        }
    }
    EditPartsInfo = originalCatalog;
    pEditGround->plot_parts = originalTemplates;
    ObjParts = originalObj;
    pEditGround->parts_info = &EditPartsInfo;
    NowEditMap = 0;
    editing = false;
    pEditGround->map_no = 0;
    pEditGround->MakePartsBox();
}

void ForestGeoramaReset() {
    initialized = editing = false;
    placements.clear();
    ++revision;
}

bool ForestGeoramaConnected() {
    const auto &p = ForestGeoramaParts();
    auto        chain = [&](int sx, int sz, int tx, int tz) {std::set<std::pair<int,int>> cells,seen;for(auto q:p){if(q.part_id==16||q.part_id==17||(q.part_id>=11&&q.part_id<=13)){cells.emplace(int(std::lround(q.pos_x)),int(std::lround(q.pos_z)));
}}
 std::vector<std::pair<int,int>> work{{sx,sz}};while(!work.empty()){auto a=work.back();work.pop_back();if(!cells.contains(a)||!seen.insert(a).second){continue;
}if(a==std::pair{tx,tz}){return true;
}for(auto d:{std::pair{0,100},std::pair{100,0},std::pair{0,-100},std::pair{-100,0}}){work.emplace_back(a.first+d.first,a.second+d.second);
}}return false; };
    // Retail CEditGround::MatatagiRequest: all three independently editable
    // reaches must bridge their fixed inlet/outlet cells.
    return chain(-550, 150, -850, 850) && chain(-550, -250, 250, -450) && chain(950, 250, 850, 950);
}

bool ForestGeoramaVisible(int plot, const std::string &path) {
    if (plot < 0 || plot >= 24) {
        return true;
    }
    auto &info = (editing ? EditPartsInfo : catalog).parts[plot];
    bool  visible = true;
    for (int e = 0; e < 6; ++e) {
        if (info.elements[e].id < 0 || info.elements[e].id >= 36) {
            continue;
        }
        char  storage[32][32]{};
        char *names[32];
        for (int i = 0; i < 32; ++i) {
            names[i] = storage[i];
        }
        GetElementObjName(&info, names, e, 0);
        for (int i = 0; i < 32 && names[i][0]; ++i) {
            const char *name = names[i];
            bool        inverse = *name == '-';
            if (*name == '-' || *name == '*') {
                ++name;
            }
            if (path.find("/" + std::string(name) + "/") != std::string::npos) {
                visible = inverse ? !info.elements[e].enabled : bool(info.elements[e].enabled);
            }
        }
    }
    return visible;
}
