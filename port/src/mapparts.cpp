#include "mapparts.hpp"
#include "world_georama.hpp"
#include "town_terrain.hpp"
#include "editloop.hpp"

#include <libvu0.h>
#include <cmath>

// Road tiles must clear the uneven ground at the same height. A small lift lets the ground win the
// depth test in places as the camera moves, exposing a different, darker surface beneath the road.
// The constant road lift is within retail's distance-dependent range and keeps adjacent tiles level.
constexpr float kPartsLiftDepth = 0.1f;
constexpr float kRoadLiftDepth = 0.5f;

PORT_OVERRIDE void CMapParts::DrawLOD(float *distance, int lowest, int highest, int *out_level) {
    sceVu0FVECTOR saved_pos;
    sceVu0FVECTOR lifted_pos;

    if (this->handle < 0) {
        return;
    }

    if (this->draw_on == 0) {
        return;
    }

    sceVu0CopyVector(saved_pos, this->pos);
    sceVu0CopyVector(lifted_pos, saved_pos);

    if (this->subtype == MAP_PARTS_SUBTYPE_ROAD && this->lift > 0.0f) {
        lifted_pos[1] += kRoadLiftDepth;
    } else if (this->lift > 0.0f) {
        if (this->lift > 1.0f) {
            lifted_pos[1] += 0.1f;
        } else {
            lifted_pos[1] += kPartsLiftDepth * this->lift;
        }
    }

    sceVu0CopyVector(this->pos, lifted_pos);
    if(WorldGeoramaCustom(pEditGround,parts_no) && parts_no==23) {
        WorldGeoramaDrawPlatform(this);
    }
    if(TownTerrainEnabled(pEditGround) && area==-1 && subtype==MAP_PARTS_SUBTYPE_ROAD)WorldGeoramaDrawingRoad=this;
    CObjectFrame::DrawLOD(distance, lowest, highest, out_level);
    WorldGeoramaDrawingRoad=nullptr;
    sceVu0CopyVector(this->pos, saved_pos);
}
