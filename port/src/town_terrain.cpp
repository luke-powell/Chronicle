#include "world_origin.hpp"
#include "dran_flight.hpp"
#include "town_terrain.hpp"
#include "town_biomes.hpp"
#include "gfx/capture.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <set>
#include <optional>

#include "camerafollow.hpp"
#include "character.hpp"
#include "collision.hpp"
#include "draw3d.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editmapscript.hpp"
#include "framevu1.hpp"
#include "gamemode.hpp"
#include "gfx/tim2.hpp"
#include "mglib.hpp"
#include "platform/paths.hpp"
#include "renderinfo.hpp"
#include "terrain_kernel.hpp"
#include "terrain_publication.hpp"
#include "texture.hpp"
#include "visualvu1.hpp"
#include "world_georama.hpp"

#include "town_canyon_join.hpp"

namespace {
struct JoinLayer {gfx::MeshHandle mesh=0;std::vector<gfx::Vertex3D> vertices;std::vector<uint32_t> indices;};
struct Chunk {
    double profileBuilt=0,profileReady=0;
    std::array<float,(terrain::Resolution+1)*(terrain::Resolution+1)> surface{};
    gfx::MeshHandle    mesh = 0;
    gfx::MeshHandle    snow = 0;
    gfx::MeshHandle hill = 0, canyonFloor=0, canyonRock=0;
    std::array<int, 4> edges{};
    float              low = 0, high = 0;
    BiomeChunk         biome;
    unsigned indexCount=0;
    std::vector<gfx::Vertex3D> vertices, snowVertices, hillVertices, canyonFloorVertices, canyonRockVertices;
    std::vector<uint32_t> indices;
    std::map<std::string,JoinLayer> joinLayers;
};

using MeshKey = std::pair<terrain::Tile, std::array<int, 4>>;

struct CachedChunk {
    Chunk    chunk;
    unsigned used = 0;
    bool dirty = false;
};

std::vector<CCPoly> nativeTerrainPolys;
std::map<MeshKey, CachedChunk> cache;
std::vector<MeshKey>           active, pending;
std::vector<MeshKey>           localPending;
std::map<MeshKey,double> profileRequests;
double localPlanX=0,localPlanZ=0;
std::vector<terrain::Publication<MeshKey>> publications;
size_t                         pendingCursor = 0;
unsigned                       cacheClock = 0;
double                         lastX = 1e30, lastZ = 1e30;
int                            lastLevel = -1;
bool walkRequested=false;
unsigned terrainRevision=0;
std::vector<terrain::Foundation> previousFoundations;
std::vector<terrain::Road> previousRoads;

// Persistent bounded workers retain their numerical caches during all travel.
// GPU uploads and coverage publication remain on the render thread.
class TerrainWorker {
    std::mutex mutex;
    std::condition_variable wake;
    std::function<void()> task;
    bool stopping=false;
    std::thread thread;
public:
    TerrainWorker():thread([this] {
        for (;;) {
            std::function<void()> next;
            { std::unique_lock lock(mutex); wake.wait(lock,[&]{return stopping || bool(task);});
              if(stopping)return; next=std::move(task); task={}; }
            next();
        }
    }) {}
    ~TerrainWorker(){ {std::lock_guard lock(mutex);stopping=true;} wake.notify_one();thread.join(); }
    std::future<Chunk> Submit(MeshKey key);
};
constexpr size_t WorkerCapacity=8;
struct Flight {
    std::unique_ptr<TerrainWorker> worker;
    std::future<Chunk> building;
    std::optional<Chunk> uploading;
    MeshKey key;
    unsigned revision=0;
};
std::array<Flight,WorkerCapacity> flights;
size_t WorkerCount(){return std::clamp(size_t(std::thread::hardware_concurrency()/2),size_t(1),WorkerCapacity);}
double walkX=0,walkZ=0;

void Destroy(Chunk &c) {
    gfx::DestroyMesh(c.mesh);gfx::DestroyMesh(c.snow);gfx::DestroyMesh(c.hill);gfx::DestroyMesh(c.canyonFloor);gfx::DestroyMesh(c.canyonRock);for(auto &[name,layer]:c.joinLayers)gfx::DestroyMesh(layer.mesh);BiomeDestroy(c.biome);
}
bool Upload(Chunk &c, std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::time_point::max()) {
    if(!c.mesh && !c.indices.empty())c.mesh=gfx::CreateMesh(c.vertices,c.indices);
    if(!c.snow && !c.indices.empty() && !c.snowVertices.empty())c.snow=gfx::CreateMesh(c.snowVertices,c.indices);
    if(!c.hill && !c.indices.empty() && !c.hillVertices.empty())c.hill=gfx::CreateMesh(c.hillVertices,c.indices);
    if(!c.canyonFloor&&!c.indices.empty()&&!c.canyonFloorVertices.empty())c.canyonFloor=gfx::CreateMesh(c.canyonFloorVertices,c.indices);
    if(!c.canyonRock&&!c.indices.empty()&&!c.canyonRockVertices.empty())c.canyonRock=gfx::CreateMesh(c.canyonRockVertices,c.indices);
    for(auto &[name,layer]:c.joinLayers)if(!layer.mesh&&!layer.indices.empty())layer.mesh=gfx::CreateMesh(layer.vertices,layer.indices);
    if(!BiomeUpload(c.biome,deadline))return false;
    // Keep the submitted terrain geometry for collision; no second heightfield.
    // Vertex/index storage is bounded by the resident chunk cache.
    std::vector<gfx::Vertex3D>().swap(c.snowVertices);
    std::vector<gfx::Vertex3D>().swap(c.hillVertices);
    std::vector<gfx::Vertex3D>().swap(c.canyonFloorVertices);std::vector<gfx::Vertex3D>().swap(c.canyonRockVertices);
    return true;
}


void ClearChunks() {
    ++terrainRevision;
    for(auto &f:flights){
        // CPU jobs own their input snapshot. Let stale results finish in the
        // background instead of blocking a door/map transition on a far forest.
        if(f.uploading){Destroy(*f.uploading);f.uploading.reset();}
    }
    for (auto &[key, value] : cache) {
        Destroy(value.chunk);
    }
    cache.clear();profileRequests.clear();
    active.clear();
    pending.clear();
    publications.clear();
    localPending.clear();
    pendingCursor = 0;
    lastX = lastZ = 1e30;
    lastLevel = -1;
}

void Prepare(CEditGround *ground) {
    auto &gate=ground->fixed_parts[2];
    CFrame *gateRoots[]={gate.frame[0],gate.frame[1],gate.frame[2],gate.frame[3],gate.collision_frame,gate.camera_frame,gate.shadow_frame};
    for(auto *root:gateRoots)if(root){
        if(auto *f=root->SearchFrame(const_cast<char*>("mon1"))){f->SetRotType(2);f->SetRotation(0,1.8f,0);}
        if(auto *f=root->SearchFrame(const_cast<char*>("mon2"))){f->SetRotType(2);f->SetRotation(0,-1.8f,0);}
    }
    static bool dumped=false;
    if(const char *path=std::getenv("DC_TOWN_BOUNDARY_OBJ");path&&!dumped){
        dumped=true;std::ofstream obj(path);size_t offset=1;
        auto dump=[&](auto &&self,CFrame *f)->void{
            for(;f;f=f->brother){
                auto *visual=static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(f)->visual);
                auto *source=visual?Draw3DFindVisual(visual->vu_data):nullptr;
                if(source){obj<<"o "<<f->name<<'\n';
                    obj<<"# vertex_colour "<<source->vertex_colour<<'\n';
                    for(auto &v:source->vertices)obj<<"v "<<v.position[0]<<' '<<v.position[1]<<' '<<v.position[2]<<'\n';
                    for(auto &v:source->vertices)obj<<"vt "<<v.uv[0]<<' '<<v.uv[1]<<'\n';
                    for(auto &v:source->vertices)obj<<"vn "<<v.normal[0]<<' '<<v.normal[1]<<' '<<v.normal[2]<<'\n';
                    for(auto &s:source->strips){obj<<"# diffuse "<<s.diffuse[0]<<' '<<s.diffuse[1]<<' '<<s.diffuse[2]<<'\n';obj<<"usemtl "<<s.texture_name<<'\n';for(unsigned i=s.first_index;i+2<s.first_index+s.index_count;i+=3)obj<<"f "<<offset+source->indices[i]<<' '<<offset+source->indices[i+1]<<' '<<offset+source->indices[i+2]<<'\n';}
                    offset+=source->vertices.size();
                }self(self,f->child);
            }
        };dump(dump,ground->fixed_parts[0].frame[0]);
    }
    // Preserve the complete retail walls, floor and platform, including UVs.
    // draw_on=0 suppresses the visual; bit 1 is required to actually draw it.
    constexpr int side=641;
    std::vector<float> raster;
    const bool buildShell=!canyon::TownShell().ready;
    if(buildShell){raster.resize(side*side,0);nativeTerrainPolys.clear();}
    auto walk = [&](auto &&self, CFrame *frame) -> void {
        for (; frame; frame = frame->brother) {
            // Retail's rectangular underside/skirt rises to y=40 and cuts
            // across the new west outlet at x=-1207.5. The open world supplies
            // the surrounding ground; retain the authored waku cliffs only.
            if(std::strcmp(frame->name,"cube2__f")==0)frame->attr.draw_on=0;
            if(std::strncmp(frame->name,"waku",4)==0){
                frame->attr.draw_on=1;
                auto *visual=static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(frame)->visual);
                auto *source=visual?Draw3DFindVisual(visual->vu_data):nullptr;
                if(source && buildShell)for(size_t i=0;i+2<source->indices.size();i+=3){
                    CCPoly poly{};
                    for(int k=0;k<3;k++){auto &v=source->vertices[source->indices[i+k]];std::copy_n(v.position,3,poly.vertex[k]);poly.vertex[k][3]=1;}
                    auto *a=poly.vertex[0],*b=poly.vertex[1],*c=poly.vertex[2];
                    double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
                    double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
                    double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
                    double len=std::sqrt(nx*nx+ny*ny+nz*nz);if(len>1e-8){poly.normal[0]=nx/len;poly.normal[1]=ny/len;poly.normal[2]=nz/len;nativeTerrainPolys.push_back(poly);}
                }
                if(source && buildShell)for(size_t i=0;i+2<source->indices.size();i+=3){
                    auto *a=source->vertices[source->indices[i]].position,*b=source->vertices[source->indices[i+1]].position,*c=source->vertices[source->indices[i+2]].position;
                    double den=(b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2]);if(std::abs(den)<1e-6)continue;
                    int x0=std::clamp(int(std::floor((std::min({a[0],b[0],c[0]})+1280)/4)),0,side-1),x1=std::clamp(int(std::ceil((std::max({a[0],b[0],c[0]})+1280)/4)),0,side-1);
                    int z0=std::clamp(int(std::floor((std::min({a[2],b[2],c[2]})+1280)/4)),0,side-1),z1=std::clamp(int(std::ceil((std::max({a[2],b[2],c[2]})+1280)/4)),0,side-1);
                    for(int z=z0;z<=z1;z++)for(int x=x0;x<=x1;x++){
                        double wx=x*4-1280,wz=z*4-1280;
                        double u=((b[2]-c[2])*(wx-c[0])+(c[0]-b[0])*(wz-c[2]))/den;
                        double v=((c[2]-a[2])*(wx-c[0])+(a[0]-c[0])*(wz-c[2]))/den;
                        if(u>=0&&v>=0&&u+v<=1)raster[z*side+x]=std::max(raster[z*side+x],float(u*a[1]+v*b[1]+(1-u-v)*c[1]));
                    }
                }
            }
            self(self, frame->child);
        }
    };
    walk(walk, ground->fixed_parts[0].frame[0]);
    if(buildShell){
        auto sample=[&](double x,double z){
            double u=(x+1280)/4,v=(z+1280)/4;int ix=int(std::floor(u)),iz=int(std::floor(v));
            if(ix<0||iz<0||ix>=side-1||iz>=side-1)return 0.;u-=ix;v-=iz;
            return std::lerp(std::lerp(double(raster[iz*side+ix]),double(raster[iz*side+ix+1]),u),std::lerp(double(raster[(iz+1)*side+ix]),double(raster[(iz+1)*side+ix+1]),u),v);
        };
        for(int a=0;a<canyon::ShellAngles;a++){
            double angle=a*6.283185307179586/canyon::ShellAngles,top=0;
            for(int r=0;r<canyon::ShellRadii;r++){
                double native=sample(std::cos(angle)*r*8,std::sin(angle)*r*8);
                if(native>top){top=native;canyon::TownShell().ridge[a]=float(r*8);}
                canyon::TownShell().heights[a*canyon::ShellRadii+r]=float(top);
            }
        }
        for(auto &radius:canyon::TownShell().ridge)if(radius<700)radius=1200;
        canyon::TownShell().ready=true;
        std::fprintf(stderr,"Norune shell: original cliff profile captured; west outlet -1200,-150\n");
    }
    if(!std::getenv("DC_TEST_SKIP_TERRAIN_DRAW")){
        std::vector<Draw3DVisual*> sources;
        auto collect=[&](auto &&self,CFrame *f)->void{
            for(;f;f=f->brother){
                if(std::strncmp(f->name,"waku",4)==0){auto *v=static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(f)->visual);if(auto *source=v?Draw3DFindVisual(v->vu_data):nullptr)sources.push_back(source);}
                self(self,f->child);
            }
        };collect(collect,ground->fixed_parts[0].frame[0]);
        bool fresh=std::any_of(sources.begin(),sources.end(),[](auto *s){return !s->native_terrain;});
        canyon_join::Build(sources);
        if(fresh){
            nativeTerrainPolys.clear();
            for(auto *source:sources)for(size_t i=0;i+2<source->indices.size();i+=3){
                CCPoly p{};for(int k=0;k<3;k++){std::copy_n(source->vertices[source->indices[i+k]].position,3,p.vertex[k]);p.vertex[k][3]=1;}
                auto *a=p.vertex[0],*b=p.vertex[1],*c=p.vertex[2];
                double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]),ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]),nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
                double length=std::sqrt(nx*nx+ny*ny+nz*nz);if(length<1e-8)continue;
                p.normal[0]=nx/length;p.normal[1]=ny/length;p.normal[2]=nz/length;nativeTerrainPolys.push_back(p);
            }
        }
    }
}

std::array<int, 4> Edges(const terrain::Tile &t, const terrain::Selection &s) {
    double x = t.left(), z = t.top(), w = t.size();
    std::array<int,4> edges={terrain::LevelAt(s, x - 1, z + w / 2), terrain::LevelAt(s, x + w + 1, z + w / 2),
            terrain::LevelAt(s, x + w / 2, z - 1), terrain::LevelAt(s, x + w / 2, z + w + 1)};
    // Build only stitches to coarser neighbors. Missing, equal, and finer
    // neighbors all generate identical geometry; share that cache entry.
    for(auto &level:edges)if(level<=t.level)level=t.level;
    return edges;
}

Chunk Build(const terrain::Tile &t, std::array<int, 4> edges) {
    gfx::capture::TileScope profileTile(t.x,t.z,t.level);
    gfx::capture::Scope jobProfile("world.build.total");
    gfx::capture::Scope terrainProfile("world.build.height_mesh");
    auto started=std::chrono::steady_clock::now();
    const int n=terrain::MeshResolution(t.level);
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    std::vector<gfx::Vertex3D> snowVertices;
    bool                       hasSnow = false;
    float                      low = 1e30f, high = -1e30f;
    double                     step = t.size() / n;
    for (int z = 0; z <= n; z++) {
        for (int x = 0; x <= n; x++) {
            double wx = t.left() + x * step, wz = t.top() + z * step;
            double h = terrain::Height(wx, wz,step);
            if (x == 0 && edges[0]>=0 && terrain::MeshSampleLevel(edges[0])>terrain::MeshSampleLevel(t.level)) {
                h = terrain::EdgeHeight(wx, wz, terrain::MeshSampleLevel(edges[0]), false);
            }
            if (x == n && edges[1]>=0 && terrain::MeshSampleLevel(edges[1])>terrain::MeshSampleLevel(t.level)) {
                h = terrain::EdgeHeight(wx, wz, terrain::MeshSampleLevel(edges[1]), false);
            }
            if (z == 0 && edges[2]>=0 && terrain::MeshSampleLevel(edges[2])>terrain::MeshSampleLevel(t.level)) {
                h = terrain::EdgeHeight(wx, wz, terrain::MeshSampleLevel(edges[2]), true);
            }
            if (z == n && edges[3]>=0 && terrain::MeshSampleLevel(edges[3])>terrain::MeshSampleLevel(t.level)) {
                h = terrain::EdgeHeight(wx, wz, terrain::MeshSampleLevel(edges[3]), true);
            }
            // A coarse mesh cannot resolve centimetre-scale lighting detail.
            // Sample normals at its footprint to avoid striped distant faces.
            double normalStep=std::max(8.,step*.5),ny=normalStep*2;
            double        nx = terrain::Height(wx - normalStep, wz,step) - terrain::Height(wx + normalStep, wz,step);
            double        nz = terrain::Height(wx, wz - normalStep,step) - terrain::Height(wx, wz + normalStep,step);
            double        length = std::sqrt(nx * nx + ny*ny + nz * nz);
            gfx::Vertex3D v{};
            v.position[0] = float(x * step);
            v.position[1] = float(h) - 0.04f;
            low = std::min(low, v.position[1]);
            high = std::max(high, v.position[1]);
            v.position[2] = float(z * step);
            v.normal[0] = float(nx / length);
            v.normal[1] = float(ny / length);
            v.normal[2] = float(nz / length);
            v.uv[0] = float((std::fmod(t.left(), 200) + x * step) / 200);
            v.uv[1] = float((std::fmod(t.top(), 200) + z * step) / 200);
            double slope = 1 - ny / length;
            double rock = terrain::RockExposure(wx,wz,h,ny/length);
            double snow = terrain::FinishClimate(terrain::BaseClimateAt(wx,wz,h),h).snow;
            snow *= std::clamp((.8 - slope) * 2.5, 0.0, 1.0)*(1-terrain::CanyonLandform(wx,wz).weight);
            v.color[0] = v.color[1] = v.color[2] = 128;
            v.color[3] = uint8_t(128 * rock);
            auto snowVertex = v;
            snowVertex.color[3] = uint8_t(128 * snow);
            hasSnow |= snow > .001;
            snowVertices.push_back(snowVertex);
            vertices.push_back(v);
        }
    }
    for (int z = 0; z < n; z++) {
        for (int x = 0; x < n; x++) {
            unsigned a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            double wx=t.left()+(x+.5)*step,wz=t.top()+(z+.5)*step;
            if(t.level<=1&&terrain::CanyonLandform(wx,wz).weight>.999999&&canyon::FacetFlip(wx,wz))
                indices.insert(indices.end(),{a,c,d,a,d,b});
            else indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    Chunk result;
    result.edges=edges;result.low=low;result.high=std::max(high+1000,float(terrain::SeaLevel+20));
    terrainProfile.End();
    auto terrainDone=std::chrono::steady_clock::now();
    if(!std::getenv("DC_TERRAIN_DIAGNOSTIC"))result.biome=BiomeBuild(t,vertices,indices);
    if(std::getenv("DC_TERRAIN_BUILD_PROFILE"))std::fprintf(stderr,"terrain build: level %d terrain %.3f biome %.3f\n",t.level,
        std::chrono::duration<double,std::milli>(terrainDone-started).count(),
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-terrainDone).count());
    bool contourTile=t.level<=1;
    for(int z=0;z<=n&&contourTile;z++)for(int x=0;x<=n;x++){
        double wx=t.left()+x*step,wz=t.top()+z*step;
        if(std::max(std::abs(wx),std::abs(wz))<=canyon::JoinExtent+512 || terrain::CanyonLandform(wx,wz).weight<.999999
            || std::abs(terrain::Height(wx,wz,step)-terrain::LandHeight(wx,wz))>.01){contourTile=false;break;}
    }
    // Cut triangles at the floor, shoulder and plateau contours. Between cuts
    // the shared distance field and profile are affine, so broad triangles
    // represent the surface exactly, rather than stretching tiny grid faces.
    if(t.level<=1){
        if(contourTile){
            indices.clear();
            const int stride=std::max(1,int(canyon::FacetSize/step));
            for(int z=0;z<n;z+=stride)for(int x=0;x<n;x+=stride){
                unsigned a=z*(n+1)+x,b=a+stride,c=a+stride*(n+1),d=c+stride;
                if(canyon::FacetFlip(t.left()+(x+stride*.5)*step,t.top()+(z+stride*.5)*step))
                    indices.insert(indices.end(),{a,c,d,a,d,b});
                else indices.insert(indices.end(),{a,c,b,b,c,d});
            }
        }
        struct ContourVertex {gfx::Vertex3D v;double distance,relief;};
        auto previous=std::move(indices);indices.clear();
        constexpr auto cuts=[] {std::array<double,canyon::ProfileCuts.size()+2> c{};c.front()=-1e30;c.back()=1e30;for(size_t i=0;i<canyon::ProfileCuts.size();i++)c[i+1]=canyon::ProfileCuts[i];return c;}();
        for(size_t i=0;i+2<previous.size();i+=3){
            std::vector<ContourVertex> tri;
            bool eligible=true;
            for(int k=0;k<3;k++){
                auto v=vertices[previous[i+k]];double wx=t.left()+v.position[0],wz=t.top()+v.position[2];
                eligible &= terrain::CanyonLandform(wx,wz).weight>.999999
                    && std::abs(terrain::Height(wx,wz,step)-terrain::LandHeight(wx,wz))<.01;
                tri.push_back({v,canyon::WallDistance(wx,wz),canyon::CrownRelief(wx,wz)});
            }
            if(!eligible){indices.insert(indices.end(),previous.begin()+i,previous.begin()+i+3);continue;}
            auto clip=[](const std::vector<ContourVertex>&input,double cut,bool above){
                std::vector<ContourVertex> out;
                for(size_t k=0;k<input.size();k++){
                    auto a=input[k],b=input[(k+1)%input.size()];
                    bool ai=above?a.distance>=cut:a.distance<=cut,bi=above?b.distance>=cut:b.distance<=cut;
                    if(ai)out.push_back(a);
                    if(ai!=bi){
                        double f=(cut-a.distance)/(b.distance-a.distance);auto v=a;
                        for(int j=0;j<3;j++)v.v.position[j]=float(std::lerp(double(a.v.position[j]),double(b.v.position[j]),f));
                        for(int j=0;j<2;j++)v.v.uv[j]=float(std::lerp(double(a.v.uv[j]),double(b.v.uv[j]),f));
                        v.distance=cut;v.relief=std::lerp(a.relief,b.relief,f);out.push_back(v);
                    }
                }
                return out;
            };
            for(size_t band=0;band+1<cuts.size();band++){
                auto region=clip(clip(tri,cuts[band],true),cuts[band+1],false);
                auto reliefClip=[](const std::vector<ContourVertex>&input,bool upper,bool above){
                    std::vector<ContourVertex> out;
                    auto distance=[&](const ContourVertex&p){double range=std::max(0.,(canyon::Profile(p.distance)-105)*.4);return p.relief+(upper?-range:range);};
                    for(size_t i=0;i<input.size();i++){
                        auto a=input[i],b=input[(i+1)%input.size()];double da=distance(a),db=distance(b);bool ai=above?da>=0:da<=0,bi=above?db>=0:db<=0;
                        if(ai)out.push_back(a);
                        if(ai!=bi){double t=da/(da-db);out.push_back({canyon_join::Mix(a.v,b.v,t),std::lerp(a.distance,b.distance,t),std::lerp(a.relief,b.relief,t)});}
                    }return out;
                };
                std::vector<std::vector<ContourVertex>> regions;
                if(band<=1)regions.push_back(region);
                else {
                    regions.push_back(reliefClip(region,false,false));
                    regions.push_back(reliefClip(reliefClip(region,false,true),true,false));
                    regions.push_back(reliefClip(region,true,true));
                }
                for(auto &polygon:regions){
                if(polygon.size()<3)continue;
                unsigned start=unsigned(vertices.size());
                for(auto p:polygon){
                    auto v=p.v;double wx=t.left()+v.position[0],wz=t.top()+v.position[2];
                    double h=terrain::Height(wx,wz,step);
                    v.position[1]=float(h)-.04f;
                    // A stitched border follows the actual coarser edge, even
                    // where a new contour vertex lands between its endpoints.
                    if(v.position[0]==0&&edges[0]>t.level)v.position[1]=float(terrain::EdgeHeight(wx,wz,edges[0],false))-.04f;
                    if(v.position[0]==t.size()&&edges[1]>t.level)v.position[1]=float(terrain::EdgeHeight(wx,wz,edges[1],false))-.04f;
                    if(v.position[2]==0&&edges[2]>t.level)v.position[1]=float(terrain::EdgeHeight(wx,wz,edges[2],true))-.04f;
                    if(v.position[2]==t.size()&&edges[3]>t.level)v.position[1]=float(terrain::EdgeHeight(wx,wz,edges[3],true))-.04f;
                    // Use the same normal on either side of every upper contour.
                    double gradient=band==0?0:canyon::ProfileGradient(p.distance);
                    double nx=(canyon::WallDistance(wx-24,wz)-canyon::WallDistance(wx+24,wz))*gradient;
                    double nz=(canyon::WallDistance(wx,wz-24)-canyon::WallDistance(wx,wz+24))*gradient;
                    double reliefWeight=terrain::Smooth(105,180,h);
                    nx+=(canyon::CrownRelief(wx-24,wz)-canyon::CrownRelief(wx+24,wz))*reliefWeight;
                    nz+=(canyon::CrownRelief(wx,wz-24)-canyon::CrownRelief(wx,wz+24))*reliefWeight;
                    double length=std::sqrt(nx*nx+48*48+nz*nz);
                    v.normal[0]=float(nx/length);v.normal[1]=float(48/length);v.normal[2]=float(nz/length);
                    if(band==0&&wx<-1000&&wx>-1500&&wz>-250&&wz<-50){
                        auto normal=canyon::FloorNormal(wx,wz);std::copy_n(normal.data(),3,v.normal);
                    }
                    v.color[3]=band>0?128:0;
                    vertices.push_back(v);v.color[3]=0;snowVertices.push_back(v);
                }
                // Contour intersections form convex polygons. Pick the fan
                // with the healthiest triangles instead of always creating a
                // sliver at the first clipped corner.
                unsigned root=0;double best=-1;
                for(unsigned candidate=0;candidate<polygon.size();candidate++){
                    double worst=1,sum=0;
                    for(unsigned k=1;k+1<polygon.size();k++){
                        auto *a=vertices[start+candidate].position,*b=vertices[start+(candidate+k)%polygon.size()].position,*c=vertices[start+(candidate+k+1)%polygon.size()].position;
                        double ab[3],ac[3],cross[3],length=0;
                        for(int j=0;j<3;j++){ab[j]=b[j]-a[j];ac[j]=c[j]-a[j];length+=ab[j]*ab[j]+ac[j]*ac[j]+(b[j]-c[j])*(b[j]-c[j]);}
                        cross[0]=ab[1]*ac[2]-ab[2]*ac[1];cross[1]=ab[2]*ac[0]-ab[0]*ac[2];cross[2]=ab[0]*ac[1]-ab[1]*ac[0];
                        double quality=length?3.464101615*std::sqrt(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2])/length:0;
                        worst=std::min(worst,quality);sum+=quality;
                    }
                    double score=worst+sum/(polygon.size()-2);if(score>best){best=score;root=candidate;}
                }
                for(unsigned k=1;k+1<polygon.size();k++)indices.insert(indices.end(),{start+root,start+unsigned((root+k)%polygon.size()),start+unsigned((root+k+1)%polygon.size())});
                }
            }
        }
    }
    // Replace the entire inner square with the conforming native collar.
    // Its outer edge is exactly on the terrain grid, at every mesh LOD.
    if(canyon::TownJoin().ready&&t.left()<canyon::JoinExtent&&t.left()+t.size()>-canyon::JoinExtent&&t.top()<canyon::JoinExtent&&t.top()+t.size()>-canyon::JoinExtent){
        auto clip=[](std::vector<gfx::Vertex3D> polygon,int axis,double cut,bool above){
            std::vector<gfx::Vertex3D> out;
            for(size_t i=0;i<polygon.size();i++){
                auto a=polygon[i],b=polygon[(i+1)%polygon.size()];double da=a.position[axis]-cut,db=b.position[axis]-cut;
                bool ai=above?da>=0:da<=0,bi=above?db>=0:db<=0;
                if(ai)out.push_back(a);if(ai!=bi)out.push_back(canyon_join::Mix(a,b,da/(da-db)));
            }return out;
        };
        // Chunk edges at levels 0..3 align with the square. Coarser triangles
        // are split against its four half-planes, so no old ground shows through.
        auto previous=std::move(indices);indices.clear();
        for(size_t i=0;i+2<previous.size();i+=3){
            std::vector<gfx::Vertex3D> inside;
            for(int k=0;k<3;k++){auto v=vertices[previous[i+k]];v.position[0]+=float(t.left());v.position[2]+=float(t.top());inside.push_back(v);}
            for(auto [axis,cut,above]:{std::tuple{0,-canyon::JoinExtent,true},std::tuple{0,canyon::JoinExtent,false},std::tuple{2,-canyon::JoinExtent,true},std::tuple{2,canyon::JoinExtent,false}}){
                auto outside=clip(inside,axis,cut,!above);inside=clip(inside,axis,cut,above);
                unsigned start=unsigned(vertices.size());for(auto v:outside){v.position[0]-=float(t.left());v.position[2]-=float(t.top());vertices.push_back(v);auto snow=v;snow.color[3]=0;snowVertices.push_back(snow);}
                for(unsigned k=1;k+1<outside.size();k++)indices.insert(indices.end(),{start,start+k,start+k+1});
            }
        }
        for(const auto &face:canyon_join::faces){
            std::vector<gfx::Vertex3D> polygon(face.v.begin(),face.v.end());
            polygon=clip(clip(clip(clip(std::move(polygon),0,t.left(),true),0,t.left()+t.size(),false),2,t.top(),true),2,t.top()+t.size(),false);
            if(polygon.size()<3)continue;
            unsigned start=unsigned(vertices.size());
            bool overlayUsed=std::any_of(polygon.begin(),polygon.end(),[](const auto &v){return v.color[3]>0;});
            JoinLayer *layer=overlayUsed?&result.joinLayers[face.material]:nullptr;unsigned overlayStart=layer?unsigned(layer->vertices.size()):0;
            for(auto v:polygon){
                double wx=v.position[0],wz=v.position[2];
                v.position[1]+=float(terrain::Height(wx,wz,step)-terrain::LandHeight(wx,wz));
                v.position[1]-=.04f*(1-v.color[3]/128.f);
                v.position[0]-=float(t.left());v.position[2]-=float(t.top());
                if(layer)layer->vertices.push_back(v);
                v.color[3]=v.position[1]>.1f?128:0;
                vertices.push_back(v);v.color[3]=0;snowVertices.push_back(v);
            }
            for(unsigned k=1;k+1<polygon.size();k++){
                indices.insert(indices.end(),{start,start+k,start+k+1});
                if(layer)layer->indices.insert(layer->indices.end(),{overlayStart,overlayStart+k,overlayStart+k+1});
            }
        }
    }
    // Clipping can coincide with an existing edge/vertex. Do not submit the
    // resulting zero-area faces to rendering or collision.
    size_t kept=0;
    for(size_t i=0;i+2<indices.size();i+=3){
        auto *a=vertices[indices[i]].position,*b=vertices[indices[i+1]].position,*c=vertices[indices[i+2]].position;
        double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        double ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        double nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        if(nx*nx+ny*ny+nz*nz>1e-12){indices[kept++]=indices[i];indices[kept++]=indices[i+1];indices[kept++]=indices[i+2];}
    }
    indices.resize(kept);
    result.indexCount=unsigned(indices.size());
    for(size_t i=0;i<result.surface.size();++i)result.surface[i]=vertices[i].position[1];
    bool hillUsed=false,nativeUsed=false;
    for(auto &baseVertex:vertices){
        auto v=baseVertex;
        auto c=terrain::CanyonLandform(t.left()+v.position[0],t.top()+v.position[2]);
        double localHeight=v.position[1]-(c.height-canyon::At(t.left()+v.position[0],t.top()+v.position[2]).height);
        v.color[3]=uint8_t(128*c.weight);result.canyonFloorVertices.push_back(v);
        v.color[3]=uint8_t(baseVertex.color[3]*c.weight);result.canyonRockVertices.push_back(v);
        baseVertex.color[3]=uint8_t(baseVertex.color[3]*(1-c.weight));nativeUsed|=c.weight>.005;
        double mask=c.weight*terrain::Smooth(80,135,localHeight);
        v.color[3]=uint8_t(128*mask);hillUsed|=mask>.005;result.hillVertices.push_back(v);
    }
    if(!hillUsed)result.hillVertices.clear();
    if(!nativeUsed){result.canyonFloorVertices.clear();result.canyonRockVertices.clear();}
    result.vertices=std::move(vertices);result.indices=std::move(indices);
    if(hasSnow)result.snowVertices=std::move(snowVertices);
    if(gfx::capture::Enabled())result.profileBuilt=gfx::capture::Now();
    return result;
}

std::future<Chunk> TerrainWorker::Submit(MeshKey key) {
    const double submitted=gfx::capture::Enabled()?gfx::capture::Now():0;
    auto job=std::make_shared<std::packaged_task<Chunk()>>(
        [key,submitted,foundations=terrain::Foundations(),roads=terrain::Roads()] {
            gfx::capture::TileScope profileTile(key.first.x,key.first.z,key.first.level);
            if(submitted)gfx::capture::Span("world.queue.worker_wait",submitted,gfx::capture::Now()-submitted,"wait");
            terrain::Foundations()=foundations;terrain::Roads()=roads;
            return Build(key.first,key.second);
        });
    auto result=job->get_future();
    {std::lock_guard lock(mutex); task=[job]{(*job)();};}
    wake.notify_one();return result;
}

// Compare old and new edits, including removed objects. Preserve every other
// cached chunk and keep dirty chunks visible until their replacements are ready.
void InvalidateEdits() {
    struct Bounds {double x0,z0,x1,z1;};
    std::vector<Bounds> bounds;
    auto changes=[&](const auto &old,const auto &now,auto add) {
        auto equal=[](const auto &a,const auto &b){return std::memcmp(&a,&b,sizeof(a))==0;};
        for(const auto &v:old)if(std::none_of(now.begin(),now.end(),[&](const auto &n){return equal(v,n);}))add(v);
        for(const auto &v:now)if(std::none_of(old.begin(),old.end(),[&](const auto &n){return equal(v,n);}))add(v);
    };
    changes(previousFoundations,terrain::Foundations(),[&](const auto &p){bounds.push_back({p.x-p.halfX-432,p.z-p.halfZ-432,p.x+p.halfX+432,p.z+p.halfZ+432});});
    changes(previousRoads,terrain::Roads(),[&](const auto &r){bounds.push_back({std::min(r.x,r.endX)-232,std::min(r.z,r.endZ)-232,std::max(r.x,r.endX)+232,std::max(r.z,r.endZ)+232});});
    previousFoundations=terrain::Foundations();previousRoads=terrain::Roads();
    if(bounds.empty())return;
    ++terrainRevision;
    unsigned dirty=0;
    for(auto &[key,value]:cache) {
        const auto &t=key.first;
        // Edge stitching and normal queries can sample beyond the tile border.
        double margin=t.size()/4;
        for(auto b:bounds)if(t.left()-margin<=b.x1 && t.left()+t.size()+margin>=b.x0 && t.top()-margin<=b.z1 && t.top()+t.size()+margin>=b.z0) {
            value.dirty=true;++dirty;break;
        }
    }
    pending=active;pendingCursor=0;
    publications.clear();
    localPending.clear();
    if(std::getenv("DC_TERRAIN_PERF"))std::fprintf(stderr,"terrain edit: invalidated %u of %zu cached chunks\n",dirty,cache.size());
}

void Update(double x, double z, int minimumLevel) {
    gfx::capture::Scope updateProfile("world.stream.update");
    auto perfStart = std::chrono::steady_clock::now();
    ++cacheClock;
    // The tracked world position determines the nested resolution rings.
    // A complete previous selection stays visible until its replacement is ready.
    double quantum = std::ldexp(terrain::BaseSize*2, minimumLevel);
    double cx = std::floor(x / quantum) * quantum, cz = std::floor(z / quantum) * quantum;
    const bool farRetarget=std::abs(cx-lastX)>4096 || std::abs(cz-lastZ)>4096;
    int    built = 0;
    bool   planned = false;
    // A walking teleport must never display a distant coarse triangle over
    // the destination's collision surface. Finish this one-time load before
    // presenting it; ordinary travel and editor panning keep the 2 ms budget.
    if (GameMode == ED_MODE_WALK && !DranFlightActive() && minimumLevel == 0 && !walkRequested && !active.empty() &&
        (std::abs(cx - lastX) > 4096 || std::abs(cz - lastZ) > 4096)) {
        if(std::getenv("DC_TERRAIN_TRACE"))std::fprintf(stderr,"terrain: loading teleport destination %.2f %.2f before display\n",x,z);
        active.clear();
        pending.clear();
        localPending.clear();publications.clear();
        pendingCursor = 0;
        lastX = lastZ = 1e30;
    }
    if(!pending.empty() && (minimumLevel!=lastLevel || cx!=lastX || cz!=lastZ)){
        pending.clear();pendingCursor=0;publications.clear();
        // An editor pan can cross kilometres in a tick. An unfinished local
        // publication for the previous cursor must not own the urgent queue.
        if(GameMode==ED_MODE_GEORAMA || std::abs(cx-lastX)>2048 || std::abs(cz-lastZ)>2048){
            localPending.clear();
            if(std::getenv("DC_TERRAIN_TRACE"))std::fprintf(stderr,"terrain: retarget %.0f %.0f obsolete local plan discarded\n",x,z);
        }
    }

    if (pending.empty() && (cx != lastX || cz != lastZ || minimumLevel != lastLevel)) {
        lastX = cx;
        lastZ = cz;
        lastLevel = minimumLevel;
        gfx::capture::Scope planningProfile("world.stream.plan");
        auto               tiles = terrain::Select(cx, cz, minimumLevel);
        terrain::Selection selection;
        for (auto t : tiles) {
            selection.emplace(t, 0);
        }
        pending.reserve(tiles.size());
        for (auto t : tiles) {
            pending.push_back({t, Edges(t, selection)});
        }
        std::stable_sort(pending.begin(),pending.end(),[&](const auto &a,const auto &b){return terrain::Distance(a.first,x,z)<terrain::Distance(b.first,x,z);});
        // Large jumps use the near refinement path and final target publication.
        // Connecting every old/new offscreen tile is quadratic wasted work while panning.
        publications=(active.empty()||farRetarget)?std::vector<terrain::Publication<MeshKey>>{}:terrain::Publications(active,pending);
        pendingCursor = 0;
        planned = true;
    }
    bool initial = active.empty();
    gfx::capture::Counter("world.initial_load",initial);
    gfx::capture::Counter("world.flight",DranFlightActive());
    if(!initial && !pending.empty() && localPending.empty()) {
        gfx::capture::Scope refinementProfile("world.stream.local_plan");
        localPlanX=x;localPlanZ=z;
        localPending=terrain::AdaptLocal(active,pending,x,z,Edges);
        // Plan several budget-neutral transfers before building. Generating
        // each intermediate seam state wastes jobs that the next split replaces.
        for(int transfer=1;transfer<4 && !localPending.empty();++transfer){
            auto next=terrain::AdaptLocal(localPending,pending,x,z,Edges);
            if(next.empty())break;
            localPending=std::move(next);
        }
        std::stable_sort(localPending.begin(),localPending.end(),[&](const auto &a,const auto &b){return terrain::Distance(a.first,x,z)<terrain::Distance(b.first,x,z);});
    }
    if(gfx::capture::Enabled()) {
        std::set<MeshKey> wanted(pending.begin(),pending.end());wanted.insert(localPending.begin(),localPending.end());
        std::erase_if(profileRequests,[&](const auto &entry){return !wanted.contains(entry.first);});
        for(auto &key:wanted)if(!cache.contains(key)||cache.at(key).dirty)profileRequests.try_emplace(key,gfx::capture::Now());
    }
    auto ready=[&](const MeshKey &key){auto it=cache.find(key);return it!=cache.end()&&!it->second.dirty;};
    const auto deadline=initial?std::chrono::steady_clock::time_point::max():perfStart+std::chrono::milliseconds(localPending.empty()?2:4);
    // Dispatch without blocking, including while another result spans several
    // upload frames. Never queue more than one chunk per worker.
    auto dispatch=[&] {
        gfx::capture::Scope dispatchProfile("world.stream.dispatch");
        for(size_t i=0;i<WorkerCount();++i){
            auto &f=flights[i];if(f.building.valid()||f.uploading)continue;
            // Never let slow continental population jobs occupy every
            // worker. Reserve half the live workers for near-ring geometry;
            // those jobs must start as soon as the tracked centre advances.
            const bool nearOnly=!initial && i<WorkerCount()/2;
            auto submit=[&](const std::vector<MeshKey> &wanted,bool urgent){for(const auto &key:wanted){
                if(nearOnly && key.first.level>2 && (!urgent || GameMode==ED_MODE_GEORAMA))continue;
                if(ready(key))continue;
                bool busy=false;for(const auto &other:flights)
                    busy|=(other.building.valid()||other.uploading)&&other.key==key&&other.revision==terrainRevision;
                if(busy)continue;
                if(!f.worker)f.worker=std::make_unique<TerrainWorker>();
                if(auto it=profileRequests.find(key);it!=profileRequests.end()){
                    gfx::capture::TileScope tile(key.first.x,key.first.z,key.first.level);
                    gfx::capture::Span("world.queue.scheduler_wait",it->second,gfx::capture::Now()-it->second,"wait");
                }
                f.key=key;f.revision=terrainRevision;f.building=f.worker->Submit(key);return true;
            }return false;};
            if(!submit(localPending,true))submit(pending,false);
        }
    };
    dispatch();
    do {
        bool progress=false;
        // Collect all completed work before choosing uploads. Worker index is
        // unrelated to the current destination's urgency.
        std::array<Flight*,WorkerCapacity> uploadOrder{};
        for(size_t i=0;i<flights.size();++i){auto &f=flights[i];uploadOrder[i]=&f;
            if(f.building.valid()&&f.building.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                f.uploading=f.building.get();
        }
        if(GameMode==ED_MODE_GEORAMA)std::stable_sort(uploadOrder.begin(),uploadOrder.end(),[&](auto a,auto b){
            bool au=std::find(localPending.begin(),localPending.end(),a->key)!=localPending.end();
            bool bu=std::find(localPending.begin(),localPending.end(),b->key)!=localPending.end();
            if(au!=bu)return au;
            return terrain::Distance(a->key.first,x,z)<terrain::Distance(b->key.first,x,z);
        });
        for(auto *flight:uploadOrder){auto &f=*flight;
            if(!f.uploading)continue;
            bool wanted=std::find(localPending.begin(),localPending.end(),f.key)!=localPending.end() ||
                        std::find(pending.begin(),pending.end(),f.key)!=pending.end();
            if(f.revision!=terrainRevision || (!wanted && GameMode==ED_MODE_GEORAMA)){
                Destroy(*f.uploading);f.uploading.reset();progress=true;
                gfx::capture::Counter("world.obsolete_upload_discarded",1);continue;
            }
            if(std::chrono::steady_clock::now()>=deadline)break;
            gfx::capture::TileScope uploadTile(f.key.first.x,f.key.first.z,f.key.first.level);
            gfx::capture::Scope uploadProfile("world.upload.total");
            if(f.uploading->profileBuilt){
                gfx::capture::Span("world.queue.upload_wait",f.uploading->profileBuilt,gfx::capture::Now()-f.uploading->profileBuilt,"wait");
                f.uploading->profileBuilt=0;
            }
            if(!Upload(*f.uploading,deadline))break;
            if(auto it=profileRequests.find(f.key);it!=profileRequests.end()){
                gfx::capture::Span("world.latency.request_to_ready",it->second,gfx::capture::Now()-it->second,"wait");profileRequests.erase(it);
            }
            if(gfx::capture::Enabled())f.uploading->profileReady=gfx::capture::Now();
            if(auto it=cache.find(f.key);it!=cache.end()){Destroy(it->second.chunk);it->second={std::move(*f.uploading),cacheClock,false};}
            else cache.emplace(f.key,CachedChunk{std::move(*f.uploading),cacheClock,false});
            f.uploading.reset();++built;progress=true;
        }
        dispatch();
        pendingCursor=std::count_if(pending.begin(),pending.end(),ready);
        if(pendingCursor==pending.size())break;
        if(!initial)break;
        // A dense forest can take much longer than its neighbours. Waiting
        // indefinitely for worker zero leaves the other completed workers idle.
        // Poll only during the loading screen; live frames never wait at all.
        if(!progress)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(true);
    if(!localPending.empty() && std::all_of(localPending.begin(),localPending.end(),ready)){
        active.swap(localPending);localPending.clear();publications.clear();
        if(std::getenv("DC_TERRAIN_PERF"))std::fprintf(stderr,"terrain publish local: focus %.0f %.0f chunks %zu plan %.0f %.0f\n",x,z,active.size(),localPlanX,localPlanZ);
    }
    for(auto it=publications.begin();localPending.empty() && it!=publications.end();){
        if(!std::all_of(it->after.begin(),it->after.end(),ready)){++it;continue;}
        std::set<MeshKey> removed(it->before.begin(),it->before.end());
        std::erase_if(active,[&](const auto &k){return removed.contains(k);});
        active.insert(active.end(),it->after.begin(),it->after.end());
        if(std::getenv("DC_TERRAIN_PERF"))std::fprintf(stderr,"terrain publish region: removed %zu added %zu\n",it->before.size(),it->after.size());
        it=publications.erase(it);
    }
    if (!pending.empty() && pendingCursor == pending.size()) {
        active.swap(pending);pending.clear();pendingCursor=0;
        localPending.clear();publications.clear();
        if(std::getenv("DC_TERRAIN_PERF"))std::fprintf(stderr,"terrain publish: focus %.0f %.0f lod %d complete %zu\n",lastX,lastZ,lastLevel,active.size());
    }
    gfx::capture::Counter("world.active_tiles",double(active.size()));
    gfx::capture::Counter("world.pending_tiles",double(pending.size()-pendingCursor));
    gfx::capture::Counter("world.cached_tiles",double(cache.size()));
    if(gfx::capture::Enabled()) {
        int busy=0,uploads=0;
        for(auto &f:flights){busy+=f.building.valid();uploads+=bool(f.uploading);}
        gfx::capture::Counter("world.workers_busy",busy);
        gfx::capture::Counter("world.upload_backlog",uploads);
        gfx::capture::Counter("world.local_pending",double(localPending.size()));
        terrain::Selection rendered;for(auto &key:active)rendered.emplace(key.first,0);
        gfx::capture::Counter("world.player_lod",terrain::LevelAt(rendered,x,z));
    }
    // Protect both complete coverage and the selection being prepared.
    for (auto &key : active) {
        auto &cached=cache.at(key);cached.used = cacheClock;
        if(cached.chunk.profileReady){
            gfx::capture::TileScope tile(key.first.x,key.first.z,key.first.level);
            gfx::capture::Span("world.latency.ready_to_visible",cached.chunk.profileReady,gfx::capture::Now()-cached.chunk.profileReady,"wait");
            cached.chunk.profileReady=0;
        }
    }
    for (auto &key : pending) {
        if (auto it = cache.find(key); it != cache.end()) {
            it->second.used = cacheClock;
        }
    }
    for(auto &key:localPending)if(auto it=cache.find(key);it!=cache.end())it->second.used=cacheClock;
    if (cache.size() > 4096) {
        std::vector<std::pair<unsigned, MeshKey>> oldest;
        for (auto &[key, value] : cache) {
            if (value.used != cacheClock) {
                oldest.push_back({value.used, key});
            }
        }
        std::sort(oldest.begin(), oldest.end());
        size_t count = std::min(oldest.size(), cache.size() - 4096);
        for (size_t i = 0; i < count; i++) {
            auto it = cache.find(oldest[i].second);
            gfx::DestroyMesh(it->second.chunk.mesh);
            gfx::DestroyMesh(it->second.chunk.snow);
            gfx::DestroyMesh(it->second.chunk.hill);
            BiomeDestroy(it->second.chunk.biome);
            cache.erase(it);
        }
    }
    double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perfStart).count();
    if (std::getenv("DC_TERRAIN_PERF")) {
        int centreLevel=-1;
        for(const auto &key:active){const auto &t=key.first;
            if(x>=t.left() && x<t.left()+t.size() && z>=t.top() && z<t.top()+t.size())centreLevel=t.level;
        }
        std::fprintf(stderr,"terrain centre: level %d\n",centreLevel);
        std::fprintf(stderr, "terrain perf: update %.3f ms built %d chunks %zu cached %zu pending %zu lod %d initial %d\n", elapsed, built, active.size(), cache.size(), pending.size() - pendingCursor, minimumLevel, int(initial));
    }
    if (planned && std::getenv("DC_TERRAIN_TRACE")) {
        int maxLevel = 0;
        for (auto &key : active) {
            maxLevel = std::max(maxLevel, key.first.level);
        }
        std::fprintf(stderr, "terrain: focus %.0f %.0f chunks %zu built %d radius %.0f levels %d..%d\n", cx, cz, active.size(), built, terrain::Radius, minimumLevel, maxLevel);
    }
}
} // namespace

bool TownTerrainEnabled(const CEditGround *ground) { return ground && ground->map_no == TOWN_NORUNE; }
void TownTerrainShutdown(){
    // Function-static procedural caches are destroyed before global workers.
    // Join explicitly while those caches and the renderer are still alive.
    ClearChunks();for(auto &flight:flights){flight.worker.reset();flight.building={};}
    gfx::capture::Flush();
}

bool TownTerrainResidentHeight(double wx,double wz,float &height,int maxLevel) {
    for(const auto &key:active){const auto &t=key.first;
        if(t.level>maxLevel)continue;
        if(wx<t.left() || wz<t.top() || wx>=t.left()+t.size() || wz>=t.top()+t.size())continue;
        auto it=cache.find(key);if(it==cache.end() || it->second.dirty)continue;
        const auto &chunk=it->second.chunk;
        double px=wx-t.left(),pz=wz-t.top();
        for(size_t i=0;i+2<chunk.indices.size();i+=3){
            auto *a=chunk.vertices[chunk.indices[i]].position,*b=chunk.vertices[chunk.indices[i+1]].position,*c=chunk.vertices[chunk.indices[i+2]].position;
            double den=(b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2]);if(std::abs(den)<1e-8)continue;
            double u=((b[2]-c[2])*(px-c[0])+(c[0]-b[0])*(pz-c[2]))/den;
            double v=((c[2]-a[2])*(px-c[0])+(a[0]-c[0])*(pz-c[2]))/den;
            if(u>=-1e-6&&v>=-1e-6&&u+v<=1.000001){height=float(u*a[1]+v*b[1]+(1-u-v)*c[1]+.04);return true;}
        }
        constexpr int n=terrain::Resolution;
        double u=(wx-t.left())/t.size()*n,v=(wz-t.top())/t.size()*n;
        int ix=std::clamp(int(u),0,n-1),iz=std::clamp(int(v),0,n-1);u-=ix;v-=iz;
        const auto &h=it->second.chunk.surface;int a=iz*(n+1)+ix,b=a+1,c=a+n+1,d=c+1;
        height=float((u+v<=1?h[a]*(1-u-v)+h[b]*u+h[c]*v:h[b]*(1-v)+h[c]*(1-u)+h[d]*(u+v-1))+.04);
        return true;
    }
    return false;
}

float TownTerrainCursorHeight(float x,float z) {
    gfx::capture::Scope profile("world.editor.cursor_surface");
    const double wx=world::X(x),wz=world::Z(z);
    float height;
    if(TownTerrainResidentHeight(wx,wz,height,terrain::MaxLevel))return height;
    // Outside prior coverage, use the cheap broad surface until resident
    // terrain arrives. This is only an aerial camera target, never collision.
    return float(terrain::FlightSurfaceHeight(wx,wz));
}

bool TownTerrainRequestWalk(float x,float z) {
    if(!TownTerrainEnabled(pEditGround))return true;
    walkX=world::X(x);walkZ=world::Z(z);walkRequested=true;
    // The normal asynchronous queue prepares the destination while the editor
    // stays open or Dran descends. No future.get or mesh build on this path.
    if(active.empty()||lastLevel!=0)return false;
    terrain::Selection selection;for(auto &key:active)if(!cache.at(key).dirty)selection.emplace(key.first,0);
    for(int dz:{-256,0,256})for(int dx:{-256,0,256})
        if(terrain::LevelAt(selection,walkX+dx,walkZ+dz)!=0)return false;
    walkRequested=false;
    if(std::getenv("DC_TERRAIN_TRACE"))std::fprintf(stderr,"terrain: landing ready %.2f %.2f lod 0\n",x,z);
    return true;
}

void TownTerrainCancelWalk(){walkRequested=false;}

void TownTerrainDraw(CEditGround *ground) {
    gfx::capture::Scope drawProfile("world.record.total");
    // Interaction regressions exercise real collision/events without building
    // the unrelated continental render mesh in their offscreen test process.
    if (!TownTerrainEnabled(ground)) {
        ClearChunks();
        return;
    }
    WorldGeoramaFoundations(ground);
    InvalidateEdits();
    WorldGeoramaTest(ground);
    world::TestTick();
    // Prepare is idempotent; map loaders may reuse the same arena address.
    Prepare(ground);
    if(std::getenv("DC_TOWN_WALK_TRACE")&&Chara){static int tick=0;if(++tick%60==0)std::fprintf(stderr,"town walk: %.3f %.3f %.3f\n",world::X(Chara->pos[0]),Chara->pos[1],world::Z(Chara->pos[2]));}
    if(std::getenv("DC_TEST_SKIP_TERRAIN_DRAW"))return;
    BiomePrepare(ground);
    static int testTicks = 0;
    ++testTicks;
    int spawnFrame=120;if(const char *frame=std::getenv("DC_TERRAIN_TEST_SPAWN_FRAME"))spawnFrame=std::atoi(frame);
    if (testTicks == spawnFrame && Chara) {
        if (const char *spawn = std::getenv("DC_TERRAIN_TEST_SPAWN")) {
            double x, z; float angle = 0;
            if (std::sscanf(spawn, "%lf,%lf,%f", &x, &z, &angle) >= 2) {
                world::Rebase(std::round(x/3200)*3200,std::round(z/3200)*3200);
                world::Assign(Chara->pos,{x,terrain::SurfaceHeight(x,z)+60,z});
                Chara->SetPosition(Chara->pos);
                if (NowCamera) {
                    auto *cam = static_cast<CCameraFollow *>(NowCamera);
                    cam->SetFollow(world::LocalX(x), float(terrain::Height(x, z)) + 65, world::LocalZ(z));
                    cam->SetAngleSoon(angle);
                    cam->Step(-1);
                }
            }
        }
    }
    float pos[4]{};
    if (Chara) {
        Chara->GetPosition(pos);
    }
    if(const char *target=std::getenv("DC_TERRAIN_TEST_LOOK_AT");target && NowCamera==&MainCamera && !DranFlightActive()){
        double x,z;
        if(std::sscanf(target,"%lf,%lf",&x,&z)==2){
            float angle=float(std::atan2(world::X(pos[0])-x,world::Z(pos[2])-z));
            if(std::getenv("DC_TERRAIN_TEST_LOOK_SWEEP") && testTicks>=180 && testTicks<300)angle+=3.14159265f;
            MainCamera.SetAngleSoon(angle);
            MainCamera.Step(-1);
        }
    }
    if (GameMode == ED_MODE_GEORAMA && ECursorFrame) {
        pos[0] = ECursorFrame->position[0];
        pos[2] = ECursorFrame->position[2];
    }
    // Camera altitude changes the view, never the world resolution beneath
    // the player/editor cursor. Landing uses this same fine ring.
    int minimumLevel = 0;
    if(walkRequested){minimumLevel=0;pos[0]=world::LocalX(walkX);pos[2]=world::LocalZ(walkZ);}
    const double focusX=world::X(pos[0]),focusZ=world::Z(pos[2]);
    Update(focusX, focusZ, minimumLevel);
    if(std::getenv("DC_WORLD_PRECISION_TRACE")) {
        float eye[4]{},target[4]{};
        if(NowCamera){NowCamera->GetPos(eye);NowCamera->GetRef(target);}
        std::fprintf(stderr,"precision camera: tick %d player %.9g %.9g eye %.9g %.9g ref %.9g %.9g\n",testTicks,pos[0],pos[2],eye[0],eye[2],target[0],target[2]);
    }
    if (std::getenv("DC_TERRAIN_TRACE") && testTicks % 120 == 0) {
        std::fprintf(stderr,"terrain surface: %.5f foundations %zu\n",world::SurfaceHeight(pos[0],pos[2]),terrain::Foundations().size());
        std::fprintf(stderr, "terrain: player %.2f %.2f %.2f ground %.2f far %.0f\n", world::X(pos[0]), pos[1], world::Z(pos[2]), world::Height(pos[0], pos[2]), mgRenderInfo.far[2]);
    }
    auto                       binding = BiomeSurfaceBinding("e01/e01b01");
    auto canyonFloorBinding=BiomeSurfaceBinding("e01/e01b02");
    canyonFloorBinding.filter=gfx::Filter::Linear;
    canyonFloorBinding.wrap_u=canyonFloorBinding.wrap_v=gfx::Wrap::Repeat;
    auto                       rockBinding = BiomeSurfaceBinding("e02/e02b21");
    auto canyonHillBinding=BiomeSurfaceBinding("native/e01/e01b05");
    canyonHillBinding.filter=gfx::Filter::Linear;
    canyonHillBinding.wrap_u=canyonHillBinding.wrap_v=gfx::Wrap::Repeat;
    auto canyonRockBinding=BiomeSurfaceBinding("native/e01/e01b06");
    canyonRockBinding.filter=gfx::Filter::Linear;
    canyonRockBinding.wrap_u=canyonRockBinding.wrap_v=gfx::Wrap::Repeat;
    if(const char *diagnostic=std::getenv("DC_TERRAIN_DIAGNOSTIC");diagnostic&&std::strcmp(diagnostic,"geometry")==0) {
        binding={};rockBinding={};
    }
    static gfx::tim2::Uploaded ice;
    if (!ice.texture || !gfx::GetTextureInfo(ice.texture)) {
        std::ifstream file(PathsDataRoot() / "dun/item/main_data/icebig.img", std::ios::binary);
        if (file) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
            const uint8_t        signature[] = {'T', 'I', 'M', '2'};
            auto                 begin = std::search(data.begin(), data.end(), std::begin(signature), std::end(signature));
            if (begin != data.end()) {
                size_t offset = size_t(begin - data.begin());
                // Retail stores the RGBA VRAM reservation as total_size for this indexed
                // image. The serialized picture is only header + indices + palette.
                if (offset + 32 <= data.size()) {
                    uint32_t reserved = 0;
                    std::memcpy(&reserved, data.data() + offset + 16, 4);
                    if (reserved < 1024 * 1024 && offset + 16 + reserved > data.size()) {
                        data.resize(offset + 16 + reserved);
                    }
                }
                auto pictures = gfx::tim2::Parse(std::span<const uint8_t>(data.data() + offset, data.size() - offset));
                if (pictures && !pictures->empty()) {
                    auto             &p = (*pictures)[0];
                    gfx::tim2::Source source;
                    source.psm = p.psm;
                    source.width = p.width;
                    source.height = p.height;
                    source.levels = p.levels;
                    source.clut = p.clut;
                    source.clut16 = p.clut16;
                    source.csm1 = p.clut_csm1;
                    source.swizzled = data.size() > 3 && std::memcmp(data.data(), "IM2", 3) == 0;
                    if (auto decoded = gfx::tim2::Decode(source)) {
                        ice = gfx::tim2::Upload(*decoded);
                    }
                }
            }
            if (ice.texture) {
                std::fprintf(stderr, "terrain: original Dark Cloud icebig texture loaded for snow caps\n");
            }
        }
    }
    gfx::TextureBinding snowBinding;
    snowBinding.texture = ice.texture;
    snowBinding.palette = ice.palette;
    rockBinding.filter = snowBinding.filter = gfx::Filter::Linear;
    rockBinding.wrap_u = rockBinding.wrap_v = snowBinding.wrap_u = snowBinding.wrap_v = gfx::Wrap::Repeat;
    binding.filter = gfx::Filter::Linear;
    binding.wrap_u = binding.wrap_v = gfx::Wrap::Repeat;
    RenderInfo info = mgRenderInfo;
    info.shadow_pass = 0;
    // EditLoop extends the shared reverse-depth projection for Norune.
    auto state = Draw3DState(info, false);
    int  drawn = 0;
    struct WaterDraw { const BiomeChunk* chunk; gfx::MeshConstants constants; gfx::MeshTransform transform; };
    std::vector<WaterDraw> waterDraws;
    state.blend = false;
    state.cull = gfx::CullMode::None;
    gfx::capture::Scope recordProfile("world.record.chunks");
    terrain::Selection desiredDraw;
    if(GameMode==ED_MODE_GEORAMA)for(const auto &key:pending)desiredDraw.emplace(key.first,0);
    for (const auto &key : active) {
        const auto   &tile = key.first;
        const auto   &chunk = cache.at(key).chunk;
        // Tiles fully covered by the authored town have no procedural faces.
        if(chunk.indexCount==0)continue;
        sceVu0FMATRIX model;
        sceVu0UnitMatrix(model);
        model[3][0] = world::LocalX(tile.left());
        model[3][2] = world::LocalZ(tile.top());
        gfx::MeshConstants constants{};
        gfx::MeshTransform transform;
        Draw3DSceneConstants(constants, info, model, &transform);
        // Conservative bounds in the actual renderer clip space. A guard band
        // keeps interpolated camera movement from revealing culled edges.
        unsigned outside = 31;
        for (int corner = 0; corner < 8; corner++) {
            float p[4] = {(corner & 1) ? float(tile.size()) : 0, (corner & 2) ? chunk.high : chunk.low, (corner & 4) ? float(tile.size()) : 0, 1};
            float clip[4] = {};
            for (int row = 0; row < 4; row++) {
                for (int col = 0; col < 4; col++) {
                    clip[row] += constants.mvp[col * 4 + row] * p[col];
                }
            }
            unsigned flags = 0;
            flags |= (clip[0] < -1.2f * clip[3]) ? 1 : 0;
            flags |= (clip[0] > 1.2f * clip[3]) ? 2 : 0;
            flags |= (clip[1] < -1.2f * clip[3]) ? 4 : 0;
            flags |= (clip[1] > 1.2f * clip[3]) ? 8 : 0;
            flags |= (clip[3] <= 0) ? 16 : 0;
            outside &= flags;
        }
        if (outside) {
            continue;
        }
        ++drawn;
        constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere;
        constants.clip_plane[0][0] = info.view_position[0] - world::LocalX(tile.left());
        constants.clip_plane[0][1] = info.view_position[1];
        constants.clip_plane[0][2] = info.view_position[2] - world::LocalZ(tile.top());
        constants.clip_plane[0][3] = 0.1f; // one game unit is ten centimetres in this landscape.
        for (int axis = 0; axis < 3; axis++) {
            constants.clip_plane[1][axis] = -constants.light_direction[0][axis];
        }
        constants.clip_plane[1][3] = 2e-6f; // clear-air aerosol scattering, per metre.
        float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 0};
        Draw3DMaterial(constants, info, white, white, black);
        constants.flags |= gfx::kMeshWorldUv | gfx::kMeshTerrainTriplanar;
        constants.world_uv[0][0]=constants.world_uv[1][2]=1.f/200;
        constants.world_uv[0][3]=float(std::fmod(tile.left()/200,4096));
        constants.world_uv[1][3]=float(std::fmod(tile.top()/200,4096));
        gfx::DrawMesh(chunk.mesh,0,chunk.indexCount,constants,binding,state,&transform);
        auto overlay=state;overlay.blend=true;overlay.alpha={};overlay.depth_write=false;overlay.depth_test=gfx::DepthTest::GEqual;
        if(chunk.canyonFloor){
            auto floor=constants;floor.flags|=gfx::kMeshTerrainNative|gfx::kMeshVertexColor;floor.flags&=~gfx::kMeshTerrainTriplanar;
            // Exact native outlet UV chart; coverage is per vertex, never a
            // chunk-centre biome decision that can create square texture seams.
            constexpr double ux=.00464379610,uz=.0000817849809,uo=5.81250272;
            constexpr double vx=-.000253333965,vz=-.00537387167,vo=-.289812982;
            floor.world_uv[0][0]=float(ux);floor.world_uv[0][2]=float(uz);
            floor.world_uv[0][3]=float(std::fmod(tile.left()*ux+tile.top()*uz+uo,4096));
            floor.world_uv[1][0]=float(vx);floor.world_uv[1][2]=float(vz);
            floor.world_uv[1][3]=float(std::fmod(tile.left()*vx+tile.top()*vz+vo,4096));
            gfx::DrawMesh(chunk.canyonFloor,0,chunk.indexCount,floor,canyonFloorBinding,overlay,&transform);
        }
        BiomeDraw(chunk.biome,constants,state,transform,testTicks,0);
        constants.flags|=gfx::kMeshVertexColor;
        constants.world_uv[0][0]=constants.world_uv[1][2]=1.f/240;
        constants.world_uv[0][3]=float(std::fmod(tile.left()/240,4096));
        constants.world_uv[1][3]=float(std::fmod(tile.top()/240,4096));
        gfx::DrawMesh(chunk.mesh,0,chunk.indexCount,constants,rockBinding,overlay,&transform);
        constants.flags|=gfx::kMeshTerrainNative|gfx::kMeshTerrainMirror;
        if(chunk.canyonRock){
            constants.world_uv[0][0]=constants.world_uv[1][2]=1.f/100;
            constants.world_uv[0][3]=float(std::fmod(tile.left()/100,4096));
            constants.world_uv[1][3]=float(std::fmod(tile.top()/100,4096));
            gfx::DrawMesh(chunk.canyonRock,0,chunk.indexCount,constants,canyonRockBinding,overlay,&transform);
        }
        if(chunk.hill){
            constants.flags&=~gfx::kMeshTerrainTriplanar;
            constants.world_uv[0][0]=constants.world_uv[1][2]=1.f/120;
            constants.world_uv[0][3]=float(std::fmod(tile.left()/120,4096));
            constants.world_uv[1][3]=float(std::fmod(tile.top()/120,4096));
            gfx::DrawMesh(chunk.hill,0,chunk.indexCount,constants,canyonHillBinding,overlay,&transform);
        }
        // Authored UV charts meet the retained native triangles exactly. Their
        // opacity tapers out across the collar into the same biome materials.
        auto joinConstants=constants;
        joinConstants.flags&=~(gfx::kMeshWorldUv|gfx::kMeshTerrainTriplanar);
        joinConstants.flags|=gfx::kMeshTerrainNative|gfx::kMeshVertexColor;
        for(const auto &[name,layer]:chunk.joinLayers){
            joinConstants.flags=(joinConstants.flags&~gfx::kMeshTerrainMirror)|((name=="e01b05"||name=="e01b06")?gfx::kMeshTerrainMirror:0u);
            auto texture=BiomeSurfaceBinding(((name=="e01b05"||name=="e01b06"?"native/e01/":"e01/")+name).c_str());
            texture.filter=gfx::Filter::Linear;texture.wrap_u=texture.wrap_v=gfx::Wrap::Repeat;
            gfx::DrawMesh(layer.mesh,0,unsigned(layer.indices.size()),joinConstants,texture,overlay,&transform);
        }
        if (chunk.snow) {
            constants.flags&=~(gfx::kMeshTerrainNative|gfx::kMeshTerrainMirror);constants.flags|=gfx::kMeshTerrainTriplanar;
            gfx::DrawMesh(chunk.snow, 0, chunk.indexCount, constants, snowBinding, overlay, &transform);
        }
        constants.flags &= ~(gfx::kMeshWorldUv | gfx::kMeshTerrainTriplanar);
        // Retained terrain is only transitional coverage. Its trees must not
        // retain old near-detail costs after the editor focus has moved away.
        int foliageLevel=tile.level;
        if(!desiredDraw.empty()){
            int desired=terrain::LevelAt(desiredDraw,tile.left()+tile.size()/2,tile.top()+tile.size()/2);
            foliageLevel=std::max(foliageLevel,desired<0?terrain::MaxLevel:desired);
        }
        BiomeDraw(chunk.biome,constants,state,transform,testTicks,1,foliageLevel);
        if(chunk.biome.water.mesh)waterDraws.push_back({&chunk.biome,constants,transform});
    }
    if(const char *path=std::getenv("DC_TERRAIN_MESH_OBJ")){
        static bool exported=false;if(!exported&&!active.empty()){
            exported=true;std::ofstream out(path);size_t offset=1;
            for(const auto &key:active){if(key.first.level!=0)continue;auto it=cache.find(key);if(it==cache.end())continue;const auto &c=it->second.chunk;
                out<<"o tile_"<<key.first.x<<'_'<<key.first.z<<"\n";
                for(const auto &v:c.vertices)out<<"v "<<key.first.left()+v.position[0]<<' '<<v.position[1]<<' '<<key.first.top()+v.position[2]<<"\n";
                for(const auto &v:c.vertices)out<<"vn "<<v.normal[0]<<' '<<v.normal[1]<<' '<<v.normal[2]<<"\n";
                for(size_t i=0;i+2<c.indices.size();i+=3)out<<"f "<<offset+c.indices[i]<<' '<<offset+c.indices[i+1]<<' '<<offset+c.indices[i+2]<<"\n";
                offset+=c.vertices.size();
            }
        }
    }
    if(std::getenv("DC_CANYON_COLLISION_TEST")){
        static bool tested=false;if(!tested&&!active.empty()){
            int checked=0,missing=0;
            for(const auto &key:active){if(key.first.level!=0)continue;auto it=cache.find(key);if(it==cache.end())continue;auto &c=it->second.chunk;
                for(size_t i=0;i+2<c.indices.size();i+=27){
                    float centre[3]{};for(int k=0;k<3;k++){auto &v=c.vertices[c.indices[i+k]];for(int j=0;j<3;j++)centre[j]+=v.position[j]/3;}
                    centre[0]=world::LocalX(key.first.left()+centre[0]);centre[2]=world::LocalZ(key.first.top()+centre[2]);
                    CBoxVu0 box{};for(int j=0;j<3;j++){box.min[j]=centre[j]-.1f;box.max[j]=centre[j]+.1f;}
                    std::array<CCPoly,400> polys{};int n=TownTerrainPolys(ground,polys.data(),box);bool found=false;
                    for(int q=0;q<n;q++){auto &p=polys[q];double error=0;for(int k=0;k<3;k++){auto &v=c.vertices[c.indices[i+k]];error+=std::abs(p.vertex[k][0]-world::LocalX(key.first.left()+v.position[0]))+std::abs(p.vertex[k][1]-v.position[1])+std::abs(p.vertex[k][2]-world::LocalZ(key.first.top()+v.position[2]));}found|=error<.01;}
                    checked++;missing+=!found;
                }
            }
            if(checked){tested=true;std::fprintf(stderr,"canyon collision: %d submitted triangles checked, %d mismatches\n",checked,missing);if(missing)std::abort();}
        }
    }
    // Forest geometry is submitted with its owning terrain chunk above.
    // Translucent rivers follow every opaque chunk, so a later bank cannot paint over water.
    for(const auto&draw:waterDraws)BiomeDraw(*draw.chunk,draw.constants,state,draw.transform,testTicks,2);
    if (std::getenv("DC_TERRAIN_PERF")) {
        std::fprintf(stderr, "terrain draw: %d of %zu chunks\n", drawn, active.size());
    }
}

int TownTerrainPolys(CEditGround *ground, CCPoly *out, const CBoxVu0 &box, int capacity) {
    if (!TownTerrainEnabled(ground) || capacity<=0) {
        return 0;
    }
    // Queries must use world-aligned triangles, even after origin rebasing.
    double step = 32;
    while ((box.max[0]-box.min[0])/step>16 || (box.max[2]-box.min[2])/step>16) step*=2;
    int count = 0;
    auto distance=[&](const CCPoly &q){double d=0;for(int k=0;k<3;k++){double c=(q.vertex[0][k]+q.vertex[1][k]+q.vertex[2][k])/3.;double v=c-(box.min[k]+box.max[k])*.5;d+=v*v;}return d;};
    auto nearer=[&](const CCPoly&a,const CCPoly&b){return distance(a)<distance(b);};
    bool heap=false;
    auto emit=[&](const CCPoly &p){
        for(int k=0;k<3;k++)if(std::max({p.vertex[0][k],p.vertex[1][k],p.vertex[2][k]})<box.min[k] || std::min({p.vertex[0][k],p.vertex[1][k],p.vertex[2][k]})>box.max[k])return;
        if(count<capacity)out[count++]=p;
        else {
            // Retail scratch buffers are bounded; retain the nearest faces for
            // broad camera queries without coarsening the collision surface.
            if(!heap){std::make_heap(out,out+count,nearer);heap=true;}
            if(nearer(p,out[0])){std::pop_heap(out,out+count,nearer);out[count-1]=p;std::push_heap(out,out+count,nearer);}
        }
    };
    if(world::X(box.min[0])<1400 && world::X(box.max[0])>-1400 && world::Z(box.min[2])<1400 && world::Z(box.max[2])>-1400)
        for(auto p:nativeTerrainPolys){for(auto &v:p.vertex){v[0]=world::LocalX(v[0]);v[2]=world::LocalZ(v[2]);}emit(p);}
    bool covered=false;
    for(const auto &key:active){
        const auto &t=key.first;
        if(t.left()>world::X(box.max[0]) || t.left()+t.size()<world::X(box.min[0]) || t.top()>world::Z(box.max[2]) || t.top()+t.size()<world::Z(box.min[2]))continue;
        auto it=cache.find(key);if(it==cache.end())continue;covered=true;
        auto &c=it->second.chunk;
        for(size_t i=0;i+2<c.indices.size();i+=3){
            CCPoly p{};for(int k=0;k<3;k++){auto &v=c.vertices[c.indices[i+k]];p.vertex[k][0]=world::LocalX(t.left()+v.position[0]);p.vertex[k][1]=v.position[1];p.vertex[k][2]=world::LocalZ(t.top()+v.position[2]);p.vertex[k][3]=1;}
            auto *a=p.vertex[0],*b=p.vertex[1],*d=p.vertex[2];
            float nx=(b[1]-a[1])*(d[2]-a[2])-(b[2]-a[2])*(d[1]-a[1]),ny=(b[2]-a[2])*(d[0]-a[0])-(b[0]-a[0])*(d[2]-a[2]),nz=(b[0]-a[0])*(d[1]-a[1])-(b[1]-a[1])*(d[0]-a[0]);
            float len=std::sqrt(nx*nx+ny*ny+nz*nz);if(len>1e-8){p.normal[0]=nx/len;p.normal[1]=ny/len;p.normal[2]=nz/len;emit(p);}
        }
    }
    if(covered)return count;
    for (double z = std::floor(box.min[2] / step) * step; z <= box.max[2]; z += step) {
        for (double x = std::floor(box.min[0] / step) * step; x <= box.max[0]; x += step) {
            // Retail collision owns the entire original enclosure, including
            // the recessed cave entrance and the fixed platform/stairs.
            auto collisionHeight=[&](double px,double pz){return world::Height(px,pz);};
            if(std::hypot(world::X(x+step/2),world::Z(z+step/2))<canyon::RidgeRadius(world::X(x+step/2),world::Z(z+step/2)))continue;
            // The editable town cells keep their original collision and Georama heights.
            if (world::X(x) >= -700 && world::X(x + step) <= 700 && world::Z(z) >= -700 && world::Z(z + step) <= 500) {
                continue;
            }
            float v[4][4] = {
                {float(x),        float(collisionHeight(x,        z)),        float(z),        1},
                {float(x + step), float(collisionHeight(x + step, z)),        float(z),        1},
                {float(x),        float(collisionHeight(x,        z + step)), float(z + step), 1},
                {float(x + step), float(collisionHeight(x + step, z + step)), float(z + step), 1}
            };
            for (auto tri : {
                     std::array<int, 3>{0, 1, 2},
                     std::array<int, 3>{2, 1, 3}
            }) {
                CCPoly p{};
                for (int k = 0; k < 3; k++) {
                    std::memcpy(p.vertex[k], v[tri[k]], sizeof(v[0]));
                }
                float ax = p.vertex[1][0] - p.vertex[0][0], ay = p.vertex[1][1] - p.vertex[0][1], az = p.vertex[1][2] - p.vertex[0][2];
                float bx = p.vertex[2][0] - p.vertex[0][0], by = p.vertex[2][1] - p.vertex[0][1], bz = p.vertex[2][2] - p.vertex[0][2];
                float nx = az * by - ay * bz, ny = ax * bz - az * bx, nz = ay * bx - ax * by;
                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                p.normal[0] = nx / len;
                p.normal[1] = ny / len;
                p.normal[2] = nz / len;
                emit(p);
            }
        }
    }
    return count;
}
