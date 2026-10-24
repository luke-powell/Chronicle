#include "world_origin.hpp"
#include "town_terrain.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

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
#include "texture.hpp"
#include "visualvu1.hpp"
#include "world_georama.hpp"

namespace {
struct Chunk {
    gfx::MeshHandle    mesh = 0;
    gfx::MeshHandle    snow = 0;
    std::array<int, 4> edges{};
    float              low = 0, high = 0;
};

using MeshKey = std::pair<terrain::Tile, std::array<int, 4>>;

struct CachedChunk {
    Chunk    chunk;
    unsigned used = 0;
};

std::map<MeshKey, CachedChunk> cache;
std::vector<MeshKey>           active, pending;
size_t                         pendingCursor = 0;
unsigned                       cacheClock = 0;
double                         lastX = 1e30, lastZ = 1e30;
int                            lastLevel = -1;
bool walkRequested=false;

void ClearChunks() {
    for (auto &[key, value] : cache) {
        gfx::DestroyMesh(value.chunk.mesh);
        gfx::DestroyMesh(value.chunk.snow);
    }
    cache.clear();
    active.clear();
    pending.clear();
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
    return {terrain::LevelAt(s, x - 1, z + w / 2), terrain::LevelAt(s, x + w + 1, z + w / 2),
            terrain::LevelAt(s, x + w / 2, z - 1), terrain::LevelAt(s, x + w / 2, z + w + 1)};
}

Chunk Build(const terrain::Tile &t, std::array<int, 4> edges) {
    constexpr int              n = terrain::Resolution;
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    std::vector<gfx::Vertex3D> snowVertices;
    bool                       hasSnow = false;
    float                      low = 1e30f, high = -1e30f;
    double                     step = t.size() / n;
    for (int z = 0; z <= n; z++) {
        for (int x = 0; x <= n; x++) {
            double wx = t.left() + x * step, wz = t.top() + z * step;
            double h = terrain::Height(wx, wz);
            if (x == 0 && edges[0] > t.level) {
                h = terrain::EdgeHeight(wx, wz, edges[0], false);
            }
            if (x == n && edges[1] > t.level) {
                h = terrain::EdgeHeight(wx, wz, edges[1], false);
            }
            if (z == 0 && edges[2] > t.level) {
                h = terrain::EdgeHeight(wx, wz, edges[2], true);
            }
            if (z == n && edges[3] > t.level) {
                h = terrain::EdgeHeight(wx, wz, edges[3], true);
            }
            double        nx = terrain::Height(wx - 8, wz) - terrain::Height(wx + 8, wz);
            double        nz = terrain::Height(wx, wz - 8) - terrain::Height(wx, wz + 8);
            double        length = std::sqrt(nx * nx + 256 + nz * nz);
            gfx::Vertex3D v{};
            v.position[0] = float(x * step);
            v.position[1] = float(h) - 0.04f;
            low = std::min(low, v.position[1]);
            high = std::max(high, v.position[1]);
            v.position[2] = float(z * step);
            v.normal[0] = float(nx / length);
            v.normal[1] = float(16 / length);
            v.normal[2] = float(nz / length);
            v.uv[0] = float((std::fmod(t.left(), 200) + x * step) / 200);
            v.uv[1] = float((std::fmod(t.top(), 200) + z * step) / 200);
            double slope = 1 - 16 / length;
            double rock = std::clamp(std::max((h - 900) / 2200, (slope - .18) * 3), 0.0, 1.0);
            double snow = std::clamp((h - 8500 + 900 * terrain::Noise(wx / 900, wz / 900)) / 2600, 0.0, 1.0);
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
    return {gfx::CreateMesh(vertices, indices), hasSnow ? gfx::CreateMesh(snowVertices, indices) : gfx::kNullMesh, edges, low, high};
}

void Update(double x, double z, int minimumLevel) {
    auto perfStart = std::chrono::steady_clock::now();
    ++cacheClock;
    // Camera altitude determines detail and the distance between updates.
    // A complete previous selection stays visible until its replacement is ready.
    double quantum = std::ldexp(256.0, minimumLevel);
    double cx = std::floor(x / quantum) * quantum, cz = std::floor(z / quantum) * quantum;
    int    built = 0;
    bool   planned = false;
    // A walking teleport must never display a distant coarse triangle over
    // the destination's collision surface. Finish this one-time load before
    // presenting it; ordinary travel and editor panning keep the 2 ms budget.
    if (GameMode == ED_MODE_WALK && minimumLevel == 0 && !active.empty() &&
        (std::abs(cx - lastX) > 4096 || std::abs(cz - lastZ) > 4096)) {
        if(std::getenv("DC_TERRAIN_TRACE"))std::fprintf(stderr,"terrain: loading teleport destination %.2f %.2f before display\n",x,z);
        active.clear();
        pending.clear();
        pendingCursor = 0;
        lastX = lastZ = 1e30;
    }
    if(!pending.empty() && minimumLevel!=lastLevel){pending.clear();pendingCursor=0;}

    if (pending.empty() && (cx != lastX || cz != lastZ || minimumLevel != lastLevel)) {
        lastX = cx;
        lastZ = cz;
        lastLevel = minimumLevel;
        auto               tiles = terrain::Select(cx, cz, minimumLevel);
        terrain::Selection selection;
        for (auto t : tiles) {
            selection.emplace(t, 0);
        }
        pending.reserve(tiles.size());
        for (auto t : tiles) {
            pending.push_back({t, Edges(t, selection)});
        }
        pendingCursor = 0;
        planned = true;
    }
    bool initial = active.empty();
    // Bound terrain work during movement. Meshes are cached with their seam
    // topology, so revisiting a boundary does not destroy and recreate them.
    while (pendingCursor < pending.size()) {
        if (!initial && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perfStart).count() >= 2.0) {
            break;
        }
        const auto &key = pending[pendingCursor++];
        auto        it = cache.find(key);
        if (it == cache.end()) {
            it = cache.emplace(key, CachedChunk{Build(key.first, key.second), cacheClock}).first;
            ++built;
        }
        it->second.used = cacheClock;
    }
    if (!pending.empty() && pendingCursor == pending.size()) {
        active.swap(pending);
        pending.clear();
        pendingCursor = 0;
    }
    // Protect both complete coverage and the selection being prepared.
    for (auto &key : active) {
        cache.at(key).used = cacheClock;
    }
    for (auto &key : pending) {
        if (auto it = cache.find(key); it != cache.end()) {
            it->second.used = cacheClock;
        }
    }
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
            cache.erase(it);
        }
    }
    double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perfStart).count();
    if (std::getenv("DC_TERRAIN_PERF")) {
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

bool TownTerrainRequestWalk(float x,float z) {
    if(!TownTerrainEnabled(pEditGround))return true;
    walkRequested=true;
    for(float dx:{-96.f,0.f,96.f})for(float dz:{-96.f,0.f,96.f}) {
        terrain::Tile tile{int64_t(std::floor((world::X(x)+dx)/terrain::BaseSize)),int64_t(std::floor((world::Z(z)+dz)/terrain::BaseSize)),0};
        if(std::none_of(active.begin(),active.end(),[&](const auto& key){return key.first==tile;}))return false;
    }
    walkRequested=false;
    if(std::getenv("DC_TERRAIN_TRACE"))std::fprintf(stderr,"terrain: landing ready %.2f %.2f lod 0\n",x,z);
    return true;
}
void TownTerrainCancelWalk(){walkRequested=false;}

void TownTerrainDraw(CEditGround *ground) {
    if (!TownTerrainEnabled(ground)) {
        ClearChunks();
        return;
    }
    if (WorldGeoramaFoundations(ground)) {
        ClearChunks();
    }
    WorldGeoramaTest(ground);
    world::TestTick();
    // Prepare is idempotent; map loaders may reuse the same arena address.
    Prepare(ground);
    static int testTicks = 0;
    ++testTicks;
    if (testTicks == 120 && Chara) {
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
    if (GameMode == ED_MODE_GEORAMA && ECursorFrame) {
        pos[0] = ECursorFrame->position[0];
        pos[2] = ECursorFrame->position[2];
    }
    int minimumLevel = 0;
    if (GameMode == ED_MODE_GEORAMA || GameMode == ED_MODE_GEORAMA_MENU_INIT || GameMode == ED_MODE_GEORAMA_MENU) {
        minimumLevel = std::clamp(int(std::floor(std::log2(std::max(1.f, EditCamera.GetDistance() / 8000.f)))), 0, terrain::MaxLevel - 1);
    }
    if(walkRequested)minimumLevel=0;
    Update(world::X(pos[0]), world::Z(pos[2]), minimumLevel);
    if(std::getenv("DC_WORLD_PRECISION_TRACE")) {
        float eye[4]{},target[4]{};
        if(NowCamera){NowCamera->GetPos(eye);NowCamera->GetRef(target);}
        std::fprintf(stderr,"precision camera: tick %d player %.9g %.9g eye %.9g %.9g ref %.9g %.9g\n",testTicks,pos[0],pos[2],eye[0],eye[2],target[0],target[2]);
    }
    if (std::getenv("DC_TERRAIN_TRACE") && testTicks % 120 == 0) {
        std::fprintf(stderr,"terrain surface: %.5f foundations %zu\n",world::SurfaceHeight(pos[0],pos[2]),terrain::foundations.size());
        std::fprintf(stderr, "terrain: player %.2f %.2f %.2f ground %.2f far %.0f\n", world::X(pos[0]), pos[1], world::Z(pos[2]), world::Height(pos[0], pos[2]), mgRenderInfo.far[2]);
    }
    auto                      *texture = TexManager.GetTexture(const_cast<char *>("e01b01"), -1);
    auto                       binding = PortTextureFromCTexture(texture).binding;
    auto                       rockBinding = PortTextureFromCTexture(TexManager.GetTexture(const_cast<char *>("e01b06"), -1)).binding;
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
    state.blend = false;
    state.cull = gfx::CullMode::None;
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
        gfx::DrawMesh(chunk.mesh, 0, terrain::Resolution * terrain::Resolution * 6, constants, binding, state, &transform);
        constants.flags |= gfx::kMeshVertexColor | gfx::kMeshWorldUv;
        constants.world_uv[0][0] = 1.f / 240;
        constants.world_uv[0][3] = float(std::fmod(tile.left(), 240) / 240);
        constants.world_uv[1][2] = 1.f / 240;
        constants.world_uv[1][3] = float(std::fmod(tile.top(), 240) / 240);
        auto overlay = state;
        overlay.blend = true;
        overlay.alpha = {};
        overlay.depth_write = false;
        overlay.depth_test = gfx::DepthTest::GEqual;
        gfx::DrawMesh(chunk.mesh, 0, terrain::Resolution * terrain::Resolution * 6, constants, rockBinding, overlay, &transform);
        if (chunk.snow) {
            gfx::DrawMesh(chunk.snow, 0, terrain::Resolution * terrain::Resolution * 6, constants, snowBinding, overlay, &transform);
        }
    }
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
