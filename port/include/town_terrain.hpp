#pragma once
class CEditGround;
class CCPoly;
class CBoxVu0;
bool TownTerrainEnabled(const CEditGround *ground);
void TownTerrainDraw(CEditGround *ground);
int  TownTerrainPolys(CEditGround *ground, CCPoly *out, const CBoxVu0 &box);

// Keep editor-to-walk transitions on fully detailed, matching terrain.
bool TownTerrainRequestWalk(float x,float z);
void TownTerrainCancelWalk();
