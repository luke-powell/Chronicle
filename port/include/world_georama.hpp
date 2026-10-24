#pragma once
class CEditGround;
class CSaveData;
bool WorldGeoramaOutside(CEditGround* ground,float x,float z);
float WorldGeoramaHeight(float x,float z);
int WorldGeoramaFind(CEditGround* ground,float x,float z);
int WorldGeoramaPlace(CEditGround* ground,int plot,float x,float z,int rotation);
int WorldGeoramaDelete(CEditGround* ground,int* plot,int* rotation,float x,float z);
void WorldGeoramaPreview(CEditGround* ground,int plot,float* position,float* model,int rotation,float* angles,int pass);
void WorldGeoramaUnlock(CSaveData* save,CEditGround* ground);
bool WorldGeoramaFoundations(CEditGround* ground);
void WorldGeoramaTest(CEditGround* ground);
