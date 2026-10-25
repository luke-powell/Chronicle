#include "terrain_kernel.hpp"
#include "forest_kernel.hpp"
#include <cassert>
#include <cstdio>
#include <set>
int main() {
 using namespace terrain;
 int land=0,sea=0,desert=0,forest=0,clearings=0,dense=0,oases=0;
 double peak=0,bestDune=1e30;std::pair<int,int> duneSite;std::set<Biome> biomes;
 for(int z=-1000000;z<1000000;z+=10000)for(int x=-1000000;x<1000000;x+=10000) {
  double h=LandHeight(x,z);auto c=FinishClimate(BaseClimateAt(x,z,h),h);auto g=GeographyAt(x,z);double d=VegetationDensity(x,z,c);
  assert(std::isfinite(h));peak=std::max(peak,h);
  if(h<SeaLevel){++sea;assert(d==0);continue;}++land;biomes.insert(c.biome);
  if(c.sand>.95&&g.range<.05&&Smooth(-.2,.35,ClimateNoise(x/47000.-34,z/47000.+19))>.95&&h<5000&&h>100&&std::hypot(x,z)<bestDune){bestDune=std::hypot(x,z);duneSite={x,z};}
  if(c.sand>.8){++desert;assert(SpeciesWeights(c)[3]==1);assert(d<.025);assert(UnderstoryDensity(x,z,c)==0);}
  if(c.forest>.7){++forest;clearings+=d<.03;dense+=d>.4;}
 }
 assert(sea>8000&&land>8000);assert(desert>500&&forest>500);assert(clearings>100&&dense>100);assert(peak>20000&&peak<35000);assert(biomes.size()>=5);
 double nearest=1e30;
 for(int z=-35;z<=35;++z)for(int x=-35;x<=35;++x) {
  auto o=OasisCell(x,z);if(!o.radius)continue;++oases;
  assert(LandHeight(o.x,o.z)<o.water);
  for(int k=0;k<8;++k){double a=k*6.28318530718/8;assert(DrylandHeight(o.x+o.radius*1.4*std::cos(a),o.z+o.radius*1.4*std::sin(a))>o.water);}
  auto c=ClimateAt(o.x+o.radius,o.z,o.water+5);assert(c.riparian>0&&c.moisture>.6);
  if(std::hypot(o.x,o.z)<nearest){nearest=std::hypot(o.x,o.z);std::printf("oasis site %.3f %.3f radius %.0f water %.0f\n",o.x,o.z,o.radius,o.water);}
 }
 assert(oases>2&&oases<150);
 int pans=0;
 for(int iz=-24;iz<=24;++iz)for(int ix=-24;ix<=24;++ix) {
  auto pan=DryBasinAt((ix+.5)*42000,(iz+.5)*42000);if(!pan.radius)continue;++pans;
  double center=DrylandHeight(pan.x,pan.z);
  assert(std::abs(center-pan.floor)<2);
  for(int k=0;k<8;++k) {
   double a=k*6.28318530718/8;
   assert(std::abs(DrylandHeight(pan.x+pan.radius*.3*std::cos(a),pan.z+pan.radius*.3*std::sin(a))-center)<3);
  }
  if(pans==1)std::printf("playa site %.0f %.0f radius %.0f floor %.0f\n",pan.x,pan.z,pan.radius,pan.floor);
 }
 assert(pans>0&&pans<80);std::printf("PASS %d rare dry basin floors\n",pans);
 std::printf("sand sea site %d %d\n",duneSite.first,duneSite.second);
 for(auto p:{std::pair{39062.,-82031.},std::pair{-11718.,89843.},std::pair{-82031.,39062.}}) {
  auto c=FinishClimate(BaseClimateAt(p.first,p.second,LandHeight(p.first,p.second)),LandHeight(p.first,p.second));
  double lo=1e20,hi=-1e20;for(int k=0;k<200;++k){double h=DuneHeight(p.first+k*20,p.second);lo=std::min(lo,h);hi=std::max(hi,h);}
  assert(hi-lo>100);std::printf("dune site %.0f %.0f sand %.3f erg %.3f height %.1f relief %.1f\n",p.first,p.second,c.sand,Smooth(-.2,.35,ClimateNoise(p.first/47000-34,p.second/47000+19)),LandHeight(p.first,p.second),hi-lo);
 }
 std::printf("PASS continent: land %d sea %d desert %d forest %d clearings %d dense %d springs %d peak %.0f\n",land,sea,desert,forest,clearings,dense,oases,peak);
}
