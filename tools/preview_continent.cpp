#include "terrain_kernel.hpp"
#include <fstream>
#include <cstdio>
#include <cstdlib>
int main(int argc,char**argv) {
 double cx=argc>1?std::atof(argv[1]):0,cz=argc>2?std::atof(argv[2]):0,span=argc>3?std::atof(argv[3]):2000000;
 int n=argc>4?std::atoi(argv[4]):512;
 std::ofstream f(argc>5?argv[5]:"continent.csv");
 f<<"x,z,height,range,plateau,sand,forest,density,snow,rain,temperature,slope\n";
 double peak=-1e30;int sea=0,dry=0,forest=0;double best[3]={1e30,1e30,1e30};
 for(int iz=0;iz<n;++iz)for(int ix=0;ix<n;++ix) {
  double x=cx+(ix-n/2)*span/n,z=cz+(iz-n/2)*span/n;
  auto g=terrain::GeographyAt(x,z);double h=terrain::LandHeight(x,z);
  auto c=terrain::FinishClimate(terrain::BaseClimateAt(x,z,h),h);
  double density=terrain::VegetationDensity(x,z,c,span/n);
  double grad=std::hypot(terrain::LandHeight(x+32,z)-terrain::LandHeight(x-32,z),terrain::LandHeight(x,z+32)-terrain::LandHeight(x,z-32))/64;
  f<<x<<','<<z<<','<<h<<','<<g.range<<','<<g.plateau<<','<<c.sand<<','<<c.forest<<','<<density<<','<<c.snow<<','<<c.rainfall<<','<<c.temperature<<','<<std::atan(grad)*180/3.141592653589793<<'\n';
  peak=std::max(peak,h);sea+=h<terrain::SeaLevel;dry+=h>terrain::SeaLevel&&c.sand>.65;forest+=c.forest>.7;
  double distance=std::hypot(x,z);
  bool candidate[]={c.sand>.9&&h>0&&grad<.35,c.forest>.85&&h>0&&density>.4,g.range>.7&&h>17000};
  for(int k=0;k<3;++k)if(candidate[k]&&distance<best[k]){best[k]=distance;std::printf("site %d %.0f %.0f height %.0f\n",k,x,z,h);}
 }
 std::printf("samples %d ocean %d dry %d forest %d peak %.0f\n",n*n,sea,dry,forest,peak);
}
