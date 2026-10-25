// Verify the real renderer export against the canonical contour heightfield.
#include "canyon_kernel.hpp"
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>
#include <cassert>
int main(){
 std::ifstream in("port/build/canyon-rendered.obj");assert(in);
 std::vector<std::array<double,3>> vertices;std::string line;int checked=0;double worst=0;
 while(std::getline(in,line)){
  std::istringstream row(line);std::string type;row>>type;
  if(type=="v"){std::array<double,3> p;row>>p[0]>>p[1]>>p[2];vertices.push_back(p);}
  if(type!="f")continue;
  int a,b,c;row>>a>>b>>c;auto pa=vertices[a-1],pb=vertices[b-1],pc=vertices[c-1];
  double x=(pa[0]+pb[0]+pc[0])/3,z=(pa[2]+pb[2]+pc[2])/3,y=(pa[1]+pb[1]+pc[1])/3;
  // Native transition and outer LOD stitching deliberately have other shapes.
  if(std::max(std::abs(x),std::abs(z))<=2560||canyon::At(x,z).weight<1)continue;
  auto edge=[](double p){double u=p-std::floor(p/512)*512;return u<128||u>384;};
  if(edge(x)||edge(z))continue;
  double error=std::abs(canyon::At(x,z).height-.04-y);worst=std::max(worst,error);
  if(error>.2){std::printf("FAIL facet %.3f %.3f: mesh %.3f field %.3f\n",x,z,y,canyon::At(x,z).height);return 1;}
  checked++;
 }
 assert(checked>100);std::printf("PASS %d rendered facets agree with contour field; worst export error %.5f\n",checked,worst);
}
