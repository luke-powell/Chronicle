#include "world_georama.hpp"
#include "terrain_kernel.hpp"
#include "town_terrain.hpp"
#include "editground.hpp"
#include "editarea.hpp"
#include "editpartsinfo.hpp"
#include "editpartsdata.hpp"
#include "editatra.hpp"
#include "editloop.hpp"
#include "savedata.hpp"
#include "framevu1.hpp"
#include "mglib.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>

bool WorldGeoramaOutside(CEditGround* ground,float x,float z) {
    return TownTerrainEnabled(ground) && ground->areas[0] && !ground->areas[0]->CheckArea(x,0,z);
}
float WorldGeoramaHeight(float x,float z) {return float(terrain::Height(x,z));}
namespace {
void Footprint(CMapParts& part,int rotation,double& hx,double& hz) {
    hx=std::max(1,part.GetWidth())*50.;hz=std::max(1,part.GetHeight())*50.;
    // GetWidth/GetHeight already account for the object's current rotation.
    if((rotation-part.rot_y)&1)std::swap(hx,hz);
}
void Snap(CMapParts& part,int rotation,float& x,float& z) {
    double hx,hz;Footprint(part,rotation,hx,hz);
    x=float(std::floor(x/100)*100+std::fmod(hx,100));
    z=float(std::floor(z/100)*100+std::fmod(hz,100));
}
// Use retail's connector selection on a temporary local grid. World objects remain
// in the native save array; this grid never limits their global coordinates.
void Connections(CEditGround* ground,float x,float z) {
    CEditArea local;
    local.SetMapInfo(0,1);local.SetSize(16,16,100,10);
    local.SetOffset(float(std::floor(x/100)*100-800),0,float(std::floor(z/100)*100-800));
    for(int i=0;i<128;i++) {
        auto& p=ground->parts[i];
        if(p.handle>=0 && local.CheckArea(p.pos[0],0,p.pos[2]))
            local.SetMapParts(i,ground->parts,p.pos[0]+1,p.pos[1],p.pos[2]+1,p.rot_y);
    }
    auto* previous=ground->areas[1];ground->areas[1]=&local;
    for(auto& p:ground->parts) {
        if(p.handle<0 || p.area!=-1 || std::abs(p.pos[0]-x)>210 || std::abs(p.pos[2]-z)>210)continue;
        if(p.subtype==MAP_PARTS_SUBTYPE_ROAD)ground->SetRoadParts(p.pos[0]+1,p.pos[1],p.pos[2]+1,0,0);
        if(p.subtype==MAP_PARTS_SUBTYPE_RIVER)ground->SetRiverParts(p.pos[0]+1,p.pos[1],p.pos[2]+1,0,0);
    }
    ground->areas[1]=previous;
}
bool Fits(CEditGround* ground,int plot,float x,float z,int rotation) {
    if(plot<0 || plot>=24 || !ground->plot_parts || !ground->parts_info)return false;
    auto& src=ground->plot_parts[plot];auto* info=ground->parts_info->GetPartsInfo(plot);
    if(src.handle<0 || !info || !info->obtained || info->placed>=info->stock)return false;
    double hx,hz;Footprint(src,rotation,hx,hz);
    // Don't overlap the retained Mayor platform, fixed buildings, or existing placements.
    if(std::abs(x)<hx+350 && z+hz>490 && z-hz<900)return false;
    for(auto& p:ground->parts) {
        if(p.handle<0)continue;
        double px,pz;Footprint(p,p.rot_y,px,pz);
        if(std::abs(x-p.pos[0])<hx+px-.1 && std::abs(z-p.pos[2])<hz+pz-.1)return false;
    }
    return true;
}
}
int WorldGeoramaFind(CEditGround* ground,float x,float z) {
    for(int i=0;i<128;i++) {
        auto& p=ground->parts[i];if(p.handle<0)continue;
        double hx,hz;Footprint(p,p.rot_y,hx,hz);
        if(std::abs(x-p.pos[0])<=hx && std::abs(z-p.pos[2])<=hz)return i;
    }
    return -1;
}
int WorldGeoramaPlace(CEditGround* ground,int plot,float x,float z,int rotation) {
    if(plot<0 || plot>=24 || !ground->plot_parts || ground->CheckEffect())return -1;
    auto& src=ground->plot_parts[plot];Snap(src,rotation,x,z);
    if(!Fits(ground,plot,x,z,rotation))return -1;
    for(int i=0;i<128;i++) {
        auto& p=ground->parts[i];if(p.handle>=0)continue;
        // Matches retail's template-copy placement, retaining the destination vtable.
        p=src;p.SetRotY(rotation);p.area=-1;p.unit_size=100;
        p.SetPosition(x,float(terrain::Height(x,z)),z);
        ground->parts_info->parts[plot].placed++;
        Connections(ground,x,z);
        ground->MakePartsBox();
        std::fprintf(stderr,"georama: placed plot %d slot %d at %.1f %.1f %.1f\n",plot,i,x,p.pos[1],z);
        return i;
    }
    return -1;
}
int WorldGeoramaDelete(CEditGround* ground,int* plot,int* rotation,float x,float z) {
    int i=WorldGeoramaFind(ground,x,z);if(i<0)return -1;
    auto& p=ground->parts[i];*plot=p.parts_no;*rotation=p.rot_y;
    if(auto* info=ground->parts_info->GetPartsInfo(*plot))info->placed=std::max(0,info->placed-1);
    p.Initialize();p.handle=-1;Connections(ground,x,z);ground->MakePartsBox();return i;
}
void WorldGeoramaPreview(CEditGround* ground,int plot,float* position,float* model,int rotation,float* angles,int pass) {
    if(plot<0 || plot>=24 || !ground->plot_parts)return;
    auto& src=ground->plot_parts[plot];if(src.handle<0)return;
    float x=position[0],z=position[2];Snap(src,rotation,x,z);
    float y=WorldGeoramaHeight(x,z);
    model[0]=x;model[1]=y+5;model[2]=z;model[3]=1;
    bool fits=Fits(ground,plot,x,z,rotation);
    sceVu0FVECTOR ambient;MGGetAmbient(ambient);
    if(!fits){sceVu0FVECTOR red={110,25,25,128};MGSetAmbient(red);}
    if(pass==ground->cursor.area) {
        ground->cursor.unit_size=100;
        ground->cursor.Draw(model,src.GetWidth(),src.GetHeight());
    }
    if(src.category_no==pass && src.frame[0]) {
        src.frame[0]->SetPosition(model);src.frame[0]->SetRotation(angles[0],angles[1],angles[2]);
        MGDraw(src.frame[0]);
    }
    MGSetAmbient(ambient);
}
void WorldGeoramaUnlock(CSaveData* save,CEditGround* ground) {
    if(!save)return;
    int unlocked=0,attachments=0;
    for(int town=0;town<6;town++) {
        auto* chips=save->GetElemData(town);int available[100]{};
        for(int i=0;i<128;i++)if(chips[i]>=0 && chips[i]<100)available[chips[i]]++;
        for(int plot=0;plot<24;plot++) {
            auto* def=GetEditAtraPartsData(town,plot);auto* saved=save->GetEditPartsInfo(town,plot);
            if(!def || !saved || def->max<=0)continue;
            saved->obtained=1;saved->stock=std::max<int>(saved->stock,def->max);++unlocked;
            for(int k=0;k<6;k++) {
                int id=def->elements[k].id;
                if(id<0 || id>=100 || saved->element_enabled[k])continue;
                if(available[id]>0){--available[id];continue;}
                if(save->AtraChipGet(town,id))++attachments;
            }
        }
    }
    if(ground && ground->parts_info) {
        for(int plot=0;plot<24;plot++) {
            auto* saved=save->GetEditPartsInfo(ground->map_no,plot);
            auto* info=ground->parts_info->GetPartsInfo(plot);
            if(saved && info){info->obtained=saved->obtained;info->stock=std::max(info->stock,int(saved->stock));}
        }
    }
    std::fprintf(stderr,"georama: unlocked %d plots across six catalogs, added %d attachment pieces\n",unlocked,attachments);
}
bool WorldGeoramaFoundations(CEditGround* ground) {
    std::vector<terrain::Foundation> next;
    for(int i=0;i<128;i++) {
        auto& p=ground->parts[i];if(p.handle<0 || p.area!=-1)continue;
        double hx,hz;Footprint(p,p.rot_y,hx,hz);
        double y=ground->effect_parts_id==i && ground->effect_count>0?ground->effect_target_alt:p.pos[1];
        next.push_back({p.pos[0],p.pos[2],hx,hz,y});
    }
    bool changed=next.size()!=terrain::foundations.size();
    if(!changed)for(size_t i=0;i<next.size();i++) {
        auto& a=next[i];auto& b=terrain::foundations[i];
        changed|=a.x!=b.x || a.z!=b.z || a.halfX!=b.halfX || a.halfZ!=b.halfZ || a.height!=b.height;
    }
    if(changed)terrain::foundations=std::move(next);
    return changed;
}

void WorldGeoramaTest(CEditGround* ground) {
    if(!std::getenv("DC_GEORAMA_TEST"))return;
    static int ticks=0;
    if(++ticks!=180)return;
    auto check=[](bool ok,const char* what){if(!ok){std::fprintf(stderr,"georama TEST FAILED: %s\n",what);std::abort();}};
    int slot=ground->SetMapParts(0,3001,0,3001,1);
    check(slot>=0,"remote placement");
    auto& p=ground->parts[slot];float x=p.pos[0],z=p.pos[2];
    check(p.area==-1 && p.rot_y==1,"remote rotation/area");
    check(ground->GetPartsID(x,0,z)==slot,"remote selection");
    check(ground->SetFocusParts(x,0,z)==slot,"remote focus");
    check(ground->SetMapParts(0,x,0,z,0)<0,"overlap rejected");
    auto saved=std::make_unique<CSaveData>(*SaveData);
    ground->Save(0,saved.get());ground->parts_info->Save(0,saved.get());
    int plot=-1,rot=-99;
    check(ground->DeleteMapParts(&plot,&rot,x,0,z)==slot && plot==0 && rot==1,"remote delete");
    check(ground->GetPartsID(x,0,z)<0,"deleted selection cleared");
    ground->Load(0,saved.get());
    slot=ground->GetPartsID(x,0,z);
    check(slot>=0 && ground->parts[slot].rot_y==1,"save/load remote placement");
    check(std::abs(ground->parts[slot].pos[0]-x)<.1 && std::abs(ground->parts[slot].pos[2]-z)<.1,"save/load no position drift");
    WorldGeoramaFoundations(ground);
    check(std::abs(terrain::Height(x,z)-ground->parts[slot].pos[1])<.1,"foundation");
    std::fprintf(stderr,"georama TEST PASS: remote place, rotate, select, overlap, delete, native save/load, foundation\n");
}
