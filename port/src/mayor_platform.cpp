#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "collision.hpp"
#include "draw3d.hpp"
#include "editground.hpp"
#include "framevu1.hpp"
#include "mglib.hpp"
#include "objanime.hpp"
#include "renderinfo.hpp"
#include "texture.hpp"
#include "visualvu1.hpp"
#include "world_georama.hpp"

namespace {
constexpr float     PivotZ = 700, MirrorZ = 775, StairsOffset = 599.65f - PivotZ;
Draw3DVisual        platform;
std::vector<CCPoly> cliffCollision, stairCollision, fenceCollision;

void Matrix(const CMapParts &p, const float *position, float m[4][4]) {
    sceVu0UnitMatrix(m);
    float c = std::cos(p.rotation.y), s = std::sin(p.rotation.y);
    m[0][0] = c;
    m[0][2] = -s;
    m[2][0] = s;
    m[2][2] = c;
    for (int a = 0; a < 3; a++) {
        m[3][a] = (position ? position : p.pos)[a];
    }
}

gfx::Vertex3D Interpolate(const gfx::Vertex3D &a, const gfx::Vertex3D &b, float t) {
    auto v = a;
    for (int i = 0; i < 3; i++) {
        v.position[i] = std::lerp(a.position[i], b.position[i], t);
        v.normal[i] = std::lerp(a.normal[i], b.normal[i], t);
    }
    for (int i = 0; i < 2; i++) {
        v.uv[i] = std::lerp(a.uv[i], b.uv[i], t);
    }
    for (int i = 0; i < 4; i++) {
        v.color[i] = uint8_t(std::lerp(float(a.color[i]), float(b.color[i]), t));
    }
    return v;
}

std::vector<gfx::Vertex3D> Clip(std::vector<gfx::Vertex3D> polygon, int axis, float limit) {
    std::vector<gfx::Vertex3D> result;
    for (size_t i = 0; i < polygon.size(); i++) {
        auto &a = polygon[i];
        auto &b = polygon[(i + 1) % polygon.size()];
        bool  inside = a.position[axis] <= limit, next = b.position[axis] <= limit;
        if (inside) {
            result.push_back(a);
        }
        if (inside != next) {
            result.push_back(Interpolate(a, b, (limit - a.position[axis]) / (b.position[axis] - a.position[axis])));
        }
    }
    return result;
}

using Triangle = std::array<gfx::Vertex3D, 3>;

struct Face {
    Triangle    vertices;
    Draw3DStrip material;
};

void AddFace(Triangle vertices, Draw3DStrip material, bool reverse = false) {
    if (reverse) {
        std::swap(vertices[1], vertices[2]);
    }
    material.first_index = unsigned(platform.indices.size());
    material.index_count = 3;
    for (auto v : vertices) {
        v.position[2] -= PivotZ;
        platform.indices.push_back(unsigned(platform.vertices.size()));
        platform.vertices.push_back(v);
    }
    platform.strips.push_back(material);
}

CCPoly CollisionFace(const gfx::Vertex3D &a, const gfx::Vertex3D &b, const gfx::Vertex3D &c) {
    CCPoly               poly{};
    const gfx::Vertex3D *v[] = {&a, &c, &b};
    for (int i = 0; i < 3; i++) {
        std::copy_n(v[i]->position, 3, poly.vertex[i]);
        poly.vertex[i][3] = 1;
    }
    float u[3], w[3];
    for (int i = 0; i < 3; i++) {
        u[i] = b.position[i] - a.position[i];
        w[i] = c.position[i] - a.position[i];
    }
    for (int i = 0; i < 3; i++) {
        poly.normal[i] = u[(i + 1) % 3] * w[(i + 2) % 3] - u[(i + 2) % 3] * w[(i + 1) % 3];
    }
    float length = std::sqrt(poly.normal[0] * poly.normal[0] + poly.normal[1] * poly.normal[1] + poly.normal[2] * poly.normal[2]);
    if (length > 1e-6f) {
        for (int i = 0; i < 3; i++) {
            poly.normal[i] /= length;
        }
    }
    return poly;
}

bool InBox(const CCPoly &poly, const CBoxVu0 &box) {
    for (int a = 0; a < 3; a++) {
        float low = std::min({poly.vertex[0][a], poly.vertex[1][a], poly.vertex[2][a]});
        float high = std::max({poly.vertex[0][a], poly.vertex[1][a], poly.vertex[2][a]});
        if (high < box.min[a] || low > box.max[a]) {
            return false;
        }
    }
    return true;
}

int TransformPolys(CMapParts &part, CCPoly *out, const CBoxVu0 &box, const std::vector<CCPoly> &collision) {
    sceVu0FMATRIX matrix;
    Matrix(part, nullptr, matrix);
    int count = 0;
    for (const auto &local : collision) {
        CCPoly poly = local;
        for (int k = 0; k < 3; k++) {
            for (int a = 0; a < 3; a++) {
                poly.vertex[k][a] = matrix[3][a] + matrix[0][a] * local.vertex[k][0] + matrix[1][a] * local.vertex[k][1] + matrix[2][a] * local.vertex[k][2];
            }
        }
        if (!InBox(poly, box)) {
            continue;
        }
        for (int a = 0; a < 3; a++) {
            poly.normal[a] = matrix[0][a] * local.normal[0] + matrix[1][a] * local.normal[1] + matrix[2][a] * local.normal[2];
        }
        out[count++] = poly;
    }
    return count;
}
} // namespace

void WorldGeoramaBuildPlatform(CEditGround *ground) {
    gfx::DestroyMesh(platform.mesh);
    platform = {};
    cliffCollision.clear();
    auto *frame = ground->fixed_parts[0].frame[0]->SearchFrame(const_cast<char *>("waku10__s"));
    auto *visual = frame ? static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(frame)->visual) : nullptr;
    auto *source = visual ? Draw3DFindVisual(visual->vu_data) : nullptr;
    if (!source || source->indices.empty()) {
        std::fprintf(stderr, "Mayor platform: original cliff mesh missing\n");
        std::abort();
    }
    // The retail platform collision includes tall boundary walls that ran
    // into the old enclosing cliff. Preserve only the staircase and gate.
    stairCollision.clear();
    fenceCollision.clear();
    auto   &stairs = ground->fixed_parts[6];
    CBoxVu0 stairBox;
    for (int i = 0; i < 3; i++) {
        stairBox.min[i] = -10000;
        stairBox.max[i] = 10000;
    }
    std::array<CCPoly, 4096> originalStairs;
    int                      stairCount = stairs.GetCollisionFrame()->PickUpNearPoly(originalStairs.data(), stairBox);
    for (int i = 0; i < stairCount; i++) {
        auto poly = originalStairs[i];
        bool keep = true;
        for (auto &v : poly.vertex) {
            for (int a = 0; a < 3; a++) {
                v[a] -= stairs.pos[a];
            }
            keep &= std::abs(v[0]) <= 75 && v[2] < 50;
            v[2] += StairsOffset;
        }
        if (keep) {
            stairCollision.push_back(poly);
        }
    }
    std::vector<Face> quadrant;
    // Reuse the finished front/left cliff, including its original irregular top,
    // grass skirt and UVs. The back/right originally joined the enclosing cliff.
    for (auto strip : source->strips) {
        for (unsigned i = strip.first_index; i + 2 < strip.first_index + strip.index_count; i += 3) {
            if (strip.texture_name == "e01b01" || strip.texture_name == "e01b03") {
                continue;
            }
            std::vector<gfx::Vertex3D> polygon;
            for (int k = 0; k < 3; k++) {
                polygon.push_back(source->vertices[source->indices[i + k]]);
            }
            if (std::any_of(polygon.begin(), polygon.end(), [](auto &v) { return v.position[1] >= 55; })) {
                continue;
            }
            polygon = Clip(Clip(std::move(polygon), 0, 0), 2, MirrorZ);
            for (size_t k = 1; k + 1 < polygon.size(); k++) {
                quadrant.push_back({
                    {polygon[0], polygon[k], polygon[k + 1]},
                    strip
                });
            }
        }
    }
    using Point = std::array<int, 3>;
    using Edge = std::pair<Point, Point>;

    struct Boundary {
        int           count = 0;
        gfx::Vertex3D a, b;
    };

    std::map<Edge, Boundary> edges;
    auto                     point = [](const gfx::Vertex3D &v) { return Point{int(std::round(v.position[0] * 1000)), int(std::round(v.position[1] * 1000)), int(std::round(v.position[2] * 1000))}; };
    for (auto &f : quadrant) {
        for (int k = 0; k < 3; k++) {
            auto  a = f.vertices[k], b = f.vertices[(k + 1) % 3];
            auto  pa = point(a), pb = point(b);
            auto &e = edges[pa < pb ? Edge{pa, pb} : Edge{pb, pa}];
            ++e.count;
            e.a = a;
            e.b = b;
        }
    }
    // Close only the original ground-level boundary below the terrain. No ramp
    // or new straight retaining wall is added at the walkable cliff edge.
    for (auto &[key, e] : edges) {
        if (e.count == 1 && e.a.position[1] < 25 && e.b.position[1] < 25) {
            if ((std::abs(e.a.position[0]) < .001f && std::abs(e.b.position[0]) < .001f) ||
                (std::abs(e.a.position[2] - MirrorZ) < .001f && std::abs(e.b.position[2] - MirrorZ) < .001f)) {
                continue;
            }
            auto a = e.a, b = e.b, lowA = a, lowB = b;
            lowA.position[1] = lowB.position[1] = -8;
            Draw3DStrip material;
            material.texture_name = "e01b06";
            material.repeat_texture = true;
            for (auto *v : {&a, &b, &lowA, &lowB}) {
                v->uv[0] = (v->position[0] + v->position[2]) / 200;
                v->uv[1] = v->position[1] / 100;
            }
            quadrant.push_back({
                {b, a, lowA},
                material
            });
            quadrant.push_back({
                {b, lowA, lowB},
                material
            });
            auto center = lowA;
            center.position[0] = 0;
            center.position[2] = MirrorZ;
            quadrant.push_back({
                {lowB, lowA, center},
                material
            });
        }
    }
    for (const auto &f : quadrant) {
        for (int x = 0; x < 2; x++) {
            for (int z = 0; z < 2; z++) {
                auto v = f.vertices;
                for (auto &p : v) {
                    if (x) {
                        p.position[0] = -p.position[0];
                        p.normal[0] = -p.normal[0];
                    }
                    if (z) {
                        p.position[2] = 2 * MirrorZ - p.position[2];
                        p.normal[2] = -p.normal[2];
                    }
                }
                AddFace(v, f.material, (x != z));
            }
        }
    }
    platform.vertex_colour = source->vertex_colour;
    // Reflected normals must agree at the symmetry planes, otherwise the
    // original directional normals create a hard lighting line on the deck.
    for (auto &v : platform.vertices) {
        if (std::abs(v.position[0]) < .01f) {
            v.normal[0] = 0;
        }
        if (std::abs(v.position[2] - (MirrorZ - PivotZ)) < .01f) {
            v.normal[2] = 0;
        }
        float length = std::hypot(v.normal[0], v.normal[1], v.normal[2]);
        if (length > 1e-6f) {
            for (float &n : v.normal) {
                n /= length;
            }
        }
    }
    for (size_t i = 0; i < platform.indices.size(); i += 3) {
        auto poly = CollisionFace(platform.vertices[platform.indices[i]], platform.vertices[platform.indices[i + 1]], platform.vertices[platform.indices[i + 2]]);
        if (std::abs(poly.normal[0]) + std::abs(poly.normal[1]) + std::abs(poly.normal[2]) > .5f) {
            cliffCollision.push_back(poly);
        }
    }
    // Extend the original alpha-textured railing using the same left/front
    // quadrant as the cliff. Keep the wooden staircase and its entrance rails.
    auto  *fenceFrame = ground->fixed_parts[6].frame[0]->SearchFrame(const_cast<char *>("nuki__A7F"));
    auto  *fenceVisual = fenceFrame ? static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(fenceFrame)->visual) : nullptr;
    auto  *fence = fenceVisual ? Draw3DFindVisual(fenceVisual->vu_data) : nullptr;
    size_t firstFenceIndex = platform.indices.size();
    if (fence) {
        std::vector<Face> panels;
        std::vector<Face> endPanel;
        for (auto strip : fence->strips) {
            for (unsigned i = strip.first_index; i + 2 < strip.first_index + strip.index_count; i += 3) {
                std::vector<gfx::Vertex3D> polygon;
                for (int k = 0; k < 3; k++) {
                    polygon.push_back(fence->vertices[fence->indices[i + k]]);
                }
                if (std::any_of(polygon.begin(), polygon.end(), [](auto &v) { return v.position[0] > -65 || v.position[2] < 30; })) {
                    continue;
                }
                if (std::all_of(polygon.begin(), polygon.end(), [](auto &v) { return v.position[0] > -93; })) {
                    endPanel.push_back({
                        {polygon[0], polygon[1], polygon[2]},
                        strip
                    });
                }
                for (auto &v : polygon) {
                    v.position[2] += 599.65f;
                }
                polygon = Clip(std::move(polygon), 2, MirrorZ);
                for (size_t k = 1; k + 1 < polygon.size(); k++) {
                    panels.push_back({
                        {polygon[0], polygon[k], polygon[k + 1]},
                        strip
                    });
                }
            }
        }
        for (auto &f : panels) {
            for (int x = 0; x < 2; x++) {
                for (int z = 0; z < 2; z++) {
                    auto v = f.vertices;
                    for (auto &p : v) {
                        if (x) {
                            p.position[0] = -p.position[0];
                            p.normal[0] = -p.normal[0];
                        }
                        if (z) {
                            p.position[2] = 2 * MirrorZ - p.position[2];
                            p.normal[2] = -p.normal[2];
                        }
                    }
                    AddFace(v, f.material, x != z);
                }
            }
        }
        // Five retail-width panels close the back; there is only one entrance.
        for (auto &f : endPanel) {
            for (int segment = 0; segment < 5; segment++) {
                auto v = f.vertices;
                for (auto &p : v) {
                    float t = (p.position[0] + 92.112f) / (92.112f - 65.605f);
                    p.position[0] = -65.605f + (segment + t) * (131.21f / 5);
                    p.position[2] = 2 * MirrorZ - (599.65f + 36.075f);
                    p.normal[2] = -p.normal[2];
                }
                AddFace(v, f.material, true);
            }
        }
        // Hide only the old perimeter panels, keeping entrance rails intact.
        // Rebuild their CPU/GPU mesh as the short staircase-side subset.
        Draw3DVisual entrance;
        entrance.vertex_colour = fence->vertex_colour;
        for (auto strip : fence->strips) {
            unsigned end = strip.first_index + strip.index_count, begin = strip.first_index;
            strip.first_index = unsigned(entrance.indices.size());
            for (unsigned i = begin; i + 2 < end; i += 3) {
                bool keep = true;
                for (int k = 0; k < 3; k++) {
                    keep &= fence->vertices[fence->indices[i + k]].position[2] < 30;
                }
                if (keep) {
                    for (int k = 0; k < 3; k++) {
                        entrance.indices.push_back(unsigned(entrance.vertices.size()));
                        entrance.vertices.push_back(fence->vertices[fence->indices[i + k]]);
                    }
                }
            }
            strip.index_count = unsigned(entrance.indices.size()) - strip.first_index;
            if (strip.index_count) {
                entrance.strips.push_back(strip);
            }
        }
        gfx::DestroyMesh(fence->mesh);
        entrance.mesh = gfx::CreateMesh(entrance.vertices, entrance.indices);
        *fence = std::move(entrance);
        // The other LODs share the same old perimeter; use the complete near
        // model until they have matching topology too.
        for (int lod = 1; lod < 4; lod++) {
            ground->fixed_parts[6].frame[lod] = ground->fixed_parts[6].frame[0];
        }
    }
    for (size_t i = firstFenceIndex; i + 2 < platform.indices.size(); i += 3) {
        auto poly = CollisionFace(platform.vertices[platform.indices[i]], platform.vertices[platform.indices[i + 1]], platform.vertices[platform.indices[i + 2]]);
        fenceCollision.push_back(poly);
        std::swap(poly.vertex[1], poly.vertex[2]);
        for (int a = 0; a < 3; a++) {
            poly.normal[a] = -poly.normal[a];
        }
        fenceCollision.push_back(poly);
    }
    platform.mesh = gfx::CreateMesh(platform.vertices, platform.indices);
    std::fprintf(stderr, "Mayor platform: original cliff mirrored, %zu triangles; retail wooden stair collision\n", cliffCollision.size());
}

void WorldGeoramaOffsetPlatform(CMapParts &part) {
    std::vector<CFrame *> shifted;
    auto                  shift = [&](CFrame *frame) {
        if (!frame || std::find(shifted.begin(), shifted.end(), frame) != shifted.end()) {
            return;
        }
        shifted.push_back(frame);
        // Keep the retail hierarchy intact. Its root-local pivot is shared by
        // model drawing, effect anchors, lighting and collision queries.
        frame->local[3][2] += StairsOffset;
        frame->world_valid = false;
    };
    for (auto *frame : part.frame) {
        shift(frame);
    }
    shift(part.collision_frame);
    shift(part.camera_frame);
    shift(part.shadow_frame);
    shift(part.shade_frame);
    part.collision_frame = nullptr;
    part.camera_frame = nullptr;
    part.bound.min[2] += StairsOffset;
    part.bound.max[2] += StairsOffset;
    if (std::getenv("DC_GEORAMA_TEST")) {
        auto *root = part.frame[0];
        root->SetPosition(0, 0, 0);
        root->SetRotation(0, 0, 0);
        std::vector<std::pair<EDIT_EFFECT_INFO *, std::array<float, 4>>> anchors;
        for (auto *effect : part.effect) {
            if (effect && effect->frame) {
                std::array<float, 4> local;
                effect->frame->GetWorldPosition(local.data(), effect->offset);
                anchors.push_back({effect, local});
            }
        }
        for (int rotation = 0; rotation < 4; rotation++) {
            part.SetRotY(rotation);
            part.SetPosition(1234, 56, 7890);
            root->SetPosition(part.pos);
            root->SetRotation(0, part.rotation.y, 0);
            sceVu0FMATRIX matrix;
            Matrix(part, nullptr, matrix);
            for (auto &[effect, local] : anchors) {
                sceVu0FVECTOR actual, expected;
                effect->frame->GetWorldPosition(actual, effect->offset);
                sceVu0ApplyMatrix(expected, matrix, local.data());
                for (int axis = 0; axis < 3; axis++) {
                    if (std::abs(actual[axis] - expected[axis]) > .02f) {
                        std::fprintf(stderr, "platform TEST FAILED: effect rotation %d axis %d: %.2f vs %.2f\n", rotation, axis, actual[axis], expected[axis]);
                        std::abort();
                    }
                }
            }
        }
        part.SetRotY(0);
        part.SetPosition(0, 0, 0);
        root->SetPosition(part.pos);
        root->SetRotation(0, 0, 0);
        std::fprintf(stderr, "platform TEST PASS: %zu effect anchors follow all four rotations\n", anchors.size());
    }
}

void WorldGeoramaDrawPlatform(CMapParts *part, const float *position) {
    for (auto &strip : platform.strips) {
        strip.texture = TexManager.GetTextureHandle(strip.texture_name.data(), -1);
    }
    sceVu0FMATRIX matrix;
    Matrix(*part, position, matrix);
    Draw3DDrawVisual(platform, matrix, mgRenderInfo, 0);
}

int WorldGeoramaPlatformPolys(CEditGround *ground, CCPoly *out, const CBoxVu0 &box) {
    int count = 0;
    for (auto &p : ground->parts) {
        if (p.handle >= 0 && p.parts_no == 23) {
            count += TransformPolys(p, out + count, box, cliffCollision);
            count += TransformPolys(p, out + count, box, stairCollision);
            count += TransformPolys(p, out + count, box, fenceCollision);
        }
    }
    return count;
}

bool WorldGeoramaPlatformHeight(CMapParts &p, float x, float z, float &height, bool includeStairs) {
    // A vertical query against the same cliff and retail stair collision used
    // by the player. No analytical/invisible replacement slope.
    if (std::abs(x - p.pos[0]) > 500 || std::abs(z - p.pos[2]) > 500) {
        return false;
    }
    std::array<CCPoly, 512> polygons;
    CBoxVu0                 box;
    box.min[0] = x - .01f;
    box.max[0] = x + .01f;
    box.min[2] = z - .01f;
    box.max[2] = z + .01f;
    box.min[1] = p.pos[1] - 20;
    box.max[1] = p.pos[1] + 150;
    int count = TransformPolys(p, polygons.data(), box, cliffCollision);
    if (includeStairs) {
        count += TransformPolys(p, polygons.data() + count, box, stairCollision);
    }
    bool   found = false;
    double best = -1e30;
    for (int i = 0; i < count; i++) {
        const auto &v = polygons[i].vertex;
        double      ax = v[0][0], az = v[0][2], bx = v[1][0], bz = v[1][2], cx = v[2][0], cz = v[2][2];
        double      det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
        if (std::abs(det) < 1e-6) {
            continue;
        }
        double a = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / det;
        double b = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / det;
        double c = 1 - a - b;
        if (a < -.0001 || b < -.0001 || c < -.0001) {
            continue;
        }
        best = std::max(best, a * v[0][1] + b * v[1][1] + c * v[2][1]);
        found = true;
    }
    if (found) {
        height = float(best);
    }
    return found;
}
