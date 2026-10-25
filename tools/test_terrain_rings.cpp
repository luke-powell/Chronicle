#include "terrain_publication.hpp"
#include <cassert>
#include <cstdio>
int main(){using namespace terrain;
 for(double origin:{0.,1e12,-1e12})for(int step=0;step<80;++step){
  double x=origin+step*379.37,z=-origin-step*641.73;
  auto tiles=Select(x,z);assert(tiles.size()==MaximumSelectedTiles);
  Selection s;std::array<int,MaxLevel+1> counts{};
  for(auto t:tiles){assert(s.emplace(t,0).second);++counts[t.level];assert(MeshResolution(t.level)==Resolution);}
  assert(counts[0]==64);for(int i=1;i<=MaxLevel;++i)assert(counts[i]==48);
  // All positions in the guaranteed 1024-unit near square stay full res.
  for(int dz=-1024;dz<=1024;dz+=128)for(int dx=-1024;dx<=1024;dx+=128)assert(LevelAt(s,x+dx,z+dz)==0);
  for(auto t:tiles){assert(LevelAt(s,t.left()+t.size()/2,t.top()+t.size()/2)==t.level);
   for(auto delta:{std::pair{-.001,.25},std::pair{1.001,.75},std::pair{.25,-.001},std::pair{.75,1.001}}){
    int neighbor=LevelAt(s,t.left()+delta.first*t.size(),t.top()+delta.second*t.size());
    assert(neighbor<0||std::abs(neighbor-t.level)<=1);
   }
  }
  for(int direction=0;direction<32;++direction){double angle=direction*6.28318530718/32;
   assert(LevelAt(s,x+Radius*std::cos(angle),z+Radius*std::sin(angle))>=0);
  }
 }
 using Key=std::pair<Tile,std::array<int,4>>;
 auto edges=[](Tile t,const Selection &s){double x=t.left(),z=t.top(),w=t.size();return std::array<int,4>{LevelAt(s,x-1,z+w/2),LevelAt(s,x+w+1,z+w/2),LevelAt(s,x+w/2,z-1),LevelAt(s,x+w/2,z+w+1)};};
 auto keys=[&](double x,double z){auto tiles=Select(x,z);Selection s;for(auto t:tiles)s.emplace(t,0);std::vector<Key> result;for(auto t:tiles)result.emplace_back(t,edges(t,s));return result;};
 auto active=keys(0,0);int published=0;
 for(int step=1;step<40;++step){auto target=keys(step*1024.,step*1024.);
  for(int update=0;update<16;++update){auto next=AdaptLocal(active,target,step*1024.,step*1024.,edges);if(next.empty())break;
   assert(next.size()==MaximumSelectedTiles);Selection s;for(auto k:next)assert(s.emplace(k.first,0).second);
   for(auto k:next)assert(k.second==edges(k.first,s));active=std::move(next);++published;
  }
 }
 // At a stationary destination local publications must converge, not
 // coarsen tiles that overlap the desired fine ring and then rebuild them.
 auto destination=keys(40000,40000);std::set<std::vector<Key>> seen;
 for(int iteration=0;iteration<512;++iteration){
  auto next=AdaptLocal(active,destination,40000,40000,edges);if(next.empty())break;
  assert(seen.insert(next).second);active=std::move(next);
 }
 Selection settled;for(auto k:active)settled.emplace(k.first,0);
 for(auto k:destination)if(k.first.level==0){auto t=k.first;assert(LevelAt(settled,t.left()+t.size()/2,t.top()+t.size()/2)==0);}
 active=keys(12000,-6000);destination=keys(35840,-48842);seen.clear();
 for(int iteration=0;iteration<512;++iteration){auto next=AdaptLocal(active,destination,35840,-48842,edges);if(next.empty())break;
  assert(seen.insert(next).second);active=std::move(next);
 }
 settled.clear();for(auto k:active)settled.emplace(k.first,0);
 for(auto k:destination)if(k.first.level==0){auto t=k.first;assert(LevelAt(settled,t.left()+t.size()/2,t.top()+t.size()/2)==0);}
 assert(published>0);std::printf("PASS %d asynchronous local publications preserve the exact tile budget\n",published);
 std::printf("PASS 240 ring positions: exactly %d tiles, %d terrain triangles, full-resolution centre, no overlaps, balanced seams, full horizon\n",MaximumSelectedTiles,MaximumSelectedTiles*Resolution*Resolution*2);
}
