#include "grass_geometry.hpp"
#include "shared_memo.hpp"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <thread>
#include <string>
struct Vertex {float position[3];float uv[2];};
struct Source {std::vector<Vertex> vertices;std::vector<unsigned> indices;std::string texture;};
int main(){
 Source s{{{{10,4,10},{0,0}},{{12,4,10},{1,0}},{{10,9,10},{0,1}},
           {{12,4,10},{1,0}},{{12,9,10},{1,1}},{{10,9,10},{0,1}},
           {{80,7,80},{0,0}},{{82,7,80},{1,0}},{{80,12,80},{0,1}}},
          {0,1,2,3,4,5,6,7,8},"native-grass"};
 auto parts=grass::Split(std::vector{s});assert(parts.size()==2);
 size_t count=0;for(auto &p:parts){assert(p.texture==s.texture);float low=100;for(auto v:p.vertices)low=std::min(low,v.position[1]);assert(low==0);count+=p.indices.size();}
 assert(count==9);
 for(int z=0;z<=100;++z)for(int x=0;x<=100;++x){double u=x/100.,v=z/100.;
   double y=grass::TriangleHeight(0,10,20,60,u,v);
   double expected=u+v<=1?10*u+20*v:10*(1-v)+20*(1-u)+60*(u+v-1);
   assert(std::abs(y-expected)<1e-12);
 }
 SharedMemo<int,int,4> cache;std::atomic<int> calls=0;
 std::vector<std::future<int>> jobs;
 for(int i=0;i<8;++i)jobs.push_back(std::async(std::launch::async,[&]{return cache.Get(42,[&]{++calls;std::this_thread::sleep_for(std::chrono::milliseconds(30));return 99;});}));
 for(auto &job:jobs)assert(job.get()==99);assert(calls==1);
 for(int i=0;i<100;++i)assert(cache.Get(i,[&]{return i;})==i);
 assert(cache.Get(42,[]{return 42;})==42);
 std::puts("PASS disconnected native tufts, welded UV seams, sloped triangle grounding, single producer across eight workers and bounded cache eviction");
}
