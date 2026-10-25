#include "terrain_kernel.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
int main() {
 using namespace terrain;
 for(int scale=0;scale<2;++scale) {
  double cell=scale?12000:1536;RockSite best;double nearest=1e30;
  for(int iz=-35;iz<=35;++iz)for(int ix=-35;ix<=35;++ix) {
   auto s=RockSiteAt((ix+.5)*cell,(iz+.5)*cell,scale);
   if(s.radius && s.strength>.65 && s.slope<(scale?.55:.15) && std::hypot(s.x,s.z)>4800+s.radius*2 && (!scale || MountainAt(s.x,s.z).height>5000) && std::hypot(s.x,s.z)<nearest){best=s;nearest=std::hypot(s.x,s.z);}
  }
  assert(best.radius>0);
  std::printf("weathering scale %d site %.3f %.3f radius %.0f height %.1f slope %.3f strength %.2f\n",scale,best.x,best.z,best.radius,best.height,best.slope,best.strength);
  std::ofstream profile(scale?"port/run/mod-tests/integration/weathering-mountain-profile.csv":"port/run/mod-tests/integration/weathering-hill-profile.csv");
  profile<<"distance,before,after,bedrock,talus\n";
  double change=0,rock=0,talus=0;
  for(int i=0;i<=300;++i) {
   double d=best.radius*(-1.2+2.8*i/300),x=best.x+best.dx*d,z=best.z+best.dz*d;
   auto w=WeatheringAt(x,z);double before=MountainAt(x,z).height,after=MountainHeight(x,z);
   assert(std::isfinite(after));change=std::max(change,std::abs(after-before));rock=std::max(rock,w.bedrock);talus=std::max(talus,w.talus);
   profile<<d<<','<<before<<','<<after<<','<<w.bedrock<<','<<w.talus<<'\n';
  }
  assert(change>(scale?30:8));assert(rock>.4&&talus>.1);
  std::ofstream map(scale?"port/run/mod-tests/integration/weathering-mountain-map.csv":"port/run/mod-tests/integration/weathering-hill-map.csv");
  map<<"x,z,before,after,rock,talus\n";
  for(int iz=0;iz<128;++iz)for(int ix=0;ix<128;++ix) {
   double x=best.x+(ix-64)*best.radius/40,z=best.z+(iz-64)*best.radius/40;auto w=WeatheringAt(x,z);
   map<<x<<','<<z<<','<<MountainAt(x,z).height<<','<<MountainHeight(x,z)<<','<<w.bedrock<<','<<w.talus<<'\n';
  }
  // Features are wholly inside their owner cell: no seams, including very far coordinates.
  for(double origin:{0.,1e8,1e12,-1e12})for(int i=-8;i<=8;++i) {
   double edge=std::floor(origin/cell)*cell;double along=origin+i*cell*.17;
   assert(std::abs(WeatheringAt(edge-.001,along).displacement-WeatheringAt(edge+.001,along).displacement)<.1);
   assert(std::abs(WeatheringAt(along,edge-.001).displacement-WeatheringAt(along,edge+.001).displacement)<.1);
  }
 }
 for(int z=-3000;z<=3000;z+=100)for(int x=-3000;x<=3000;x+=100)
  if(std::hypot(x,z)<3200)assert(WeatheringAt(x,z).displacement==0);
 for(int i=0;i<360;++i) {
  double a=i*3.141592653589793/180;
  assert(std::abs(WeatheringAt(3200.001*std::cos(a),3200.001*std::sin(a)).displacement)<.001);
 }
 std::puts("PASS hill and mountain ledges, downslope debris, protected town and far-coordinate continuity");
}
