#include "dran_flight.hpp"
#include "flight_camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "camera_port.hpp"
#include "camerafollow.hpp"
#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dataread_port.hpp"
#include "ebattle.hpp"
#include "edit.hpp"
#include "editloop.hpp"
#include "editmapscript.hpp"
#include "forest_kernel.hpp"
#include "frame.hpp"
#include "gametext.hpp"
#include "glyph_draw.hpp"
#include "gamepad.hpp"
#include "platform/input.hpp"
#include "terrain_kernel.hpp"
#include "town_combat.hpp"
#include "town_terrain.hpp"
#include "world_origin.hpp"

extern int viewMode;
int        PortEdCheckKeyMode(int mode);

namespace {
alignas(64) unsigned char storage[16 * 1024 * 1024];
CDataAlloc2<1> arena;
alignas(64) unsigned char riderStorage[4 * 1024 * 1024];
CDataAlloc2<1>            riderArena;
std::vector<unsigned int> riderPack;
CCharacter                dran;
CMainChara               *owner = nullptr;
CFrame                   *ownerFrame = nullptr;
CFrame                   *riderSocket = nullptr;
std::vector<unsigned int> pack;
bool                      active = false, landing = false;
float                     speed = 0, yaw = 0, bank = 0;
float                     oldDistance = 0, oldHeight = 0;
flightcamera::Orbit cameraOrbit;
float lookYaw=0,lookPitch=0;
world::Position           departure;
float                     departureYaw = 0;
int                       ticks = 0, messageTicks = 0;
constexpr float           Clearance = 90.0f;

bool Trace() { return std::getenv("DC_DRAN_TRACE") != nullptr; }

// Retail SYNC_CHARA_CHARA follows a named animated frame with SetReference.
// Keep gameplay coordinates in world space (collision, saves, origin rebasing),
// but evaluate the riding pose and cloth in the cutscene socket's local space.
class RiderAttachment {
    CCharacter   &rider;
    CFrame       *parent;
    int           reference;
    sceVu0FVECTOR position;
    CVector3_f_   rotation;

public:
    explicit RiderAttachment(CCharacter &character)
        : rider(character), parent(character.frame->parent), reference(character.frame->reference),
          rotation(character.rotation) {
        sceVu0CopyVector(position, rider.pos);
        rider.frame->SetReference(riderSocket);
        rider.SetPosition(0, 0, 0);
        rider.SetRotation(0, 0, 0);
    }

    ~RiderAttachment() {
        rider.frame->parent = parent;
        rider.frame->reference = reference;
        rider.SetPosition(position);
        rider.SetRotation(rotation.x, rotation.y, rotation.z);
    }
};

bool Load(CMainChara *rider) {
    const char *path = "gedit/s30/chara/e411cc12a.chr";
    auto        size = DataFileSize(path);
    if (!size) {
        std::fprintf(stderr, "Dran: missing %s\n", path);
        return false;
    }
    pack.resize(((size + 2047) / 2048) * 512);
    LoadFile(const_cast<char *>(path), pack.data(), nullptr);
    arena.base = arena.buffer = storage;
    arena.limit = sizeof(storage) / 16;
    arena.used = 0;
    dran.Initialize();
    // Append to the town character texture block without discarding Toan.
    dran.LoadPackData3(pack.data(), const_cast<char *>("e411cc12a.cfg"), &arena, 8, &arena, 1, 0);
    if (!dran.frame || !dran.motion[0]) {
        std::fprintf(stderr, "dran: model load failed frame=%p motion=%p\n", (void *) dran.frame, (void *) dran.motion[0]);
        return false;
    }
    dran.SetMotion(0, 0);
    riderSocket = dran.frame->SearchFrame(const_cast<char *>("to_c"));
    if (!riderSocket) {
        std::fprintf(stderr, "dran: missing cutscene rider socket to_c\n");
        return false;
    }
    const char *ridePath = "gedit/s30/chara/e411cc01d.chr";
    auto        rideSize = DataFileSize(ridePath);
    if (!rideSize) {
        return false;
    }
    riderPack.resize(((rideSize + 2047) / 2048) * 512);
    LoadFile(const_cast<char *>(ridePath), riderPack.data(), nullptr);
    int            cfgSize = 0;
    char          *cfg = reinterpret_cast<char *>(GetPackFile(riderPack.data(), const_cast<char *>("e411cc01d.cfg"), &cfgSize));
    constexpr char motion[] =
        "MOTION 6, \"e411cc01d.mot\", \"e411cc01d.bbp\", \"e411cc01d.wgt\"\n"
        "KEY_START 1200\nKEY 10,30,0.20\nMOTION_END\n";
    if (!cfg || cfgSize < int(sizeof(motion))) {
        return false;
    }
    std::memset(cfg, ' ', cfgSize);
    std::memcpy(cfg, motion, sizeof(motion) - 1);
    riderArena.base = riderArena.buffer = riderStorage;
    riderArena.limit = sizeof(riderStorage) / 16;
    riderArena.used = 0;
    rider->LoadPackData(riderPack.data(), const_cast<char *>("e411cc01d.cfg"), &riderArena, &riderArena, nullptr);
    if (Trace()) {
        std::fprintf(stderr, "dran: riding animation %p\n", (void *) rider->motion[6]);
    }
    return rider->motion[6] != nullptr;
}

bool SafeLanding(float x, float z) {
    double wx = world::X(x), wz = world::Z(z);
    double h = world::SurfaceHeight(x, z);
    if (h < terrain::SeaLevel + 8) {
        return false;
    }
    auto river = terrain::RiverAt(wx, wz);
    if (river.width > 0 && river.distance < river.width + 15) {
        return false;
    }
    // Require a reasonably flat clearing for Dran's body, outside buildings.
    for (const auto &p : terrain::Foundations()) {
        if (std::abs(wx - p.x) < p.halfX + 70 && std::abs(wz - p.z) < p.halfZ + 70) {
            return false;
        }
    }
    for (int dx : {-60, 0, 60}) {
        for (int dz : {-60, 0, 60}) {
            if (std::abs(world::SurfaceHeight(x + dx, z + dz) - h) > 24) {
                return false;
            }
        }
    }
    for (const auto &tree : forest::Generate(wx - 100, wz - 100, 200)) {
        if (std::hypot(tree.x - wx, tree.z - wz) < 65) {
            return false;
        }
    }
    return true;
}

void ClearEvents() {
    EdMoveCharaInfo.event_no = EdMoveCharaInfo.system_event_no = -1;
    EdMoveCharaInfo.event_ready = EdMoveCharaInfo.acted = 0;
}
} // namespace

bool DranFlightActive() { return active; }

void DranFlightReset() {
    if (active) {
        MainCamera.SetDistance(oldDistance);
        MainCamera.SetHeight(oldHeight);
    }
    active = landing = false;
    owner = nullptr;
    ownerFrame = nullptr;
    riderSocket = nullptr;
    speed = bank = 0;
}

bool DranFlightStep(CMainChara *rider) {
    if (active && (owner != rider || ownerFrame != rider->frame || !TownTerrainEnabled(pEditGround))) {
        DranFlightReset();
    }
    bool allowed = TownTerrainEnabled(pEditGround) && GameMode == ED_MODE_WALK &&
                   !EdMoveCharaInfo.interior && !EdMoveCharaInfo.fishing && !EdMoveCharaInfo.key_lock &&
                   !viewMode && PortEdCheckKeyMode(1);
    if (Trace() && ((GamePad.GetPadDown() & PAD_L3) != 0 && !GamePad.On(PAD_SELECT))) {
        std::fprintf(stderr, "dran: summon allowed=%d grounded=%d combat=%d events=%d/%d\n", allowed, rider->move_info.landed, TownCombatBusy(), EdMoveCharaInfo.event_no, EdMoveCharaInfo.system_event_no);
    }
    if (!active) {
        if (!allowed || !((GamePad.GetPadDown() & PAD_L3) != 0 && !GamePad.On(PAD_SELECT)) || TownCombatBusy() || !rider->move_info.landed ||
            EdMoveCharaInfo.event_no > 0 || EdMoveCharaInfo.system_event_no > 0) {
            return false;
        }
        if (!Load(rider)) {
            return false;
        }
        owner = rider;
        ownerFrame = rider->frame;
        active = true;
        landing = false;
        speed = bank = 0;
        ticks = 0;
        yaw = rider->rotation.y;
        departure = world::PositionOf(rider->pos);
        departureYaw = yaw;
        oldDistance = MainCamera.GetDistance();
        oldHeight = MainCamera.GetHeight();
        cameraOrbit.Reset(yaw);
        lookYaw = 0;
        lookPitch = std::atan2(95.f, 260.f);
        TownCombatReset();
        if (Trace()) {
            std::fprintf(stderr, "dran: takeoff\n");
        }
    } else if (allowed && ((GamePad.GetPadDown() & PAD_L3) != 0 && !GamePad.On(PAD_SELECT))) {
        if (landing) {
            landing = false;
            TownTerrainCancelWalk();
            messageTicks = 0;
        } else {
            landing = SafeLanding(rider->pos[0], rider->pos[2]);
            messageTicks = landing ? 0 : 180;
        }
        if (Trace()) {
            std::fprintf(stderr, "dran: %s\n", landing ? "landing requested" : "keep flying (landing cancelled or no clearing)");
        }
    }
    ClearEvents();
    if (!allowed) {
        return true;
    }
    ++ticks;
    if (messageTicks > 0) {
        --messageTicks;
    }
    float turn = GamePad.GetLXf();
    float throttle = -GamePad.GetLYf();
    yaw = std::remainder(yaw - turn * .025f, 6.28318530718f);
    const auto &look = InputGetMouseLook();
    bank += (-turn * .25f - bank) * .1f;
    float target = landing ? 0 : std::max(0.f, throttle) * (GamePad.On(PAD_R1) ? 80.f : 28.f);
    speed += std::clamp(target - speed, -2.f, .7f);
    float x = rider->pos[0], y = rider->pos[1], z = rider->pos[2];
    // Sweep the terrain along the movement, so boost cannot skip a ridge.
    float nx = x + std::sin(yaw) * speed, nz = z + std::cos(yaw) * speed;
    auto flightFloor = [](float x, float z) {
        return float(terrain::FlightSurfaceHeight(world::X(x), world::Z(z)));
    };
    float floor = flightFloor(x, z);
    int   samples = int(std::ceil(speed / 32.f));
    for (int i = 1; i <= samples; ++i) {
        floor = std::max(floor, flightFloor(x + (nx - x) * i / samples, z + (nz - z) * i / samples));
    }
    if (landing) {
        nx = x;
        nz = z;
        floor = float(world::SurfaceHeight(x, z));
        bool terrainReady=TownTerrainRequestWalk(x,z);
        y = std::max(floor, y - 5.f);
        if (y <= floor + .1f && terrainReady) {
            rider->SetPosition(x, floor, z);
            rider->GetVelocity()->x = rider->GetVelocity()->y = rider->GetVelocity()->z = 0;
            rider->move_info.landed = 0; // Retail collision establishes the actual ground polygon next tick.
            rider->SetMotion(0, 4);
            rider->Step();
            DranFlightReset();
            MainCamera.SetFollow(x, floor + 14, z);
            MainCamera.Step(-1);
            if (Trace()) {
                std::fprintf(stderr, "dran: landed %.3f %.3f %.3f\n", x, floor, z);
            }
            return true;
        }
    } else {
        float climb = (GamePad.On(PAD_CROSS) ? 1.f : 0.f) - (GamePad.On(PAD_CIRCLE) ? 1.f : 0.f);
        y += climb * 12.f;
        // Gradual launch; terrain-following clearance thereafter. Flying does not apply walking gravity.
        float minimum = floor + Clearance;
        if (y < minimum) {
            y = ticks < 45 ? std::min(minimum, y + 4.f) : minimum;
        }
        y = std::min(y, 30000.f);
    }
    cameraOrbit.Step(1./60,yaw,speed,y-rider->pos[1],
        -look.yaw-GamePad.GetRXf()*.035,-look.pitch+GamePad.GetRYf()*.025);
    lookYaw=float(std::remainder(cameraOrbit.yaw-yaw,2*flightcamera::Pi));
    lookPitch=float(cameraOrbit.pitch);
    rider->SetPosition(nx, y, nz);
    rider->SetRotation(0, yaw, 0);
    rider->GetVelocity()->x = rider->GetVelocity()->y = rider->GetVelocity()->z = 0;
    rider->SetMotion(1200, 0);
    dran.SetPosition(nx, y, nz);
    dran.SetRotation(0, yaw, bank);
    dran.Step();
    {
        RiderAttachment attachment(*rider);
        rider->Step();
        rider->ClothStep(0);
    }
    MainCamera.FollowOn();
    MainCamera.SetFollow(nx, y + 40, nz);
    constexpr float orbitRadius = 276.8122f;
    MainCamera.SetDistance(orbitRadius * std::cos(lookPitch));
    MainCamera.SetHeight(orbitRadius * std::sin(lookPitch));
    MainCamera.SetAngle(yaw + 3.14159265f + lookYaw);
    MainCamera.Step(1);
    if (Trace() && ticks % 30 == 0) {
        std::fprintf(stderr, "dran: flight x=%.3f y=%.3f z=%.3f speed=%.2f floor=%.3f\n", nx, y, nz, speed, floor);
        std::fprintf(stderr, "dran: camera yaw=%.4f pitch=%.4f distance=%.3f height=%.3f worldyaw=%.4f tick=%u\n", lookYaw, lookPitch, MainCamera.GetDistance(), MainCamera.GetHeight(),cameraOrbit.yaw,ticks);
    }
    return true;
}

bool DranFlightDraw(CCharacter *rider) {
    if (!active || rider != owner || rider->frame != ownerFrame) {
        return false;
    }
    // Derive from the rebased player every draw; never keep stale local world coordinates.
    dran.SetPosition(rider->pos[0], rider->pos[1], rider->pos[2]);
    dran.Draw();
    {
        RiderAttachment attachment(*rider);
        rider->CCharacter::Draw();
        if (Trace() && ticks % 30 == 0) {
            sceVu0FMATRIX socketWorld, riderWorld;
            riderSocket->GetLWMatrix(socketWorld);
            rider->frame->GetLWMatrix(riderWorld);
            float error = 0;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    error = std::max(error, std::abs(socketWorld[i][j] - riderWorld[i][j]));
                }
            }
            std::fprintf(stderr, "dran: attachment socket=to_c error=%.6f bank=%.3f\n", error, bank);
        }
    }
    return true;
}

void DranFlightHUD() {
    if (GameMode != ED_MODE_WALK || !TownTerrainEnabled(pEditGround)) {
        return;
    }
    // Same native message font/outline and device-aware glyph renderer as
    // Georama's help line. Keep layouts instead of rebuilding text every tick.
    static std::array<GameText, 8> labels;
    static bool initialized = false;
    if (!initialized) {
        for (auto &label : labels) {
            label.KeepGameRandom(true);
            label.Mes().style = MES_EDGE_TABLE;
        }
        initialized = true;
    }
    auto hint = [&](int slot, glyphs::Button button, const char *text, int x, int y, const char *fallback) {
        if (!GlyphDrawSlot(button, CRect_i_(x, y, 28, 22), 128)) {
            labels[7].Set(fallback);
            labels[7].Draw(x, y);
        }
        labels[slot].Set(text);
        labels[slot].FitWidth(155);
        labels[slot].Draw(x + 34, y);
    };
    if (!active) {
        hint(0, glyphs::Button::L3, "Call Dran", 20, 438, "L3");
        return;
    }
    if (messageTicks && !landing) {
        labels[6].Set("Find a flat, open clearing to land");
        labels[6].Draw(20, 390);
    }
    hint(0, glyphs::Button::LStick, "Fly", 20, 416, "LS");
    hint(1, glyphs::Button::RStick, "Look", 220, 416, "RS");
    hint(2, glyphs::Button::R1, "Boost", 420, 416, "R1");
    hint(3, glyphs::Button::Cross, "Climb", 20, 442, "X");
    hint(4, glyphs::Button::Circle, "Descend", 220, 442, "O");
    hint(5, glyphs::Button::L3, landing ? "Cancel landing" : "Land", 420, 442, "L3");
}

void DranFlightSavePosition(world::Position &position, float &heading) {
    if (active) {
        position = departure;
        heading = departureYaw;
    }
}
