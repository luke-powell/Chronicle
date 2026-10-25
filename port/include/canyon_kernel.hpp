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
inline constexpr std::array<Point,6> Exit={Point{-700,-350},{-855,-350},{-925,-150},{-1141.54,-139.68},{-1500,-150},{-2000,-150}};
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
    for(int z=cell(std::min(l.a.z,l.b.z)-400);z<=cell(std::max(l.a.z,l.b.z)+400);z++)for(int x=cell(std::min(l.a.x,l.b.x)-400);x<=cell(std::max(l.a.x,l.b.x)+400);x++)bins[z*Side+x].push_back(id);
   }
  }
 }
 double distance(Point p)const{
  int x=int(std::floor((p.x-Min)/Bin)),z=int(std::floor((p.z-Min)/Bin));if(x<0||z<0||x>=Side||z>=Side)return 400;
  double result=400;for(int id:bins[z*Side+x])result=std::min(result,Segment(p,lines[id].a,lines[id].b));return result;
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
// A contour-aligned mesh uses these same piecewise planar distance samples.
// Interpolating height instead would round off the shoulder and stretch grid
// triangles over the entire cliff. Keep the sampling lattice global across tiles.
constexpr double FacetSize=128;
inline double RawDistance(double x,double z){
 auto d=Owner(x,z);double u=x-d.x,v=z-d.z;
 double warp=Smooth(1800,2800,std::hypot(u,v));
 return Corridors().distance({u+warp*80*std::sin(v/510+Unit(d.seed)*6),v+warp*60*std::sin(u/630+Unit(d.seed+1)*6)});
}
inline double FacetSample(double px,double pz){
   struct Entry{int64_t x=0,z=0;double distance=0;bool valid=false;};
   static thread_local std::array<Entry,4096> cache;
   int64_t ix=int64_t(std::floor(px/FacetSize)),iz=int64_t(std::floor(pz/FacetSize));
   auto &entry=cache[Hash(uint64_t(ix)^Hash(uint64_t(iz)))&(cache.size()-1)];
   if(!entry.valid||entry.x!=ix||entry.z!=iz)entry={ix,iz,RawDistance(px,pz),true};
   return entry.distance;
}
inline bool FacetFlip(double x,double z){
 double sx=std::floor(x/FacetSize)*FacetSize,sz=std::floor(z/FacetSize)*FacetSize;
 return FacetSample(sx,sz)+FacetSample(sx+FacetSize,sz+FacetSize)
     <FacetSample(sx+FacetSize,sz)+FacetSample(sx,sz+FacetSize);
}
inline double FacetDistance(double x,double z){
 double sx=std::floor(x/FacetSize)*FacetSize,sz=std::floor(z/FacetSize)*FacetSize;
 double u=(x-sx)/FacetSize,v=(z-sz)/FacetSize;
 double a=FacetSample(sx,sz),b=FacetSample(sx+FacetSize,sz),c=FacetSample(sx,sz+FacetSize),d=FacetSample(sx+FacetSize,sz+FacetSize);
 // Follow the channel, rather than bridging diagonally across it. A fixed
 // diagonal can turn a narrow diagonal corridor into a 100-unit-high dam.
 if(a+d<b+c)return u>=v?a*(1-u)+b*(u-v)+d*v:a*(1-v)+c*(v-u)+d*u;
 return u+v<=1 ? a*(1-u-v)+b*u+c*v : b*(1-v)+c*(1-u)+d*(u+v-1);
}
inline double Profile(double distance){
 return 105*std::clamp((distance-65)/17.,0.,1.)+95*std::clamp((distance-82)/123.,0.,1.);
}
inline double WallDistance(double x,double z){
 auto owner=Owner(x,z);double r=std::hypot(x,z),distance=FacetDistance(x,z);
 if(owner.ix==0&&owner.iz==0&&r<1950){
   double width=std::lerp(47.,65.,Smooth(1200,1750,r));
   distance=std::lerp(Path({x,z},Exit)+65-width,distance,Smooth(1700,1950,r));
 }
 return distance;
}
inline std::array<float,3> FloorNormal(double x,double z){
 // Retail shares floor vertices with the walls, so these endpoint normals
 // are tilted. Continue its interpolated lighting across the actual exit edge,
 // then return smoothly to an upward normal over the next 30 metres.
 double t=std::clamp((z+186.24)/93.1201,0.,1.);
 double edgeX=std::lerp(-1140.72,-1142.36,t);
 double blend=Smooth(0,300,edgeX-x);
 return {float(std::lerp(std::lerp(.033013,-.450063,t),0.,blend)),
         float(std::lerp(std::lerp(.697421,.712994,t),1.,blend)),
         float(std::lerp(std::lerp(.715901,-.537664,t),0.,blend))};
}
inline std::array<double,2> Centre(const District &d){return {d.x,d.z};}
struct Sample {double height=0,weight=0,wall=0,channel=1e9,water=0,width=0,soil=.65;};
// Authored cliff heights are sampled once from the original meshes, before
// workers start. Outward running maxima bury only the unseen back slopes and
// continue the actual ridge into a traversable plateau.
constexpr int ShellAngles=512,ShellRadii=257;
struct Shell {std::array<float,ShellAngles*ShellRadii> heights{};std::array<float,ShellAngles> ridge{};bool ready=false;};
inline Shell &TownShell(){static Shell s;return s;}
inline void RestoreShellRidges(){
 auto &s=TownShell();
 for(int a=0;a<ShellAngles;a++){
   double top=0;s.ridge[a]=0;
   for(int r=0;r<ShellRadii;r++)if(s.heights[a*ShellRadii+r]>top){top=s.heights[a*ShellRadii+r];s.ridge[a]=float(r*8);}
   if(s.ridge[a]<700)s.ridge[a]=1200;
 }
 s.ready=true;
}
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
 double distance=WallDistance(x,z);
 double height=Profile(distance);
 double face=height/WallHeight;
 if(d.ix==0&&d.iz==0){
   double native=ShellHeight(x,z),ridge=RidgeRadius(x,z);
   // Preserve the native ground and wall profile; the connector starts at
   // the actual west exit, not in a newly cut hole on another side.
   double distance=Path({x,z},Exit);
   double shell=std::min(native,WallHeight*Smooth(42,90,distance));
   double connector=height;
   // The radial native raster is a join reference, not a wall generator:
   // extending it produces a corrugated skirt at the cliff foot. Finish the
   // blend beneath the retained native mesh, then use one clean cross-section.
   connector+=(native-WallHeight)*Smooth(105,200,connector)*(1-Smooth(ridge,ridge+300,r));
   shell=std::lerp(shell,connector,Smooth(ridge-160,ridge,r));
   height=shell;
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
