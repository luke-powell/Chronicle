#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Flat-floored slot canyon districts. The town is an authored clearing inside
// this biome, not a scaled procedural approximation of Norune.
namespace canyon {
constexpr uint64_t WorldSeed=0x4e4f52554e453031ULL;
constexpr double CellSize=52000, WallHeight=200, WalkSlope=.65;
inline uint64_t Hash(uint64_t n){n^=n>>30;n*=0xbf58476d1ce4e5b9ULL;n^=n>>27;n*=0x94d049bb133111ebULL;return n^(n>>31);}
inline double Unit(uint64_t n){return double(Hash(n)>>11)/9007199254740992.;}
inline double Smooth(double a,double b,double x){double t=std::clamp((x-a)/(b-a),0.,1.);return t*t*(3-2*t);}
struct Point {double x,z;};
inline double Segment(Point p,Point a,Point b){double x=b.x-a.x,z=b.z-a.z;double t=std::clamp(((p.x-a.x)*x+(p.z-a.z)*z)/(x*x+z*z),0.,1.);return std::hypot(p.x-a.x-t*x,p.z-a.z-t*z);}
template<size_t N> inline double Path(Point p,const std::array<Point,N>&points){double d=1e30;for(size_t i=1;i<N;++i)d=std::min(d,Segment(p,points[i-1],points[i]));return d;}
// Match the retail western opening, including its dogleg inside the cliff.
inline constexpr std::array<Point,6> Exit={Point{-700,-350},{-855,-350},{-925,-150},{-1200,-150},{-1500,-150},{-2000,-150}};
// Branches share approach tangents, rather than attaching as perpendicular T's.
inline const std::vector<std::vector<Point>> &Routes(){
 static const std::vector<std::vector<Point>> routes={
 {{-1500,-150},{-1950,-150},{-2350,0},{-2650,400},{-2800,900},{-3100,1500},{-3700,2200},{-4300,3100},{-4700,4300},{-4500,6000},{-5200,8500},{-6500,12500},{-7000,18000}},
 {{-2800,900},{-3100,1500},{-4100,1750},{-5400,1300},{-6500,1700},{-7700,2800},{-9200,3100},{-11000,4500},{-14500,5000},{-20000,5500}},
 {{-4300,3100},{-4700,4300},{-5800,5100},{-7200,5400},{-8400,6800},{-10100,7300},{-12000,9500},{-16000,12000}},
 {{-6500,1700},{-7700,2800},{-7900,4300},{-7200,5400}},
 {{-4100,1750},{-5400,1300},{-6100,300},{-5900,-1000},{-6800,-2300},{-7500,-4000},{-7200,-5700},{-8200,-8000},{-9000,-12000},{-11000,-17000}},
 {{-5900,-1000},{-6800,-2300},{-8500,-2600},{-10000,-1800},{-12000,-2100},{-14500,-3900},{-19000,-5000}},
 {{-7500,-4000},{-7200,-5700},{-5800,-6600},{-4200,-6300},{-2800,-7600},{-1000,-8300},{1800,-9700},{4000,-14000}},
 {{-6800,-2300},{-7500,-4000},{-6800,-4900},{-5200,-4700},{-4100,-3300},{-2600,-2900},{-900,-3100},{800,-2600},{2200,-3000},{3800,-4500},{5800,-4900},{9500,-6500},{15000,-7500}},
 {{-3700,2200},{-4300,3100},{-3600,4000},{-1800,4400},{-500,3700},{1000,3300},{2500,3900},{4000,3600},{5500,4500},{9000,5400},{14000,8500}},
 {{-1800,4400},{-500,3700},{1000,3300},{1900,2000},{2400,700},{2100,-800},{2200,-1900},{2200,-3000}},
 {{2500,3900},{4000,3600},{4600,2100},{4400,500},{5200,-900},{5800,-2600},{5800,-4900}},
 {{-4700,4300},{-4500,6000},{-3400,7600},{-1700,8300},{0,9700},{2500,11000},{4000,15500}},
 {{-3400,7600},{-1700,8300},{-500,7700},{1000,6200},{2500,3900}},
 {{-10000,-1800},{-12000,-2100},{-13200,-1000},{-13000,1200},{-11000,4500}},
 {{-9200,3100},{-11000,4500},{-12000,6500},{-12000,9500}},
 {{-8500,-2600},{-10000,-1800},{-10500,-400},{-9900,1500},{-9200,3100}}
 };return routes;
}
// Spatially index the smoothed corridors once. Height queries visit only
// nearby segments, so additional branches do not multiply every terrain query.
struct Network {
 struct Line{Point a,b;};
 static constexpr int Side=88;static constexpr double Bin=512,Min=-22528;
 std::vector<Line> lines;std::array<std::vector<int>,Side*Side> bins;
 Network(){
  for(const auto &route:Routes())for(size_t i=0;i+1<route.size();i++){
   Point a=route[i?i-1:i],b=route[i],c=route[i+1],d=route[std::min(i+2,route.size()-1)];
   auto at=[&](double t){auto value=[&](double a,double b,double c,double d){return .5*((2*b)+(-a+c)*t+(2*a-5*b+4*c-d)*t*t+(-a+3*b-3*c+d)*t*t*t);};return Point{value(a.x,b.x,c.x,d.x),value(a.z,b.z,c.z,d.z)};};
   int steps=std::max(4,int(std::hypot(c.x-b.x,c.z-b.z)/100));
   for(int k=0;k<steps;k++){
    Line l{at(double(k)/steps),at(double(k+1)/steps)};int id=int(lines.size());lines.push_back(l);
    auto cell=[](double v){return std::clamp(int(std::floor((v-Min)/Bin)),0,Side-1);};
    for(int z=cell(std::min(l.a.z,l.b.z)-210);z<=cell(std::max(l.a.z,l.b.z)+210);z++)for(int x=cell(std::min(l.a.x,l.b.x)-210);x<=cell(std::max(l.a.x,l.b.x)+210);x++)bins[z*Side+x].push_back(id);
   }
  }
 }
 double distance(Point p)const{
  int x=int(std::floor((p.x-Min)/Bin)),z=int(std::floor((p.z-Min)/Bin));if(x<0||z<0||x>=Side||z>=Side)return 205;
  double result=205;for(int id:bins[z*Side+x])result=std::min(result,Segment(p,lines[id].a,lines[id].b));return result;
 }
};
inline const Network &Corridors(){static const Network n;return n;}
inline double Outline(double u,double v,uint64_t seed){
 double a=std::atan2(v,u),phase=Unit(seed+2)*6.283185307179586;
 double radius=15000*(1+.19*std::sin(3*a+phase)+.12*std::cos(5*a-phase)+.07*std::sin(9*a+2*phase));
 return std::hypot(u/1.15,v)-radius;
}
struct District {int64_t ix=0,iz=0;uint64_t seed=0;double x=0,z=0,elevation=0;};
inline District Generate(int64_t ix,int64_t iz){return {ix,iz,Hash(uint64_t(ix)^Hash(uint64_t(iz)+WorldSeed)),ix*CellSize,iz*CellSize,0};}
struct Site{District district=Generate(0,0);};
inline const Site &Home(){static const Site site;return site;}
inline District Owner(double x,double z){return Generate(int64_t(std::floor(x/CellSize+.5)),int64_t(std::floor(z/CellSize+.5)));}
inline std::array<double,2> Centre(const District &d){return {d.x,d.z};}
struct Sample {double height=0,weight=0,wall=0,channel=1e9,water=0,width=0,soil=.65;};
// Authored cliff heights are sampled once from the original meshes, before
// workers start. Outward running maxima bury only the unseen back slopes and
// continue the actual ridge into a traversable plateau.
constexpr int ShellAngles=512,ShellRadii=257;
struct Shell {std::array<float,ShellAngles*ShellRadii> heights{};std::array<float,ShellAngles> ridge{};bool ready=false;};
inline Shell &TownShell(){static Shell s;return s;}
inline double RidgeRadius(double x,double z){
 const auto &s=TownShell();if(!s.ready)return 1100;
 double a=std::atan2(z,x)*ShellAngles/6.283185307179586;if(a<0)a+=ShellAngles;
 int i=int(a)%ShellAngles;return std::lerp(double(s.ridge[i]),double(s.ridge[(i+1)%ShellAngles]),a-std::floor(a))+8;
}
inline double ShellHeight(double x,double z){
 const auto &s=TownShell();double r=std::hypot(x,z);
 if(!s.ready)return WallHeight*Smooth(0,260,std::max(std::abs(x)-700,std::abs(z+100)-600));
 double a=std::atan2(z,x)*ShellAngles/6.283185307179586;if(a<0)a+=ShellAngles;
 int i=int(a)%ShellAngles,j=(i+1)%ShellAngles;double u=a-std::floor(a);
 double t=std::clamp(r/8.,0.,double(ShellRadii-1));int k=std::min(int(t),ShellRadii-2);double v=t-k;
 return std::lerp(std::lerp(double(s.heights[i*ShellRadii+k]),double(s.heights[i*ShellRadii+k+1]),v),std::lerp(double(s.heights[j*ShellRadii+k]),double(s.heights[j*ShellRadii+k+1]),v),u);
}
inline Sample At(double x,double z){
 auto d=Owner(x,z);double u=x-d.x,v=z-d.z,r=std::hypot(u,v);
 if(r>=26000)return {};
 double weight=1-Smooth(-1200,1200,Outline(u,v,d.seed));
 if(weight==0)return {};
 // Each district has the same scale, with seeded bends away from the town.
 double warp=Smooth(1800,2800,std::hypot(u,v));
 Point p{u+warp*80*std::sin(v/510+Unit(d.seed)*6),v+warp*60*std::sin(u/630+Unit(d.seed+1)*6)};
 double distance=Corridors().distance(p);
 double height=105*Smooth(65,82,distance)+95*Smooth(82,205,distance);
 double face=height/WallHeight;
 if(d.ix==0&&d.iz==0){
   double shell=ShellHeight(x,z);
   // Preserve the native ground and wall profile; the connector starts at
   // the actual west exit, not in a newly cut hole on another side.
   shell=std::min(shell,WallHeight*Smooth(42,90,Path({x,z},Exit)));
   height=std::lerp(shell,height,Smooth(1400,1950,r));
   if(x>=-700&&x<=700&&z>=-700&&z<=500)height=0;
 }
 return {height,weight,face,1e9,0,0,.65};
}
// Share this exclusion across near instances, grass and distant forest LOD.
inline double FloraMask(double x,double z){
 auto c=At(x,z);
 if(std::abs(x)<1300&&std::abs(z)<1300)return 0;
 return 1-c.weight*(1-Smooth(175,195,c.height));
}

}
