#include "terrain_kernel.hpp"
#include <cassert>
#include <fstream>
#include <cstdio>
int main(){
 std::ifstream in("port/build/norune-shell.bin",std::ios::binary);auto &shell=canyon::TownShell();
 assert(in.read(reinterpret_cast<char *>(shell.heights.data()),sizeof(shell.heights)));canyon::RestoreShellRidges();
 for(int z=-700;z<=500;z+=16)for(int x=-700;x<=700;x+=16)assert(std::abs(terrain::LandHeight(x,z))<.01);
 int exits=0;bool first=false,previous=false;
 for(int i=0;i<2048;i++){
  double a=i*6.283185307179586/2048;bool open=terrain::LandHeight(1250*std::cos(a),1250*std::sin(a))<10;
  if(i==0)first=open;else exits+=open&&!previous;previous=open;
 }exits+=first&&!previous;assert(exits==1);
 int bare=0,plateau=0;
 for(int z=-12000;z<12000;z+=32)for(int x=-18000;x<18000;x+=32){
  auto c=canyon::At(x,z);if(c.weight<1)continue;
  if(c.height<175){assert(canyon::FloraMask(x,z)==0);++bare;}
  if(c.height>=195&&std::hypot(x,z)>2000){assert(canyon::FloraMask(x,z)>.99);++plateau;}
 }
 assert(bare>1000&&plateau>1000);std::printf("PASS flora: %d bare floor/wall samples, %d plateau samples eligible\n",bare,plateau);
 double worst=0;
 for(int z=-11000;z<11000;z+=97)for(int x=-11000;x<11000;x+=101){
  auto a=canyon::At(x,z);assert(a.height>=0&&a.height<270&&a.width==0);
  double jump=std::abs(terrain::LandHeight(x+.01,z)-terrain::LandHeight(x-.01,z));worst=std::max(worst,jump);assert(jump<1);
 }
 std::printf("PASS exact native town footprint, one exit, flat slot floors, wall bounds, no artificial water, continuity %.5f\n",worst);
}
