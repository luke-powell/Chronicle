#include "town_terrain.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "camerafollow.hpp"
#include "character.hpp"
#include "collision.hpp"
#include "draw3d.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "framevu1.hpp"
#include "mglib.hpp"
#include "renderinfo.hpp"
#include "terrain_kernel.hpp"
#include "texture.hpp"
#include "visualvu1.hpp"

namespace {
struct Chunk {
    gfx::MeshHandle    mesh = 0;
    std::array<int, 4> edges{};
};

std::map<terrain::Tile, Chunk> chunks;
double                         lastX = 1e30, lastZ = 1e30;

void Prepare(CEditGround *ground) {
    // Only the first Norune fixed part contains the enclosing waku cliff meshes.
    // The low faces of waku10 are the Mayor's actual platform and access slope.
    auto walk = [](auto &&self, CFrame *frame) -> void {
        for (; frame; frame = frame->brother) {
            if (std::strcmp(frame->name, "waku10__s") == 0) {
                auto *visual = static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(frame)->visual);
                auto *mesh = visual ? Draw3DFindVisual(visual->vu_data) : nullptr;
                if (mesh && !mesh->indices.empty()) {
                    std::vector<uint32_t> kept;
                    for (auto &strip : mesh->strips) {
                        unsigned first = unsigned(kept.size());
                        for (unsigned i = strip.first_index; i + 2 < strip.first_index + strip.index_count; i += 3) {
                            bool keep = true;
                            for (int k = 0; k < 3; k++) {
                                keep &= mesh->vertices[mesh->indices[i + k]].position[1] < 55;
                            }
                            if (keep) {
                                kept.insert(kept.end(), mesh->indices.begin() + i, mesh->indices.begin() + i + 3);
                            }
                        }
                        strip.first_index = first;
                        strip.index_count = unsigned(kept.size()) - first;
                    }
                    gfx::DestroyMesh(mesh->mesh);
                    mesh->indices = std::move(kept);
                    mesh->mesh = gfx::CreateMesh(mesh->vertices, mesh->indices);
                    std::fprintf(stderr, "terrain: preserved Mayor platform, %zu triangles\n", mesh->indices.size() / 3);
                    mesh->indices.clear();
                }
            } else if (std::strncmp(frame->name, "waku", 4) == 0 || std::strcmp(frame->name, "cube2__f") == 0) {
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
            v.position[2] = float(z * step);
            v.normal[0] = float(nx / length);
            v.normal[1] = float(16 / length);
            v.normal[2] = float(nz / length);
            v.uv[0] = float(x * step / 100);
            v.uv[1] = float(z * step / 100);
            std::fill(std::begin(v.color), std::end(v.color), 255);
            vertices.push_back(v);
        }
    }
    for (int z = 0; z < n; z++) {
        for (int x = 0; x < n; x++) {
            unsigned a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    return {gfx::CreateMesh(vertices, indices), edges};
}

void Update(double x, double z) {
    // Selection only changes at half-chunk intervals; meshes are reused by global key.
    double cx = std::floor(x / 256) * 256, cz = std::floor(z / 256) * 256;
    if (cx == lastX && cz == lastZ) {
        return;
    }
    lastX = cx;
    lastZ = cz;
    auto               tiles = terrain::Select(cx, cz);
    terrain::Selection selection;
    for (auto t : tiles) {
        selection.emplace(t, 0);
    }
    for (auto it = chunks.begin(); it != chunks.end();) {
        if (!selection.contains(it->first)) {
            gfx::DestroyMesh(it->second.mesh);
            it = chunks.erase(it);
        } else {
            ++it;
        }
    }
    int built = 0, maxLevel = 0;
    for (auto t : tiles) {
        maxLevel = std::max(maxLevel, t.level);
        auto edges = Edges(t, selection);
        auto it = chunks.find(t);
        if (it != chunks.end() && it->second.edges == edges) {
            continue;
        }
        if (it != chunks.end()) {
            gfx::DestroyMesh(it->second.mesh);
        }
        chunks[t] = Build(t, edges);
        ++built;
    }
    if (std::getenv("DC_TERRAIN_TRACE")) {
        std::fprintf(stderr, "terrain: focus %.0f %.0f chunks %zu built %d radius %.0f levels 0..%d\n", cx, cz, chunks.size(), built, terrain::Radius, maxLevel);
    }
}
} // namespace

bool TownTerrainEnabled(const CEditGround *ground) { return ground && ground->map_no == TOWN_NORUNE; }

void TownTerrainDraw(CEditGround *ground) {
    if (!TownTerrainEnabled(ground)) {
        for (auto &[key, chunk] : chunks) {
            gfx::DestroyMesh(chunk.mesh);
        }
        chunks.clear();
        lastX = lastZ = 1e30;
        return;
    }
    // Prepare is idempotent; map loaders may reuse the same arena address.
    Prepare(ground);
    static int testTicks = 0;
    ++testTicks;
    if (testTicks == 120 && Chara) {
        if (const char *spawn = std::getenv("DC_TERRAIN_TEST_SPAWN")) {
            float x, z, angle = 0;
            if (std::sscanf(spawn, "%f,%f,%f", &x, &z, &angle) >= 2) {
                Chara->SetPosition(x, float(terrain::Height(x, z)) + 60, z);
                if (NowCamera) {
                    auto *cam = static_cast<CCameraFollow *>(NowCamera);
                    cam->SetFollow(x, float(terrain::Height(x, z)) + 65, z);
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
    Update(pos[0], pos[2]);
    if (std::getenv("DC_TERRAIN_TRACE") && testTicks % 120 == 0) {
        std::fprintf(stderr, "terrain: player %.2f %.2f %.2f ground %.2f far %.0f\n", pos[0], pos[1], pos[2], terrain::Height(pos[0], pos[2]), mgRenderInfo.far[2]);
    }
    auto *texture = TexManager.GetTexture(const_cast<char *>("e01b01"), -1);
    auto  binding = PortTextureFromCTexture(texture).binding;
    binding.filter = gfx::Filter::Linear;
    binding.wrap_u = binding.wrap_v = gfx::Wrap::Repeat;
    RenderInfo info = mgRenderInfo;
    info.shadow_pass = 0;
    // EditLoop extends the shared reverse-depth projection for Norune.
    auto state = Draw3DState(info, false);
    state.blend = false;
    state.cull = gfx::CullMode::None;
    for (auto &[tile, chunk] : chunks) {
        sceVu0FMATRIX model;
        sceVu0UnitMatrix(model);
        model[3][0] = float(tile.left());
        model[3][2] = float(tile.top());
        gfx::MeshConstants constants{};
        gfx::MeshTransform transform;
        Draw3DSceneConstants(constants, info, model, &transform);
        constants.flags = gfx::kMeshLit;
        float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 0};
        Draw3DMaterial(constants, info, white, white, black);
        gfx::DrawMesh(chunk.mesh, 0, terrain::Resolution * terrain::Resolution * 6, constants, binding, state, &transform);
    }
}

int TownTerrainKeepPlatform(CCPoly *polys, int count) {
    int kept = 0;
    for (int i = 0; i < count; i++) {
        bool keep = true;
        for (auto &v : polys[i].vertex) {
            keep &= v[1] < 55 && std::abs(v[0]) < 350 && v[2] >= 499 && v[2] < 900;
        }
        if (keep) {
            polys[kept++] = polys[i];
        }
    }
    return kept;
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
            if (x >= -700 && x + step <= 700 && z >= -700 && z + step <= 500) {
                continue;
            }
            float v[4][4] = {
                {float(x),        float(terrain::Height(x,        z)),        float(z),        1},
                {float(x + step), float(terrain::Height(x + step, z)),        float(z),        1},
                {float(x),        float(terrain::Height(x,        z + step)), float(z + step), 1},
                {float(x + step), float(terrain::Height(x + step, z + step)), float(z + step), 1}
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
