#include "terrain_kernel.hpp"
#include <cassert>
#include <fstream>
#include <queue>
#include <cstdio>
int main(){
 auto &shell=canyon::TownShell();std::ifstream in("port/build/norune-shell.bin",std::ios::binary);
 assert(in.read(reinterpret_cast<char *>(shell.heights.data()),sizeof(shell.heights)));shell.ready=true;
 constexpr int nx=751,nz=876;constexpr double step=32,x0=-22016,z0=-10016;
 std::vector<double> h(nx*nz);std::vector<bool> seen(nx*nz);
 for(int z=0;z<nz;z++)for(int x=0;x<nx;x++)h[z*nx+x]=terrain::LandHeight(x0+x*step,z0+z*step);
 auto idx=[](double x,double z){return int(std::round((z-z0)/step))*nx+int(std::round((x-x0)/step));};
 std::queue<int> q;q.push(idx(0,0));seen[q.front()]=true;
 while(!q.empty()){int i=q.front();q.pop();int x=i%nx,z=i/nx;
  for(auto [dx,dz]:{std::pair{-1,0},std::pair{1,0},std::pair{0,-1},std::pair{0,1}}){int xx=x+dx,zz=z+dz;if(xx<0||zz<0||xx>=nx||zz>=nz)continue;int j=zz*nx+xx;
   if(!seen[j]&&std::abs(h[j]-h[i])<=step*canyon::WalkSlope){seen[j]=true;q.push(j);}
  }
 }
 for(auto p:{canyon::Point{-800,-350},{-950,-150},{-1200,-150},{-1800,-150},{-2650,400},{-3100,1500},{-4300,3100},{-7700,2800},{-11000,4500},{-19000,5500}}){
  bool connected=false;for(int dz=-3;dz<=3;dz++)for(int dx=-3;dx<=3;dx++){int i=idx(p.x+dx*step,p.z+dz*step);if(i>=0&&i<int(h.size())&&seen[i]&&h[i]<(canyon::At(p.x,p.z).weight>.99?5:100))connected=true;}
  if(!connected){std::printf("FAIL unreachable %.0f %.0f h %.2f\n",p.x,p.z,h[idx(p.x,p.z)]);return 1;}
 }
 for(int z=-700;z<=500;z+=16)for(int x=-700;x<=700;x+=16)assert(seen[idx(x,z)]);
 std::puts("PASS slope navigation: entire retail footprint, native exit, curved approach, branching network, open world");
}
