#include "town_combat.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "town_terrain.hpp"
#include "draw3d.hpp"
#include "mglib.hpp"
#include "renderinfo.hpp"

#include <vector>

#include "btmisc.hpp"
#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dataread_port.hpp"
#include "dngstatusdata.hpp"
#include "ebattle.hpp"
#include "edit.hpp"
#include "editloop.hpp"
#include "editmapscript.hpp"
#include "frame.hpp"
#include "gamepad.hpp"
#include "itemdata.hpp"
#include "savedata.hpp"
#include "snd.hpp"
#include "platform/paths.hpp"

extern int viewMode;
extern int chara_mode;
int PortEdCheckKeyMode(int mode);
extern unsigned int *snd_read_buf;

namespace {
// A separate arena keeps the extra motions out of the town's tightly packed PS2 arenas.
alignas(64) unsigned char motionStorage[8 * 1024 * 1024];
alignas(64) unsigned char weaponStorage[2 * 1024 * 1024];
CDataAlloc2<1> motionArena;
CDataAlloc2<1> weaponArena;
CCharacter weapon;
CCharacter *owner;
CFrame *ownerFrame;
std::vector<unsigned int> motionPack;
std::vector<unsigned int> weaponPack;
std::vector<unsigned int> voicePack;
int weaponItem = -1;
enum class Action { Idle, Slash1, Slash2, Slash3, Slash4, Slash5, Guard, Charge, ChargedSlash, ChargeApex, ChargeFall, ChargeLandWait, ChargeRecover };
Action action = Action::Idle;
int queued;
int heldFrames;
int chargeFrames;
int actionTicks;
bool soundPlayed;
bool visible;
bool jumping;
bool leapLaunched;
float jumpStartY, jumpPeakY;

struct Tuning {
    float attackSpeed = 1.0f;
    float jumpVelocity = 2.0f;
    int comboHits = 5;
    int chargeFrames = 30;
} tuning;

bool Trace() { return std::getenv("DC_TOWN_COMBAT_TRACE") != nullptr; }

// Settings reload at a simulation boundary. Failed/partial editor writes leave the
// previous settings intact. A code rebuild still requires restarting the process.
void ReloadTuning() {
    static int poll;
    static bool initialized;
    static std::filesystem::file_time_type stamp;
    if (poll++ % 30 != 0) return;
    auto path = PathsSaveRoot() / "town-combat.json";
    std::error_code error;
    if (!initialized && !std::filesystem::exists(path, error)) {
        std::ofstream file(path);
        file << "{\n  \"attack_speed\": 1.0,\n  \"charge_jump_velocity\": 2.0,\n"
                "  \"combo_hits\": 5,\n  \"charge_frames\": 30\n}\n";
    }
    auto now = std::filesystem::last_write_time(path, error);
    if (error || (initialized && now == stamp)) return;
    try {
        std::ifstream file(path);
        auto json = nlohmann::json::parse(file);
        Tuning next;
        next.attackSpeed = json.value("attack_speed", 1.0f);
        next.jumpVelocity = json.value("charge_jump_velocity", 2.0f);
        next.comboHits = json.value("combo_hits", 5);
        next.chargeFrames = json.value("charge_frames", 30);
        if (!std::isfinite(next.attackSpeed) || !std::isfinite(next.jumpVelocity) ||
            next.attackSpeed < 0.25f || next.attackSpeed > 3.0f ||
            next.jumpVelocity < 1.0f || next.jumpVelocity > 8.0f ||
            next.comboHits < 1 || next.comboHits > 5 ||
            next.chargeFrames < 1 || next.chargeFrames > 180)
            throw std::runtime_error("town combat settings outside supported range");
        tuning = next;
        stamp = now;
        initialized = true;
        std::fprintf(stderr, "town combat: settings reloaded speed=%.2f jump=%.2f combo=%d charge=%d\n",
                     tuning.attackSpeed, tuning.jumpVelocity, tuning.comboHits, tuning.chargeFrames);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "town combat: keeping previous settings: %s\n", e.what());
    }
}

void CancelAction() {
    action = Action::Idle;
    queued = 0;
    visible = false;
    heldFrames = chargeFrames = 0;
    actionTicks = 0;
    soundPlayed = false;
}

void ResetArena(CDataAlloc2<1> &arena, unsigned char *storage, int bytes) {
    arena.base = arena.buffer = storage;
    arena.limit = bytes / 16;
    arena.used = 0;
}

bool ReadPack(const char *path, std::vector<unsigned int> &pack) {
    auto size = DataFileSize(path);
    if (!size) return false;
    // LoadFile emulates a disc read, including the last full 2048-byte sector.
    pack.resize(((size + 2047) / 2048) * 512);
    LoadFile(const_cast<char *>(path), pack.data(), nullptr);
    return true;
}

bool Prepare(CMainChara *chara) {
    if (owner == chara && ownerFrame == chara->frame && chara->motion_start[7] == 1000)
        return true;
    TownCombatReset();
    // Only the normal town Toan model has these base motions. Fishing, ladders and
    // cutscene replacements retain their own animation tables.
    if (!chara->frame || chara->motion_end[0] != 11 ||
        !ReadPack("dun/mainchara/c01d.chr", motionPack)) return false;
    int size = 0;
    auto *config = reinterpret_cast<char *>(GetPackFile(motionPack.data(), const_cast<char *>("base.cfg"), &size));
    constexpr char extension[] =
        "MOTION 7, \"c01d.mot\", \"c01d.bbp\", \"c01d.wgt\"\n"
        "SHADOW_MOTION \"c01s.mot\", \"c01s.bbp\", \"c01s.wgt\"\n"
        "KEY_START 1000\n"
        // ToanKey_Play uses retail combo motions 36..40, not the old 11/12 clips.
        "KEY 820,830,0.3\nKEY 830,838,0.3\nKEY 838,847,0.3\n"
        "KEY 847,857,0.3\nKEY 856,884,0.3\nKEY 216,224,0.3\n"
        "KEY 160,170,0.4\nKEY 180,191,0.4\nKEY 191,192,0.0\n"
        "KEY 191,196,0.2\nKEY 196,196,0.0\nKEY 196,206,0.4\nMOTION_END\n";
    if (!config || size < sizeof(extension)) return false;
    std::memset(config, ' ', size);
    std::memcpy(config, extension, sizeof(extension) - 1);
    ResetArena(motionArena, motionStorage, sizeof(motionStorage));
    chara->LoadPackData(motionPack.data(), const_cast<char *>("base.cfg"), &motionArena, &motionArena, nullptr);
    owner = chara;
    ownerFrame = chara->frame;
    std::fprintf(stderr, "town combat: Toan sword motions ready\n");
    return true;
}

bool Equip(CMainChara *chara) {
    if (!SaveData) return false;
    auto *status = SaveData->GetDngStatus();
    int slot = status->equipped_weapon_slot[0];
    if (slot < 0 || slot >= 11) return false;
    int item = status->chara_weapons[0][slot].item_no;
    if (item < ITEM_WEAPON_DAGGER_BROKEN || item >= ITEM_WEAPON_WOODENSLINGSHOT_BROKEN) return false;
    if (item == weaponItem && weapon.frame) return true;
    char name[64] = {}, cfg[64] = {}, path[128];
    BtGetWeaponNamePath3(name, cfg, item);
    if (!name[0]) return false;
    std::snprintf(path, sizeof(path), "commenu/weapon/%s", name);
    if (!ReadPack(path, weaponPack)) return false;
    ResetArena(weaponArena, weaponStorage, sizeof(weaponStorage));
    weapon.Initialize();
    // Share the town character texture block; keep its existing textures.
    weapon.LoadPackData3(weaponPack.data(), cfg, &weaponArena, 8, &weaponArena, 1, 0);
    CFrame *hand = chara->frame->SearchFrame(const_cast<char *>("weapon"));
    if (!hand || !weapon.frame) return false;
    weapon.frame->SetReference(hand);
    weapon.SetPosition(0.0f, 0.0f, 0.0f);
    weapon.SetRotation(0.0f, 0.0f, 0.0f);
    weaponItem = item;
    return true;
}

bool Allowed() {
    return !EdMoveCharaInfo.interior && !EdMoveCharaInfo.fishing &&
           !EdMoveCharaInfo.key_lock && !(chara_mode & 1) && !viewMode &&
           GameMode == ED_MODE_WALK && PortEdCheckKeyMode(1);
}

bool IsSlash() { return action >= Action::Slash1 && action <= Action::Slash5; }

int MotionIndex() { return static_cast<int>(action) - 1; }

void StartAction(Action next) {
    action = next;
    if (next == Action::ChargedSlash) leapLaunched = false;
    actionTicks = 0;
    soundPlayed = false;
    if (Trace() && next != Action::Idle)
        std::fprintf(stderr, "town combat: motion %d\n", 1000 + MotionIndex());
}

void PlaySwingSound() {
    // Town sound packs do not load Toan's voice/sword bank. Use a separately
    // sized buffer and restore the engine's shared loader pointer afterwards.
    auto size = DataFileSize("sound/voice/voice0.snd");
    if (!size) return;
    // SndSetReadBuffer rounds the pointer UP to 64-byte alignment, then the disc
    // loader writes whole sectors. Reserve both forms of padding.
    voicePack.resize(((size + 2047) / 2048) * 512 + 16);
    auto *previous = snd_read_buf;
    SndSetReadBuffer(voicePack.data());
    SndVoiceLoad(0);
    SndSetReadBuffer(previous);
    int index = MotionIndex();
    int swing = index == 1 || index == 4 || action == Action::ChargeFall ? 401 : 400;
    int shout = index == 1 || index == 2 ? 421 : index == 4 || action == Action::ChargeFall ? 422 : 420;
    SndSePlay(swing, -1, 0);
    SndSePlay(shout, -1, 0);
    if (Trace()) std::fprintf(stderr, "town combat: sound %d shout %d\n", swing, shout);
}
}

void TownCombatReset() {
    owner = nullptr;
    ownerFrame = nullptr;
    weaponItem = -1;
    CancelAction();
}

bool TownCombatBusy() {
    // Input and drawing may run more often than simulation. Never mutate action
    // state from either: a one-frame floor/landing flag must not cancel a swing.
    return action != Action::Idle && Allowed();
}

bool TownCombatBeforeMove(CMainChara *chara) {
    ReloadTuning();
    if (!Allowed()) { CancelAction(); jumping = false; return false; }
    if (chara->motion_end[0] != 11) return false;
    if (action == Action::ChargedSlash && !leapLaunched && chara->motion[7] &&
        chara->motion[7]->state.time >= 187.0f && chara->move_info.landed) {
        CVector3_f_ velocity;
        chara->GetVelocity(&velocity);
        velocity.y = tuning.jumpVelocity;
        chara->SetVelocity(velocity);
        jumping = true;
        leapLaunched = true;
        jumpStartY = jumpPeakY = chara->pos[1];
        if (Trace()) std::fprintf(stderr, "town combat: jump velocity %.2f\n", velocity.y);
    }
    return true; // Suppress the retail DEBUG Circle jump; only the charged attack leaps.
}

void TownCombatStep(CMainChara *chara) {
    visible = false;
    // Actual interactions interrupt; merely being near an event doesn't end an
    // attack already underway. Landing/falling bits are not control locks.
    if (!Allowed() ||
        EdMoveCharaInfo.event_no > 0 || EdMoveCharaInfo.system_event_no > 0) {
        CancelAction();
        return;
    }
    if (jumping) {
        jumpPeakY = std::max(jumpPeakY, chara->pos[1]);
        if (chara->move_info.landed) {
            if (Trace()) std::fprintf(stderr, "town combat: jump height %.2f\n", jumpPeakY - jumpStartY);
            jumping = false;
        }
    }
    if (action == Action::Idle && (EdMoveCharaInfo.acted || EdMoveCharaInfo.event_ready)) return;
    if (!Prepare(chara)) return;
    bool pressed = GamePad.Down(PAD_CROSS) != 0;
    bool held = GamePad.On(PAD_CROSS) != 0;
    bool guard = GamePad.On(PAD_R1) != 0;
    heldFrames = held ? heldFrames + 1 : 0;
    if (IsSlash() && pressed)
        queued = std::min(queued + 1, std::max(0, tuning.comboHits - 1 - MotionIndex()));
    bool attacking = IsSlash() || action == Action::ChargedSlash ||
                     action == Action::ChargeFall || action == Action::ChargeRecover;
    auto *motion = chara->motion[7];
    int index = MotionIndex();
    if ((IsSlash() || action == Action::ChargeFall) && actionTicks > 0) {
        constexpr float soundFrame[] = {825, 833, 842, 852, 864};
        float at = IsSlash() ? soundFrame[index] : 193.0f;
        if (!soundPlayed && motion->state.time >= at) {
            PlaySwingSound();
            soundPlayed = true;
        }
    }
    // CCharacter::Step computes motion_state BEFORE honoring the restart flag.
    // Thus replaying an ended clip can report "finished" on its very first tick.
    // Inspect our motion set's actual time instead of that stale flag (town code
    // also changes motion_no to its locomotion clip before calling this hook).
    bool finished = attacking && actionTicks > 0 &&
        motion->state.time >= motion->motion_info[index].end -
                              motion->motion_info[index].speed * tuning.attackSpeed - 0.01f;
    if (finished) {
        if (action == Action::ChargedSlash) StartAction(Action::ChargeApex);
        else if (action == Action::ChargeFall) StartAction(Action::ChargeLandWait);
        else if (IsSlash() && queued > 0 && index + 1 < tuning.comboHits) {
            --queued;
            StartAction(static_cast<Action>(static_cast<int>(action) + 1));
        } else if (action == Action::Slash1 && held && heldFrames >= actionTicks) {
            StartAction(Action::Charge);
            chargeFrames = 0;
        } else StartAction(Action::Idle);
    } else if (action == Action::ChargeApex) {
        CVector3_f_ velocity;
        chara->GetVelocity(&velocity);
        if (velocity.y <= -0.4f || chara->move_info.landed) StartAction(Action::ChargeFall);
    } else if (action == Action::ChargeLandWait) {
        if (chara->move_info.landed) StartAction(Action::ChargeRecover);
    } else if (action == Action::Charge) {
        ++chargeFrames;
        if (!held) StartAction(chargeFrames >= tuning.chargeFrames ? Action::ChargedSlash : Action::Idle);
    } else if (action == Action::Guard && !guard) StartAction(Action::Idle);
    if (action == Action::Idle && chara->move_info.landed) {
        if (pressed) { StartAction(Action::Slash1); queued = 0; }
        else if (guard) StartAction(Action::Guard);
    }
    if (action == Action::Idle) {
        return;
    }
    if (!Equip(chara)) { CancelAction(); return; }
    index = MotionIndex();
    bool looping = action == Action::Guard || action == Action::Charge ||
                   action == Action::ChargeApex || action == Action::ChargeLandWait;
    chara->SetMotion(1000 + index, (looping ? 0 : 2) | (actionTicks == 0 ? 4 : 0),
                     motion->motion_info[index].speed * tuning.attackSpeed);
    visible = true;
    if (Trace()) std::fprintf(stderr, "town combat: tick motion=%d age=%d time=%.2f x=%.2f y=%.2f z=%.2f\n",
                             1000 + index, actionTicks, motion->state.time, chara->pos[0], chara->pos[1], chara->pos[2]);
    ++actionTicks;
}

void TownCombatDraw(CCharacter *chara) {
    if (!visible || chara != owner || chara->frame != ownerFrame || !Allowed() || !weapon.frame) return;
    weapon.Step();
    weapon.Draw();
}

PORT_OVERRIDE void CMainChara::Draw() {
    CCharacter::Draw();
    TownCombatDraw(this);
}
