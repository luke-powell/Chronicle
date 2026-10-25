#include "terrain_publication.hpp"
#include <cassert>
#include <cstdio>
using Key=std::pair<terrain::Tile,std::array<int,4>>;
std::array<int,4> edges(terrain::Tile t,const terrain::Selection &s){double x=t.left(),z=t.top(),w=t.size();return {terrain::LevelAt(s,x-1,z+w/2),terrain::LevelAt(s,x+w+1,z+w/2),terrain::LevelAt(s,x+w/2,z-1),terrain::LevelAt(s,x+w/2,z+w+1)};}
std::vector<Key> selection(double x,double z,int lod){auto ts=terrain::Select(x,z,lod);terrain::Selection s;for(auto t:ts)s.emplace(t,0);std::vector<Key> out;for(auto t:ts)out.emplace_back(t,edges(t,s));return out;}
int main(){for(auto offset:{0.,1e12,-1e12})for(int lod:{0,2,6}){
 auto active=selection(offset,offset,0),desired=selection(offset+4096,offset+2048,lod);
 auto groups=terrain::Publications(active,desired);
 // Any completion order must leave a non-overlapping selection whose edge
 // stitching matches its actual neighbours, including negative/far origins.
 std::reverse(groups.begin(),groups.end());
 for(auto &g:groups){std::set<Key> remove(g.before.begin(),g.before.end());std::erase_if(active,[&](auto &k){return remove.contains(k);});active.insert(active.end(),g.after.begin(),g.after.end());
   terrain::Selection s;for(auto &k:active)assert(s.emplace(k.first,0).second);
   for(auto &k:active){auto t=k.first;assert(edges(t,s)==k.second);assert(terrain::LevelAt(s,t.left()+t.size()*.5,t.top()+t.size()*.5)==t.level);}
   for(int z=-8;z<=8;++z)for(int x=-8;x<=8;++x)assert(terrain::LevelAt(s,offset+x*512,offset+z*512)>=0);
 }
 assert(std::set<Key>(active.begin(),active.end())==std::set<Key>(desired.begin(),desired.end()));
 std::printf("PASS independent publication %.0f lod %d: %zu regions\n",offset,lod,groups.size());
}
 auto active=selection(-98000,86000,6),target=selection(-98000,86000,0);
 int refinements=0;
 for(;refinements<128;++refinements){auto next=terrain::AdaptLocal(active,target,-98000,86000,edges);if(next.empty())break;
   assert(next.size()==active.size()+3);terrain::Selection s;for(auto &k:next)s.emplace(k.first,0);
   for(auto &k:next)assert(edges(k.first,s)==k.second);
   active=std::move(next);
   bool ready=true;for(int dz:{-256,0,256})for(int dx:{-256,0,256})ready&=terrain::LevelAt(s,-98000+dx,86000+dz)==0;
   if(ready)break;
 }
 assert(refinements<32);std::printf("PASS landing detail ready after %d local refinements without waiting for the horizon\n",refinements+1);
 // Travel must retire obsolete fine tiles as well as add new detail. Check
 // every intermediate mesh's edge metadata, not just the final selection.
 active=selection(0,0,0);
 const auto initialCount=active.size();
 unsigned merged=0;
 for(int step=1;step<=32;++step){
   double x=step*2048.,z=step*1024.;target=selection(x,z,2);
   for(int update=0;update<12;++update){
     auto next=terrain::AdaptLocal(active,target,x,z,edges);if(next.empty())break;
     if(next.size()<active.size())++merged;
     terrain::Selection s;for(auto &k:next)assert(s.emplace(k.first,0).second);
     for(auto &k:next){assert(edges(k.first,s)==k.second);auto t=k.first;
       assert(terrain::LevelAt(s,t.left()+t.size()/2,t.top()+t.size()/2)==t.level);}
     assert(next.size()<=std::max(initialCount,target.size()+132));
     active=std::move(next);
   }
 }
 assert(merged>0);std::printf("PASS sustained travel: %u coarsenings, bounded tiles and consistent seams\n",merged);
}
