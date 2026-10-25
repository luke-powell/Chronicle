#pragma once
class CMainChara;
class CCharacter;

bool DranFlightActive();
bool DranFlightStep(CMainChara *rider);
bool DranFlightDraw(CCharacter *rider);
void DranFlightReset();

void DranFlightHUD();

namespace world {
struct Position;
}

// Mid-flight saves resume at the last takeoff position, never over water or inside a tree.
void DranFlightSavePosition(world::Position &position, float &heading);
