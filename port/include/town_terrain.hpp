#pragma once
class CEditGround;
class CCPoly;
class CBoxVu0;
bool TownTerrainEnabled(const CEditGround *ground);
void TownTerrainDraw(CEditGround *ground);
int  TownTerrainPolys(CEditGround *ground, CCPoly *out, const CBoxVu0 &box);
int  TownTerrainKeepPlatform(CCPoly *polys, int count);
