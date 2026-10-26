#pragma once
class CCPoly;
class CBoxVu0;
void ForestTownPrepare();
void ForestTownDraw();
void ForestTownReset();
void ForestTownShutdown();
int  ForestTownPolys(CCPoly *out, const CBoxVu0 &box, int capacity);
