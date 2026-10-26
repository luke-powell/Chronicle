#pragma once
#include <string>
#include <vector>

#include "savedata.hpp"
class CMapParts;
class CEditGround;
const std::vector<SV_GRD_PART> &ForestGeoramaParts();
void                            ForestGeoramaEnter();
void                            ForestGeoramaTick();
void                            ForestGeoramaSave();
void                            ForestGeoramaReset();
bool                            ForestGeoramaActive();
unsigned                        ForestGeoramaRevision();
bool                            ForestGeoramaConnected();
void                            ForestGeoramaPreview(int plot, float x, float y, float z, int rotation);

bool ForestGeoramaVisible(int plot, const std::string &objectPath);
