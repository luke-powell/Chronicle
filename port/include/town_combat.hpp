#pragma once

class CCharacter;
class CMainChara;

void TownCombatReset();
bool TownCombatBusy();
bool TownCombatBeforeMove(CMainChara *chara);
void TownCombatStep(CMainChara *chara);
void TownCombatDraw(CCharacter *chara);
