#include "night_spawn.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main(){
    using namespace nightspawn;
    assert(!InSpawnShell(480*480));assert(InSpawnShell(481*481));
    assert(InSpawnShell(1280*1280));assert(!InSpawnShell(1281*1281));
    assert(CanSpawn(true,false,true,true,13,500*500));
    assert(!CanSpawn(false,false,true,true,0,500*500));
    assert(!CanSpawn(true,true,true,true,0,500*500));
    assert(!CanSpawn(true,false,false,true,0,500*500));
    assert(!CanSpawn(true,false,true,false,0,500*500));
    assert(!CanSpawn(true,false,true,true,14,500*500));
    assert(!Despawn(320*320,10000,0));
    assert(!Despawn(321*321,600,0));
    assert(Despawn(321*321,601,0));
    assert(!Despawn(321*321,601,1));
    assert(Despawn(1281*1281,0,1));
    assert(!InSpawnShell(DistanceSquared(100,1500,100)));
    for(int hp:{12,23,75}){int remainder=0,damage=0;for(int second=1;second<=8;++second){damage+=SunDamage(hp,remainder);assert(damage==hp*second/8);}assert(damage==hp);}
    std::puts("PASS night/light/surface/cap, spherical distance limits, idle and far despawning");
}
