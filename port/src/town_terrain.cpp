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

namespace {
struct Chunk {
    double profileBuilt=0,profileReady=0;
    std::array<float,(terrain::Resolution+1)*(terrain::Resolution+1)> surface{};
    gfx::MeshHandle    mesh = 0;
    gfx::MeshHandle    snow = 0;
    std::array<int, 4> edges{};
    float              low = 0, high = 0;
    BiomeChunk         biome;
    unsigned indexCount=0;
    std::vector<gfx::Vertex3D> vertices, snowVertices;
    std::vector<uint32_t> indices;
};

using MeshKey = std::pair<terrain::Tile, std::array<int, 4>>;

struct CachedChunk {
    Chunk    chunk;
    unsigned used = 0;
    bool dirty = false;
};

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
    gfx::DestroyMesh(c.mesh);gfx::DestroyMesh(c.snow);BiomeDestroy(c.biome);
}
bool Upload(Chunk &c, std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::time_point::max()) {
    if(!c.mesh)c.mesh=gfx::CreateMesh(c.vertices,c.indices);
    if(!c.snow && !c.snowVertices.empty())c.snow=gfx::CreateMesh(c.snowVertices,c.indices);
    if(!BiomeUpload(c.biome,deadline))return false;
    std::vector<gfx::Vertex3D>().swap(c.vertices);
    std::vector<gfx::Vertex3D>().swap(c.snowVertices);
    std::vector<uint32_t>().swap(c.indices);
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
        gfx::DestroyMesh(value.chunk.mesh);
        gfx::DestroyMesh(value.chunk.snow);
        BiomeDestroy(value.chunk.biome);
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
    // Only the first Norune fixed part contains the enclosing waku cliff meshes.
    // The Mayor platform is now an independent, closed Georama model.
    auto walk = [](auto &&self, CFrame *frame) -> void {
        for (; frame; frame = frame->brother) {
            if (std::strncmp(frame->name, "waku", 4) == 0 || std::strcmp(frame->name, "cube2__f") == 0) {
                frame->attr.draw_on = 2;
            }
            self(self, frame->child);
        }
    };
    walk(walk, ground->fixed_parts[0].frame[0]);
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
            snow *= std::clamp((.8 - slope) * 2.5, 0.0, 1.0);
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
            indices.insert(indices.end(), {a, c, b, b, c, d});
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
    result.indexCount=unsigned(indices.size());
    for(size_t i=0;i<vertices.size();++i)result.surface[i]=vertices[i].position[1];
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
    if(std::getenv("DC_TEST_SKIP_TERRAIN_DRAW"))return;
    // Prepare is idempotent; map loaders may reuse the same arena address.
    Prepare(ground);
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
    auto                       rockBinding = BiomeSurfaceBinding("e02/e02b21");
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
        constants.world_uv[0][0] = 1.f / 200;
        constants.world_uv[0][3] = float(std::fmod(tile.left()/200,4096));
        constants.world_uv[1][2] = 1.f / 200;
        constants.world_uv[1][3] = float(std::fmod(tile.top()/200,4096));
        gfx::DrawMesh(chunk.mesh, 0, chunk.indexCount, constants, binding, state, &transform);
        BiomeDraw(chunk.biome,constants,state,transform,testTicks,0);
        constants.flags |= gfx::kMeshVertexColor | gfx::kMeshWorldUv | gfx::kMeshTerrainTriplanar;
        constants.world_uv[0][0] = 1.f / 240;
        constants.world_uv[0][3] = float(std::fmod(tile.left()/240,4096));
        constants.world_uv[1][2] = 1.f / 240;
        constants.world_uv[1][3] = float(std::fmod(tile.top()/240,4096));
        auto overlay = state;
        overlay.blend = true;
        overlay.alpha = {};
        overlay.depth_write = false;
        overlay.depth_test = gfx::DepthTest::GEqual;
        gfx::DrawMesh(chunk.mesh, 0, chunk.indexCount, constants, rockBinding, overlay, &transform);
        if (chunk.snow) {
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
    // Forest geometry is submitted with its owning terrain chunk above.
    // Translucent rivers follow every opaque chunk, so a later bank cannot paint over water.
    for(const auto&draw:waterDraws)BiomeDraw(*draw.chunk,draw.constants,state,draw.transform,testTicks,2);
    if (std::getenv("DC_TERRAIN_PERF")) {
        std::fprintf(stderr, "terrain draw: %d of %zu chunks\n", drawn, active.size());
    }
}

int TownTerrainPolys(CEditGround *ground, CCPoly *out, const CBoxVu0 &box) {
    if (!TownTerrainEnabled(ground)) {
        return 0;
    }
    // Small local patch, with a hard bound for the camera's much larger query.
    double step = 32;
    while ((box.max[0] - box.min[0]) / step > 8 || (box.max[2] - box.min[2]) / step > 8) {
        step *= 2;
    }
    int count = 0;
    for (double z = std::floor(box.min[2] / step) * step; z <= box.max[2]; z += step) {
        for (double x = std::floor(box.min[0] / step) * step; x <= box.max[0]; x += step) {
            // The editable town cells keep their original collision and Georama heights.
            if (world::X(x) >= -700 && world::X(x + step) <= 700 && world::Z(z) >= -700 && world::Z(z + step) <= 500) {
                continue;
            }
            float v[4][4] = {
                {float(x),        float(world::Height(x,        z)),        float(z),        1},
                {float(x + step), float(world::Height(x + step, z)),        float(z),        1},
                {float(x),        float(world::Height(x,        z + step)), float(z + step), 1},
                {float(x + step), float(world::Height(x + step, z + step)), float(z + step), 1}
            };
            for (auto tri : {
                     std::array<int, 3>{0, 1, 2},
                     std::array<int, 3>{2, 1, 3}
            }) {
                auto &p = out[count++];
                p = {};
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
            }
        }
    }
    return count;
}
