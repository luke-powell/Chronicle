#include "forest_town.hpp"

#include <cstring>
#include <fstream>
#include <map>
#include <memory>

#include "character.hpp"
#include "collision.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dataread_port.hpp"
#include "editloop.hpp"
#include "forest_georama.hpp"
#include "forest_town_layout.hpp"
#include "frame.hpp"
#include "mds.hpp"
#include "mdt.hpp"
#include "mglib.hpp"
#include "platform/paths.hpp"
#include "renderinfo.hpp"
#include "savedata.hpp"
#include "town_biomes.hpp"
#include "world_origin.hpp"

namespace {
struct PartCollision : CCPolyBox {
    std::string objects;
    PartCollision() = default;

    PartCollision(const CCPolyBox &p) : CCPolyBox(p) {}
};

struct Model {
    std::string                                 name;
    std::vector<BiomeMesh>                      meshes;
    std::vector<PartCollision>                  collision;
    float                                       radius = 0;
    std::vector<authoredwater::SurfaceTriangle> shell;
    std::vector<authoredwater::Triangle>        beds;
};

struct Instance {
    Model *model;
    double x, y, z, yaw;
    int    plot = -1;
};

std::map<std::string, Model>         models;
std::vector<Instance>                instances;
Instance                             preview{};
bool                                 previewVisible = false;
bool                                 ready = false, treantReady = false;
std::shared_ptr<authoredwater::Beds> beds;

bool WaterMaterial(const std::string &texture) { return texture.ends_with("/e01b20") || texture.ends_with("/e01b22") || texture == "scene/e02/e02b12" || texture == "scene/e02/e02b13" || texture == "scene/e02/e02b14" || texture == "scene/e02/e02b15"; }

alignas(64) unsigned char treantStorage[8 * 1024 * 1024];
CDataAlloc2<1>            treantArena;
CCharacter                treant;
std::vector<unsigned int> treantPack;

std::vector<uint8_t> Read(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(f)), {}};
}

uint32_t U32(const std::vector<uint8_t> &d, size_t p) {
    uint32_t n = 0;
    if (p + 4 <= d.size()) {
        std::memcpy(&n, d.data() + p, 4);
    }
    return n;
}

std::vector<PartCollision> ReadCollision(const char *town, const char *part) {
    auto                 scene = Read(PathsDataRoot() / "gedit" / town / "scene.scn");
    std::vector<uint8_t> d;
    for (unsigned i = 0; i < U32(scene, 4); ++i) {
        size_t e = 16 + i * 48;
        if (e + 48 > scene.size()) {
            break;
        }
        if (std::strncmp(reinterpret_cast<const char *>(scene.data() + e), part, 16)) {
            continue;
        }
        size_t b = U32(scene, e + 16), size = U32(scene, e + 20);
        if (b + size > scene.size() || size < 224) {
            break;
        }
        size_t offset = U32(scene, b + 120), length = U32(scene, b + 124);
        if (!length || offset + length > size) {
            break;
        }
        d.assign(scene.begin() + b + offset, scene.begin() + b + offset + length);
        break;
    }
    std::vector<PartCollision> result;
    unsigned                   count = U32(d, 8);
    if (16 + size_t(count) * sizeof(MDS_OBJECT) > d.size()) {
        return result;
    }
    std::vector<std::array<float, 16>> matrices(count);
    for (unsigned i = 0; i < count; ++i) {
        MDS_OBJECT o;
        std::memcpy(&o, d.data() + 16 + i * sizeof(o), sizeof(o));
        auto &matrix = matrices[i];
        std::memcpy(matrix.data(), o.matrix, 64);
        if (o.parent >= 0 && unsigned(o.parent) < i) {
            float product[4][4];
            Draw3DMul(product, reinterpret_cast<const float (*)[4]>(matrices[o.parent].data()), o.matrix);
            std::memcpy(matrix.data(), product, 64);
        }
        // Retain the original solid cliffs and ramps. The editor grid itself
        // is the only nonphysical collision helper.
        if (std::strcmp(o.name, "edit") == 0) {
            continue;
        }
        if (o.data_ofs <= 0 || size_t(o.data_ofs) + sizeof(MDT_HEADER) > d.size()) {
            continue;
        }
        MDT_HEADER h;
        size_t     at = o.data_ofs;
        std::memcpy(&h, d.data() + at, sizeof(h));
        if (h.size < sizeof(h) || at + h.size > d.size() || h.mesh_ofs < 0) {
            continue;
        }
        size_t mesh = at + h.mesh_ofs;
        if (mesh + 24 > at + h.size) {
            continue;
        }
        unsigned triangles = U32(d, mesh + 20);
        if (mesh + 24 + size_t(triangles) * 20 > at + h.size) {
            continue;
        }
        for (unsigned t = 0; t < triangles; ++t) {
            PartCollision p{};
            p.objects = "/" + std::string(o.name) + "/";
            for (int parent = o.parent; parent >= 0 && unsigned(parent) < i;) {
                MDS_OBJECT ancestor;
                std::memcpy(&ancestor, d.data() + 16 + parent * sizeof(ancestor), sizeof(ancestor));
                p.objects += std::string(ancestor.name) + "/";
                parent = ancestor.parent;
            }
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                unsigned index = U32(d, mesh + 24 + t * 20 + k * 4);
                size_t   v = at + h.vertex_ofs + size_t(index) * 16;
                if (index >= unsigned(h.vertex_num) || v + 16 > at + h.size) {
                    valid = false;
                    break;
                }
                float source[4];
                std::memcpy(source, d.data() + v, 16);
                for (int axis = 0; axis < 3; ++axis) {
                    p.poly.vertex[k][axis] = matrix[12 + axis];
                    for (int a = 0; a < 3; ++a) {
                        p.poly.vertex[k][axis] += source[a] * matrix[a * 4 + axis];
                    }
                }
                p.poly.vertex[k][3] = 1;
            }
            if (!valid) {
                continue;
            }
            auto &a = p.poly.vertex[0], &b = p.poly.vertex[1], &c = p.poly.vertex[2];
            float n[]{(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                      (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                      (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
            float length = std::hypot(std::hypot(n[0], n[1]), n[2]);
            if (length < 1e-6) {
                continue;
            }
            for (int axis = 0; axis < 3; ++axis) {
                p.poly.normal[axis] = n[axis] / length;
                p.box.min[axis] = std::min({a[axis], b[axis], c[axis]});
                p.box.max[axis] = std::max({a[axis], b[axis], c[axis]});
            }
            int info = int32_t(U32(d, mesh + 24 + t * 20 + 12));
            if (h.info_ofs > 0 && info >= 0 && size_t(h.info_ofs) + size_t(info + 1) * 16 <= size_t(h.size)) {
                std::memcpy(&p.poly.info, d.data() + at + h.info_ofs + size_t(info) * 16, 16);
            }
            result.push_back(p);
        }
    }
    return result;
}

void Add(const char *town, const char *part, double x, double z, double yaw = 0, double y = foresttown::Elevation, int plot = -1, bool cacheOnly = false) {
    auto [it, inserted] = models.try_emplace(std::string(town) + "/" + part);
    auto &model = it->second;
    if (inserted) {
        model.name = part;
        auto source = BiomeReadScene(town, part);
        if (std::getenv("DC_FOREST_AUDIT")) {
            float low = 1e20f, high = -1e20f;
            for (auto &m : source) {
                for (auto &v : m.vertices) {
                    low = std::min(low, v.position[1]);
                    high = std::max(high, v.position[1]);
                }
            }
            std::fprintf(stderr, "forest bounds: %s height %g..%g\n", part, low, high);
        }
        model.collision = ReadCollision(town, part);
        std::map<std::string, BiomeMesh> batches;
        bool                             hasWaterBase = std::any_of(source.begin(), source.end(), [](const auto &m) { return m.texture.ends_with("/e01b20"); });
        if (!hasWaterBase) {
            std::vector<std::pair<float, float>> points;
            float                                low = 1e20f, high = -1e20f;
            for (const auto &m : source) {
                if (m.texture.ends_with("/e01b22")) {
                    for (const auto &v : m.vertices) {
                        points.emplace_back(v.position[0], v.position[2]);
                        low = std::min(low, v.position[1]);
                        high = std::max(high, v.position[1]);
                    }
                }
            }
            if (points.size() >= 3 && high - low < .5f) {
                // The retail ripple mesh contains only two shoreline strips.
                // Fill between their outer boundaries; native banks occlude the
                // inside corners, preserving the original visible channel shape.
                std::sort(points.begin(), points.end());
                points.erase(std::unique(points.begin(), points.end()), points.end());
                auto                                 cross = [](auto a, auto b, auto c) { return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first); };
                std::vector<std::pair<float, float>> hull;
                for (auto point : points) {
                    while (hull.size() > 1 && cross(hull[hull.size() - 2], hull.back(), point) <= 0) {
                        hull.pop_back();
                    }
                    hull.push_back(point);
                }
                size_t lower = hull.size();
                for (int i = int(points.size()) - 2; i >= 0; --i) {
                    auto point = points[i];
                    while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), point) <= 0) {
                        hull.pop_back();
                    }
                    hull.push_back(point);
                }
                if (hull.size() > 3) {
                    hull.pop_back();
                    BiomeMesh surface;
                    surface.texture = "scene/e02/e01b20";
                    for (auto [px, pz] : hull) {
                        gfx::Vertex3D v{};
                        v.position[0] = px;
                        v.position[1] = low;
                        v.position[2] = pz;
                        v.normal[1] = 1;
                        v.uv[0] = px / 100;
                        v.uv[1] = pz / 100;
                        std::fill_n(v.color, 4, 128);
                        surface.vertices.push_back(v);
                    }
                    for (unsigned i = 1; i + 1 < hull.size(); ++i) {
                        surface.indices.insert(surface.indices.end(), {0, i + 1, i});
                    }
                    source.push_back(std::move(surface));
                    hasWaterBase = true;
                }
            }
        }
        // The riverbank atlas includes a hard cutout grass fringe. Reuse the
        // original grass material on the top faces so the fringe meets the
        // continuous forest floor without a dark, pixel-stepped outline.
        auto overWater = [&](double x, double z) {
            for (const auto &water : source) {
                if (water.texture.ends_with("/e01b20")) {
                    for (size_t i = 0; i + 2 < water.indices.size(); i += 3) {
                        auto   a = water.vertices[water.indices[i]].position, b = water.vertices[water.indices[i + 1]].position, c = water.vertices[water.indices[i + 2]].position;
                        double den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
                        if (std::abs(den) < 1e-6) {
                            continue;
                        }
                        double u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / den, v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / den;
                        if (u >= 0 && v >= 0 && u + v <= 1) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };
        std::vector<BiomeMesh> bankCaps;
        for (auto &mesh : source) {
            if (mesh.texture == "scene/e02/e02b09" && std::strcmp(part, "e02g07") != 0) {
                BiomeMesh cap;
                cap.texture = "scene/e02/e02b02";
                cap.sceneObjects = mesh.sceneObjects;
                std::vector<uint32_t> kept;
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                    auto  &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]], &c = mesh.vertices[mesh.indices[i + 2]];
                    double ux = b.position[0] - a.position[0], uy = b.position[1] - a.position[1], uz = b.position[2] - a.position[2];
                    double vx = c.position[0] - a.position[0], vy = c.position[1] - a.position[1], vz = c.position[2] - a.position[2];
                    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                    float  lo = std::min({a.position[1], b.position[1], c.position[1]}), hi = std::max({a.position[1], b.position[1], c.position[1]});
                    if (lo > -8 && hi < 1 && !(hi - lo < .01f && lo > .075f) && std::abs(ny) > .65 * std::sqrt(nx * nx + ny * ny + nz * nz) && !overWater((a.position[0] + b.position[0] + c.position[0]) / 3., (a.position[2] + b.position[2] + c.position[2]) / 3.)) {
                        for (auto v : {a, b, c}) {
                            v.normal[0] = v.normal[2] = 0;
                            v.normal[1] = 1;
                            cap.indices.push_back(unsigned(cap.vertices.size()));
                            cap.vertices.push_back(v);
                        }
                    } else {
                        kept.insert(kept.end(), mesh.indices.begin() + i, mesh.indices.begin() + i + 3);
                    }
                }
                mesh.indices = std::move(kept);
                if (!cap.indices.empty()) {
                    bankCaps.push_back(std::move(cap));
                }
            }
        }
        for (auto &cap : bankCaps) {
            source.push_back(std::move(cap));
        }
        for (auto &mesh : source) {
            // e01b22 is a transparent mask for the retail screen-refraction
            // pass. Use the original water texels on that exact footprint.
            if (mesh.texture.ends_with("/e01b22")) {
                if (hasWaterBase) {
                    continue;
                }
                mesh.texture = "scene/e02/e01b20";
            }
            if (std::strcmp(part, "s0304") == 0) {
                mesh.texture = "scene/e02/e01b20";
            }
            if (std::strcmp(part, "e02g04") == 0 || std::strcmp(part, "s0101") == 0) {
                // Keep only authored terrain, never treehouse roofs or foliage.
                bool ground = (std::strcmp(part, "e02g04") == 0 && mesh.sceneObjects.find("/waku") != std::string::npos) || mesh.texture.ends_with("/s02e0102") || mesh.texture.ends_with("/s02e0104") || mesh.texture.ends_with("/s02e0105");
                if (ground) {
                    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                        authoredwater::SurfaceTriangle t;
                        for (int k = 0; k < 3; ++k) {
                            auto &v = mesh.vertices[mesh.indices[i + k]];
                            t.x[k] = v.position[0];
                            t.y[k] = v.position[1];
                            t.z[k] = v.position[2];
                        }
                        model.shell.push_back(t);
                    }
                }
            }
            if (mesh.texture.ends_with("/e01b20")) {
                for (auto &v : mesh.vertices) {
                    // Retail refraction layers differ by fractions of a unit. A
                    // shared physical surface avoids cracks between adjacent pieces.
                    if (v.position[1] > -13.2f && v.position[1] < -12.7f) {
                        v.position[1] = -12.8f;
                    }
                    for (int axis : {0, 2}) {
                        if (std::abs(v.position[axis] - std::round(v.position[axis])) < .01f) {
                            v.position[axis] = std::round(v.position[axis]);
                        }
                    }
                }
            }
            // Native ground and water define the bed beneath the procedural mesh.
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                authoredwater::Triangle t{};
                double                  high = -1e30, low = 1e30;
                for (int k = 0; k < 3; ++k) {
                    const auto &v = mesh.vertices[mesh.indices[i + k]];
                    t.x[k] = v.position[0];
                    t.z[k] = v.position[2];
                    high = std::max(high, double(v.position[1]));
                    low = std::min(low, double(v.position[1]));
                }
                double area = (t.x[1] - t.x[0]) * (t.z[2] - t.z[0]) - (t.x[2] - t.x[0]) * (t.z[1] - t.z[0]);
                // Sloping bank polygons already cover the channel walls. Excavating
                // their entire projected footprint to their lowest point creates
                // an exposed trench outside the original bank.
                bool groundPlane = high - low < .5 && (mesh.texture.ends_with("/e02b01") || mesh.texture.ends_with("/e02b02") || mesh.texture.ends_with("/e02b03") || mesh.texture.ends_with("/s02e0102") || mesh.texture.ends_with("/s02e0104") || mesh.texture.ends_with("/s02e0105"));
                if (std::abs(area) > 1 && high < 2 && low > -100 && (WaterMaterial(mesh.texture) || groundPlane)) {
                    t.bed = low - (WaterMaterial(mesh.texture) ? 20 : 2);
                    model.beds.push_back(t);
                }
            }
            auto &batch = batches[mesh.texture + mesh.sceneObjects];
            batch.sceneObjects = mesh.sceneObjects;
            batch.texture = mesh.texture;
            unsigned base = unsigned(batch.vertices.size());
            for (auto &v : mesh.vertices) {
                model.radius = std::max(model.radius, std::hypot(v.position[0], v.position[2]));
            }
            batch.vertices.insert(batch.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            for (auto index : mesh.indices) {
                batch.indices.push_back(base + index);
            }
        }
        for (auto &[key, batch] : batches) {
            batch.count = unsigned(batch.indices.size());
            model.meshes.push_back(std::move(batch));
        }
        for (auto &m : model.meshes) {
            m.mesh = gfx::CreateMesh(m.vertices, m.indices);
            if (!WaterMaterial(m.texture)) {
                m.vertices.clear();
            }
            m.indices.clear();
        }
        std::fprintf(stderr, "forest town: %s/%s strips=%zu collision=%zu\n", town, part, model.meshes.size(), model.collision.size());
    }
    if (cacheOnly) {
        return;
    }
    instances.push_back({&model, x, y, z, yaw, plot});
    for (auto t : model.shell) {
        for (int k = 0; k < 3; ++k) {
            double lx = t.x[k], lz = t.z[k];
            t.x[k] = x + std::cos(yaw) * lx + std::sin(yaw) * lz;
            t.z[k] = z - std::sin(yaw) * lx + std::cos(yaw) * lz;
            t.y[k] += y;
        }
        beds->AddSurface(t);
    }
    for (auto t : model.beds) {
        for (int k = 0; k < 3; ++k) {
            double lx = t.x[k], lz = t.z[k];
            t.x[k] = x + std::cos(yaw) * lx + std::sin(yaw) * lz;
            t.z[k] = z - std::sin(yaw) * lx + std::cos(yaw) * lz;
        }
        t.bed += y;
        beds->Add(t);
    }
}

void LoadTreant() {
    const char *path = "gedit/s03/chara/zyuraku.chr";
    auto        size = DataFileSize(path);
    if (!size) {
        return;
    }
    treantPack.resize(((size + 2047) / 2048) * 512);
    LoadFile(const_cast<char *>(path), treantPack.data(), nullptr);
    treantArena.base = treantArena.buffer = treantStorage;
    treantArena.limit = sizeof(treantStorage) / 16;
    treantArena.used = 0;
    treant.Initialize();
    treant.LoadPackData3(treantPack.data(), const_cast<char *>("zyuraku.cfg"), &treantArena, 8, &treantArena, 1, 0);
    treantReady = treant.frame && treant.motion[0];
    if (treantReady) {
        treant.SetMotion(0, 0);
        treant.SetScale(10, 10, 10);
        treant.SetRotation(0, 3.141592653589793f, 0);
    }
    std::fprintf(stderr, "forest town: animated Treant %s\n", treantReady ? "ready" : "FAILED");
}
} // namespace

void ForestTownPrepare() {
    static unsigned generation = ~0u;
    unsigned        current = ForestGeoramaRevision();
    if (ready && generation == current) {
        return;
    }
    generation = current;
    instances.clear();
    foresttown::RiverConnected() = ForestGeoramaConnected();
    std::fprintf(stderr, "forest river: %s\n", foresttown::RiverConnected() ? "connected / Treant alive" : "disconnected / Treant dormant");
    ready = true;
    beds = std::make_shared<authoredwater::Beds>();
    using namespace foresttown;
    for (auto r : std::array<std::array<double, 4>, 3>{
             {{{-1100, 100, -500, 900}}, {{-900, -800, 300, -200}}, {{500, 200, 1200, 1000}}}
    }) {
        beds->protectedAreas.push_back({Village.x + r[0], Village.z + r[1], Village.x + r[2], Village.z + r[3]});
    }
    Add("e02", "e02g04", Village.x, Village.z);
    // The retail authored layout gives a complete village on fresh saves;
    // existing Matataki placements take precedence without modifying the save.
    constexpr const char *parts[]{"e02h01", "e02h02", "e02h03", "e02h04", "e02h05", "e02h06", "e02h07", "e02h08",
                                  "e02h09", "e02h09", "e02h09", "e02c09", "e02c15", "e02c16", "e02h10", "e02t01", nullptr, nullptr, "e02m01", "e02m02"};

    struct Placement {
        int   part, rotation;
        float x, y, z;
    };

    std::vector<Placement> placements;
    for (auto p : ForestGeoramaParts()) {
        placements.push_back({p.part_id, p.rot_y, p.pos_x, p.pos_y, p.pos_z});
        if (p.part_id >= 0 && p.part_id < int(std::size(parts)) && parts[p.part_id]) {
            Add("e02", parts[p.part_id], Village.x + p.pos_x, Village.z + p.pos_z, p.rot_y * 1.5707963267948966, Elevation + p.pos_y, p.part_id);
        }
    }
    // Match CEditArea::SetRiverParts exactly, using the original 100-unit grid.
    for (const auto &p : placements) {
        if (p.part == 16 || p.part == 17) {
            bool          n[4]{};
            int           count = 0;
            constexpr int dx[]{0, 100, 0, -100}, dz[]{-100, 0, 100, 0};
            for (int k = 0; k < 4; ++k) {
                for (const auto &q : placements) {
                    if ((q.part == 16 || q.part == 17) && std::abs(q.x - p.x - dx[k]) < 1 && std::abs(q.z - p.z - dz[k]) < 1) {
                        n[k] = true;
                        break;
                    }
                }
            }
            // CEditArea::GetPartsExtra supplies six connections outside its grids.
            constexpr std::array<std::array<double, 2>, 6> mouths{
                {{-550, 50}, {-850, 950}, {-550, -150}, {350, -450}, {950, 150}, {850, 1050}}
            };
            for (int k = 0; k < 4; ++k) {
                for (auto q : mouths) {
                    if (std::abs(q[0] - p.x - dx[k]) < 1 && std::abs(q[1] - p.z - dz[k]) < 1) {
                        n[k] = true;
                    }
                }
            }
            for (bool v : n) {
                count += v;
            }
            int shape = 5, rotation = 0;
            if (count == 4) {
                shape = 4;
            }
            if (count == 3) {
                shape = 3;
                for (int k = 0; k < 4; ++k) {
                    if (!n[k]) {
                        rotation = 3 - k;
                    }
                }
            }
            if (count == 2) {
                if (n[0] && n[2]) {
                    shape = 2;
                    rotation = 0;
                } else if (n[1] && n[3]) {
                    shape = 2;
                    rotation = 1;
                } else {
                    shape = 1;
                    for (int k = 0; k < 4; ++k) {
                        if (n[k] && n[(k + 1) % 4]) {
                            rotation = (4 - k) % 4;
                        }
                    }
                }
            }
            if (count == 1) {
                shape = 6;
                for (int k = 0; k < 4; ++k) {
                    if (n[k]) {
                        rotation = (6 - k) % 4;
                    }
                }
            }
            if (p.part == 17) {
                shape = 7;
                rotation = p.rotation;
            }
            std::string part = "e02c0" + std::to_string(shape);
            Add("e02", part.c_str(), Village.x + p.x, Village.z + p.z, rotation * 1.5707963267948966, Elevation + p.y);
        }
    }
    // Fixed creek mouths, reservoir, sluice and original pond surfaces.
    for (auto p : std::array<std::array<double, 3>, 5>{
             {{-550, -180, 0}, {-550, 80, 0}, {950, 180, 0}, {-850, 920, 0}, {320, -450, 1}}
    }) {
        Add("e02", "e02g07", Village.x + p[0], Village.z + p[1], p[2] * 1.5707963267948966);
    }
    Add("e02", "e02c12", Village.x + 850, Village.z + 1000);
    Add("e02", "e02c17", Village.x + 850, Village.z + 1115);
    for (auto part : {"e02c10", "e02c13", "e02c14"}) {
        Add("e02", part, Village.x, Village.z);
    }
    Add("e02", "e02c11", Village.x + 850, Village.z + 1250);
    Add("s01", "s0101", Treehouse.x, Treehouse.z, 1.5707963267948966);

    authoredwater::Current() = beds;
    for (auto town : {"e02", "s01", "s03"}) {
        BiomeLoadSceneTextures(town);
    }
    unsigned missing = 0, triangles = 0;
    for (const auto &[key, model] : models) {
        for (const auto &m : model.meshes) {
            triangles += m.count / 3;
            if (!m.mesh || !m.count || !BiomeSurfaceBinding(m.texture.c_str()).texture) {
                ++missing;
                std::fprintf(stderr, "forest town: missing mesh/texture %s\n", m.texture.c_str());
            }
        }
    }
    std::fprintf(stderr, "forest town: asset audit triangles=%u missing=%u\n", triangles, missing);
    auto &trunk = models["Treant body"];
    if (trunk.collision.empty()) {
        trunk.radius = 70;
        for (int i = 0; i < 16; ++i) {
            double a = i * 6.283185307179586 / 16, b = (i + 1) * 6.283185307179586 / 16;
            float  x0 = float(70 * std::cos(a)), z0 = float(70 * std::sin(a)), x1 = float(70 * std::cos(b)), z1 = float(70 * std::sin(b));
            for (int t = 0; t < 2; ++t) {
                CCPolyBox p{};
                float     points[4][4]{
                    {x0, 0,   z0, 1},
                    {x1, 0,   z1, 1},
                    {x0, 180, z0, 1},
                    {x1, 180, z1, 1}
                };
                int ids[2][3]{
                    {0, 2, 1},
                    {1, 2, 3}
                };
                for (int k = 0; k < 3; ++k) {
                    std::memcpy(p.poly.vertex[k], points[ids[t][k]], 16);
                }
                p.poly.normal[0] = float(std::cos((a + b) * .5));
                p.poly.normal[2] = float(std::sin((a + b) * .5));
                for (int axis = 0; axis < 3; ++axis) {
                    p.box.min[axis] = std::min({p.poly.vertex[0][axis], p.poly.vertex[1][axis], p.poly.vertex[2][axis]});
                    p.box.max[axis] = std::max({p.poly.vertex[0][axis], p.poly.vertex[1][axis], p.poly.vertex[2][axis]});
                }
                trunk.collision.push_back(p);
            }
        }
    }
    instances.push_back({&trunk, Treant.x, Elevation, Treant.z, 0});
    if (!treantReady) {
        LoadTreant();
    }
    std::fprintf(stderr, "forest town: integrated %zu instances; village %.0f,%.0f treehouse %.0f,%.0f Treant %.0f,%.0f\n", instances.size(), Village.x, Village.z, Treehouse.x, Treehouse.z, Treant.x, Treant.z);
}

void ForestTownReset() {
    ready = false;
    treantReady = false;
    instances.clear();
    authoredwater::Current().reset();
}

void ForestTownShutdown() {
    ForestTownReset();
    for (auto &[key, model] : models) {
        for (auto &m : model.meshes) {
            gfx::DestroyMesh(m.mesh);
        }
    }
    models.clear();
}

void ForestTownDraw() {
    ForestTownPrepare();
    auto info = mgRenderInfo;
    info.shadow_pass = 0;
    auto state = Draw3DState(info, false);
    state.blend = false;
    state.cull = gfx::CullMode::None;
    state.alpha_test = true;
    state.alpha_func = gfx::AlphaFunc::Greater;
    state.alpha_ref = 32;
    static double tick = 0;
    double        seconds = (++tick) / 30.;
    auto          drawing = instances;
    if (previewVisible && ForestGeoramaActive() && NowSelectParts >= 0) {
        drawing.push_back(preview);
    }
    previewVisible = false;
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 1 && treantReady && (!Chara || std::hypot(world::X(Chara->pos[0]) - foresttown::Treant.x, world::Z(Chara->pos[2]) - foresttown::Treant.z) < 22000)) {
            treant.SetPosition(world::LocalX(foresttown::Treant.x), float(foresttown::Elevation), world::LocalZ(foresttown::Treant.z));
            bool alive = foresttown::RiverConnected();
            if (alive) {
                treant.Step();
            }
            CFrameAttr bark;
            bark.use_color = !alive;
            bark.color[0] = 65;
            bark.color[1] = 48;
            bark.color[2] = 32;
            bark.color[3] = 128;
            treant.frame->SetAttr(bark, 1, 0x600);
            for (const char *name : {"sphere6_1_1", "sphere7_1_1"}) {
                if (auto *eye = treant.frame->SearchFrame(const_cast<char *>(name))) {
                    CFrameAttr visibility;
                    visibility.draw_on = alive;
                    eye->SetAttr(visibility, 1, 1);
                }
            }
            treant.Draw();
        }
        for (const auto &object : drawing) {
            Draw3DIdentityScope identity(&object, 1000);
            if (Chara && std::hypot(world::X(Chara->pos[0]) - object.x, world::Z(Chara->pos[2]) - object.z) > 22000) {
                continue;
            }
            sceVu0FMATRIX model;
            sceVu0UnitMatrix(model);
            float c = std::cos(object.yaw), s = std::sin(object.yaw);
            model[0][0] = c;
            model[0][2] = -s;
            model[2][0] = s;
            model[2][2] = c;
            model[3][0] = world::LocalX(object.x);
            model[3][1] = float(object.y);
            model[3][2] = world::LocalZ(object.z);
            gfx::MeshConstants constants{};
            gfx::MeshTransform transform;
            Draw3DSceneConstants(constants, info, model, &transform);
            constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere;
            float white[4]{1, 1, 1, 1}, black[4]{};
            Draw3DMaterial(constants, info, white, white, black);
            // Atmospheric distances use the same model-local eye as the terrain.
            double ex = info.view_position[0] - world::LocalX(object.x), ez = info.view_position[2] - world::LocalZ(object.z);
            constants.clip_plane[0][0] = float(c * ex - s * ez);
            constants.clip_plane[0][1] = float(info.view_position[1] - object.y);
            constants.clip_plane[0][2] = float(s * ex + c * ez);
            constants.clip_plane[0][3] = .1f;
            for (int axis = 0; axis < 3; ++axis) {
                constants.clip_plane[1][axis] = -constants.light_direction[0][axis];
            }
            constants.clip_plane[1][3] = 2e-6f;
            for (const auto &m : object.model->meshes) {
                if (const char *hide = std::getenv("DC_FOREST_HIDE_OBJECT"); hide && m.sceneObjects.find(hide) != std::string::npos) {
                    continue;
                }

                if (!ForestGeoramaVisible(object.plot, m.sceneObjects)) {
                    continue;
                }
                bool   water = WaterMaterial(m.texture);
                CFrame nativeFrame;
                auto   end = m.sceneObjects.find('/', 1);
                auto   name = m.sceneObjects.empty() ? std::string{} : m.sceneObjects.substr(1, end == std::string::npos ? 0 : end - 1);
                std::strncpy(nativeFrame.name, name.c_str(), sizeof(nativeFrame.name) - 1);
                if (!name.empty()) {
                    SetFrameAttr(&nativeFrame, 0);
                }
                const auto &attr = nativeFrame.attr;
                bool        translucent = !attr.depth_write || attr.blend_mode != 0 || (attr.alpha_ref >= 0 && attr.alpha_ref < 32);
                if (pass != (water ? 2 : translucent ? 1
                                                     : 0)) {
                    continue;
                }
                auto drawState = state;
                auto material = constants;
                material.flags |= gfx::kMeshVertexColor;
                if (m.texture == "scene/e02/e02b02") {
                    // Original grass and procedural fill share texel scale and phase.
                    material.flags |= gfx::kMeshWorldUv;
                    material.world_uv[0][0] = c / 200;
                    material.world_uv[0][2] = s / 200;
                    material.world_uv[1][0] = -s / 200;
                    material.world_uv[1][2] = c / 200;
                    material.world_uv[0][3] = float(std::fmod(object.x / 200, 4096));
                    material.world_uv[1][3] = float(std::fmod(object.z / 200, 4096));
                }

                if (attr.use_color) {
                    material.flags &= ~gfx::kMeshLit;
                }
                drawState.alpha_ref = attr.alpha_ref >= 0 ? attr.alpha_ref : 32;
                drawState.depth_write = attr.depth_write;
                drawState.blend = translucent;
                if (translucent) {
                    drawState.alpha = {};
                }
                if (attr.blend_mode) {
                    drawState.alpha.a = attr.blend_mode > 0 ? 0 : 2;
                    drawState.alpha.b = attr.blend_mode > 0 ? 2 : 0;
                    drawState.alpha.c = 0;
                    drawState.alpha.d = 1;
                }

                if (water) {
                    auto vertices = m.vertices;
                    bool still = object.model->name == "e02c13" || object.model->name == "e02c14" || object.model->name == "s0304";
                    for (auto &v : vertices) {
                        double phase = v.position[0] * .025 + v.position[2] * .018;
                        v.uv[0] += float(.008 * std::sin(seconds * .7 + phase));
                        v.uv[1] += float((still ? 0 : -seconds * .06) + .008 * std::cos(seconds * .53 - phase));
                        // Retail texanime.cfg scrolls the two waterfall source
                        // textures horizontally at -3.5 and +2 pixels per tick.
                        if (m.texture.ends_with("/e02b14") || m.texture.ends_with("/e02b15")) {
                            v.uv[0] = m.vertices[&v - vertices.data()].uv[0] + float(seconds * 30 * (m.texture.ends_with("/e02b14") ? -3.5 : 2.) / 128.);
                            v.uv[1] = m.vertices[&v - vertices.data()].uv[1];
                        }
                        if (m.texture.ends_with("/e01b20")) {
                            if (object.model->name == "e02c14") {
                                double wx = object.x + c * v.position[0] + s * v.position[2], wz = object.z - s * v.position[0] + c * v.position[2];
                                double phase = wx * .062831853 + wz * .041;
                                v.uv[0] = float(wx / 100 + .006 * std::sin(seconds * .7 + phase));
                                v.uv[1] = float(wz / 100 - seconds * .06 + .006 * std::cos(seconds * .53 - phase));
                            }
                            v.normal[0] = v.normal[2] = 0;
                            v.normal[1] = 1;
                        }
                        double dx = constants.clip_plane[0][0] - v.position[0], dy = constants.clip_plane[0][1] - v.position[1], dz = constants.clip_plane[0][2] - v.position[2];
                        double facing = std::abs(dy) / std::max(1., std::sqrt(dx * dx + dy * dy + dz * dz));
                        double fresnel = .04 + .35 * std::pow(1 - facing, 5);
                        v.color[0] = uint8_t(90 + 20 * fresnel);
                        v.color[1] = uint8_t(112 + 12 * fresnel);
                        v.color[2] = uint8_t(118 + 10 * fresnel);
                        v.color[3] = uint8_t(124 + 4 * fresnel);
                    }
                    gfx::UpdateMeshVertices(m.mesh, 0, vertices);
                    material.flags |= gfx::kMeshVertexColor;
                    drawState.blend = true;
                    drawState.alpha = {};
                    drawState.depth_write = false;
                    drawState.alpha_test = false;
                }
                gfx::DrawMesh(m.mesh, 0, m.count, material, BiomeSurfaceBinding(m.texture.c_str()), drawState, &transform);
            }
        }
    }
}

int ForestTownPolys(CCPoly *out, const CBoxVu0 &box, int capacity) {
    if (!ready || capacity <= 0) {
        return 0;
    }
    int count = 0;
    for (const auto &object : instances) {
        double ox = world::LocalX(object.x), oz = world::LocalZ(object.z), c = std::cos(object.yaw), s = std::sin(object.yaw);
        double qx = (box.min[0] + box.max[0]) * .5 - ox, qz = (box.min[2] + box.max[2]) * .5 - oz;
        if (std::hypot(qx, qz) > object.model->radius + std::hypot(box.max[0] - box.min[0], box.max[2] - box.min[2])) {
            continue;
        }
        for (const auto &source : object.model->collision) {
            if (!ForestGeoramaVisible(object.plot, source.objects)) {
                continue;
            }
            // Transform the query into model space for a conservative broad phase.
            double cx = (box.min[0] + box.max[0]) * .5 - ox, cz = (box.min[2] + box.max[2]) * .5 - oz;
            double ex = (box.max[0] - box.min[0]) * .5, ez = (box.max[2] - box.min[2]) * .5;
            double lx = c * cx - s * cz, lz = s * cx + c * cz, rx = std::abs(c) * ex + std::abs(s) * ez, rz = std::abs(s) * ex + std::abs(c) * ez;
            if (lx + rx < source.box.min[0] || lx - rx > source.box.max[0] || lz + rz < source.box.min[2] || lz - rz > source.box.max[2] ||
                box.max[1] < source.box.min[1] + object.y || box.min[1] > source.box.max[1] + object.y) {
                continue;
            }
            auto &p = out[count++];
            p = source.poly;
            for (auto &v : p.vertex) {
                double x = v[0], z = v[2];
                v[0] = float(ox + c * x + s * z);
                v[1] += float(object.y);
                v[2] = float(oz - s * x + c * z);
            }
            float nx = p.normal[0], nz = p.normal[2];
            p.normal[0] = float(c * nx + s * nz);
            p.normal[2] = float(-s * nx + c * nz);
            if (count == capacity) {
                return count;
            }
        }
    }
    return count;
}

void ForestGeoramaPreview(int plot, float x, float y, float z, int rotation) {
    constexpr const char *names[]{"e02h01", "e02h02", "e02h03", "e02h04", "e02h05", "e02h06", "e02h07", "e02h08", "e02h09", "e02h09", "e02h09", "e02c09", "e02c15", "e02c16", "e02h10", "e02t01", "e02c05", "e02c07", "e02m01", "e02m02"};
    if (plot < 0 || plot >= 20) {
        return;
    }
    auto found = models.find(std::string("e02/") + names[plot]);
    if (found == models.end()) {
        Add("e02", names[plot], 0, 0, 0, 0, -1, true);
        BiomeLoadSceneTextures("e02");
        found = models.find(std::string("e02/") + names[plot]);
    }
    if (found == models.end()) {
        return;
    }
    preview = {&found->second, world::X(x), y, world::Z(z), rotation * 1.5707963267948966, plot};
    previewVisible = true;
}
