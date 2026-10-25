#ifdef PORT
#include "gfx/capture.hpp"
#endif
#include "dran_flight.hpp"
#include "world_origin.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

#include "camerafollow.hpp"
#include "character.hpp"
#include "ebattle.hpp"
#include "editarea.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editmapscript.hpp"
#include "editpartsinfo.hpp"
#include "effect.hpp"
#include "effectgroup.hpp"
#include "framevu1.hpp"
#include "gamemode.hpp"
#include "gfx/displaylist.hpp"
#include "gfx/gfx.hpp"
#include "npcharacter.hpp"
#include "platform/files.hpp"
#include "runeffect.hpp"
#include "save_slots.hpp"
#include "savedata.hpp"
#include "terrain_kernel.hpp"
#include "town_terrain.hpp"
#include "world_georama.hpp"

extern CCameraFollow EventCamera;

namespace world {
double originX = 0, originZ = 0;

namespace {
struct Tracked {
    Position             global;
    std::array<float, 3> local;
};

bool                            active = false;
std::map<float *, Tracked>      positions;
std::map<CEditArea *, Position> areaOrigins;
nlohmann::json                  savedParts = nlohmann::json::array();
std::set<float *>               shifted;
nlohmann::json savedPlayer;
bool restorePlayer=false;
int restoreWalkTicks=0;

// Keep the double authority when projecting far inactive objects to floats.
// Only actual changes to the native position feed back into world coordinates;
// an origin shift itself never overwrites the authoritative value.
Tracked &Sync(float *p) {
    auto [it, inserted] = positions.try_emplace(p, Tracked{
                                                       {X(p[0]), p[1], Z(p[2])},
                                                       {p[0],    p[1], p[2]   }
    });
    auto &t = it->second;
    if (!inserted) {
        t.global.x += double(p[0]) - t.local[0];
        t.global.y += double(p[1]) - t.local[1];
        t.global.z += double(p[2]) - t.local[2];
        t.local = {p[0], p[1], p[2]};
    }
    return t;
}

void Shift(float *p, double nx, double nz) {
    if (!p || !shifted.insert(p).second) {
        return;
    }
    auto &t = Sync(p);
    p[0] = float(t.global.x - nx);
    p[2] = float(t.global.z - nz);
    t.local = {p[0], p[1], p[2]};
}

void Camera(CCameraFollow &c, double x, double z) {
    for (float *p : {c.pos, c.ref, c.next_pos, c.next_ref, c.follow}) {
        Shift(p, x, z);
    }
}

void Actor(CCharacter &c, double x, double z) {
    Shift(c.pos, x, z);
    c.SetPosition(c.pos);
    if (&c == Chara) {
        auto &move = static_cast<CMainChara &>(c).move_info;
        Shift(move.ground_point, x, z);
        for (auto *poly : {&move.poly, &move.ground_poly}) {
            for (auto &v : poly->vertex) {
                Shift(v, x, z);
            }
        }
    }
}

std::string Fingerprint(const std::filesystem::path &dir) {
    std::ifstream f(dir / "save.dat", std::ios::binary);
    uint64_t      h = 14695981039346656037ULL;
    char          c;
    while (f.get(c)) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return std::to_string(h);
}
} // namespace

double Height(double x, double z) { return terrain::Height(X(x), Z(z)); }

double SurfaceHeight(double x, double z) { return terrain::SurfaceHeight(X(x), Z(z)); }

double NaturalHeight(double x, double z) { return terrain::NaturalHeight(X(x), Z(z)); }

Position PositionOf(float *p) { return Sync(p).global; }

void Assign(float *p, Position v) {
    p[0] = LocalX(v.x);
    p[1] = float(v.y);
    p[2] = LocalZ(v.z);
    positions[p] = {
        v, {p[0], p[1], p[2]}
    };
}

void Reset() {
    DranFlightReset();
    originX = originZ = 0;
    positions.clear();
    areaOrigins.clear();
    active = true;
}

void Forget(float *p) { positions.erase(p); }

void Rebase(double nx, double nz) {
    if (nx == originX && nz == originZ) {
        return;
    }
    if (!TownTerrainEnabled(pEditGround)) {
        return;
    }
    shifted.clear();
    auto *g = pEditGround;
    for (auto &p : g->parts) {
        if (p.handle >= 0) {
            Shift(p.pos, nx, nz);
            p.SetPosition(p.pos);
        }
    }
    for (auto &p : g->fixed_parts) {
        if (p.handle >= 0) {
            Shift(p.pos, nx, nz);
            p.SetPosition(p.pos);
        }
    }
    for (auto *area : g->areas) {
        if (area) {
            // Grid origins are fixed global anchors; never accumulate rounded shifts.
            float v[4] = {area->offset_x, area->offset_y, area->offset_z, 1};
            auto [it, inserted] = areaOrigins.try_emplace(area, Position{X(v[0]), v[1], Z(v[2])});
            area->SetOffset(float(it->second.x - nx), v[1], float(it->second.z - nz));
        }
    }
    if (Chara) {
        Actor(*Chara, nx, nz);
    }
    for (auto &c : EdVillager) {
        if (c.initialized) {
            Actor(c, nx, nz);
            for (auto &seq : c.sequences) {
                if (seq.operation == NP_SEQUENCE_MOVE) {
                    Shift(seq.destination, nx, nz);
                }
            }
        }
    }
    for (auto *c : {&MainCamera, &EditCamera, &TalkCamera, &ViewCamera, &IntroCamera, &EventCamera}) {
        Camera(*c, nx, nz);
    }
    if (NowCamera) {
        for (float *p : {NowCamera->pos, NowCamera->ref, NowCamera->next_pos, NowCamera->next_ref}) {
            Shift(p, nx, nz);
        }
    }
    for (auto &water : g->water_surfaces) {
        if (water.parts_no < 0) {
            Shift(water.offset, nx, nz);
        }
    }
    if (EdEventInfo.world_coord_enable) {
        Shift(reinterpret_cast<float *>(EdEventInfo.world_coord + 8), nx, nz);
    }
    for (float *p : {NowCursorPos, NextCursorPos, NowPartsCursorPos, fix_camera_pos, fix_chara_pos}) {
        Shift(p, nx, nz);
    }
    if (ECursorFrame) {
        Shift(ECursorFrame->position, nx, nz);
        ECursorFrame->SetPosition(ECursorFrame->position);
    }
    for (int i = 0; i < EdMoveCharaInfo.point_count; i++) {
        auto &p = EdMoveCharaInfo.points[i];
        // Plot-linked events store building-local coordinates even though
        // map_object is null: EdGetEvent resolves parts_no at query time.
        // Only unowned event points live directly in world coordinates.
        if (!p.map_object && p.parts_no < 0) {
            Shift(p.position, nx, nz);
            Shift(p.extent, nx, nz);
        }
    }
    Shift(EdMoveCharaInfo.param.position, nx, nz);
    Shift(EdMoveCharaInfo.param.camera_pos, nx, nz);
    for (int i = 0; i < EdEffectGroup.capacity; i++) {
        if (EdEffectGroup.effect_table[i].active) {
            Shift(EdEffectGroup.effect_table[i].position, nx, nz);
        }
    }
    for (auto &p : RunEffect.position) {
        Shift(p, nx, nz);
    }
    for (int i = 0; i < 8; i++) {
        Shift(SystemEffect[i].position, nx, nz);
    }
    originX = nx;
    originZ = nz;
    g->MakePartsBox();
    gfx::CutInterpolation();
    if (std::getenv("DC_WORLD_PRECISION_TRACE")) {
        std::fprintf(stderr, "world origin: %.17g %.17g\n", nx, nz);
    }
}

void BeginTick() {
#ifdef PORT
    gfx::capture::Scope profile("world.origin.begin_tick");
#endif
    if (!active || !TownTerrainEnabled(pEditGround)) {
        return;
    }
    static bool previewSiteApplied=false;
    if(!previewSiteApplied) {
        previewSiteApplied=true;
        if(const char *site=std::getenv("DC_WORLD_PREVIEW_SITE")) {
            double x,z;float yaw=0;
            if(std::sscanf(site,"%lf,%lf,%f",&x,&z,&yaw)>=2) {
                savedPlayer={{"town",0},{"x",x},{"y",terrain::SurfaceHeight(x,z)},{"z",z},{"yaw",yaw}};
                restorePlayer=true;restoreWalkTicks=0;
            }
        }
    }
    if(GameMode!=ED_MODE_WALK)restoreWalkTicks=0;
    if(restorePlayer && Chara && GameMode==ED_MODE_WALK && ++restoreWalkTicks<=2) {
        // The retail arrival event can overwrite the first placement. Keep the
        // restore armed until two consecutive controllable walking ticks.
        if(restoreWalkTicks==2)restorePlayer=false;
        Position p{savedPlayer.at("x"),savedPlayer.at("y"),savedPlayer.at("z")};
        Rebase(std::round(p.x/3200)*3200,std::round(p.z/3200)*3200);
        // Preserve the saved elevation (including platforms), lifting only if
        // a later terrain edit would otherwise restore the player underground.
        p.y=std::max(p.y,terrain::SurfaceHeight(p.x,p.z));
        Assign(Chara->pos,p);Chara->SetPosition(Chara->pos);
        Chara->GetVelocity()->x=Chara->GetVelocity()->y=Chara->GetVelocity()->z=0;
        float yaw=savedPlayer.value("yaw",0.f);
        Chara->SetRotation(0,yaw,0);
        MainCamera.SetFollow(Chara->pos[0],Chara->pos[1]+14,Chara->pos[2]);
        MainCamera.SetAngleSoon(yaw-3.14159265f);MainCamera.Step(-1);
        gfx::CutInterpolation();
        std::fprintf(stderr,"world player restored: %.17g %.17g %.17g yaw %.9g\n",p.x,p.y,p.z,yaw);
    }
    // Interior coordinates belong to their own retail scene.
    if (GameMode == ED_MODE_MAP_JUMP) {
        Rebase(0, 0);
        return;
    }
    float *focus = Chara ? Chara->pos : nullptr;
    if ((GameMode == ED_MODE_GEORAMA || GameMode == ED_MODE_GEORAMA_MENU || GameMode == ED_MODE_GEORAMA_MENU_INIT) && ECursorFrame) {
        focus = ECursorFrame->position;
    }
    if (!focus) {
        return;
    }
    auto p = PositionOf(focus);
    if (std::abs(focus[0]) > 3200 || std::abs(focus[2]) > 3200) {
        Rebase(std::round(p.x / 3200) * 3200, std::round(p.z / 3200) * 3200);
    }
}

void Loaded(CEditGround *g) {
    if (!TownTerrainEnabled(g)) {
        return;
    }
    // Match legacy placements by plot, rotation and their rounded saved position.
    // Native load order may differ for roads, bridges and supports.
    std::set<int> used;
    for (auto &p : g->parts) {
        if (p.handle >= 0) {
            positions.erase(p.pos);
            for (size_t i = 0; i < savedParts.size(); i++) {
                if (used.contains(int(i))) {
                    continue;
                }
                const auto &s = savedParts[i];
                double      x = s.at("x"), y = s.at("y"), z = s.at("z");
                if (p.parts_no != s.at("plot").get<int>() || p.rot_y != s.at("rotation").get<int>() || std::abs(X(p.pos[0]) - float(x)) > 8 || std::abs(Z(p.pos[2]) - float(z)) > 8) {
                    continue;
                }
                p.SetPosition(LocalX(x), float(y), LocalZ(z));
                positions[p.pos] = {
                    {x,        y,        z       },
                    {p.pos[0], p.pos[1], p.pos[2]}
                };
                if (std::getenv("DC_WORLD_PRECISION_TRACE")) {
                    std::fprintf(stderr, "world load placement: %d %.17g %.17g %.17g\n", p.parts_no, x, y, z);
                }
                used.insert(int(i));
                break;
            }
            Sync(p.pos);
        }
    }
}

void Load(const std::filesystem::path &dir) {
    savedPlayer=nullptr;restorePlayer=false;restoreWalkTicks=0;
    savedParts = nlohmann::json::array();
    try {
        std::ifstream f(dir / "world.json");
        if (!f) {
            return;
        }
        auto j = nlohmann::json::parse(f);
        if (j.at("version") != 1 || j.at("save_fingerprint") != Fingerprint(dir)) {
            return;
        }
        if (!j.at("parts").is_array() || j.at("parts").size() > 128) {
            return;
        }
        for (const auto &p : j.at("parts")) {
            for (const char *axis : {"x", "y", "z"}) {
                if (!std::isfinite(p.at(axis).get<double>())) {
                    return;
                }
            }
            (void) p.at("plot").get<int>();
            (void) p.at("rotation").get<int>();
        }
        savedParts = j.at("parts");
        if(j.contains("player") && j["player"].is_object()) {
            const auto &p=j["player"];
            bool valid=p.value("town",-1)==0;
            for(const char *axis:{"x","y","z","yaw"})valid=valid && p.contains(axis) && p[axis].is_number() && std::isfinite(p[axis].get<double>());
            if(valid){savedPlayer=p;restorePlayer=true;}
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "world save: ignoring invalid precision data: %s\n", e.what());
    }
}

void Capture() {
    if (active && TownTerrainEnabled(pEditGround)) {
        savedParts = nlohmann::json::array();
        for (auto &p : pEditGround->parts) {
            if (p.handle >= 0) {
                auto v = PositionOf(p.pos);
                savedParts.push_back({
                    {"plot",     p.parts_no},
                    {"rotation", p.rot_y   },
                    {"x",        v.x       },
                    {"y",        v.y       },
                    {"z",        v.z       }
                });
            }
        }
    }
}

void Leave() {
    DranFlightReset();
    Capture();
    active = false;
}

void Save(const std::filesystem::path &dir) {
    Capture();
    savedPlayer=nullptr;
    if(active && TownTerrainEnabled(pEditGround) && Chara && GameMode!=ED_MODE_MAP_JUMP) {
        auto p=PositionOf(Chara->pos);float rotation[4]{};Chara->GetRotation(rotation);
        DranFlightSavePosition(p, rotation[1]);
        savedPlayer={{"town",0},{"x",p.x},{"y",p.y},{"z",p.z},{"yaw",rotation[1]}};
    }
    try {
        auto data = nlohmann::json{
            {"version",          1               },
            {"save_fingerprint", Fingerprint(dir)},
            {"parts",            savedParts      },
            {"player",           savedPlayer     }
        }
                        .dump(2);
        if (FilesWrite(dir / "world.json", data.data(), data.size(), true) != FilesResult::kWritten) {
            std::fprintf(stderr, "world save: could not write precision data\n");
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "world save: %s\n", e.what());
    }
}

void TestTick() {
    static int ticks = 0;
    ++ticks;
    if (ticks != 220 || !TownTerrainEnabled(pEditGround)) {
        return;
    }
    if (std::getenv("DC_WORLD_REBASE_TEST")) {
        std::vector<Position> before;
        for (auto &p : pEditGround->parts) {
            if (p.handle >= 0) {
                before.push_back(PositionOf(p.pos));
            }
        }
        auto   player = PositionOf(Chara->pos);
        struct LocalEvent {int index;std::array<float,4> position,extent;};
        std::vector<LocalEvent> localEvents;
        for(int i=0;i<EdMoveCharaInfo.point_count;++i){
            auto &p=EdMoveCharaInfo.points[i];
            if(p.parts_no>=0 || p.map_object){
                LocalEvent e{i};std::copy_n(p.position,4,e.position.begin());
                std::copy_n(p.extent,4,e.extent.begin());localEvents.push_back(e);
            }
        }
        double x = originX, z = originZ;
        for (int i = 0; i < 20; i++) {
            Rebase(x + 100000000, z - 100000000);
            for(auto &e:localEvents){
                auto &p=EdMoveCharaInfo.points[e.index];
                if(!std::equal(e.position.begin(),e.position.end(),p.position) ||
                   !std::equal(e.extent.begin(),e.extent.end(),p.extent))std::abort();
            }
            Rebase(x, z);
        }
        size_t i = 0;
        for (auto &p : pEditGround->parts) {
            if (p.handle >= 0) {
                auto after = PositionOf(p.pos);
                auto v = before[i++];
                if (after.x != v.x || after.y != v.y || after.z != v.z) {
                    std::abort();
                }
            }
        }
        auto after = PositionOf(Chara->pos);
        if (after.x != player.x || after.y != player.y || after.z != player.z) {
            std::abort();
        }
        std::fprintf(stderr, "world test: 40 origin shifts preserve exact double positions and building-local interactions\n");
    }
    if (std::getenv("DC_WORLD_RELOAD_TEST")) {
        CMemoryCardAccess card;
        card.Initialize();
        if (card.LoadFromMc(kSaveSlotAutosave) != 1) {
            std::abort();
        }
        pEditGround->parts_info->Load(0, SaveData, 0);
        pEditGround->Load(0, SaveData);
        std::fprintf(stderr, "world test: native save reloaded with nonzero origin\n");
    }
    if (std::getenv("DC_WORLD_SAVE_TEST")) {
        for (auto &p : pEditGround->parts) {
            if (p.handle >= 0 && p.parts_no == 13) {
                auto v = PositionOf(p.pos);
                v.x += .125;
                v.z += .25;
                Assign(p.pos, v);
                p.SetPosition(p.pos);
                break;
            }
        }
        pEditGround->Save(0, SaveData);
        pEditGround->parts_info->Save(0, SaveData);
        if (!SaveSlotWriteAutosave()) {
            std::abort();
        }
        std::fprintf(stderr, "world test: wrote fractional far placement via native autosave\n");
    }
}

} // namespace world
