#include "town_mobs.hpp"
#include "town_combat.hpp"
#include "gamepad.hpp"
#include "gametext.hpp"
#include "glyph_draw.hpp"
#include "snd.hpp"
#include "sound_effect.h"
#include "audio/mixer.hpp"
#include "night_spawn.hpp"
#include "town_terrain.hpp"
#include "terrain_kernel.hpp"
#include "world_origin.hpp"
#include "dran_flight.hpp"
#include "gfx/capture.hpp"
#include "character.hpp"
#include "camera.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dataread_port.hpp"
#include "dngstatusdata.hpp"
#include "edit.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editloop3.hpp"
#include "editmapscript.hpp"
#include "ebattle.hpp"
#include "gamemode.hpp"
#include "frame.hpp"
#include "mapparts.hpp"
#include "monstorunit.hpp"
#include "objanime.hpp"
#include "savedata.hpp"
#include "texture.hpp"
#include "mglib.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>

namespace {
// Assets are shared, but each instance owns its motion state, as in the retail
// CMonstorUnit. No dungeon scripts or room pointers are installed in town.
constexpr int Kinds=3;
constexpr int RetailId[Kinds]={1,3,52}; // Skeleton Soldier, Dasher, Cave Bat.
struct Asset {CCharacter model;std::vector<unsigned int> pack;};
struct Mob {
    CCharacter model;
    world::Position position;
    int kind=0,hp=0,idle=0,decision=0,death=0,sunTicks=0,sunRemainder=0;
    int action=0,age=0,cooldown=0,flash=0; // 0 locomotion, 1 attack, 2 hit reaction
    bool struck=false;
    float yaw=0;
    bool moving=false,aggro=false;
};
std::array<Asset,Kinds> assets;
std::array<Mob,nightspawn::Cap> mobs;
alignas(64) unsigned char storage[16*1024*1024];
CDataAlloc2<1> arena;
bool loaded=false,failed=false;
std::shared_ptr<audio::Bank> combatBank;
int tick=0,subtick=0,packRemaining=0,packKind=0;
double packX=0,packZ=0;
int locked=-1,playerInvulnerability=0,knockout=0;
world::Position respawn;
const char *Names[]={"Skeleton Soldier","Dasher","Cave Bat"};
std::uint32_t randomState=0x8a3e4f19;
std::vector<world::Position> lights;
std::map<std::pair<std::int64_t,std::int64_t>,bool> dryCells;
bool Trace(){return std::getenv("DC_MOBS_TRACE")!=nullptr;}
std::uint32_t Random(){randomState^=randomState<<13;randomState^=randomState>>17;randomState^=randomState<<5;return randomState;}
double Unit(){return Random()/4294967296.0;}
int Count(){int n=0;for(auto &m:mobs)n+=m.hp>0;return n;}
bool Load(){
    if(loaded || failed)return loaded;
    gfx::capture::Scope scope("world.mobs.load");
    arena.base=arena.buffer=storage;arena.used=0;arena.limit=sizeof(storage)/16;
    for(int k=0;k<Kinds;++k){
        auto &a=assets[k];char path[96];
        std::snprintf(path,sizeof(path),"dun/monstor/%s.chr",MonstorTable[RetailId[k]].model_name[0]);
        int size=DataFileSize(path);if(!size){failed=true;return false;}
        a.pack.resize(((size+2047)/2048)*512);
        LoadFile(path,a.pack.data(),nullptr);
        a.model.Initialize();
        a.model.LoadPackData3(a.pack.data(),const_cast<char*>("info.cfg"),&arena,8,&arena,1,0);
        if(!a.model.frame || !a.model.motion[0]){failed=true;return false;}
        SetFrameAttr(a.model.frame,1);
        a.model.foot_sound_enable=a.model.event_enable=0;
        a.model.SetMotion(0,0);
    }
    // Keep dungeon impacts on an unused mixer port; do not replace town/door banks.
    std::vector<unsigned int> sounds((DataFileSize("sound/set/snd100.snd")+2047)/2048*512);
    if(!sounds.empty()){
        LoadFile(const_cast<char*>("sound/set/snd100.snd"),sounds.data(),nullptr);
        int hs=0,bs=0;
        auto *hd=reinterpret_cast<const std::uint8_t*>(GetPackFile(sounds.data(),const_cast<char*>("d01a_i.hd"),&hs));
        auto *bd=reinterpret_cast<const std::uint8_t*>(GetPackFile(sounds.data(),const_cast<char*>("d01a_i.bd"),&bs));
        if(hd && bd && hs>0 && bs>0)combatBank=audio::Bank::Create({hd,size_t(hs)},{bd,size_t(bs)});
        if(combatBank){auto &mix=audio::DefaultMixer();mix.BindBank(9,combatBank);mix.SetAttribute(9,0x3040);mix.SetVolume(9,256);}
        if(combatBank && std::getenv("DC_TEST_MOBS_COMBAT"))for(int note:{20,21,23}){
            audio::Mixer check;check.BindBank(9,combatBank);check.SetAttribute(9,0x3040);check.SetVolume(9,256);
            check.ShortMessage(9,0xC0|(36<<8));
            std::array<std::uint8_t,7> message={0xFD,0x10,0,std::uint8_t(note),0xFF,127,0};check.HsMessage(9,message);
            std::vector<float> pcm(48000);check.Render(pcm.data(),24000);float peak=0;
            for(float sample:pcm)peak=std::max(peak,std::abs(sample));
            std::fprintf(stderr,"night mobs: audio verify note %d peak %.6f\n",note,double(peak));
        }
    }
    loaded=true;
    std::fprintf(stderr,"night mobs: loaded Skeleton Soldier, Dasher, Cave Bat; cap %d\n",nightspawn::Cap);
    return true;
}
bool Clear(double x,double z,double radius=8){
    for(const auto &f:terrain::Foundations())
        if(std::abs(x-f.x)<f.halfX+radius && std::abs(z-f.z)<f.halfZ+radius)return false;
    return true;
}
void GatherLights(){
    lights.clear();
    auto scan=[](auto &parts){for(auto &p:parts){
        if(p.handle<0)continue;
        for(int i=0;i<MAP_PARTS_EFFECT_MAX;++i){
            auto *e=p.effect[i];
            if(p.effect_on[i]<=0 || !e || !e->frame || !CheckEditEffect(e,NowTime))continue;
            if(e->kind!=EDIT_EFFECT_FIRE_SMALL && e->kind!=EDIT_EFFECT_FIRE_MEDIUM && e->kind!=EDIT_EFFECT_FIRE_LARGE)continue;
            float local[4];e->frame->GetWorldPosition(local,e->offset);
            lights.push_back({world::X(local[0]),local[1],world::Z(local[2])});
        }
    }};
    scan(pEditGround->parts);scan(pEditGround->fixed_parts);
}
bool Lit(world::Position p){
    // Continuous analogue of a level-14 torch losing one level per metre.
    for(auto l:lights)if(nightspawn::DistanceSquared(p.x-l.x,p.y-l.y,p.z-l.z)<140*140)return true;
    return false;
}
bool Surface(double x,double z,float &height){
    return TownTerrainResidentHeight(x,z,height,1) && height>terrain::SeaLevel+2 && Clear(x,z);
}
bool Dry(double x,double z){
    auto key=std::pair{std::int64_t(std::floor(x/8)),std::int64_t(std::floor(z/8))};
    auto found=dryCells.find(key);if(found!=dryCells.end())return found->second;
    if(dryCells.size()>=4096)dryCells.clear();
    auto r=terrain::RiverAt(key.first*8.+4,key.second*8.+4);
    return dryCells[key]=r.width<=0 || r.distance>r.width+14;
}
bool Visible(world::Position a,world::Position b){
    for(int i=1;i<8;++i){
        double t=i/8.0,x=a.x+(b.x-a.x)*t,z=a.z+(b.z-a.z)*t;
        float height;
        if(!Clear(x,z,2) || !TownTerrainResidentHeight(x,z,height,1) || height>a.y+(b.y-a.y)*t+15)return false;
    }
    return true;
}
void Spawn(world::Position player,bool night){
    if(tick%5!=0)return; // 4 attempts/s instead of 20: 80% reduction.
    if(!night || Count()>=nightspawn::Cap){packRemaining=0;return;}
    if(packRemaining==0){
        // Sample eligible 16m cells around the player, then a small same-kind pack.
        constexpr double cell=16*nightspawn::Block;
        packX=(std::floor(player.x/cell)+int(Random()%17)-8)*cell+Unit()*cell;
        packZ=(std::floor(player.z/cell)+int(Random()%17)-8)*cell+Unit()*cell;
        unsigned choice=Random()%10;packKind=choice<4?0:choice<8?1:2;
        packRemaining=1+Random()%4;
    }
    --packRemaining;
    double x=packX+(Unit()-Unit())*4*nightspawn::Block;
    double z=packZ+(Unit()-Unit())*4*nightspawn::Block;
    float y;
    if(!Surface(x,z,y))return;
    world::Position pos{x,y,z};
    auto river=terrain::RiverAt(x,z,y);
    bool dry=river.width<=0 || river.distance>river.width+8;
    double d2=nightspawn::DistanceSquared(x-player.x,y-player.y,z-player.z);
    if(!nightspawn::CanSpawn(night,Lit(pos),dry,true,Count(),d2))return;
    for(auto &other:mobs)if(other.hp>0 && std::hypot(x-other.position.x,z-other.position.z)<16)return;
    for(auto &m:mobs)if(m.hp<=0 && m.death==0){
        m.kind=packKind;m.position=pos;m.hp=MonstorTable[RetailId[m.kind]].max_hp;
        m.action=m.age=m.cooldown=m.flash=0;m.struck=false;
        m.idle=0;m.decision=0;m.sunTicks=m.sunRemainder=0;m.aggro=false;m.yaw=float(Unit()*6.283185307);
        m.model=assets[m.kind].model;
        m.model.motion[0]=&m.model.motion_type;
        m.model.cloth=m.model.cloth_buf;
        m.model.SetMotion(0,4);
        if(Trace())std::fprintf(stderr,"night mobs: spawn kind %d x %.3f y %.3f z %.3f distance %.3f count %d\n",m.kind,x,double(y),z,std::sqrt(d2),Count());
        return;
    }
}
void Animate(Mob &m){
    m.model.SetPosition(world::LocalX(m.position.x),float(m.position.y),world::LocalZ(m.position.z));
    m.model.SetRotation(0,m.yaw,0);
    m.model.Step();
}
void BeginAction(Mob &m,int action,int motion){
    m.action=action;m.age=0;m.moving=false;m.struck=false;
    m.model.SetMotion(motion,6);
}
bool Finished(Mob &m){
    auto *info=m.model.GetMotionInfo(m.model.motion_no);
    return m.age>1 && info && m.model.GetNowTime()>=info->end-std::max(.01f,info->speed)-.01f;
}
void Die(Mob &m){
    m.hp=0;m.death=180;BeginAction(m,3,11);
    if(locked==int(&m-mobs.data()))locked=-1;
    if(Trace())std::fprintf(stderr,"night mobs: death kind %d motion %d\n",m.kind,m.model.motion_no);
}
void ImpactSound(int note){
    if(!combatBank)return;
    auto &mix=audio::DefaultMixer();mix.ShortMessage(9,0xC0|(36<<8));
    std::array<std::uint8_t,7> message={0xFD,0x10,0,std::uint8_t(note),0xFF,127,0};mix.HsMessage(9,message);
    if(Trace())std::fprintf(stderr,"night mobs: impact sound %d\n",note);
}
void Hurt(Mob &m,int damage){
    m.hp-=damage;m.aggro=true;m.flash=12;
    if(m.hp<=0)Die(m);
    else BeginAction(m,2,8);
    if(Trace())std::fprintf(stderr,"night mobs: hurt kind %d hp %d motion %d\n",m.kind,m.hp,m.model.motion_no);
    ImpactSound(20);
}

}

void TownMobsReset(){
    for(auto &m:mobs){m.hp=0;m.death=0;}
    audio::DefaultMixer().Stop(9);combatBank.reset();
    locked=-1;playerInvulnerability=knockout=0;
    loaded=failed=false;subtick=tick=packRemaining=0;lights.clear();dryCells.clear();
}

void TownMobsTick(){
    gfx::capture::Scope scope("world.mobs.tick");
    if(!TownTerrainEnabled(pEditGround) || !Chara || EdMoveCharaInfo.interior || GameMode!=ED_MODE_WALK || DranFlightActive())return;
    if(!Load())return;
    if(subtick==0 && std::getenv("DC_TEST_MOBS_NIGHT")){
        NowTime=6;SaveData->SetNowTime(NowTime);
    }
    if(subtick==0)respawn=world::PositionOf(Chara->pos);
    // Retail simulation is 60 Hz; spawn/despawn decisions match Minecraft's 20 Hz.
    bool decisionTick=(++subtick%3)==0;
    bool night=EdGetTime(NowTime)==ED_TIME_NIGHT;
    auto player=world::PositionOf(Chara->pos);
    if(playerInvulnerability>0)--playerInvulnerability;
    if(knockout>0){
        if(--knockout==0){
            world::Assign(Chara->pos,respawn);
            auto *status=SaveData->GetDngStatus();status->SetNextLife(0,status->max_hp[0],0);
            TownCombatReset();for(auto &m:mobs){m.hp=m.death=0;}locked=-1;
            std::fprintf(stderr,"night mobs: knockout recovery at arrival position\n");
        }
        return;
    }
    if(locked>=0 && (mobs[locked].hp<=0 || std::hypot(mobs[locked].position.x-player.x,mobs[locked].position.z-player.z)>480 || !Visible(player,mobs[locked].position)))locked=-1;
    if((GamePad.GetPadDown()&PAD_R3) && !GamePad.On(PAD_SELECT) && !EdMoveCharaInfo.key_lock){
        if(locked>=0)locked=-1;
        else {
            double best=320;
            for(int i=0;i<nightspawn::Cap;++i)if(mobs[i].hp>0){
                double d=std::hypot(mobs[i].position.x-player.x,mobs[i].position.z-player.z);
                if(d<best && Visible(player,mobs[i].position)){best=d;locked=i;}
            }
        }
        if(Trace())std::fprintf(stderr,"night mobs: lock %d\n",locked);
    }
    if(decisionTick){++tick;if(tick%20==1)GatherLights();}
    if(decisionTick && std::getenv("DC_TEST_MOBS_CYCLE")){
        if(tick==400){NowTime=1;SaveData->SetNowTime(NowTime);night=false;std::fprintf(stderr,"night mobs: test dawn\n");}
        if(tick==450 && !std::getenv("DC_TEST_MOBS_SUN")){
            player.x+=4000;player.y=terrain::SurfaceHeight(player.x,player.z);
            world::Assign(Chara->pos,player);
            std::fprintf(stderr,"night mobs: test travel\n");
        }
    }
    // Deterministic combat fixtures use only copied saves and the same combat paths.
    if(std::getenv("DC_TEST_MOBS_COMBAT")){
        if(subtick==1480){
            TownCombatReceiveHit(1000,0);knockout=150;
            std::fprintf(stderr,"night mobs: test knockout hp %d\n",int(SaveData->GetDngStatus()->hp[0]));
        }
        int phase=(subtick-120)/450,at=(subtick-120)%450;
        if(subtick>=120 && phase<3){
            auto &m=mobs[0];
            if(at==0){
                for(auto &other:mobs){other.hp=other.death=0;}
                m.kind=phase;m.hp=MonstorTable[RetailId[phase]].max_hp;m.death=0;
                m.action=m.cooldown=m.age=m.flash=0;m.struck=false;m.aggro=true;m.idle=0;m.decision=10000;
                m.model=assets[phase].model;m.model.motion[0]=&m.model.motion_type;m.model.cloth=m.model.cloth_buf;
                m.model.SetMotion(0,4);
                m.position={player.x+std::sin(Chara->rotation.y)*20,player.y,player.z+std::cos(Chara->rotation.y)*20};
                playerInvulnerability=0;
                auto *status=SaveData->GetDngStatus();status->SetNextLife(0,status->max_hp[0],0);
                int slot=status->equipped_weapon_slot[0];if(slot>=0 && slot<11)status->chara_weapons[0][slot].attack=3;
                std::fprintf(stderr,"night mobs: test combat kind %d\n",phase);
            }
            // An explicit wind-up followed by a dodge exercises impact-time range checks.
            if(at==140){m.position={player.x,player.y,player.z+20};m.yaw=3.14159265f;BeginAction(m,1,phase==2?15:13);}
            if(at==150){world::Assign(Chara->pos,{player.x+80,player.y,player.z});player=world::PositionOf(Chara->pos);}
            if(at==250){
                m.position={player.x,player.y,player.z+20};m.aggro=true;Chara->rotation.y=0;
                BeginAction(m,1,phase==2?15:13);TownMobsSwordHit(static_cast<CMainChara*>(Chara));
                std::fprintf(stderr,"night mobs: test interrupted action %d\n",m.action);
            }
            if(at==320)Hurt(m,1000);
            if(at>250 && at<300 && at%10==0)std::fprintf(stderr,"night mobs: test reaction kind %d motion %d time %.2f\n",phase,m.model.motion_no,m.model.GetNowTime());
            if(at>320 && at%20==0)std::fprintf(stderr,"night mobs: test death kind %d motion %d time %.2f\n",phase,m.model.motion_no,m.model.GetNowTime());
        }
    }
    for(auto &m:mobs){
        if(m.flash>0)--m.flash;
        if(m.cooldown>0)--m.cooldown;
        if(m.hp<=0){
            if(m.death>0){
                --m.death;++m.age;Animate(m);
                if(Finished(m)){
                    int next=m.kind==2?std::min(14,m.model.motion_no+1):12;
                    if(m.model.motion_no!=next){m.model.SetMotion(next,6);m.age=0;}
                }
            }
            continue;
        }
        double d2=nightspawn::DistanceSquared(m.position.x-player.x,m.position.y-player.y,m.position.z-player.z);
        if(decisionTick){
            m.idle=d2<=nightspawn::IdleDistance*nightspawn::IdleDistance?0:m.idle+1;
            if(nightspawn::Despawn(d2,m.idle,Random())){
                m.hp=0;if(Trace())std::fprintf(stderr,"night mobs: despawn distance %.3f count %d\n",std::sqrt(d2),Count());continue;
            }
            if(m.action==0 && --m.decision<=0){m.decision=20+Random()%80;m.moving=(Random()%4)!=0;m.yaw=float(Unit()*6.283185307);}
            if(m.aggro && d2>480*480)m.aggro=false;
            if(!m.aggro && d2<320*320 && std::abs(m.position.y-player.y)<100 && Visible(m.position,player)){
                m.aggro=true;
                if(Trace())std::fprintf(stderr,"night mobs: aggro kind %d distance %.3f\n",m.kind,std::sqrt(d2));
            }
            if(night)m.sunTicks=0;
            else if(++m.sunTicks==nightspawn::TickRate){
                m.sunTicks=0;
                m.hp-=nightspawn::SunDamage(MonstorTable[RetailId[m.kind]].max_hp,m.sunRemainder);
                if(Trace())std::fprintf(stderr,"night mobs: daylight kind %d hp %d slot %td\n",m.kind,m.hp,&m-mobs.data());
                if(m.hp<=0){Die(m);continue;}
            }
        }
        if(m.action==1 || m.action==2){
            ++m.age;Animate(m);
            auto *info=m.model.GetMotionInfo(m.model.motion_no);
            if(m.action==1 && !m.struck && info && m.model.GetNowTime()>=info->start+(info->end-info->start)*.55f){
                m.struck=true;
                double dx=player.x-m.position.x,dz=player.z-m.position.z,range=std::hypot(dx,dz);
                bool facing=range<1 || (dx*std::sin(m.yaw)+dz*std::cos(m.yaw))/range>.5;
                bool hit=range<28 && std::abs(player.y-m.position.y)<24 && facing && Visible(m.position,player);
                if(hit && playerInvulnerability==0){
                    auto *status=SaveData->GetDngStatus();
                    int damage=std::max(1,(m.kind==1?8:m.kind==2?3:5)-int(status->defense[0])/4);
                    bool damaged=TownCombatReceiveHit(damage,float(std::atan2(-dx,-dz)));
                    ImpactSound(damaged?21:23);
                    playerInvulnerability=45;
                    if(status->hp[0]<=0){knockout=150;locked=-1;}
                    if(Trace())std::fprintf(stderr,"night mobs: attack impact kind %d damage %d player_hp %d\n",m.kind,damaged?damage:0,int(status->hp[0]));
                }else if(Trace())std::fprintf(stderr,"night mobs: attack missed kind %d range %.2f\n",m.kind,range);
            }
            if(Finished(m)){m.action=0;m.cooldown=45;m.model.SetMotion(0,4);}
            continue;
        }
        if(m.aggro && m.cooldown==0 && std::hypot(player.x-m.position.x,player.z-m.position.z)<24 && std::abs(player.y-m.position.y)<24 && Visible(m.position,player)){
            m.yaw=float(std::atan2(player.x-m.position.x,player.z-m.position.z));
            BeginAction(m,1,m.kind==2?15:13);Animate(m);
            if(Trace())std::fprintf(stderr,"night mobs: attack start kind %d\n",m.kind);
            continue;
        }
        if(m.aggro){
            m.yaw=float(std::atan2(player.x-m.position.x,player.z-m.position.z));
            m.moving=std::hypot(player.x-m.position.x,player.z-m.position.z)>18;
        }
        if(m.moving){
            double speed=m.aggro?(m.kind==1?1.2:m.kind==2?1.0:.65):(m.kind==1?.45:.25);
            bool moved=false;
            for(double turn:{0.,.65,-.65,1.2,-1.2}){
                double angle=m.yaw+turn,x=m.position.x+std::sin(angle)*speed,z=m.position.z+std::cos(angle)*speed;
                float y;
                // Probe ahead to avoid taking tiny steps into walls or river banks.
                double px=x+std::sin(angle)*12,pz=z+std::cos(angle)*12;float ahead;
                if(!Surface(x,z,y) || std::abs(y-m.position.y)>2 || !Surface(px,pz,ahead) || std::abs(ahead-y)>12)continue;
                if(!Dry(px,pz))continue;
                bool blocked=false;
                for(auto &other:mobs)if(&other!=&m && other.hp>0 && std::hypot(x-other.position.x,z-other.position.z)<12){blocked=true;break;}
                if(blocked)continue;
                m.position={x,y,z};m.yaw=float(angle);moved=true;break;
            }
            if(!moved){m.moving=false;m.decision=0;}
            if(m.aggro && decisionTick && tick%20==0 && Trace())
                std::fprintf(stderr,"night mobs: pursue kind %d before %.3f after %.3f\n",m.kind,std::sqrt(d2),std::sqrt(nightspawn::DistanceSquared(m.position.x-player.x,m.position.y-player.y,m.position.z-player.z)));
        }
        int motion=m.moving?1:0;
        if(m.model.motion_no!=motion)m.model.SetMotion(motion,0);
        Animate(m);
    }
    if(decisionTick && !std::getenv("DC_TEST_MOBS_COMBAT"))Spawn(player,night);
    if(Trace() && decisionTick && tick%100==0)std::fprintf(stderr,"night mobs: tick %d night %d count %d\n",tick,int(night),Count());
}

void TownMobsFaceTarget(CMainChara *player){
    if(locked<0 || !player || mobs[locked].hp<=0 || DranFlightActive())return;
    auto p=world::PositionOf(player->pos);
    player->rotation.y=float(std::atan2(mobs[locked].position.x-p.x,mobs[locked].position.z-p.z));
}

void TownMobsCamera(){
    if(locked>=0 && Chara && GameMode==ED_MODE_WALK && !EdMoveCharaInfo.interior && !DranFlightActive()){
        auto p=world::PositionOf(Chara->pos);auto t=mobs[locked].position;
        double dx=t.x-p.x,dz=t.z-p.z,length=std::max(1.,std::hypot(dx,dz));dx/=length;dz/=length;
        float eye[4];NowCamera->GetPos(eye);
        float ground=float(p.y);TownTerrainResidentHeight(p.x-dx*60,p.z-dz*60,ground,1);
        float wanted[3]={world::LocalX(p.x-dx*60),float(std::max(p.y+32,double(ground)+15)),world::LocalZ(p.z-dz*60)};
        for(int i=0;i<3;++i)eye[i]+=(wanted[i]-eye[i])*.18f;
        NowCamera->SetPos(eye);NowCamera->SetRef(world::LocalX(p.x+dx*18),float(p.y+12),world::LocalZ(p.z+dz*18));
        NowCamera->CCamera::Step(-1);
    }
    if(std::getenv("DC_TEST_MOBS_VIEW")){
        int kind=std::getenv("DC_TEST_MOBS_COMBAT")?std::clamp((subtick-120)/450,0,2):std::min(2,tick/160);
        for(auto &m:mobs)if((m.hp>0 || m.death>0) && m.kind==kind){
            float h=float(m.position.y);TownTerrainResidentHeight(m.position.x+35,m.position.z+40,h,1);
            NowCamera->SetPos(nullptr,world::LocalX(m.position.x+35),float(std::max(m.position.y+65,double(h)+30)),world::LocalZ(m.position.z+40));
            NowCamera->SetRef(world::LocalX(m.position.x),float(m.position.y+8),world::LocalZ(m.position.z));
            NowCamera->CCamera::Step(-1);
            break;
        }
    }
}

void TownMobsDraw(){
    gfx::capture::Scope scope("world.mobs.draw");
    if(!loaded || !TownTerrainEnabled(pEditGround) || EdMoveCharaInfo.interior || GameMode!=ED_MODE_WALK || DranFlightActive())return;
    TexManager.ReloadTexture(Vif1Packet,8);
    for(auto &m:mobs)if(m.hp>0 || m.death>0){
        // Frames are shared. Evaluate this instance's frozen pose immediately
        // before drawing; never advance gameplay or animation from render passes.
        auto state=m.model.motion_type.state;
        int flags=m.model.motion_flags;
        m.model.motion_flags=(flags&~4)|1;
        m.model.ambient_tint[0]=m.flash?90:(&m== (locked>=0?&mobs[locked]:nullptr)?25:0);
        m.model.ambient_tint[1]=m.flash?20:0;
        m.model.ambient_offset[3]=m.death>0?std::min(128.f,m.death*128.f/30):128.f;
        Animate(m);m.model.Draw();
        m.model.motion_type.state=state;m.model.motion_flags=flags;
    }
}

void TownMobsSwordHit(CMainChara *player){
    if(!loaded || !player)return;
    auto position=world::PositionOf(player->pos);
    int damage=10;
    if(SaveData){auto *s=SaveData->GetDngStatus();int slot=s->equipped_weapon_slot[0];if(slot>=0 && slot<11)damage=std::max(1,int(s->chara_weapons[0][slot].attack));}
    for(auto &m:mobs)if(m.hp>0){
        double dx=m.position.x-position.x,dz=m.position.z-position.z,range=std::hypot(dx,dz);
        if(range>32 || std::abs(m.position.y-position.y)>24)continue;
        if(range>1 && (dx*std::sin(player->rotation.y)+dz*std::cos(player->rotation.y))/range<.1)continue;
        if(!Visible(position,m.position))continue;
        Hurt(m,std::max(1,damage-int(MonstorTable[RetailId[m.kind]].defense)));
        if(Trace())std::fprintf(stderr,"night mobs: sword hit kind %d hp %d\n",m.kind,m.hp);
    }
}

void TownMobsHUD(){
    if(!loaded || GameMode!=ED_MODE_WALK || EdMoveCharaInfo.interior || DranFlightActive())return;
    static GameText label;label.KeepGameRandom(true);
    char text[128];
    if(locked>=0)std::snprintf(text,sizeof(text),"Locked: %s  HP %d/%d",Names[mobs[locked].kind],mobs[locked].hp,MonstorTable[RetailId[mobs[locked].kind]].max_hp);
    else std::snprintf(text,sizeof(text),"Lock on");
    GlyphDrawSlot(glyphs::Button::R3,CRect_i_(260,438,28,22),128);
    label.Set(text);label.FitWidth(320);label.Draw(294,438);
    if(SaveData){static GameText hp;hp.KeepGameRandom(true);auto *status=SaveData->GetDngStatus();std::snprintf(text,sizeof(text),"HP %d / %d%s",int(status->hp[0]),int(status->max_hp[0]),knockout?"  Knocked out":"");hp.Set(text);hp.Draw(20,20);}
}
