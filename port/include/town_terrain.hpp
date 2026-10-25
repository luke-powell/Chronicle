#pragma once
class CEditGround;
class CCPoly;
class CBoxVu0;
bool TownTerrainEnabled(const CEditGround *ground);
void TownTerrainDraw(CEditGround *ground);
void TownTerrainShutdown();
int  TownTerrainPolys(CEditGround *ground, CCPoly *out, const CBoxVu0 &box);

// Keep editor-to-walk transitions on fully detailed, matching terrain.
bool TownTerrainRequestWalk(float x,float z);
void TownTerrainCancelWalk();

// Camera/cursor height while surveying from far above; never placement/collision.
float TownTerrainCursorHeight(float x,float z);
