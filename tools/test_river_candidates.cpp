#include "terrain_kernel.hpp"
#include <cassert>
#include <cstdio>
#include <future>
int main(){using namespace terrain;
 for(auto cell:{DrainKey{7,-1},DrainKey{-48,42},DrainKey{-2,-55},DrainKey{488281250,-488281250}}){
   auto candidates=RiverCandidates(cell.first,cell.second);std::vector<DrainKey> exhaustive;
   double x=cell.first*2048.,z=cell.second*2048.;
   for(int dz=-6;dz<=6;++dz)for(int dx=-6;dx<=6;++dx){auto &r=DrainReach(cell.first+dx,cell.second+dz);double m=r.lake?1300:std::max(r.width,r.endWidth)+1000;
     if(r.width>0&&x+2048>=r.minX-m&&x<=r.maxX+m&&z+2048>=r.minZ-m&&z<=r.maxZ+m)exhaustive.emplace_back(cell.first+dx,cell.second+dz);
   }
   assert(exhaustive==candidates);
   std::vector<std::future<std::vector<DrainKey>>> workers;
   for(int i=0;i<8;++i)workers.push_back(std::async(std::launch::async,[&]{return RiverCandidates(cell.first,cell.second);}));
   for(auto &worker:workers)assert(worker.get()==exhaustive);
   std::printf("PASS exact river candidates versus exhaustive 169 reaches at %lld %lld, eight workers\n",(long long)cell.first,(long long)cell.second);
 }
}
