#pragma once
#include <filesystem>
class CEditGround;

// Norune's persistent coordinates are doubles. Retail engine positions are
// projections into a shared nearby scene origin, including collision and camera
// state. Terrain generation and saves cross the boundary through these helpers.
// The terrain's bounded elevations remain relative to sea level; horizontal
// origin shifts keep walking/camera coordinates within a few thousand units.
namespace world {
struct Position {
    double x = 0, y = 0, z = 0;
};

extern double originX, originZ;

inline double X(double local) { return originX + local; }

inline double Z(double local) { return originZ + local; }

inline float LocalX(double global) { return float(global - originX); }

inline float LocalZ(double global) { return float(global - originZ); }

Position PositionOf(float *local);
void     Assign(float *local, Position global);
double   Height(double x, double z);
double   SurfaceHeight(double x, double z);
double   NaturalHeight(double x, double z);
void     Reset();
void     Leave();
void     Forget(float *local);
void     BeginTick();
void     TestTick();
void     Rebase(double x, double z);
void     Loaded(CEditGround *ground);
void     Save(const std::filesystem::path &directory);
void     Load(const std::filesystem::path &directory);
} // namespace world
