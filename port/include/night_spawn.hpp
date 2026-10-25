#pragma once
#include <cmath>
#include <cstdint>

namespace nightspawn {
// Ten retail world units are approximately one metre (Toan is ~18 units tall).
constexpr double Block=10, MinDistance=48*Block, MaxDistance=128*Block;
constexpr double IdleDistance=32*Block;
constexpr int Cap=14, TickRate=20, IdleTicks=30*TickRate;
inline int SunDamage(int maxHp,int &remainder){remainder+=maxHp;int damage=remainder/8;remainder%=8;return damage;}
inline double DistanceSquared(double x,double y,double z){return x*x+y*y+z*z;}
inline bool InSpawnShell(double d2){return d2>MinDistance*MinDistance && d2<=MaxDistance*MaxDistance;}
inline bool CanSpawn(bool night,bool lit,bool dry,bool clear,int population,double d2){
    return night && !lit && dry && clear && population<Cap && InSpawnShell(d2);
}
inline bool Despawn(double d2,int idle,std::uint32_t roll){
    return d2>MaxDistance*MaxDistance || (d2>IdleDistance*IdleDistance && idle>IdleTicks && roll%800==0);
}
}
