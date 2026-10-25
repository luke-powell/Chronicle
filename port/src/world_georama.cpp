#ifdef PORT
#include "gfx/capture.hpp"
#endif
#include "world_origin.hpp"
#include "world_georama.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "draw3d.hpp"
#include "editarea.hpp"
#include "editatra.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editloop3.hpp"
#include "editmapscript.hpp"
#include "editpartsdata.hpp"
#include "editpartsinfo.hpp"
#include "framevu1.hpp"
#include "mglib.hpp"
#include "renderinfo.hpp"
#include "savedata.hpp"
#include "terrain_kernel.hpp"
#include "texture.hpp"
#include "town_terrain.hpp"
#include "visualvu1.hpp"

bool                   WorldGeoramaLoading = false;
CMapParts             *WorldGeoramaDrawingRoad = nullptr;
static unsigned        foundationRevision = 0;
static constexpr int   MayorHouse = 22, MayorPlatform = 23;
static constexpr float PlatformTop = 47.424f;

bool WorldGeoramaCustom(CEditGround *ground, int plot) { return TownTerrainEnabled(ground) && (plot == MayorHouse || plot == MayorPlatform); }

void WorldGeoramaRegister(CSaveData *save, CEditGround *ground) {
    if (!TownTerrainEnabled(ground)) {
        return;
    }
    world::Reset();
    WorldGeoramaBuildPlatform(ground);
    terrain::Foundations().clear();
    terrain::Roads().clear();
    ++foundationRevision;
    bool                      migrate = !save->GetEditPartsInfo(0, MayorPlatform)->obtained;
    static EPARTS_INFO_HEADER headers[2];
    static unsigned char      cells[2][49];
    for (int k = 0; k < 2; k++) {
        int   plot = MayorHouse + k;
        auto &def = EditPartsData[0].parts[plot];
        def = {};
        def.max = 1;
        def.tex_no = k ? 12 : 0;
        def.msg_no = plot;
        EditElementData[0][plot] = {def.tex_no, plot, 0};
        for (auto &e : def.elements) {
            e.id = -1;
            e.required_element = -1;
            e.npc_no = -1;
        }
        auto &h = headers[k];
        h = {};
        h.width = k ? 7 : 3;
        h.height = k ? 7 : 2;
        h.cell = cells[k];
        std::fill_n(cells[k], 49, 0x80);
        std::fill_n(h.element_id, 6, -1);
        auto &info = ground->parts_info->parts[plot];
        info = {};
        info.parts_no = plot;
        info.obtained = 1;
        info.stock = 1;
        info.tex_no = def.tex_no;
        info.width = h.width;
        info.height = h.height;
        info.header = &h;
        info.completion_flags = 1;
        for (auto &e : info.elements) {
            e.id = -1;
            e.required_element = -1;
            e.npc_no = -1;
        }
        auto &p = ground->plot_parts[plot];
        p = ground->fixed_parts[5 + k];
        p.handle = p.parts_no = p.def_parts_no = plot;
        p.info = &info;
        p.area = -1;
        p.unit_size = 100;
        p.subtype = 0;
        p.category_no = 1;
        p.draw_on = 1;
        p.SetRotY(0);
        p.SetPosition(0, 0, 0);
        p.draw_distance = 0;
        if (k) WorldGeoramaOffsetPlatform(p);
        ground->fixed_parts[5 + k].draw_on = 0;
        // Keep fixed house as the event-system anchor, but collision belongs to
        // its movable copy. The platform's old stairs are drawn with the new piece.
        ground->fixed_parts[5 + k].collision_frame = nullptr;
        ground->fixed_parts[5 + k].camera_frame = nullptr;
        auto *saved = save->GetEditPartsInfo(0, plot);
        saved->obtained = 1;
        saved->stock = 1;
        saved->completion_flags = 1;
    }
    if (migrate) {
        int   count;
        auto *parts = save->GetParts(0, &count);
        if (count <= 126) {
            parts[count++] = {MayorPlatform, 0, 0, 0, 700};
            parts[count++] = {MayorHouse, 0, 0, PlatformTop, 792.740f};
            parts[count].part_id = -1;
        }
    }
}

void WorldGeoramaLoaded(CSaveData *save, CEditGround *ground) {
    if (!TownTerrainEnabled(ground)) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        auto &event = EditMapInfo->work.events.points[i];
        if (event.map_object == &ground->fixed_parts[5]) {
            event.parts_no = MayorHouse;
            event.map_object = &ground->plot_parts[MayorHouse];
        }
    }
    for (int i = 0; i < ground->people_count; i++) {
        if (ground->people[i]->parts == &ground->fixed_parts[5]) {
            ground->people[i]->parts = &ground->plot_parts[MayorHouse];
        }
    }
    WorldGeoramaFoundations(ground);
    if (std::getenv("DC_TEST_LOAD_SLOT")) {
        for (auto &p : ground->parts) {
            if (p.handle >= 0) {
                std::fprintf(stderr, "loaded placement %d at %.3f %.3f %.3f\n", p.parts_no, p.pos[0], p.pos[1], p.pos[2]);
                if(std::getenv("DC_GEORAMA_TRACE"))std::fprintf(stderr,"plot bounds %d grid %d %d local %.1f %.1f %.1f %.1f rotation %d\n",p.parts_no,p.GetWidth(),p.GetHeight(),p.bound.min[0],p.bound.max[0],p.bound.min[2],p.bound.max[2],p.rot_y);
            }
        }
    }
}

void WorldGeoramaDrawRoad(const Draw3DVisual &source, const float model[4][4], const RenderInfo &info, int program) {
    gfx::capture::Scope profile("world.georama.road_drape");
    auto *owner=WorldGeoramaDrawingRoad;
    auto global=owner?world::PositionOf(owner->pos):world::Position{world::X(model[3][0]),0,world::Z(model[3][2])};
    const double wx=global.x+(owner?double(model[3][0])-owner->pos[0]:0);
    const double wz=global.z+(owner?double(model[3][2])-owner->pos[2]:0);
    auto height = [&](double x,double z){return terrain::SurfaceHeight(wx+x,wz+z);};
    // Meshes are object-relative. Origin shifts only change their draw matrix,
    // not their drape or stable instance identity.
    using Key = std::tuple<gfx::MeshHandle, uintptr_t, double, double, float, float>;
    static std::map<Key, Draw3DVisual> cache;
    static unsigned                    revision = ~0u;
    if (revision != foundationRevision) {
        for (auto &[key, v] : cache) {
            gfx::DestroyMesh(v.mesh);
        }
        cache.clear();
        revision = foundationRevision;
    }
    Key key{source.mesh,reinterpret_cast<uintptr_t>(owner),global.x,global.z,model[0][0],model[0][2]};
    auto [it, inserted] = cache.try_emplace(key);
    auto &out = it->second;
    if (inserted) {
        gfx::capture::Scope buildProfile("world.georama.road_build");
        if(std::getenv("DC_TERRAIN_TRACE"))std::fprintf(stderr,"road cache: build %.3f %.3f revision %u\n",global.x,global.z,foundationRevision);
        out.vertex_colour = source.vertex_colour;
        for (auto strip : source.strips) {
            unsigned end = strip.first_index + strip.index_count, begin = strip.first_index;
            strip.first_index = unsigned(out.indices.size());
            for (unsigned t = begin; t + 2 < end; t += 3) {
                gfx::Vertex3D v[3];
                for (int k = 0; k < 3; k++) {
                    v[k] = source.vertices[source.indices[t + k]];
                    float p[3];
                    std::copy_n(v[k].position, 3, p);
                    for (int a = 0; a < 3; a++) {
                        v[k].position[a] = model[0][a] * p[0] + model[1][a] * p[1] + model[2][a] * p[2];
                    }
                }
                double longest = 0;
                for (int k = 0; k < 3; k++) {
                    longest = std::max(longest, std::hypot(double(v[k].position[0] - v[(k + 1) % 3].position[0]), double(v[k].position[2] - v[(k + 1) % 3].position[2])));
                }
                int  n = std::clamp(int(std::ceil(longest / 12)), 1, 64);
                auto emit = [&](int i, int j) {
                    float         w[3] = {1.f - float(i + j) / n, float(i) / n, float(j) / n};
                    gfx::Vertex3D a = v[0];
                    for (int c = 0; c < 3; c++) {
                        a.position[c] = w[0] * v[0].position[c] + w[1] * v[1].position[c] + w[2] * v[2].position[c];
                        a.normal[c] = c == 1 ? 1 : 0;
                    }
                    for (int c = 0; c < 2; c++) {
                        a.uv[c] = w[0] * v[0].uv[c] + w[1] * v[1].uv[c] + w[2] * v[2].uv[c];
                    }
                    a.position[1] = float(height(a.position[0], a.position[2])) + .25f;
                    out.indices.push_back(unsigned(out.vertices.size()));
                    out.vertices.push_back(a);
                };
                for (int i = 0; i < n; i++) {
                    for (int j = 0; j < n - i; j++) {
                        emit(i, j);
                        emit(i + 1, j);
                        emit(i, j + 1);
                        if (i + j < n - 1) {
                            emit(i + 1, j);
                            emit(i + 1, j + 1);
                            emit(i, j + 1);
                        }
                    }
                }
            }
            strip.index_count = unsigned(out.indices.size()) - strip.first_index;
            out.strips.push_back(strip);
        }
        // Refine where a road triangle crosses a terrain triangle's slope
        // change. Uniform subdivision alone can cut into a foundation edge.
        auto vertices = std::move(out.vertices);
        auto indices = std::move(out.indices);
        auto midpoint = [&](const gfx::Vertex3D &a, const gfx::Vertex3D &b) {
            auto m = a;
            for (int c = 0; c < 3; c++) {
                m.position[c] = (a.position[c] + b.position[c]) * .5f;
            }
            for (int c = 0; c < 2; c++) {
                m.uv[c] = (a.uv[c] + b.uv[c]) * .5f;
            }
            m.position[1] = float(height(m.position[0], m.position[2])) + .25f;
            return m;
        };
        auto refine = [&](auto &&self, const gfx::Vertex3D &a, const gfx::Vertex3D &b, const gfx::Vertex3D &c, int depth) -> void {
            auto   ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            double error = std::max({std::abs(double(ab.position[1] - (a.position[1] + b.position[1]) * .5f)),
                                     std::abs(double(bc.position[1] - (b.position[1] + c.position[1]) * .5f)),
                                     std::abs(double(ca.position[1] - (c.position[1] + a.position[1]) * .5f)),
                                     std::abs((a.position[1] + b.position[1] + c.position[1]) / 3. - .25 - height((a.position[0] + b.position[0] + c.position[0]) / 3., (a.position[2] + b.position[2] + c.position[2]) / 3.))});
            if (error > .1 && depth < 6) {
                self(self, a, ab, ca, depth + 1);
                self(self, ab, b, bc, depth + 1);
                self(self, ca, bc, c, depth + 1);
                self(self, ab, bc, ca, depth + 1);
            } else {
                for (auto v : {a, b, c}) {
                    float  x = v.position[0], z = v.position[2];
                    double nx = height(x - 1, z) - height(x + 1, z), nz = height(x, z - 1) - height(x, z + 1), len = std::sqrt(nx * nx + 4 + nz * nz);
                    v.normal[0] = float(nx / len);
                    v.normal[1] = float(2 / len);
                    v.normal[2] = float(nz / len);
                    out.indices.push_back(unsigned(out.vertices.size()));
                    out.vertices.push_back(v);
                }
            }
        };
        for (auto &strip : out.strips) {
            unsigned begin = strip.first_index, end = begin + strip.index_count;
            strip.first_index = unsigned(out.indices.size());
            for (unsigned i = begin; i + 2 < end; i += 3) {
                refine(refine, vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]], 0);
            }
            strip.index_count = unsigned(out.indices.size()) - strip.first_index;
        }
        if (std::getenv("DC_GEORAMA_TRACE")) {
            double maxGap = 0;
            for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
                double x = 0, y = 0, z = 0;
                for (int k = 0; k < 3; k++) {
                    auto &p = out.vertices[out.indices[i + k]].position;
                    x += p[0] / 3.;
                    y += p[1] / 3.;
                    z += p[2] / 3.;
                }
                maxGap = std::max(maxGap, std::abs(y - height(x, z) - .25));
            }
            std::fprintf(stderr, "road drape: %zu triangles max surface error %.5f\n", out.indices.size() / 3, maxGap);
        }
        out.mesh = gfx::CreateMesh(out.vertices, out.indices);
    }
    sceVu0FMATRIX identity;
    sceVu0UnitMatrix(identity);
    identity[3][0]=model[3][0];identity[3][2]=model[3][2];
    auto *saved = WorldGeoramaDrawingRoad;
    WorldGeoramaDrawingRoad = nullptr;
    Draw3DDrawVisual(out, identity, info, program);
    WorldGeoramaDrawingRoad = saved;
}

bool WorldGeoramaOutside(CEditGround *ground, float x, float z) {
    if (!TownTerrainEnabled(ground) || !ground->areas[0]) {
        return false;
    }
    if (!ground->areas[0]->CheckArea(x, 0, z)) {
        return true;
    }
    for (auto &p : ground->parts) {
        if (p.handle >= 0 && p.parts_no == MayorPlatform) {
            float hx = 350, hz = 350;
            if (std::abs(x - p.pos[0]) <= hx && std::abs(z - p.pos[2]) <= hz) {
                return true;
            }
        }
    }
    return false;
}

float WorldGeoramaHeight(float x, float z) {
#ifdef PORT
    gfx::capture::Scope profile("world.editor.exact_height");
#endif
    float h = float(world::SurfaceHeight(x, z));
    if (TownTerrainEnabled(pEditGround)) {
        for (auto &p : pEditGround->parts) {
            if (p.handle < 0 || p.parts_no != MayorPlatform) {
                continue;
            }
            float surface;
            if(WorldGeoramaPlatformHeight(p,x,z,surface,true))h=std::max(h,surface);
        }
    }
    return h;
}

namespace {
void Footprint(CMapParts &part, int rotation, double &hx, double &hz) {
    hx = std::max(1, part.GetWidth()) * 50.;
    hz = std::max(1, part.GetHeight()) * 50.;
    // GetWidth/GetHeight already account for the object's current rotation.
    if ((rotation - part.rot_y) & 1) {
        std::swap(hx, hz);
    }
}

void Snap(CMapParts &part, int rotation, float &x, float &z) {
    double hx, hz;
    Footprint(part, rotation, hx, hz);
    x = float(std::floor(x / 100) * 100 + std::fmod(hx, 100));
    z = float(std::floor(z / 100) * 100 + std::fmod(hz, 100));
}

bool OnPlatform(CMapParts &support, float x, float z, double hx = 0, double hz = 0) {
    for(int ix=-1;ix<=1;ix++)for(int iz=-1;iz<=1;iz++) {
        float surface;
        if(!WorldGeoramaPlatformHeight(support,x+float(ix*hx),z+float(iz*hz),surface,false) || surface-support.pos[1]<40)return false;
    }
    return true;
}

float SupportHeight(CEditGround *ground, int plot, float x, float z, int rotation) {
    double hx, hz;
    Footprint(ground->plot_parts[plot], rotation, hx, hz);
    if (plot != MayorPlatform) {
        for (auto &p : ground->parts) {
            if (p.handle >= 0 && p.parts_no == MayorPlatform && OnPlatform(p, x, z, hx, hz)) {
                return p.pos[1] + PlatformTop;
            }
        }
    }
    return float(world::Height(x, z));
}

// Use retail's connector selection on a temporary local grid. World objects remain
// in the native save array; this grid never limits their global coordinates.
void Connections(CEditGround *ground, float x, float z) {
    CEditArea local;
    local.SetMapInfo(0, 1);
    local.SetSize(16, 16, 100, 10);
    local.SetOffset(float(std::floor(x / 100) * 100 - 800), 0, float(std::floor(z / 100) * 100 - 800));
    for (int i = 0; i < 128; i++) {
        auto &p = ground->parts[i];
        if (p.handle >= 0 && local.CheckArea(p.pos[0], 0, p.pos[2])) {
            local.SetMapParts(i, ground->parts, p.pos[0] + 1, p.pos[1], p.pos[2] + 1, p.rot_y);
        }
    }
    auto *previous = ground->areas[1];
    ground->areas[1] = &local;
    for (auto &p : ground->parts) {
        if (p.handle < 0 || p.area != -1 || std::abs(p.pos[0] - x) > 210 || std::abs(p.pos[2] - z) > 210) {
            continue;
        }
        if (p.subtype == MAP_PARTS_SUBTYPE_ROAD) {
            ground->SetRoadParts(p.pos[0] + 1, p.pos[1], p.pos[2] + 1, 0, 0);
        }
        if (p.subtype == MAP_PARTS_SUBTYPE_RIVER) {
            ground->SetRiverParts(p.pos[0] + 1, p.pos[1], p.pos[2] + 1, 0, 0);
        }
    }
    ground->areas[1] = previous;
}

bool Fits(CEditGround *ground, int plot, float x, float z, int rotation) {
    if (plot < 0 || plot >= 24 || !ground->plot_parts || !ground->parts_info) {
        return false;
    }
    auto &src = ground->plot_parts[plot];
    auto *info = ground->parts_info->GetPartsInfo(plot);
    if (src.handle < 0 || !info || !info->obtained || info->placed >= info->stock) {
        return false;
    }
    double hx, hz;
    Footprint(src, rotation, hx, hz);
    for (auto &p : ground->parts) {
        if (p.handle < 0) {
            continue;
        }
        if (plot != MayorPlatform && p.parts_no == MayorPlatform && OnPlatform(p, x, z, hx, hz)) {
            continue;
        }
        double px, pz;
        Footprint(p, p.rot_y, px, pz);
        if (std::abs(x - p.pos[0]) < hx + px - .1 && std::abs(z - p.pos[2]) < hz + pz - .1) {
            return false;
        }
    }
    return true;
}
} // namespace

int WorldGeoramaFind(CEditGround *ground, float x, float z) {
    // Buildings take selection priority; exposed platform edges select the support.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 128; i++) {
            auto &p = ground->parts[i];
            if (p.handle < 0) {
                continue;
            }
            if ((p.parts_no == MayorPlatform) != (pass == 1)) {
                continue;
            }
            double hx, hz;
            Footprint(p, p.rot_y, hx, hz);
            if (std::abs(x - p.pos[0]) <= hx && std::abs(z - p.pos[2]) <= hz) {
                return i;
            }
        }
    }
    return -1;
}

int WorldGeoramaPlace(CEditGround *ground, int plot, float x, float z, int rotation, float savedY) {
    if (plot < 0 || plot >= 24 || !ground->plot_parts || ground->CheckEffect()) {
        return -1;
    }
    auto &src = ground->plot_parts[plot];
    if (WorldGeoramaLoading) {
        x -= 1;
        z -= 1;
    } else {
        Snap(src, rotation, x, z);
    }
    if (!WorldGeoramaLoading && !Fits(ground, plot, x, z, rotation)) {
        return -1;
    }
    for (int i = 0; i < 128; i++) {
        auto &p = ground->parts[i];
        if (p.handle >= 0) {
            continue;
        }
        // Matches retail's template-copy placement, retaining the destination vtable.
        world::Forget(p.pos);
        p = src;
        p.SetRotY(rotation);
        p.area = -1;
        p.unit_size = 100;
        p.SetPosition(x, WorldGeoramaLoading ? savedY : SupportHeight(ground, plot, x, z, rotation), z);
        ground->parts_info->parts[plot].placed++;
        Connections(ground, x, z);
        ground->MakePartsBox();
        std::fprintf(stderr, "georama: placed plot %d slot %d at %.1f %.1f %.1f\n", plot, i, x, p.pos[1], z);
        return i;
    }
    return -1;
}

int WorldGeoramaDelete(CEditGround *ground, int *plot, int *rotation, float x, float z) {
    int i = WorldGeoramaFind(ground, x, z);
    if (i < 0) {
        return -1;
    }
    auto &p = ground->parts[i];
    *plot = p.parts_no;
    *rotation = p.rot_y;
    if (p.parts_no == MayorPlatform) {
        for (auto &other : ground->parts) {
            if (other.handle >= 0 && other.parts_no != MayorPlatform && OnPlatform(p, other.pos[0], other.pos[2])) {
                return -1;
            }
        }
    }
    if (auto *info = ground->parts_info->GetPartsInfo(*plot)) {
        info->placed = std::max(0, info->placed - 1);
    }
    p.Initialize();
    p.handle = -1;
    Connections(ground, x, z);
    ground->MakePartsBox();
    return i;
}

void WorldGeoramaPreview(CEditGround *ground, int plot, float *position, float *model, int rotation, float *angles, int pass) {
    if (plot < 0 || plot >= 24 || !ground->plot_parts) {
        return;
    }
    auto &src = ground->plot_parts[plot];
    if (src.handle < 0) {
        return;
    }
    float x = position[0], z = position[2];
    Snap(src, rotation, x, z);
    float y = SupportHeight(ground, plot, x, z, rotation);
    model[0] = x;
    model[1] = y + 5;
    model[2] = z;
    model[3] = 1;
    bool          fits = Fits(ground, plot, x, z, rotation);
    sceVu0FVECTOR ambient;
    MGGetAmbient(ambient);
    if (!fits) {
        sceVu0FVECTOR red = {110, 25, 25, 128};
        MGSetAmbient(red);
    }
    if (pass == ground->cursor.area) {
        ground->cursor.unit_size = 100;
        ground->cursor.Draw(model, src.GetWidth(), src.GetHeight());
    }
    if (src.category_no == pass && src.frame[0]) {
        auto preview = src;
        preview.SetRotY(rotation);
        preview.SetPosition(model);
        float distance[4] = {0, 0, 0, 0};
        int   lod = 0;
        preview.DrawLOD(distance, 0, 0, &lod);
    }
    MGSetAmbient(ambient);
}

void WorldGeoramaUnlock(CSaveData *save, CEditGround *ground) {
    if (!save) {
        return;
    }
    int unlocked = 0, attachments = 0;
    for (int town = 0; town < 6; town++) {
        auto *chips = save->GetElemData(town);
        int   available[100]{};
        for (int i = 0; i < 128; i++) {
            if (chips[i] >= 0 && chips[i] < 100) {
                available[chips[i]]++;
            }
        }
        for (int plot = 0; plot < 24; plot++) {
            auto *def = GetEditAtraPartsData(town, plot);
            auto *saved = save->GetEditPartsInfo(town, plot);
            if (!def || !saved || def->max <= 0) {
                continue;
            }
            saved->obtained = 1;
            saved->stock = std::max<int>(saved->stock, def->max);
            ++unlocked;
            for (int k = 0; k < 6; k++) {
                int id = def->elements[k].id;
                if (id < 0 || id >= 100 || saved->element_enabled[k]) {
                    continue;
                }
                if (available[id] > 0) {
                    --available[id];
                    continue;
                }
                if (save->AtraChipGet(town, id)) {
                    ++attachments;
                }
            }
        }
    }
    if (ground && ground->parts_info) {
        for (int plot = 0; plot < 24; plot++) {
            auto *saved = save->GetEditPartsInfo(ground->map_no, plot);
            auto *info = ground->parts_info->GetPartsInfo(plot);
            if (saved && info) {
                info->obtained = saved->obtained;
                info->stock = std::max(info->stock, int(saved->stock));
            }
        }
    }
    std::fprintf(stderr, "georama: unlocked %d plots across six catalogs, added %d attachment pieces\n", unlocked, attachments);
}

bool WorldGeoramaFoundations(CEditGround *ground) {
    std::vector<terrain::Foundation> next;
    std::vector<CMapParts *>         roadParts;
    for (auto &p : ground->parts) {
        if (p.handle >= 0 && p.area == -1 && p.subtype == MAP_PARTS_SUBTYPE_ROAD) {
            roadParts.push_back(&p);
        }
    }
    static std::vector<std::pair<double, double>> lastRoads;
    std::vector<std::pair<double, double>>        roadPositions;
    for (auto *p : roadParts) {
        auto global=world::PositionOf(p->pos);
        roadPositions.emplace_back(global.x,global.z);
    }
    for (int i = 0; i < 128; i++) {
        auto &p = ground->parts[i];
        if (p.handle < 0 || p.area != -1 || p.subtype == MAP_PARTS_SUBTYPE_ROAD) {
            continue;
        }
        bool supported = false;
        for (auto &platform : ground->parts) {
            if (platform.handle >= 0 && platform.parts_no == MayorPlatform && p.parts_no != MayorPlatform && OnPlatform(platform, p.pos[0], p.pos[2])) {
                supported = true;
            }
        }
        if (supported) {
            continue;
        }
        double hx, hz;
        Footprint(p, p.rot_y, hx, hz);
        double y = ground->effect_parts_id == i && ground->effect_count > 0 ? ground->effect_target_alt : p.pos[1];
        auto global = world::PositionOf(p.pos);
        next.push_back({global.x, global.z, hx, hz, y});
    }
    bool changed = next.size() != terrain::Foundations().size() || roadPositions != lastRoads;
    if (!changed) {
        for (size_t i = 0; i < next.size(); i++) {
            auto &a = next[i];
            auto &b = terrain::Foundations()[i];
            changed |= a.x != b.x || a.z != b.z || a.halfX != b.halfX || a.halfZ != b.halfZ || a.height != b.height;
        }
    }
    if (changed) {
        terrain::Foundations() = std::move(next);
        terrain::Roads().clear();
        lastRoads = roadPositions;
        ++foundationRevision;
        std::vector<double>           heights(roadParts.size());
        std::vector<bool>             anchors(roadParts.size());
        std::vector<std::vector<int>> neighbors(roadParts.size());
        for (size_t i = 0; i < roadParts.size(); i++) {
            double sum = 0;
            int    hits = 0;
            for (auto &f : terrain::Foundations()) {
                double d = std::hypot(std::max(0.0, std::abs(roadPositions[i].first - f.x) - f.halfX), std::max(0.0, std::abs(roadPositions[i].second - f.z) - f.halfZ));
                if (d <= 55) {
                    sum += f.height;
                    ++hits;
                }
            }
            anchors[i] = hits > 0;
            heights[i] = hits ? sum / hits : terrain::NaturalHeight(roadPositions[i].first,roadPositions[i].second);
            for (size_t j = 0; j < roadParts.size(); j++) {
                if (i != j && std::abs(std::hypot(roadPositions[i].first-roadPositions[j].first,roadPositions[i].second-roadPositions[j].second) - 100) < 1) {
                    neighbors[i].push_back(int(j));
                }
            }
        }
        // Solve the road network with the adjoining foundations as fixed ends.
        // Isolated networks retain their natural mean height.
        for (int iteration = 0; iteration < 1000; iteration++) {
            double change = 0;
            for (size_t i = 0; i < heights.size(); i++) {
                if (!anchors[i] && !neighbors[i].empty()) {
                    double h = 0;
                    for (int j : neighbors[i]) {
                        h += heights[j];
                    }
                    h /= neighbors[i].size();
                    change = std::max(change, std::abs(h - heights[i]));
                    heights[i] = h;
                }
            }
            if (change < .0001) {
                break;
            }
        }
        for (size_t i = 0; i < roadParts.size(); i++) {
            if (neighbors[i].empty()) {
                terrain::Roads().push_back({roadPositions[i].first, roadPositions[i].second, roadPositions[i].first, roadPositions[i].second, heights[i], heights[i]});
            }
            for (int j : neighbors[i]) {
                if (j > int(i)) {
                    terrain::Roads().push_back({roadPositions[i].first, roadPositions[i].second, roadPositions[j].first, roadPositions[j].second, heights[i], heights[j]});
                }
            }
        }
    }
    for (auto &p : ground->parts) {
        if (p.handle >= 0 && p.area == -1 && p.subtype == MAP_PARTS_SUBTYPE_ROAD) {
            auto global=world::PositionOf(p.pos);
            p.SetPosition(p.pos[0],float(terrain::Height(global.x,global.z)),p.pos[2]);
        }
    }
    for (auto &p : ground->parts) {
        if (p.handle >= 0 && p.parts_no == MayorHouse) {
            ground->fixed_parts[5].SetPosition(p.pos);
            ground->fixed_parts[5].SetRotY(p.rot_y);
        }
    }
    return changed;
}

void WorldGeoramaTest(CEditGround *ground) {
    if (!std::getenv("DC_GEORAMA_TEST")) {
        return;
    }
    static int ticks = 0;
    if (++ticks != 180) {
        return;
    }
    auto check = [](bool ok, const char *what) {if(!ok){std::fprintf(stderr,"georama TEST FAILED: %s\n",what);std::abort();} };
    int  slot = ground->SetMapParts(0, 3001, 0, 3001, 1);
    check(slot >= 0, "remote placement");
    auto &p = ground->parts[slot];
    float x = p.pos[0], z = p.pos[2];
    check(p.area == -1 && p.rot_y == 1, "remote rotation/area");
    check(ground->GetPartsID(x, 0, z) == slot, "remote selection");
    check(ground->SetFocusParts(x, 0, z) == slot, "remote focus");
    check(ground->SetMapParts(0, x, 0, z, 0) < 0, "overlap rejected");
    auto saved = std::make_unique<CSaveData>(*SaveData);
    ground->Save(0, saved.get());
    ground->parts_info->Save(0, saved.get());
    int plot = -1, rot = -99;
    check(ground->DeleteMapParts(&plot, &rot, x, 0, z) == slot && plot == 0 && rot == 1, "remote delete");
    check(ground->GetPartsID(x, 0, z) < 0, "deleted selection cleared");
    ground->Load(0, saved.get());
    slot = ground->GetPartsID(x, 0, z);
    check(slot >= 0 && ground->parts[slot].rot_y == 1, "save/load remote placement");
    check(std::abs(ground->parts[slot].pos[0] - x) < .1 && std::abs(ground->parts[slot].pos[2] - z) < .1, "save/load no position drift");
    WorldGeoramaFoundations(ground);
    check(std::abs(world::Height(x, z) - ground->parts[slot].pos[1]) < .1, "foundation");
    int house = WorldGeoramaFind(ground, 0, 792.74f);
    check(house >= 0 && ground->parts[house].parts_no == MayorHouse, "legacy mayor house migrated");
    int housePlot, houseRotation;
    check(WorldGeoramaDelete(ground, &housePlot, &houseRotation, 0, 792.74f) >= 0, "pick up mayor house");
    int platformPlot, platformRotation;
    check(WorldGeoramaDelete(ground, &platformPlot, &platformRotation, 250, 700) >= 0, "pick up independent platform");
    check(WorldGeoramaPlace(ground, MayorPlatform, 6001, 5001, 1) >= 0, "place rotated platform");
    int platform = WorldGeoramaFind(ground, 6050, 5000);
    check(platform >= 0 && ground->parts[platform].parts_no == MayorPlatform, "find moved platform");
    auto &support = ground->parts[platform];
    int   movedHouse = WorldGeoramaPlace(ground, MayorHouse, 6151, 5001, 1);
    check(movedHouse >= 0, "place house on platform");
    check(std::abs(ground->parts[movedHouse].pos[1] - support.pos[1] - PlatformTop) < .01, "house rests on support");
    float houseY = ground->parts[movedHouse].pos[1];
    ground->Save(0, saved.get());
    ground->parts_info->Save(0, saved.get());
    ground->Load(0, saved.get());
    movedHouse = WorldGeoramaFind(ground, 6150, 5050);
    check(movedHouse >= 0 && ground->parts[movedHouse].parts_no == MayorHouse, "reload moved house");
    check(std::abs(ground->parts[movedHouse].pos[1] - houseY) < .01, "reload preserves support height");
    ED_EVENT_PARAM door{};
    check(EdSearchEvent(&door, const_cast<char *>("i01h06"), 0, NowTime) != 0, "mayor entrance exists");
    check(door.parts_no == MayorHouse && std::abs(door.position[0] - ground->parts[movedHouse].pos[0]) < 150 && std::abs(door.position[2] - ground->parts[movedHouse].pos[2]) < 150, "door follows moved house");
    std::fprintf(stderr, "georama TEST PASS: independent mayor pieces, support, rotation and save/load\n");
    std::fprintf(stderr, "georama TEST PASS: remote place, rotate, select, overlap, delete, native save/load, foundation\n");
}
