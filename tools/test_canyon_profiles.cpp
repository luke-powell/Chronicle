#include "terrain_kernel.hpp"
#include <cstdio>
#include <fstream>
// Detect erosion-created W profiles: the uncut center is a local low,
// yet both flanks become at least 25 m lower after incision.
int main() {
 using namespace terrain;
 int count=0; double worst=0;
 auto position=[](int axis,double along,double across){
  double a=axis*3.141592653589793/4;
  return std::array<double,2>{along*std::cos(a)-across*std::sin(a),along*std::sin(a)+across*std::cos(a)};
 };std::ofstream out("port/build/canyon-profile.csv");
 // Fixed reproducer: formerly a 401 m double trench. Preserve CSV evidence
 // even when the broad regression scan finds no remaining failures.
 out<<"x,z,base,eroded\n";
 for(int i=-20;i<=20;++i){double x=24064+i*512.,z=90112.;out<<x<<','<<z<<','<<BaseMountainAt(x,z).height<<','<<MountainAt(x,z).height<<'\n';}
 out.close();
 for(int axis=0;axis<4;++axis)for(int line=-60;line<=60;++line){
  constexpr int N=2001;std::array<double,N>b,h;
  for(int i=0;i<N;++i){double a=(i-1000)*512.,c=line*8192.;auto p=position(axis,a,c);double x=p[0],z=p[1];b[i]=BaseMountainAt(x,z).height;h[i]=MountainAt(x,z).height;}
  for(int i=20;i<N-20;++i){
   if(b[i]<3000 || b[i]>b[i-1] || b[i]>b[i+1])continue;
   int l=i,r=i;for(int j=i-16;j<i;++j)if(h[j]<h[l])l=j;for(int j=i+1;j<=i+16;++j)if(h[j]<h[r])r=j;
   double depth=std::min(h[i]-h[l],h[i]-h[r]);
   if(b[l]<b[i]-50||b[r]<b[i]-50||depth<250)continue;
   ++count;if(depth>worst){worst=depth;out.close();out.open("port/build/canyon-profile.csv");out<<"x,z,base,eroded\n";for(int j=i-20;j<=i+20;++j){double a=(j-1000)*512.,c=line*8192.;auto p=position(axis,a,c);out<<p[0]<<','<<p[1]<<','<<b[j]<<','<<h[j]<<'\n';}std::printf("worst axis %d center %.0f %.0f depth %.2f\n",axis,(i-1000)*512.,line*8192.,depth);}
  }
 }
 std::printf("W artifacts %d worst %.2f\n",count,worst);return count?1:0;
}
