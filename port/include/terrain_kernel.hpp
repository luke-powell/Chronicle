#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <tuple>
#include <vector>
#include "shared_memo.hpp"
#include "canyon_kernel.hpp"
#include "forest_town_layout.hpp"
#include "authored_water.hpp"

// Pure, global-coordinate terrain. No renderer, saves, or game assets required.
namespace terrain {
constexpr int    Resolution = 16;
constexpr double BaseSize = 512;
// 112,640 game units per mile; the outer ring guarantees over 2,300 miles.
constexpr int    MaxLevel = 18;
constexpr double Radius = BaseSize * (1u << MaxLevel) * 2;

// Every ring doubles the world-space sample spacing, never the mesh budget.
inline int MeshResolution(int) { return Resolution; }
inline int MeshSampleLevel(int level) { return level; }
constexpr int RingWidth = 8;
constexpr int RingTileCount = RingWidth*RingWidth;
constexpr int RingBandCount = RingTileCount*3/4;
constexpr int MaximumSelectedTiles = RingTileCount + MaxLevel*RingBandCount;


struct Tile {
    int64_t x, z;
    int     level;
    auto    operator<=>(const Tile &) const = default;

    double size() const { return std::ldexp(BaseSize, level); }

    double left() const { return x * size(); }

    double top() const { return z * size(); }
};

inline uint64_t Mix(uint64_t v) {
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

inline double Noise(double x, double z) {
    auto   ix = int64_t(std::floor(x)), iz = int64_t(std::floor(z));
    double u = x - ix, v = z - iz;
    auto   smooth = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    u = smooth(u);
    v = smooth(v);
    auto h = [](int64_t a, int64_t b) { return double(Mix(uint64_t(a) ^ Mix(uint64_t(b) + 4271)) >> 11) / 9007199254740992.0 * 2 - 1; };
    return std::lerp(std::lerp(h(ix, iz), h(ix + 1, iz), u), std::lerp(h(ix, iz + 1), h(ix + 1, iz + 1), u), v);
}

inline double Smooth(double a, double b, double x) {
    double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

inline double ClimateNoise(double x, double z) {
    double rx = .8 * x - .6 * z, rz = .6 * x + .8 * z;
    auto   ix = int64_t(std::floor(rx)), iz = int64_t(std::floor(rz));
    double u = rx - ix, v = rz - iz;
    auto   fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto   dot = [](int64_t x, int64_t z, double u, double v) {
        constexpr double g[8][2] = {
            {1,          0         },
            {-1,         0         },
            {0,          1         },
            {0,          -1        },
            {.70710678,  .70710678 },
            {-.70710678, .70710678 },
            {.70710678,  -.70710678},
            {-.70710678, -.70710678}
        };
        auto k = Mix(uint64_t(x) ^ Mix(uint64_t(z) + 7619)) & 7;
        return g[k][0] * u + g[k][1] * v;
    };
    return 1.65 * std::lerp(std::lerp(dot(ix, iz, u, v), dot(ix + 1, iz, u - 1, v), fade(u)),
                            std::lerp(dot(ix, iz + 1, u, v - 1), dot(ix + 1, iz + 1, u - 1, v - 1), fade(u)), fade(v));
}

// Regional structure is evaluated before climate and local relief. The home
// continent is an irregular connected landmass, not a repeating biome tile.
constexpr double SeaLevel = -300;
struct Geography { double continental, land, range, plateau; };
inline Geography GeographyAt(double x,double z) {
    double wx=x+155000*ClimateNoise(x/850000+39,z/850000-13);
    double wz=z+155000*ClimateNoise(x/850000-71,z/850000+53);
    double home=1.15*std::exp(-std::pow((wx-90000)/620000,2)-std::pow((wz+30000)/460000,2));
    double continent=.68*ClimateNoise(wx/720000+21,wz/720000-17)
                    +.22*ClimateNoise(wx/260000-18,wz/260000+61)-.28+home;
    // Rugged coastal margins break into bays, peninsulas and offshore islands.
    double coast=.10*ClimateNoise(wx/67000+6,wz/67000-25)+.035*ClimateNoise(wx/21000-42,wz/21000+16);
    continent+=coast*(1-Smooth(.12,.55,std::abs(continent)));
    double islands=.22*Smooth(.24,.72,ClimateNoise(wx/85000+77,wz/85000-41))
                   *(1-Smooth(-.4,-.12,continent))*Smooth(-.65,-.4,continent);
    continent+=islands;
    double land=Smooth(-.035,.08,continent);
    // Long, warped fold belts with regional breaks. Uplift can actually be zero.
    double fold=std::abs(ClimateNoise(wx/340000+8,wz/340000-32));
    double range=std::pow(1-Smooth(.045,.32,fold),1.5)
                *Smooth(-.30,.3,ClimateNoise(wx/710000-4,wz/710000+18))*land;
    double plateau=Smooth(.10,.55,ClimateNoise(wx/210000+92,wz/210000-5))
                   *(1-range)*land;
    return {continent,land,range,plateau};
}

// Separate the range envelope, valley floor, shoulders and resistant strata.
// These analytic landforms have global ownership and no simulation tile edges.
struct MountainForm {
    double height, valley, bench;
    double erosionBase=0;
};

inline MountainForm MountainAtUncached(double x, double z) {
    double town = Smooth(1250, 2150, std::hypot(x, z));
    double hills = town * (95 * Noise(x / 2400, z / 2400) + 22 * Noise(x / 850, z / 850) + 5 * Noise(x / 280, z / 280));
    double approach = Smooth(24000, 85000, std::hypot(x, z));
    if (approach == 0) {
        return {hills, 0, 0};
    }
    auto geography=GeographyAt(x,z);
    // Broad ranges keep the established world scale, with wider foothills.
    double mx = x / 7.5, mz = z / 7.5;
    double wx = mx + 6500 * ClimateNoise(mx / 27000 + 17, mz / 27000 - 31);
    double wz = mz + 6500 * ClimateNoise(mx / 27000 - 48, mz / 27000 + 9);
    double mass = geography.range * (.65 + .35 * Smooth(-.45,.45,ClimateNoise(wx/62000+4,wz/62000+7)));
    // The range envelope is continental, but individual ridges are kilometres
    // across. Previously these frequencies were also divided by 7.5, making
    // a single smooth flank span an entire range; the .18 octave falloff then
    // removed almost all subsidiary spurs.
    double rx=x+2400*ClimateNoise(x/32000+17,z/32000-31);
    double rz=z+2400*ClimateNoise(x/32000-48,z/32000+9);
    double ridges = 0, weight = 1, amplitude = .70, scale = 55000;
    for (int octave = 0; octave < 4; ++octave) {
        double n=ClimateNoise(rx / scale + 11, rz / scale - 8);
        double ridge = std::max(0.,1.05-std::sqrt(n*n+.0025));
        ridge = std::pow(ridge,1.8);
        ridges += amplitude * ridge * weight;
        weight = std::clamp(ridge * 1.9, 0., 1.);
        amplitude *= .34;
        scale *= .47;
    }
    double relief = mass * 24000 * ridges;
    // Wide, gently undulating valley floors; glacial-style shoulders concentrate
    // steepness at valley walls instead of spreading it across every hillside.
    double vx = rx + 1100 * ClimateNoise(rx / 6800 + 16, rz / 6800 - 28);
    double vz = rz + 1100 * ClimateNoise(rx / 6800 - 73, rz / 6800 + 19);
    double channel = std::abs(ClimateNoise(vx / 51000 + 54, vz / 51000 - 27));
    double glaciation = Smooth(.2, .8, ClimateNoise(wx / 19000 - 18, wz / 19000 + 61) + .5);
    double valleyWall = Smooth(.035, std::lerp(.32, .48, glaciation), channel);
    double floor = SeaLevel + 3500*geography.continental + mass*(1300+650*ClimateNoise(wx/17000-7,wz/17000+31));
    relief *= 1-.50*(1-valleyWall);
    // Resistant strata produce local benches and escarpments, never global steps.
    double benches = Smooth(-.1, .6, ClimateNoise(wx / 12000 - 81, wz / 12000 + 15)) *
                     Smooth(1000, 2800, relief) * (1 - Smooth(10000, 17000, relief));
    double shelfLevel = 5200 + 2700 * ClimateNoise(wx / 16000 + 33, wz / 16000 + 4);
    double delta = relief - shelfLevel;
    double bench = benches * (1 - Smooth(650, 1800, std::abs(delta)));
    relief -= .88 * delta * bench;
    // Broken crags on exposed shoulders, subdued soil-covered detail on benches.
    double rough = mass * (1 - bench) * valleyWall;
    double detail = rough * (450 * ClimateNoise(rx / 2600 + 7, rz / 2600 - 49) + 180 * ClimateNoise(rx / 800, rz / 800) + 55 * ClimateNoise(rx / 260 + 24, rz / 260 - 17));
    double foothills = geography.land*(1-mass)*(230*ClimateNoise(x/13000-31,z/13000+6)+45*ClimateNoise(x/1800+13,z/1800-7));
    // Broad sedimentary tablelands: varied soft shoulders, isolated caprock and
    // incised corridors, rather than terracing every mountain at the same height.
    double cap=Smooth(-.12,.22,ClimateNoise(x/37000+17,z/37000-14));
    double mesa=geography.plateau*(900+2700*cap);
    double cut=1-Smooth(.015,.09,std::abs(ClimateNoise((x+5000*ClimateNoise(x/21000,z/21000))/58000+19,z/58000-23)));
    mesa*=1-.88*cut;
    foothills+=mesa;
    double elevation = floor + relief + detail + foothills;
    return {hills + approach * elevation, approach * geography.range * (1 - valleyWall), approach * bench,
            hills+approach*(floor+relief+foothills)};
}

inline MountainForm BaseMountainAt(double x,double z) {
    // Climate, rock sites, drainage and surface normals repeatedly ask for the
    // same immutable base landscape. Keep a bounded, thread-local point cache.
    struct Sample { double x=0,z=0;MountainForm form{};bool valid=false; };
    static thread_local std::array<Sample,65536> cache;
    auto &s=cache[Mix(std::bit_cast<uint64_t>(x)^Mix(std::bit_cast<uint64_t>(z)))&(cache.size()-1)];
    if(s.valid&&s.x==x&&s.z==z)return s.form;
    auto form=MountainAtUncached(x,z);s={x,z,form,true};return form;
}

// Analytic, slope-directed erosion appearance, not a droplet simulation.
// Local kernels avoid the global-pivot distortion of rotated stripe noise.
// The accumulated gradient turns subsequent, smaller gullies into branches.
inline MountainForm MountainAt(double x,double z) {
    struct Sample {double x=0,z=0;MountainForm form{};bool valid=false;};
    static thread_local std::array<Sample,65536> cache;
    auto &slot=cache[Mix(std::bit_cast<uint64_t>(x)^Mix(std::bit_cast<uint64_t>(z)))&(cache.size()-1)];
    if(slot.valid&&slot.x==x&&slot.z==z)return slot.form;
    auto result=BaseMountainAt(x,z);
    double incisionBudget=0;
    double regional=Smooth(3200,16000,std::hypot(x,z))*Smooth(150,1600,result.height);
    if(regional>0) {
        // Orient kilometre-scale drainage with structural relief, excluding
        // the small crag noise that otherwise rotates gullies at every rock.
        double gx=(BaseMountainAt(x+256,z).erosionBase-BaseMountainAt(x-256,z).erosionBase)/512;
        double gz=(BaseMountainAt(x,z+256).erosionBase-BaseMountainAt(x,z-256).erosionBase)/512;
        double slope=std::hypot(gx,gz);
        // Limit incision to locally available relief. Slope alone otherwise
        // excavates both walls below a comparatively untouched valley floor.
        double localFloor=result.height;
        for(auto offset:std::array<std::array<double,2>,4>{{{1024,0},{-1024,0},{0,1024},{0,-1024}}})
            localFloor=std::min(localFloor,BaseMountainAt(x+offset[0],z+offset[1]).height);
        incisionBudget=.35*std::max(0.,result.height-localFloor);
        double size=18000,amplitude=regional*(90*Smooth(.025,.18,slope)+4000*Smooth(.25,.85,slope));
        for(int octave=0;octave<4;++octave) {
            double slope=std::hypot(gx,gz),px=-gz/std::max(slope,.001),pz=gx/std::max(slope,.001);
            double qx=x/size,qz=z/size;
            auto ix=int64_t(std::floor(qx)),iz=int64_t(std::floor(qz));
            double sum=0,value=0,dx=0,dz=0;
            for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i) {
                auto h=Mix(uint64_t(ix+i)^Mix(uint64_t(iz+j)+7219));
                double cx=ix+i+.35+.3*double(h&65535)/65535;
                double cz=iz+j+.35+.3*double((h>>16)&65535)/65535;
                double ux=qx-cx,uz=qz-cz,r=ux*ux+uz*uz;
                double w=std::pow(std::max(0.,1-r/1.5625),3);
                double phase=6.28318530718*(ux*px+uz*pz);
                double co=std::cos(phase),si=std::sin(phase);
                sum+=w;value+=w*co;dx+=w*si*px;dz+=w*si*pz;
            }
            if(sum>0) {
                double fade=Smooth(.015,.16,slope);
                double co=std::tanh(2*value/sum),channel=.5*(1-co);
                result.height-=amplitude*channel*channel*channel*fade;
                double derivative=6*channel*channel*(1-co*co);
                gx-=amplitude*3.14159265359*dx/(sum*size)*fade*derivative;
                gz-=amplitude*3.14159265359*dz/(sum*size)*fade*derivative;
            }
            size*=.45;amplitude*=.35;
        }
    }
    double incision=BaseMountainAt(x,z).height-result.height;
    // Smooth saturation retains small gullies without a hard clipping seam.
    if(incision>0)result.height=BaseMountainAt(x,z).height-
        incisionBudget*std::tanh(incision/std::max(incisionBudget,1e-9));
    slot={x,z,result,true};return result;
}
struct Weathering { double displacement=0, bedrock=0, talus=0; };
struct RockSite { double x=0,z=0,dx=0,dz=0,slope=0,radius=0,height=0,strength=0; };
inline RockSite RockSiteAt(double x,double z,int scale) {
    const double cell=scale?12000:1536;
    auto ix=int64_t(std::floor(x/cell)),iz=int64_t(std::floor(z/cell));
    static thread_local std::map<std::tuple<int64_t,int64_t,int>,RockSite> cache;
    auto key=std::tuple{ix,iz,scale};if(auto it=cache.find(key);it!=cache.end())return it->second;
    RockSite site;uint64_t seed=Mix(uint64_t(ix)^Mix(uint64_t(iz)+91751+scale*613));
    if(seed%4!=0) {
        double cx=(ix+.35+.3*double((seed>>8)&65535)/65535)*cell;
        double cz=(iz+.35+.3*double((seed>>24)&65535)/65535)*cell;
        double h=MountainAt(cx,cz).height;
        double hardness=Smooth(-.15,.45,ClimateNoise(cx/17000+35,cz/17000-71));
        if(h>SeaLevel+90 && std::hypot(cx,cz)>3600 && hardness>.02) {
            double sample=scale?256:64;
            double gx=(MountainAt(cx+sample,cz).height-MountainAt(cx-sample,cz).height)/(2*sample);
            double gz=(MountainAt(cx,cz+sample).height-MountainAt(cx,cz-sample).height)/(2*sample);
            double slope=std::hypot(gx,gz);
            double strength=hardness*Smooth(scale?.015:.006,scale?.10:.035,slope)*(1-Smooth(1.25,2.2,slope));
            if(strength>.01) {
                double radius=(scale?1700:210)+(scale?650:80)*double((seed>>40)&255)/255;
                double height=scale?std::clamp(radius*slope*.42,90.,850.):std::clamp(radius*slope*1.1,24.,110.);
                site={cx,cz,-gx/std::max(slope,.001),-gz/std::max(slope,.001),slope,radius,height,strength};
            }
        }
    }
    if(cache.size()>32768)cache.clear();cache.emplace(key,site);return site;
}
inline Weathering WeatheringAt(double x,double z) {
    Weathering result;
    const double townBlend=Smooth(3200,4800,std::hypot(x,z));
    if(townBlend==0)return result;
    for(int scale=0;scale<2;++scale) {
        auto site=RockSiteAt(x,z,scale);if(!site.radius)continue;
        double px=x-site.x,pz=z-site.z;
        double u=(px*site.dx+pz*site.dz)/site.radius;
        double v=(-px*site.dz+pz*site.dx)/site.radius;
        if(u<-.95 || u>1.45 || std::abs(v)>1.0)continue;
        double joints=ClimateNoise(x/(scale?410:85)+17,z/(scale?410:85)-23);
        double width=1-Smooth(.35+.12*joints,.92+.05*joints,std::abs(v));
        double cap=Smooth(-.95,-.55,u)*(1-Smooth(-.12,.04,u));
        double strength=width*site.strength*townBlend;
        // Resistant ledge above an asymmetric retreating face. Cancelling most
        // of the local slope creates a real shoulder, rather than a noise bump.
        double bench=site.height+.82*site.slope*u*site.radius;
        double fracture=(scale?35:6)*joints*(1-std::abs(v));
        result.displacement+=strength*cap*(bench+fracture);
        result.bedrock=std::max(result.bedrock,strength*cap);
        // Debris rests below the face and thins downslope. The runout depends
        // on the available slope below a 34-degree loose-material threshold.
        double amount=site.height*.32;
        double run=std::min(site.radius*1.4,site.radius*.3+amount/std::max(.10,.6745-site.slope));
        double apron=Smooth(-.04,.10,u)*(1-Smooth(0,run,std::max(0.,u*site.radius)));
        apron*=1-Smooth(.55,.85,site.slope);
        result.displacement+=strength*amount*apron;
        result.talus=std::max(result.talus,strength*apron);
    }
    return result;
}
inline double MountainHeight(double x, double z) { return MountainAt(x, z).height+WeatheringAt(x,z).displacement; }

// Exposed rock follows steepness and broken uplands; altitude alone is not rock.
inline double RockExposure(double x, double z, double height, double normalY);

inline double SlopeVegetation(double normalY) {
    return 1 - Smooth(.075, .25, 1 - normalY);
}

enum class Biome {
    Meadow,
    Woodland,
    Rainforest,
    Wetland,
    Dryland,
    Alpine
};

// Design reference: Glitchfiend/BiomesOPlenty, revision cc292da4989591b081bc57326648a4ac3724ca8d:
// BOPOverworldBiomeBuilder (climate/altitude regions), BOPMaterialRuleData
// (surface rules), and BOPVegetationPlacements (filtered, clustered features).
// The noise, downhill drainage and continuous meshes here are our own implementation.
// Climate and terrain share continuous global-coordinate gradient noise.

struct Climate {
    double                temperature, moisture, forest, sand, snow;
    Biome                 biome;
    double                rainfall = 0, seasonality = 0, soil = 0, wetland = 0;
    double aridity=0, riparian=0, land=1;
    std::array<double, 6> weights{};
};

struct Weather {
    double heat, rain, season, soil;
};

inline Weather WeatherNode(int64_t ix, int64_t iz) {
    static thread_local std::map<std::pair<int64_t, int64_t>, Weather> cache;
    auto                                                               key = std::pair{ix, iz};
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    double x = ix * 4096., z = iz * 4096.;
    double wx = x + 18000 * ClimateNoise(x / 160000 + 17, z / 160000 - 9);
    double wz = z + 18000 * ClimateNoise(x / 160000 - 31, z / 160000 + 23);
    double heat = .62 + .48*ClimateNoise(wx/520000+12,wz/520000-19) + .12*ClimateNoise(wx/180000,wz/180000+91);
    auto geography=GeographyAt(x,z);
    double rain=.48+.50*ClimateNoise(wx/380000-65,wz/380000+27)+.12*ClimateNoise(wx/95000+21,wz/95000);
    rain-=.24*Smooth(.15,.8,geography.continental); // continental interiors lose maritime moisture
    rain+=.12*(1-Smooth(.02,.20,std::abs(geography.continental)));
    // Prevailing west/southwest winds: uplift adds rain, upstream ridges remove it.
    double h = MountainHeight(x, z), up = MountainHeight(x - 32000, z - 16000), barrier = MountainHeight(x - 110000, z - 55000);
    rain += .25 * std::clamp((h - up) / 4000., -1., 1.) - .42 * Smooth(800, 6500, std::max(up, barrier) - h);
    double homeClimate=1-Smooth(18000,65000,std::hypot(x,z));
    rain=std::lerp(rain,.72,homeClimate);heat=std::lerp(heat,.58,homeClimate);
    Weather w{heat, std::clamp(rain, 0., 1.), std::clamp(.45 + .4 * ClimateNoise(wx / 73000 + 43, wz / 73000 - 71), 0., 1.),
              std::clamp(.5 + .35 * ClimateNoise(wx / 19000 - 8, wz / 19000 + 4), 0., 1.)};
    if (cache.size() > 32768) {
        cache.clear();
    }
    cache.emplace(key, w);
    return w;
}

inline Weather WeatherAt(double x, double z) {
    auto   ix = int64_t(std::floor(x / 4096)), iz = int64_t(std::floor(z / 4096));
    double u = Smooth(0, 1, x / 4096 - ix), v = Smooth(0, 1, z / 4096 - iz);
    auto   a = WeatherNode(ix, iz), b = WeatherNode(ix + 1, iz), c = WeatherNode(ix, iz + 1), d = WeatherNode(ix + 1, iz + 1);
    auto   lerp = [&](double Weather::*p) { return std::lerp(std::lerp(a.*p, b.*p, u), std::lerp(c.*p, d.*p, u), v); };
    return {lerp(&Weather::heat), lerp(&Weather::rain), lerp(&Weather::season), lerp(&Weather::soil)};
}

inline Climate BaseClimateAt(double x, double z, double height) {
    auto    w = WeatherAt(x, z);
    Climate c{};
    c.temperature = std::clamp(w.heat - std::max(0.,height) / 22000, 0., 1.);
    c.land=Smooth(SeaLevel+3,SeaLevel+80,height);
    c.rainfall = w.rain;
    c.seasonality = w.season;
    c.soil = w.soil;
    // Water balance: heat/seasonality remove water; retentive soil holds it.
    c.moisture = std::clamp(w.rain + .14 * (w.soil - .5) - .2 * (c.temperature - .45) - .18 * w.season, 0., 1.);
    auto district=canyon::At(x,z);
    district.weight*=1-Smooth(1800,3500,height);
    c.soil=std::lerp(c.soil,district.soil,district.weight);
    c.moisture=std::lerp(c.moisture,.55+.16*district.soil-.18*district.wall,district.weight);
    const double forestTown=foresttown::Forest(x,z);
    c.moisture=std::lerp(c.moisture,.84,forestTown);
    c.temperature=std::lerp(c.temperature,.52,forestTown);
    c.seasonality=std::lerp(c.seasonality,.3,forestTown);
    c.aridity=Smooth(.10,.38,.46-c.moisture);
    return c;
}

inline Climate FinishClimate(Climate c, double height, double riparian = 0) {
    c.riparian=riparian;
    c.moisture = std::lerp(c.moisture, std::max(c.moisture, .86), riparian);
    double treeline = 10500 + 4000 * c.temperature;
    c.forest = c.land * Smooth(.32, .72, c.moisture) * (1 - Smooth(treeline - 3000, treeline + 1500, height));
    c.sand = c.aridity * (1-.95*riparian);
    c.snow = Smooth(14500 + 5000 * c.temperature, 19000 + 5000 * c.temperature, height);
    c.wetland = std::max(riparian*Smooth(.58,.85,c.moisture)*(1-Smooth(150,1400,height)),
                         Smooth(.65,.9,c.moisture)*Smooth(.55,.8,c.soil)*(1-Smooth(200,800,height)));
    double tropical = Smooth(.54, .8, c.temperature) * (1 - .5 * c.seasonality);
    double land = 1 - c.snow, dry = land * c.sand, wet = (land - dry) * c.wetland;
    double trees = (land - dry - wet) * c.forest;
    c.weights = {land - dry - wet - trees, trees * (1 - tropical), trees * tropical, wet, dry, c.snow};
    c.biome = Biome(std::max_element(c.weights.begin(), c.weights.end()) - c.weights.begin());
    return c;
}

inline double RockExposure(double x, double z, double height, double normalY) {
    double cliff = Smooth(.06, .32, 1 - normalY);
    double broken = Smooth(.18, .65, ClimateNoise(x / 1900 + 84, z / 1900 - 12));
    double alpine = Smooth(12500, 18500, height) * (.3 + .45 * broken);
    auto weathered=WeatheringAt(x,z);
    double exposed=std::max(weathered.bedrock,weathered.talus*.85);
    auto c=FinishClimate(BaseClimateAt(x,z,height),height);
    double sandSea=c.sand*Smooth(-.2,.35,ClimateNoise(x/47000-34,z/47000+19))
                   *(1-Smooth(.15,.55,GeographyAt(x,z).range));
    // Slip faces are loose sand, not exposed bedrock. True escarpments retain rock.
    return std::clamp(std::max({cliff,alpine,exposed}),0.,1.)*(1-.98*sandSea*(1-Smooth(.22,.40,1-normalY)));
}

inline Climate ClimateAt(double x, double z, double height, double footprint = 0);

// The same multiscale habitat masks drive individual placement AND horizon
// canopy. Climate allows woodland; soil, stands, meadows and gaps decide where.
inline double VegetationDensity(double x, double z, const Climate &c, double footprint = 0) {
    double wx=x+1300*ClimateNoise(x/12000+52,z/12000-8);
    double wz=z+1300*ClimateNoise(x/12000-12,z/12000+37);
    double stand=Smooth(-.25,.35,ClimateNoise(wx/17000+11,wz/17000-32));
    double meadow=Smooth(.05,.48,ClimateNoise(wx/6300-51,wz/6300+8));
    double gap=Smooth(.08,.50,ClimateNoise(wx/750+73,wz/750-12));
    // Filter only unresolved gaps. Region-scale openings remain at far LOD.
    gap=std::lerp(gap,.22,Smooth(250,1600,footprint));
    meadow=std::lerp(meadow,.27,Smooth(2500,18000,footprint));
    stand=std::lerp(stand,.50,Smooth(9000,70000,footprint));
    double woodland=c.forest*(.2+.8*stand)*(1-.98*meadow)*(1-.92*gap);
    double copses=.035*Smooth(.2,.6,ClimateNoise(wx/2200+21,wz/2200+33))*(1-c.forest);
    double dry=.005+.019*Smooth(-.1,.5,ClimateNoise(wx/2600-15,wz/2600+42));
    double habitat=std::lerp(woodland+copses,dry,Smooth(.18,.65,c.sand));
    auto weathered=WeatheringAt(x,z);
    double exposed=std::max(weathered.bedrock,weathered.talus*.65);
    return std::max(canyon::FloraMask(x,z)*habitat*(1-.95*exposed),.55*foresttown::Forest(x,z))*c.land*(1-c.snow);
}
inline double UnderstoryDensity(double x,double z,const Climate &c) {
    double patch=Smooth(-.05,.45,ClimateNoise(x/320+18,z/320-73));
    auto weathered=WeatheringAt(x,z);
    return std::max(canyon::FloraMask(x,z),foresttown::Forest(x,z))*c.land*(1-c.snow)*(1-Smooth(.12,.55,c.sand))*(.35+.45*c.forest)*(.25+.75*patch)*(1-std::max(weathered.bedrock,weathered.talus*.8));
}

// Climate-independent upper envelope of VegetationDensity. Forest fraction,
// desert blending, land and snow only reduce this bound. Rejecting a candidate
// above it avoids exact terrain/drainage queries without losing a single tree.
inline double VegetationDensityBound(double x,double z){
    double wx=x+1300*ClimateNoise(x/12000+52,z/12000-8);
    double wz=z+1300*ClimateNoise(x/12000-12,z/12000+37);
    double stand=Smooth(-.25,.35,ClimateNoise(wx/17000+11,wz/17000-32));
    double meadow=Smooth(.05,.48,ClimateNoise(wx/6300-51,wz/6300+8));
    double gap=Smooth(.08,.50,ClimateNoise(wx/750+73,wz/750-12));
    double woodland=(.2+.8*stand)*(1-.98*meadow)*(1-.92*gap);
    double copses=.035*Smooth(.2,.6,ClimateNoise(wx/2200+21,wz/2200+33));
    double dry=.005+.019*Smooth(-.1,.5,ClimateNoise(wx/2600-15,wz/2600+42));
    auto weathered=WeatheringAt(x,z);
    return std::max(canyon::FloraMask(x,z)*std::max({woodland,copses,dry})*(1-.95*std::max(weathered.bedrock,weathered.talus*.65)),.55*foresttown::Forest(x,z));
}

inline std::array<double, 4> SpeciesWeights(const Climate &c) {
    double arid = Smooth(.18,.65,c.sand), tropical = (1 - arid) * std::max(Smooth(.54, .8, c.temperature) * Smooth(.25, .65, c.moisture),c.aridity*c.riparian);
    double woodland = (1 - arid - tropical) * Smooth(.35, .8, c.forest) * .8;
    return {1 - arid - tropical - woodland, woodland, tropical, arid};
}

inline int TreeSpecies(const Climate &c, double random) {
    auto w = SpeciesWeights(c);
    for (int i = 0; i < 3; ++i) {
        if (random < w[i]) {
            return i;
        }
        random -= w[i];
    }
    return 3;
}

struct DrainNode {
    double x, z, height;
};

inline double DuneHeight(double x,double z) {
    // Wind-aligned, asymmetric crests: long stoss slopes and shorter slip faces.
    // Large transverse dunes and smaller superimposed ripples have distinct scales.
    double warp=2200*ClimateNoise(x/43000-11,z/43000+7)
               +420*ClimateNoise(x/7000+31,z/7000-19)+130*ClimateNoise(x/1800-7,z/1800+41);
    double along=.89*x+.45*z+warp;
    auto dune=[](double phase) {
        double u=phase-std::floor(phase);
        return u<.78?Smooth(0,1,u/.78):1-Smooth(0,1,(u-.78)/.22);
    };
    double broken=Smooth(-.2,.4,ClimateNoise(x/61000+29,z/61000-64));
    double patch=Smooth(-.45,.35,ClimateNoise(x/4300-19,z/4300+17));
    double crest=std::lerp(dune(along/1900),dune((along+650*ClimateNoise(x/6200,z/6200))/1900)*patch,broken);
    double height=(180+180*Smooth(-.3,.5,ClimateNoise(x/15000+51,z/15000-12))) * crest;
    return height+30*dune((along+120*ClimateNoise(x/900,z/900))/390)*std::lerp(1.,patch,broken);
}

struct DryBasin { double x=0,z=0,radius=0,floor=0; };
inline DryBasin DryBasinAt(double x,double z) {
    constexpr double cell=42000;
    auto ix=int64_t(std::floor(x/cell)),iz=int64_t(std::floor(z/cell));
    static thread_local std::map<std::pair<int64_t,int64_t>,DryBasin> cache;
    auto key=std::pair{ix,iz};
    if(auto it=cache.find(key);it!=cache.end())return it->second;
    DryBasin basin;
    uint64_t seed=Mix(uint64_t(ix)^Mix(uint64_t(iz)+623109));
    if(seed%5==0) {
        double cx=(ix+.25+.5*double((seed>>8)&65535)/65535)*cell;
        double cz=(iz+.25+.5*double((seed>>24)&65535)/65535)*cell;
        auto g=GeographyAt(cx,cz);double h=MountainHeight(cx,cz);
        if(std::hypot(cx,cz)>85000 && g.range<.08 && g.plateau<.15 && h>SeaLevel+250 && h<4500 && BaseClimateAt(cx,cz,h).aridity>.65) {
            // Seek the existing valley bottom inside the owner's guard band.
            // A random centre almost always falls on a slope, not a depression.
            for(int step=0;step<9;++step) {
                double stride=1800*std::pow(.75,step),bestX=cx,bestZ=cz,best=h;
                for(int direction=0;direction<8;++direction) {
                    double a=direction*6.28318530718/8;
                    double px=std::clamp(cx+stride*std::cos(a),(ix+.2)*cell,(ix+.8)*cell);
                    double pz=std::clamp(cz+stride*std::sin(a),(iz+.2)*cell,(iz+.8)*cell);
                    double ph=MountainHeight(px,pz);
                    if(ph<best){best=ph;bestX=px;bestZ=pz;}
                }
                cx=bestX;cz=bestZ;h=best;
            }
            double radius=3600+double((seed>>40)&2047),rim=1e30;
            for(int i=0;i<8;++i){double a=i*6.28318530718/8;rim=std::min(rim,MountainHeight(cx+radius*1.3*std::cos(a),cz+radius*1.3*std::sin(a)));}
            // Sediment fills a pre-existing depression; never stamp flat discs
            // arbitrarily across slopes or mountain crests.
            if(h>SeaLevel+100 && rim>h+20)basin={cx,cz,radius,h+.2*(rim-h)};
        }
    }
    if(cache.size()>8192)cache.clear();cache.emplace(key,basin);return basin;
}
inline double DrylandHeight(double x, double z) {
    double base = MountainHeight(x, z);
    if (std::hypot(x,z)<3000 || base<SeaLevel-50 || base>7000)return base;
    auto climate=FinishClimate(BaseClimateAt(x,z,base),base);
    double fade=Smooth(3000,9000,std::hypot(x,z));
    // Sand seas occupy selected depositional basins; other deserts remain rocky.
    double erg=Smooth(-.2,.35,ClimateNoise(x/47000-34,z/47000+19));
    double basin=1-Smooth(.15,.55,GeographyAt(x,z).range);
    double coast=Smooth(SeaLevel-50,SeaLevel+180,base);
    double dunes=climate.sand*erg*basin*coast*(1-Smooth(6200,7000,base))*DuneHeight(x,z);
    double wet=Smooth(.68,.9,climate.moisture)*(1-Smooth(50,300,base));
    double h=base+fade*(dunes-.55*wet*std::max(0.,base));
    auto pan=DryBasinAt(x,z);
    if(pan.radius) {
        double blend=1-Smooth(pan.radius*.5,pan.radius*1.3,std::hypot(x-pan.x,z-pan.z));
        h=std::lerp(h,pan.floor+1.5*ClimateNoise(x/700,z/700),blend);
    }
    return h;
}

struct Oasis {double x=0,z=0,radius=0,water=0;};
inline Oasis OasisCell(int64_t ix,int64_t iz) {
    static thread_local std::map<std::pair<int64_t,int64_t>,Oasis> cache;
    auto key=std::pair{ix,iz};if(auto it=cache.find(key);it!=cache.end())return it->second;
    uint64_t seed=Mix(uint64_t(ix)^Mix(uint64_t(iz)+719031));
    Oasis o;
    if(seed%18==0) {
        double x=(ix+.15+.7*double((seed>>8)&65535)/65535)*30000;
        double z=(iz+.15+.7*double((seed>>24)&65535)/65535)*30000;
        double h=DrylandHeight(x,z);auto c=BaseClimateAt(x,z,h);auto g=GeographyAt(x,z);
        if(c.aridity>.7 && g.range<.3 && h>SeaLevel+150 && h<5500) {
            double radius=160+double((seed>>40)&255),lo=h,hi=h;
            for(int i=0;i<8;++i) {
                double a=i*6.28318530718/8;
                double v=DrylandHeight(x+radius*1.4*std::cos(a),z+radius*1.4*std::sin(a));
                lo=std::min(lo,v);hi=std::max(hi,v);
            }
            if(hi-lo<100)o={x,z,radius,lo-8};
        }
    }
    if(cache.size()>8192)cache.clear();cache.emplace(key,o);return o;
}
inline Oasis OasisAt(double x,double z) {
    // Springs never cross the 15% owner-cell guard band, so one cell suffices.
    auto o=OasisCell(int64_t(std::floor(x/30000)),int64_t(std::floor(z/30000)));
    if(o.radius && std::hypot(x-o.x,z-o.z)<o.radius*2.5)return o;
    return {};
}
inline double ContinentalLandHeight(double x,double z) {
    double h=DrylandHeight(x,z);
    if(std::hypot(x,z)<3000 || h<SeaLevel)return h;
    auto o=OasisAt(x,z);
    if(o.radius) {
        double d=std::hypot(x-o.x,z-o.z);
        h=std::lerp(h,std::min(h,o.water-24),1-Smooth(o.radius*.5,o.radius*1.7,d));
    }
    return h;
}

inline canyon::Sample CanyonLandform(double x,double z) {
    auto sample=canyon::At(x,z);
    if(sample.weight==0)return sample;
    auto owner=canyon::Owner(x,z);
    struct Region {int64_t x=0,z=0;double delta=0,weight=0;bool valid=false;};
    static thread_local std::array<Region,256> regions;
    auto &region=regions[Mix(uint64_t(owner.ix)^Mix(uint64_t(owner.iz)))&255];
    if(!region.valid || region.x!=owner.ix || region.z!=owner.iz){
        auto centre=canyon::Centre(owner);double base=ContinentalLandHeight(centre[0],centre[1]);
        // Canyon districts belong to low sedimentary tablelands, never overwrite
        // alpine ranges or manufacture islands from the ocean floor.
        double suitability=(1-Smooth(1800,3500,base))*Smooth(SeaLevel,SeaLevel+150,base);
        region={owner.ix,owner.iz,base-owner.elevation+canyon::Home().district.elevation,suitability,true};
    }
    sample.height+=region.delta;sample.water+=region.delta;sample.weight*=region.weight;
    return sample;
}
inline double LandHeight(double x,double z) {
    auto c=CanyonLandform(x,z);
    if(c.weight>=1)return c.height;
    double base=ContinentalLandHeight(x,z);
    return std::lerp(base,c.height,c.weight);
}

inline DrainNode DrainPoint(int64_t x, int64_t z) {
    static thread_local std::map<std::pair<int64_t, int64_t>, DrainNode> cache;
    auto                                                                 key = std::pair{x, z};
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    constexpr double spacing = 2048;
    uint64_t         seed = Mix(uint64_t(x) ^ Mix(uint64_t(z) + 8917));
    double           px = (x + .5 + (double(seed & 65535) / 65535 - .5) * .86) * spacing;
    double           pz = (z + .5 + (double((seed >> 16) & 65535) / 65535 - .5) * .86) * spacing;
    DrainNode        node{px, pz, LandHeight(px, pz) - 18};
    if (cache.size() > 65536) {
        cache.clear();
    }
    cache.emplace(key, node);
    return node;
}

using DrainKey = std::pair<int64_t, int64_t>;

// Immutable terrain sampling for drainage, separate from carved terrain (no cycle).
inline double DrainSurface(double x, double z) {
    struct Sample {
        double x = 0, z = 0, h = 0;
        bool   valid = false;
    };

    static thread_local std::array<Sample, 131072> cache;
    auto                                           index = Mix(std::bit_cast<uint64_t>(x) ^ Mix(std::bit_cast<uint64_t>(z))) & (cache.size() - 1);
    auto                                          &v = cache[index];
    if (v.valid && v.x == x && v.z == z) {
        return v.h;
    }
    double h = LandHeight(x, z);
    v = {x, z, h, true};
    return h;
}

constexpr double RiverMaxCut = 128;

inline bool ChannelSegment(const DrainNode &a, const DrainNode &b) {
    if (b.height > a.height + 1e-8) {
        return false;
    }
    // Check at the walking mesh spacing: eroded gullies can fall between the
    // previous 64-unit samples and leave an otherwise downhill reach airborne.
    int steps = std::max(1, int(std::ceil(std::hypot(b.x - a.x, b.z - a.z) / 32)));
    for (int i = 1; i < steps; ++i) {
        double t = double(i) / steps, water = std::lerp(a.height, b.height, t);
        double ground = DrainSurface(std::lerp(a.x, b.x, t), std::lerp(a.z, b.z, t));
        if (water > ground - 2 || ground - water > RiverMaxCut) {
            return false;
        }
    }
    return true;
}

inline std::vector<DrainNode> RouteChannel(DrainNode a, DrainNode b) {
    // First try the terrain-following direct corridor, with a descending profile.
    std::vector<DrainNode> direct{a};
    int                    steps = std::max(1, int(std::ceil(std::hypot(b.x - a.x, b.z - a.z) / 128)));
    for (int i = 1; i <= steps; ++i) {
        double    t = double(i) / steps, x = std::lerp(a.x, b.x, t), z = std::lerp(a.z, b.z, t);
        double    ground = DrainSurface(x, z), water = i == steps ? b.height : std::min(direct.back().height, ground - 18);
        DrainNode next{x, z, water};
        if (water < b.height || water > direct.back().height || ground - water > RiverMaxCut || !ChannelSegment(direct.back(), next)) {
            direct.clear();
            break;
        }
        direct.push_back(next);
    }
    if (!direct.empty()) {
        return direct;
    }
    // Bounded A* searches around obstacles. Each state carries its water level:
    // it cannot climb back out of a lower hollow or tunnel through a mountain.
    constexpr double cell = 256;
    auto             sx = int64_t(std::floor(a.x / cell)), sz = int64_t(std::floor(a.z / cell));
    auto             ex = int64_t(std::floor(b.x / cell)), ez = int64_t(std::floor(b.z / cell));
    auto             ox = std::min(sx, ex) - 3, oz = std::min(sz, ez) - 3;
    int              nx = int(std::abs(ex - sx)) + 7, nz = int(std::abs(ez - sz)) + 7;
    auto             index = [&](int64_t x, int64_t z) { return int(z - oz) * nx + int(x - ox); };
    int              start = index(sx, sz), goal = index(ex, ez);
    if (start == goal) {
        return {};
    }

    struct State {
        int       cell, parent;
        double    cost;
        DrainNode p;
    };

    std::vector<State> states{
        {start, -1, 0, a}
    };
    std::vector<int> best(nx * nz, -1);
    best[start] = 0;
    using Queue = std::pair<double, int>;
    std::priority_queue<Queue, std::vector<Queue>, std::greater<Queue>> queue;
    queue.emplace(0, 0);
    while (!queue.empty() && states.size() < 2048) {
        int id = queue.top().second;
        queue.pop();
        auto current = states[id];
        if (best[current.cell] != id) {
            continue;
        }
        if (current.cell == goal) {
            std::vector<DrainNode> path;
            for (int at = id; at >= 0; at = states[at].parent) {
                path.push_back(states[at].p);
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        int cx = current.cell % nx, cz = current.cell / nx;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                int x = cx + dx, z = cz + dz;
                if ((!dx && !dz) || x < 0 || z < 0 || x >= nx || z >= nz) {
                    continue;
                }
                int cellIndex = z * nx + x;
                if (cellIndex == start) {
                    continue;
                }
                double px = (ox + x + .5) * cell, pz = (oz + z + .5) * cell;
                if (cellIndex == goal) {
                    px = b.x;
                    pz = b.z;
                }
                double ground = DrainSurface(px, pz), water = std::min(current.p.height, ground - 18);
                if (cellIndex == goal) {
                    water = b.height;
                }
                if (water < b.height || water > current.p.height || ground - water > RiverMaxCut) {
                    continue;
                }
                double distance = std::hypot(px - current.p.x, pz - current.p.z);
                double cost = current.cost + distance * (1 + std::max(0., ground - water - 18) / 32);
                int    previous = best[cellIndex];
                if (previous >= 0 && cost >= states[previous].cost && water <= states[previous].p.height) {
                    continue;
                }
                DrainNode next{px, pz, water};
                if (!ChannelSegment(current.p, next)) {
                    continue;
                }
                int nextId = int(states.size());
                states.push_back({cellIndex, id, cost, next});
                best[cellIndex] = nextId;
                queue.emplace(cost + std::hypot(px - b.x, pz - b.z), nextId);
            }
        }
    }
    return {};
}

struct DrainRoute {
    DrainKey               destination;
    std::vector<DrainNode> points;
};

inline DrainRoute ComputeDrainRoute(DrainKey key) {
    auto                                     a = DrainPoint(key.first, key.second);
    std::vector<std::pair<double, DrainKey>> candidates;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dz) {
                continue;
            }
            DrainKey candidate{key.first + dx, key.second + dz};
            auto     b = DrainPoint(candidate.first, candidate.second);
            if (b.height < a.height) {
                candidates.emplace_back(-(a.height - b.height) / std::hypot(a.x - b.x, a.z - b.z), candidate);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    DrainRoute route{
        key, {a, a}
    };
    for (auto [score, candidate] : candidates) {
        auto b = DrainPoint(candidate.first, candidate.second);
        auto path = RouteChannel(a, b);
        if (!path.empty()) {
            route = {candidate, std::move(path)};
            break;
        }
    }
    // Only shallow enclosed pits attempt a longer breach; the same path checks
    // apply. Deep closed basins stay closed rather than drawing a false outlet.
    if (route.destination == key) {
        candidates.clear();
        for (int dz = -3; dz <= 3; ++dz) {
            for (int dx = -3; dx <= 3; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) < 2) {
                    continue;
                }
                DrainKey candidate{key.first + dx, key.second + dz};
                auto     b = DrainPoint(candidate.first, candidate.second);
                if (b.height >= a.height) {
                    continue;
                }
                double barrier = 0;
                for (int k = 1; k < 8; ++k) {
                    double t = k / 8.;
                    barrier = std::max(barrier, DrainSurface(std::lerp(a.x, b.x, t), std::lerp(a.z, b.z, t)) - a.height);
                }
                if (barrier < RiverMaxCut) {
                    candidates.emplace_back(barrier + std::hypot(a.x - b.x, a.z - b.z) * .008, candidate);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (size_t i = 0; i < std::min(size_t(3), candidates.size()); ++i) {
            auto candidate = candidates[i].second;
            auto path = RouteChannel(a, DrainPoint(candidate.first, candidate.second));
            if (!path.empty()) {
                route = {candidate, std::move(path)};
                break;
            }
        }
    }
    return route;
}

inline const DrainRoute &DrainRouteAt(DrainKey key) {
    static SharedMemo<DrainKey,DrainRoute,65536> shared;
    static thread_local std::map<DrainKey,DrainRoute> local;
    if(auto it=local.find(key);it!=local.end())return it->second;
    auto route=shared.Get(key,[&]{return ComputeDrainRoute(key);});
    if(local.size()>8192)local.clear();
    return local.emplace(key,std::move(route)).first->second;
}

inline DrainKey Downstream(DrainKey key) { return DrainRouteAt(key).destination; }

inline const std::vector<DrainKey> &Tributaries(DrainKey key) {
    static thread_local std::map<DrainKey, std::vector<DrainKey>> cache;
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    std::vector<DrainKey> incoming;
    for (int dz = -3; dz <= 3; ++dz) {
        for (int dx = -3; dx <= 3; ++dx) {
            DrainKey from{key.first + dx, key.second + dz};
            if (from != key && Downstream(from) == key) {
                incoming.push_back(from);
            }
        }
    }
    if (cache.size() > 65536) {
        cache.clear();
    }
    return cache.emplace(key, std::move(incoming)).first->second;
}

inline double Runoff(DrainKey key, int depth = 12) {
    auto   point = DrainPoint(key.first, key.second);
    auto   weather = WeatherAt(point.x, point.z);
    // Effective runoff after soil absorption and seasonal evaporation. No
    // universal baseflow: most arid channels are dry, with rare imported rivers.
    double effective=std::max(0.,weather.rain-.22-.18*weather.heat-.08*weather.season);
    double rain=3.2*effective*effective;
    if (!depth) {
        return rain;
    }
    static thread_local std::map<std::tuple<int64_t, int64_t, int>, double> cache;
    auto                                                                    id = std::tuple{key.first, key.second, depth};
    if (auto it = cache.find(id); it != cache.end()) {
        return it->second;
    }
    static SharedMemo<std::tuple<int64_t,int64_t,int>,double,131072> shared;
    double total=shared.Get(id,[&]{
        double total=rain;
        auto incoming=Tributaries(key);
        for(auto from:incoming)total+=Runoff(from,depth-1);
        return total;
    });
    if (cache.size() > 131072) {
        cache.clear();
    }
    cache.emplace(id, total);
    return total;
}

inline std::pair<double, double> RiverTangent(DrainKey key) {
    auto   center = DrainPoint(key.first, key.second), before = center;
    auto   destination = Downstream(key);
    auto   incoming = Tributaries(key);
    auto   after = DrainPoint(destination.first, destination.second);
    double largest = 0;
    for (auto from : incoming) {
        double flow = Runoff(from);
        if (flow > largest) {
            largest = flow;
            before = DrainPoint(from.first, from.second);
        }
    }
    double dx = after.x - before.x, dz = after.z - before.z, length = std::max(1., std::hypot(dx, dz));
    return {dx / length, dz / length};
}

struct RiverReach {
    std::vector<DrainNode> points;
    double                 width, endWidth;
    bool                   lake;
    double                 minX, maxX, minZ, maxZ;
};

inline RiverReach ComputeDrainReach(int64_t x,int64_t z) {
    DrainKey key{x,z};
    auto       a = DrainPoint(x, z);
    auto       destination = Downstream(key);
    auto       b = DrainPoint(destination.first, destination.second);
    RiverReach reach{};
    reach.lake = b.height == a.height;
    double runoff = Runoff(key);
    auto   channelWidth = [](double flow, const DrainNode &node) {
        double dx = (DrainSurface(node.x + 128, node.z) - DrainSurface(node.x - 128, node.z)) / 256;
        double dz = (DrainSurface(node.x, node.z + 128) - DrainSurface(node.x, node.z - 128)) / 256;
        double confinement = 1 / (1 + 3 * std::hypot(dx, dz));
        return Smooth(3.5, 12., flow) * std::min(420., 22 + 15 * std::sqrt(flow)) * std::max(.18, confinement);
    };
    reach.width = channelWidth(runoff, a);
    reach.endWidth = channelWidth(Runoff(destination), b);
    if(a.height<SeaLevel)reach.width=0;
    reach.points = DrainRouteAt(key).points;
    if (!reach.lake && reach.width > 0) {
        // Prefer a coherent curved reach at confluences when its entire water
        // profile fits the terrain. Invalid curves never replace the routed path.
        auto                   ta = RiverTangent(key), tb = RiverTangent(destination);
        double                 length = std::hypot(b.x - a.x, b.z - a.z);
        int                    count = std::max(8, int(std::ceil(length / 96)));
        std::vector<DrainNode> curve{a};
        for (int k = 1; k <= count; ++k) {
            double    t = double(k) / count, t2 = t * t, t3 = t2 * t;
            double    h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
            double    x = h00 * a.x + h10 * length * ta.first + h01 * b.x + h11 * length * tb.first;
            double    z = h00 * a.z + h10 * length * ta.second + h01 * b.z + h11 * length * tb.second;
            double    ground = DrainSurface(x, z), water = k == count ? b.height : std::min(curve.back().height, ground - 18);
            DrainNode p{x, z, water};
            if (water < b.height || ground - water > RiverMaxCut || !ChannelSegment(curve.back(), p)) {
                curve.clear();
                break;
            }
            curve.push_back(p);
        }
        if (!curve.empty()) {
            reach.points = std::move(curve);
        }

        // Round corners only where the new curve still follows real terrain.
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t k = 1; k + 1 < reach.points.size(); ++k) {
                auto      before = reach.points[k - 1], p = reach.points[k], after = reach.points[k + 1];
                DrainNode smooth{(before.x + 2 * p.x + after.x) / 4, (before.z + 2 * p.z + after.z) / 4, (before.height + 2 * p.height + after.height) / 4};
                double    ground = DrainSurface(smooth.x, smooth.z);
                if (ground - smooth.height >= 2 && ground - smooth.height <= RiverMaxCut && ChannelSegment(before, smooth) && ChannelSegment(smooth, after)) {
                    reach.points[k] = smooth;
                }
            }
        }
    }
    reach.minX = reach.maxX = a.x;
    reach.minZ = reach.maxZ = a.z;
    for (auto &p : reach.points) {
        reach.minX = std::min(reach.minX, p.x);
        reach.maxX = std::max(reach.maxX, p.x);
        reach.minZ = std::min(reach.minZ, p.z);
        reach.maxZ = std::max(reach.maxZ, p.z);
    }
    return reach;
}

inline const RiverReach &DrainReach(int64_t x,int64_t z) {
    static SharedMemo<DrainKey,RiverReach,16384> shared;
    static thread_local std::map<DrainKey,RiverReach> local;
    DrainKey key{x,z};
    if(auto it=local.find(key);it!=local.end())return it->second;
    auto reach=shared.Get(key,[&]{return ComputeDrainReach(x,z);});
    if(local.size()>4096)local.clear();
    return local.emplace(key,std::move(reach)).first->second;
}

struct RiverSample {
    double distance = 1e30, water = 0, width = 60;
    bool   lake = false;
    double riparian = 0;
    double flowX=0,flowZ=0;
};

inline const std::vector<DrainKey> &RiverCandidates(int64_t cx, int64_t cz) {
    static thread_local std::map<DrainKey, std::vector<DrainKey>> cache;
    DrainKey                                                      key{cx, cz};
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    std::vector<DrainKey> keys;
    double                x = cx * 2048., z = cz * 2048.;
    for (int dz = -6; dz <= 6; ++dz) {
        for (int dx = -6; dx <= 6; ++dx) {
            // Reject impossible intersections before recursively accumulating
            // twelve generations of tributaries. Routed points bound the path;
            // the optional Hermite curve stays in its Bezier control hull,
            // whose tangent handles extend at most chordLength/3. Smoothing
            // is convex and cannot expand that bound. Width never exceeds 420.
            const auto &route=DrainRouteAt({cx+dx,cz+dz});
            const auto &a=route.points.front(),&b=route.points.back();
            double handle=std::hypot(b.x-a.x,b.z-a.z)/3;
            double minX=std::min(a.x,b.x)-handle,maxX=std::max(a.x,b.x)+handle;
            double minZ=std::min(a.z,b.z)-handle,maxZ=std::max(a.z,b.z)+handle;
            for(auto &p:route.points){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minZ=std::min(minZ,p.z);maxZ=std::max(maxZ,p.z);}
            if(x+2048<minX-1420||x>maxX+1420||z+2048<minZ-1420||z>maxZ+1420)continue;
            const auto &r = DrainReach(cx + dx, cz + dz);
            double      margin = r.lake ? 1300 : std::max(r.width, r.endWidth) + 1000;
            if (r.width > 0 && x + 2048 >= r.minX - margin && x <= r.maxX + margin && z + 2048 >= r.minZ - margin && z <= r.maxZ + margin) {
                keys.emplace_back(cx + dx, cz + dz);
            }
        }
    }
    if (cache.size() > 8192) {
        cache.clear();
    }
    return cache.emplace(key, std::move(keys)).first->second;
}

inline RiverSample RiverAt(double x, double z, double surface = -1e30) {
    auto forestStream=foresttown::Stream(x,z);
    // The native creek owns the upstream side of the shared cross-section.
    forestStream.distance=std::max(forestStream.distance,forestStream.width+foresttown::Village.z+1600-z);
    if(forestStream.distance<forestStream.width+220 && !foresttown::RiverConnected())return {};
    if(forestStream.distance<forestStream.width+220)return {forestStream.distance,forestStream.water,forestStream.width,forestStream.lake,1-Smooth(forestStream.width,forestStream.width+150,forestStream.distance),forestStream.flowX,forestStream.flowZ};
    auto canyonSample=CanyonLandform(x,z);
    if(canyonSample.weight>=1){
        double wet=canyonSample.width>0?1-Smooth(canyonSample.width+10,canyonSample.width+100,canyonSample.channel):0;
        return {canyonSample.channel,canyonSample.water,canyonSample.width,false,wet};
    }

    RiverSample result;
    if (std::hypot(x, z) < 3000 || LandHeight(x,z)<SeaLevel-100) {
        return result;
    }
    auto spring=OasisAt(x,z);
    if(spring.radius) {
        double d=std::hypot(x-spring.x,z-spring.z);
        result={d,spring.water,spring.radius,true,(1-Smooth(spring.radius,spring.radius*2.5,d))};
    }
    auto cx = int64_t(std::floor(x / 2048)), cz = int64_t(std::floor(z / 2048));
    for (auto key : RiverCandidates(cx, cz)) {
        const auto &reach = DrainReach(key.first, key.second);
        if (reach.width == 0) {
            continue;
        }
        double margin = reach.lake ? 1300 : std::max(reach.width, reach.endWidth) + 1000;
        if (x < reach.minX - margin || x > reach.maxX + margin || z < reach.minZ - margin || z > reach.maxZ + margin) {
            continue;
        }
        for (int k = 0; k < (reach.lake ? 1 : int(reach.points.size()) - 1); k++) {
            const auto &a = reach.points[k], &b = reach.points[k + 1];
            double      vx = b.x - a.x, vz = b.z - a.z, ll = vx * vx + vz * vz;
            double      t = ll ? std::clamp(((x - a.x) * vx + (z - a.z) * vz) / ll, 0.0, 1.0) : 0;
            double      distance = std::hypot(x - a.x - vx * t, z - a.z - vz * t);
            double      width = reach.lake ? std::min(420., 100. + reach.width * 1.5) : std::lerp(reach.width, reach.endWidth, (k + t) / (reach.points.size() - 1));
            double      water = std::lerp(a.height, b.height, t);
            // Combine all nearby reaches continuously, including confluences.
            if (surface != -1e30) {
                double wet = Smooth(8, 70, width) * (1 - Smooth(0, 200 + 2 * width, std::max(0., distance - width))) * (1 - Smooth(15, 160, std::max(0., surface - water)));
                result.riparian = std::max(result.riparian, wet);
            }
            if (distance - width < result.distance - result.width) {
                double speed=reach.lake?0:std::clamp(4.+160.*std::max(0.,a.height-b.height)/std::max(1.,std::sqrt(ll)),4.,22.);
                result = {distance, water, width, reach.lake, result.riparian, ll?speed*vx/std::sqrt(ll):0,ll?speed*vz/std::sqrt(ll):0};
            }
        }
    }
    result.width*=1-canyonSample.weight;result.riparian*=1-canyonSample.weight;
    return result;
}

inline Climate ClimateAt(double x, double z, double height, double footprint) {
    auto   c = BaseClimateAt(x, z, height);
    double riparian = 0;
    // Subpixel waterways integrate out at coarse terrain LOD; no far drainage queries.
    if (footprint < 2048 && height > SeaLevel+3 && height < 8500) {
        auto r = RiverAt(x, z, height);
        riparian = r.riparian * (1 - Smooth(256, 2048, footprint));
    }
    return FinishClimate(c, height, riparian);
}

inline double NaturalHeightUncached(double x, double z, double spacing) {
    double base = LandHeight(x, z);
    base=std::lerp(base,foresttown::Elevation+12*Noise(x/700,z/700),foresttown::Forest(x,z));
    double canyonWeight=CanyonLandform(x,z).weight;
    if(canyonWeight>=1)return base; // already carved once by its own drainage
    if (spacing >= 512 || base < SeaLevel-100) {
        return base;
    }
    if (std::hypot(x, z) < 3000) {
        return base;
    }
    auto   river = RiverAt(x, z);
    double influence = (1-canyonWeight) * (1 - Smooth(river.width + 64, river.width + 220, river.distance)) * Smooth(3000, 4500, std::hypot(x, z)) * (1 - Smooth(64, 512, spacing));
    double bed = river.water - 22 + 16 * Smooth(0, river.width, river.distance);
    return std::lerp(base, std::min(base, bed), influence);
}

inline double NaturalHeight(double x, double z, double spacing = 32) {
    // Terrain triangles, vegetation and collision repeatedly sample the same
    // global vertices. Cache only immutable nature; foundations/roads stay live.
    struct Sample {
        double x = 0, z = 0, spacing = 0, height = 0;
        bool   valid = false;
    };

    static thread_local std::array<Sample, 65536> cache;
    auto                                          index = Mix(std::bit_cast<uint64_t>(x) ^ Mix(std::bit_cast<uint64_t>(z)) ^ Mix(std::bit_cast<uint64_t>(spacing))) & (cache.size() - 1);
    auto                                         &sample = cache[index];
    if (sample.valid && sample.x == x && sample.z == z && sample.spacing == spacing) {
        return sample.height;
    }
    double height = NaturalHeightUncached(x, z, spacing);
    sample = {x, z, spacing, height, true};
    return height;
}

struct Foundation {
    double x, z, halfX, halfZ, height;
    double blendRadius=400;
    bool water=false;
    bool operator==(const Foundation &) const = default;
};

inline std::vector<Foundation> &Foundations() {
    static thread_local std::vector<Foundation> data;
    return data;
}

struct Road {
    double x, z, endX, endZ, height, endHeight;
    bool operator==(const Road &) const = default;
};

inline std::vector<Road> &Roads() {
    static thread_local std::vector<Road> data;
    return data;
}

inline double Height(double x, double z, double spacing = 32) {
    double weighted = 0, weights = 0, influence = 0;
    for (const auto &p : Foundations()) {
        double d = std::hypot(std::max(0.0, std::abs(x - p.x) - p.halfX), std::max(0.0, std::abs(z - p.z) - p.halfZ));
        // A building's footprint is a hard constraint. Nearby placements may
        // grade the surroundings, but must never lift or erode its foundation.
        if (d < .00001) {
            return authoredwater::Carve(x,z,p.height,spacing);
        }
        if (d >= p.blendRadius) {
            continue;
        }
        double t = d / p.blendRadius;
        t = t * t * (3 - 2 * t);
        double w = (1 - t) / (d * d);
        weighted += w * p.height;
        weights += w;
        influence = std::max(influence, 1 - t);
    }
    double natural=authoredwater::FitShell(x,z,foresttown::Grade(x,z,NaturalHeight(x,z,spacing)));
    double base=weights>0?std::lerp(natural,weighted/weights,influence):natural;
    double roadSum = 0, roadWeight = 0, nearest = 1e30;
    for (const auto &r : Roads()) {
        double dx = r.endX - r.x, dz = r.endZ - r.z, length = dx * dx + dz * dz;
        double t = length ? std::clamp(((x - r.x) * dx + (z - r.z) * dz) / length, 0.0, 1.0) : 0;
        double d = std::hypot(x - r.x - t * dx, z - r.z - t * dz);
        if (d >= 200) {
            continue;
        }
        double w = std::pow(1 - d / 200, 2) / (1 + d * d);
        roadSum += w * std::lerp(r.height, r.endHeight, t);
        roadWeight += w;
        nearest = std::min(nearest, d);
    }
    if (roadWeight) {
        double t = std::clamp((nearest - 55) / 145, 0.0, 1.0);
        t = t * t * (3 - 2 * t);
        base = std::lerp(roadSum / roadWeight, base, t);
    }
    auto stream=foresttown::Stream(x,z);
    double wet=1-Smooth(stream.width+24,stream.width+90,stream.distance);
    base=std::lerp(base,std::min(base,stream.water-24),wet);
    return authoredwater::Carve(x,z,base,spacing);
}

// Surface of the actual finest terrain triangles, also used by draped roads.
inline double SurfaceHeight(double x, double z, double spacing = 32) {
    double sx = std::floor(x / 32) * 32, sz = std::floor(z / 32) * 32, u = (x - sx) / 32, v = (z - sz) / 32;
    double b = Height(sx + 32, sz, spacing), c = Height(sx, sz + 32, spacing);
    return u + v <= 1 ? Height(sx, sz, spacing) * (1 - u - v) + b * u + c * v : b * (1 - v) + c * (1 - u) + Height(sx + 32, sz + 32, spacing) * (u + v - 1);
}

// River carving only lowers nature; foundations and roads blend monotonically.
// Skipping carving at the same triangle vertices is therefore an upper bound
// on walking collision, without generating drainage networks on the game thread.
inline double FlightSurfaceHeight(double x, double z) {
    return SurfaceHeight(x, z, 512);
}

inline double Distance(const Tile &t, double x, double z) {
    return std::hypot(std::max({t.left() - x, 0.0, x - t.left() - t.size()}),
                      std::max({t.top() - z, 0.0, z - t.top() - t.size()}));
}

// Nested square rings, snapped to pairs of tiles so the previous ring's
// boundary always lies on the next ring's grid. Each ring is 8x8 minus a
// 4x4 hole: exactly 48 tiles; the innermost ring has 64. No view direction,
// camera height or terrain noise participates in residency.
inline std::vector<Tile> Select(double x, double z, int minimumLevel = 0) {
    minimumLevel=std::clamp(minimumLevel,0,MaxLevel);
    std::vector<Tile> out;
    out.reserve(RingTileCount+(MaxLevel-minimumLevel)*RingBandCount);
    int64_t previousX=0,previousZ=0;
    for(int level=minimumLevel;level<=MaxLevel;++level) {
        double size=std::ldexp(BaseSize,level);
        int64_t left=int64_t(std::floor(x/(2*size)))*2-RingWidth/2;
        int64_t top=int64_t(std::floor(z/(2*size)))*2-RingWidth/2;
        for(int dz=0;dz<RingWidth;++dz)for(int dx=0;dx<RingWidth;++dx){
            int64_t tx=left+dx,tz=top+dz;
            if(level>minimumLevel && tx>=previousX/2 && tx<(previousX+RingWidth)/2 &&
                tz>=previousZ/2 && tz<(previousZ+RingWidth)/2)continue;
            out.push_back({tx,tz,level});
        }
        previousX=left;previousZ=top;
    }
    return out;
}

using Selection = std::map<Tile, int>;

inline int LevelAt(const Selection &selection, double x, double z) {
    for (int l = 0; l <= MaxLevel; l++) {
        double s = std::ldexp(BaseSize, l);
        if (selection.contains({int64_t(std::floor(x / s)), int64_t(std::floor(z / s)), l})) {
            return l;
        }
    }
    return -1;
}

inline double EdgeHeight(double x, double z, int level, bool alongX) {
    double step = std::ldexp(BaseSize, level) / Resolution;
    double p = alongX ? x : z, lo = std::floor(p / step) * step;
    return std::lerp(Height(alongX ? lo : x, alongX ? z : lo, step),
                     Height(alongX ? lo + step : x, alongX ? z : lo + step, step), (p - lo) / step);
}
} // namespace terrain
