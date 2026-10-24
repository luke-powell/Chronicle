#include "town_biomes.hpp"

#include <chrono>
#include <cstring>
#include <fstream>

#include "editground.hpp"
#include "forest_kernel.hpp"
#include "framevu1.hpp"
#include "gfx/tim2.hpp"
#include "mds.hpp"
#include "mdt.hpp"
#include "platform/paths.hpp"
#include "renderinfo.hpp"
#include "texture.hpp"
#include "visualvu1.hpp"
#include "world_origin.hpp"

namespace {
BiomeMesh PrepareMesh(std::vector<gfx::Vertex3D> vertices, std::vector<uint32_t> indices, std::string texture = {}) {
    return {0, unsigned(indices.size()), std::move(texture), std::move(vertices), std::move(indices)};
}

std::map<std::string, gfx::tim2::Uploaded>   textures;
std::map<std::string, std::array<double, 3>> canopyColors;
std::array<std::array<double, 3>, 5>         speciesCanopyColors{};

struct Source {
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    std::string                texture;
};

std::vector<Source>                               trees;
std::array<std::vector<Source>, 3>                regionalTrees;
std::array<std::vector<Source>, 3>                undergrowth;
std::vector<Source>                               cacti;
std::map<std::string, bool>                       requiredTextures;
std::array<std::array<std::vector<Source>, 5>, 3> treeLods;
std::array<float,5> treeHeights{};
gfx::MeshHandle                                   treeIdentity = 0;

uint32_t U32(const std::vector<uint8_t> &data, size_t offset) {
    uint32_t value = 0;
    if (offset + 4 <= data.size()) {
        std::memcpy(&value, data.data() + offset, 4);
    }
    return value;
}

void MakeCanopyMips(gfx::tim2::Image &image) {
    // Area-filter premultiplied colour so distant trees leave coverage instead
    // of disappearing or becoming dark fringes at transparent texels.
    while (image.levels.back().width > 1) {
        const auto      &previous = image.levels.back();
        gfx::tim2::Level mip;
        mip.width = mip.height = previous.width / 2;
        mip.pixels.resize(mip.width * mip.height * 4);
        for (unsigned z = 0; z < mip.height; z++) {
            for (unsigned x = 0; x < mip.width; x++) {
                double                a = 0;
                std::array<double, 3> rgb{};
                for (int dz = 0; dz < 2; dz++) {
                    for (int dx = 0; dx < 2; dx++) {
                        size_t i = ((z * 2 + dz) * previous.width + x * 2 + dx) * 4;
                        double w = previous.pixels[i + 3];
                        a += w;
                        for (int k = 0; k < 3; k++) {
                            rgb[k] += previous.pixels[i + k] * w;
                        }
                    }
                }
                size_t i = (z * mip.width + x) * 4;
                for (int k = 0; k < 3; k++) {
                    mip.pixels[i + k] = a ? uint8_t(rgb[k] / a) : 0;
                }
                mip.pixels[i + 3] = uint8_t(a / 4);
            }
        }
        image.levels.push_back(std::move(mip));
    }
}

gfx::TextureHandle UploadCanopy(const gfx::tim2::Image &image) {
    const auto &base = image.levels.front();
    auto        texture = gfx::CreateTexture({base.width, base.height, gfx::TextureFormat::Rgba8, unsigned(image.levels.size()), true});
    for (unsigned level = 0; level < image.levels.size(); level++) {
        const auto &mip = image.levels[level];
        gfx::UpdateTextureNow(texture, level, 0, 0, mip.width, mip.height, mip.pixels.data());
    }
    if (!texture) {
        std::fprintf(stderr, "canopy: texture allocation failed\n");
    }
    return texture;
}

void LoadTextures(const char *town) {
    std::ifstream        file(PathsDataRoot() / "gedit" / town / "img.pak", std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    for (size_t cursor = 0; cursor + 76 <= data.size() && data[cursor];) {
        size_t begin = cursor + U32(data, cursor + 64), size = U32(data, cursor + 68), next = U32(data, cursor + 72);
        if (begin + size > data.size() || next < 76) {
            break;
        }
        if (size >= 16 && (std::memcmp(data.data() + begin, "IMG", 3) == 0 || std::memcmp(data.data() + begin, "IM2", 3) == 0)) {
            bool     swizzled = data[begin + 2] == '2';
            unsigned count = U32(data, begin + 4);
            for (unsigned i = 0; i < count && 16 + (i + 1) * 48 <= size; i++) {
                size_t      entry = begin + 16 + i * 48;
                std::string name(reinterpret_cast<const char *>(data.data() + entry), strnlen(reinterpret_cast<const char *>(data.data() + entry), 32));
                if (name != "e02b02" && name != "e04b01" && name != "e03b03" && name != "e03b07" &&
                    !requiredTextures.contains(name) && name != "e02b18" && name != "s02b23" && name != "e03b04" && name != "e03b06" && name != "e04b10" && name != "e04b04") {
                    continue;
                }
                size_t offset = begin + U32(data, entry + 32);
                if (offset + 32 > begin + size) {
                    continue;
                }
                std::vector<uint8_t> image(data.begin() + offset, data.begin() + begin + size);
                size_t               reserved = U32(image, 16);
                if (reserved < 1024 * 1024 && reserved + 16 > image.size()) {
                    image.resize(reserved + 16);
                }
                auto pictures = gfx::tim2::Parse(image);
                if (!pictures || pictures->empty()) {
                    continue;
                }
                auto             &p = (*pictures)[0];
                gfx::tim2::Source source;
                source.psm = p.psm;
                source.width = p.width;
                source.height = p.height;
                source.levels = p.levels;
                source.clut = p.clut;
                source.clut16 = p.clut16;
                source.csm1 = p.clut_csm1;
                source.swizzled = swizzled;
                if (auto decoded = gfx::tim2::Decode(source)) {
                    std::array<double, 3> sum{}, all{};
                    double                weight = 0, allWeight = 0;
                    const auto           &level = decoded->levels.front();
                    for (size_t pixel = 0; pixel < size_t(level.width) * level.height; ++pixel) {
                        uint32_t rgba;
                        if (decoded->format == gfx::TextureFormat::Index8) {
                            rgba = decoded->palette[level.pixels[pixel]];
                        } else {
                            std::memcpy(&rgba, level.pixels.data() + pixel * 4, 4);
                        }
                        double r = rgba & 255, g = (rgba >> 8) & 255, b = (rgba >> 16) & 255;
                        double w = double(rgba >> 24) / 255;
                        all[0] += r * w;
                        all[1] += g * w;
                        all[2] += b * w;
                        allWeight += w;
                        // Canopy colour comes from the owned foliage texels,
                        // excluding transparent borders and brown trunk pixels.
                        if (g >= r * .95 && g > b * 1.1) {
                            sum[0] += r * w;
                            sum[1] += g * w;
                            sum[2] += b * w;
                            weight += w;
                        }
                    }
                    if (weight == 0) {
                        sum = all;
                        weight = allWeight;
                    }
                    if (weight > 0) {
                        for (auto &c : sum) {
                            c /= weight;
                        }
                        canopyColors[name] = sum;
                    }
                    // Queens' water tile contains a foam/beach strip in its lower half.
                    // Keep the original open-water texels as a repeatable surface.
                    if (name == "e03b07") {
                        for (auto &level : decoded->levels) {
                            level.height = std::max(1u, level.height / 2);
                            level.pixels.resize(size_t(level.width) * level.height * (decoded->format == gfx::TextureFormat::Index8 ? 1 : 4));
                        }
                    }
                    textures[name] = gfx::tim2::Upload(*decoded);
                }
            }
        }
        cursor += next;
    }
}

std::vector<Source> ReadTree(const char *town, const char *part, const char *filter = nullptr, int lod = 0) {
    // Read the original static scene resource directly into immutable render geometry.
    // No Georama slots, texture-manager slots, or retail model arenas are consumed.
    std::ifstream        file(PathsDataRoot() / "gedit" / town / "scene.scn", std::ios::binary);
    std::vector<uint8_t> scene((std::istreambuf_iterator<char>(file)), {}), data;
    for (unsigned i = 0; i < U32(scene, 4); i++) {
        size_t entry = 16 + i * 48;
        if (entry + 48 > scene.size()) {
            break;
        }
        if (std::strncmp(reinterpret_cast<const char *>(scene.data() + entry), part, 16)) {
            continue;
        }
        size_t begin = U32(scene, entry + 16), size = U32(scene, entry + 20);
        if (begin + size > scene.size()) {
            return {};
        }
        const uint8_t signature[] = {'M', 'D', 'S', 0};
        auto          first = std::search(scene.begin() + begin, scene.begin() + begin + size, std::begin(signature), std::end(signature));
        for (int level = 0; level < lod && first != scene.begin() + begin + size; level++) {
            auto next = std::search(first + 4, scene.begin() + begin + size, std::begin(signature), std::end(signature));
            if (next == scene.begin() + begin + size) {
                break;
            }
            first = next;
        }
        data.assign(first, scene.begin() + begin + size);
        break;
    }
    if (data.size() < 16) {
        return {};
    }
    auto count = U32(data, 8);
    if (16 + size_t(count) * sizeof(MDS_OBJECT) > data.size()) {
        return {};
    }
    std::vector<std::array<float, 16>> matrices(count);
    std::vector<Source>                result;
    for (unsigned i = 0; i < count; i++) {
        MDS_OBJECT object;
        std::memcpy(&object, data.data() + 16 + i * sizeof(object), sizeof(object));
        auto &matrix = matrices[i];
        std::memcpy(matrix.data(), object.matrix, 64);
        if (object.parent >= 0 && unsigned(object.parent) < i) {
            float product[4][4];
            Draw3DMul(product, reinterpret_cast<const float (*)[4]>(matrices[object.parent].data()), object.matrix);
            std::memcpy(matrix.data(), product, 64);
        }
        if (filter && !std::strstr(object.name, filter)) {
            continue;
        }
        size_t offset = object.data_ofs;
        if (!offset || offset + sizeof(MDT_HEADER) > data.size()) {
            continue;
        }
        MDT_HEADER h;
        std::memcpy(&h, data.data() + offset, sizeof(h));
        if (h.size < sizeof(h) || offset + h.size > data.size() || h.mesh_ofs < 0 || size_t(h.mesh_ofs) + 16 > h.size) {
            continue;
        }
        // Ground decals from the original Georama tree must not become floating square platforms.
        if ((std::strcmp(part, "e01t02") == 0 && std::strstr(object.name, "nuki")) || std::strstr(object.name, "kage") || std::strstr(object.name, "jimen") || std::strstr(object.name, "cube37")) {
            continue;
        }
        std::string texture;
        size_t      cursor = offset + h.mesh_ofs + 16;
        unsigned    strips = U32(data, offset + h.mesh_ofs + 8);
        auto        vec = [&](int section, unsigned index, float *out) {
            size_t at = offset + section + size_t(index) * 16;
            if (section < 0 || at + 16 > offset + h.size) {
                return false;
            }
            std::memcpy(out, data.data() + at, 16);
            return true;
        };
        for (unsigned s = 0; s < strips; s++) {
            if (cursor + 12 > offset + h.size) {
                break;
            }
            unsigned prim = U32(data, cursor), n = U32(data, cursor + 4);
            int      material = int32_t(U32(data, cursor + 8));
            cursor += 12;
            size_t stride = h.colour_ofs > 0 ? 16 : 12;
            if (cursor + size_t(n) * stride > offset + h.size) {
                break;
            }
            if (material >= 0) {
                size_t at = offset + h.info_ofs + size_t(material) * sizeof(MDT_MATERIAL);
                if (at + sizeof(MDT_MATERIAL) > offset + h.size) {
                    break;
                }
                MDT_MATERIAL m;
                std::memcpy(&m, data.data() + at, sizeof(m));
                texture.assign(m.texture, strnlen(m.texture, 44));
            }
            Source source;
            source.texture = texture;
            for (unsigned k = 0; k < n; k++, cursor += stride) {
                float p[4], normal[4], uv[4];
                if (!vec(h.vertex_ofs, U32(data, cursor), p) || !vec(h.normal[1], U32(data, cursor + 4), normal) || !vec(h.uv[1], U32(data, cursor + 8), uv)) {
                    return {};
                }
                gfx::Vertex3D vertex{};
                for (int a = 0; a < 3; a++) {
                    vertex.position[a] = matrix[12 + a];
                    for (int b = 0; b < 3; b++) {
                        vertex.position[a] += matrix[b * 4 + a] * p[b];
                        vertex.normal[a] += matrix[b * 4 + a] * normal[b];
                    }
                }
                vertex.uv[0] = uv[0];
                vertex.uv[1] = uv[1];
                std::fill_n(vertex.color, 4, 128);
                source.vertices.push_back(vertex);
            }
            if (prim == 4) {
                Draw3DStripToList(source.indices, 0, n);
            } else {
                for (unsigned k = 0; k + 2 < n; k += 3) {
                    source.indices.insert(source.indices.end(), {k, k + 1, k + 2});
                }
            }
            if (!source.indices.empty()) {
                requiredTextures[source.texture] = true;
                result.push_back(std::move(source));
            }
        }
    }
    if (filter && !result.empty()) {
        float lo[3] = {1e20f, 1e20f, 1e20f}, hi[3] = {-1e20f, -1e20f, -1e20f};
        for (const auto &src : result) {
            for (const auto &v : src.vertices) {
                for (int a = 0; a < 3; a++) {
                    lo[a] = std::min(lo[a], v.position[a]);
                    hi[a] = std::max(hi[a], v.position[a]);
                }
            }
        }
        for (auto &src : result) {
            for (auto &v : src.vertices) {
                v.position[0] -= (lo[0] + hi[0]) * .5f;
                v.position[1] -= lo[1];
                v.position[2] -= (lo[2] + hi[2]) * .5f;
            }
        }
    }
    std::fprintf(stderr, "biomes: loaded retail %s, %zu draw strips\n", part, result.size());
    return result;
}

gfx::TextureBinding Binding(const std::string &name) {
    gfx::TextureBinding result;
    if (auto it = textures.find(name); it != textures.end()) {
        result.texture = it->second.texture;
        result.palette = it->second.palette;
    } else {
        result = PortTextureFromCTexture(TexManager.GetTexture(const_cast<char *>(name.c_str()), -1)).binding;
    }
    result.filter = gfx::Filter::Linear;
    result.wrap_u = result.wrap_v = gfx::Wrap::Repeat;
    return result;
}

bool Occupied(double x, double z) {
    for (auto &p : terrain::Foundations()) {
        if (std::abs(x - p.x) < p.halfX + 90 && std::abs(z - p.z) < p.halfZ + 90) {
            return true;
        }
    }
    for (auto &r : terrain::Roads()) {
        double dx = r.endX - r.x, dz = r.endZ - r.z, ll = dx * dx + dz * dz;
        double t = ll ? std::clamp(((x - r.x) * dx + (z - r.z) * dz) / ll, 0.0, 1.0) : 0;
        if (std::hypot(x - r.x - t * dx, z - r.z - t * dz) < 120) {
            return true;
        }
    }
    return false;
}

// Low-poly silhouettes reuse the game's own stone texture and the colour
// sampled from its foliage. These replace the old "desert tree" palm/stump mix.
std::vector<Source> DesertModel(int species,int lod) {
    Source out;out.texture=species==3?"canopy1":"e01b06";
    auto tube=[&](float x,float y,float z,float radius,float height,int sides) {
        unsigned first=unsigned(out.vertices.size());
        for(int ring=0;ring<3;++ring)for(int i=0;i<sides;++i) {
            double angle=6.28318530718*i/sides;
            float r=ring==2?radius*.35f:radius;
            gfx::Vertex3D v{};v.position[0]=x+r*std::cos(angle);v.position[1]=y+height*(ring==0?0:ring==1?.85f:1.f);v.position[2]=z+r*std::sin(angle);
            v.normal[0]=std::cos(angle);v.normal[1]=ring==2?.6f:0;v.normal[2]=std::sin(angle);
            v.uv[0]=float(i)/sides;v.uv[1]=ring*.5f;std::fill_n(v.color,4,128);out.vertices.push_back(v);
        }
        for(int ring=0;ring<2;++ring)for(int i=0;i<sides;++i) {
            unsigned a=first+ring*sides+i,b=first+ring*sides+(i+1)%sides,c=a+sides,d=b+sides;
            out.indices.insert(out.indices.end(),{a,b,c,b,d,c});
        }
        for(int i=1;i<sides-1;++i)out.indices.insert(out.indices.end(),{first+2*sides,first+2*sides+unsigned(i),first+2*sides+unsigned(i+1)});
    };
    if(species==3) {
        int sides=lod==0?9:lod==1?6:4;
        tube(0,0,0,3.6f,55,sides);
        tube(-9,21,0,2.7f,21,sides);tube(10,13,0,2.5f,23,sides);
        // Arm elbows remain in every LOD so the silhouette never becomes a tree.
        tube(-4.5f,20,0,5.5f,5,sides);tube(5,12,0,6,5,sides);
    } else {
        int sides=lod==0?9:lod==1?6:4;
        tube(0,-4,0,16,25,sides);
        for(size_t i=0;i<out.vertices.size();++i) {
            auto &v=out.vertices[i];double r=.82+.35*double(terrain::Mix(i+71)&65535)/65535;
            v.position[0]*=r*1.2;v.position[1]*=r*.72;v.position[2]*=r*.85;
        }
    }
    return {std::move(out)};
}

void IndexTreeModel(std::vector<Source> &model) {
    for(auto &src:model) {
        std::map<std::array<float,8>,unsigned> unique;
        std::vector<gfx::Vertex3D> vertices;
        std::vector<unsigned> remap(src.vertices.size());
        for(size_t i=0;i<src.vertices.size();++i) {
            const auto &v=src.vertices[i];
            std::array<float,8> key{v.position[0],v.position[1],v.position[2],v.normal[0],v.normal[1],v.normal[2],v.uv[0],v.uv[1]};
            auto [it,inserted]=unique.emplace(key,unsigned(vertices.size()));
            if(inserted)vertices.push_back(v);
            remap[i]=it->second;
        }
        for(auto &index:src.indices)index=remap[index];
        src.vertices=std::move(vertices);
    }
}

const std::vector<forest::Tree> &TreePopulation(const terrain::Tile &tile) {
    struct Entry { std::vector<forest::Tree> trees; uint64_t used; };
    static thread_local std::map<terrain::Tile,Entry> cache;
    static thread_local std::vector<terrain::Foundation> foundations;
    static thread_local std::vector<terrain::Road> roads;
    static thread_local size_t count=0;
    static thread_local uint64_t clock=0;
    auto same=[](const auto &a,const auto &b){return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);};
    if(!same(foundations,terrain::Foundations()) || !same(roads,terrain::Roads())) {
        cache.clear();count=0;foundations=terrain::Foundations();roads=terrain::Roads();
    }
    ++clock;
    if(auto it=cache.find(tile);it!=cache.end()){it->second.used=clock;return it->second.trees;}
    auto population=forest::Generate(tile.left(),tile.top(),tile.size());
    count+=population.size();
    auto [it,inserted]=cache.emplace(tile,Entry{std::move(population),clock});
    // Seam topology and terrain mesh resolution do not alter the population.
    // Keep it across those rebuilds, with a bounded CPU cache for world travel.
    while(count>2000000 || cache.size()>4096) {
        auto oldest=cache.end();
        for(auto candidate=cache.begin();candidate!=cache.end();++candidate)
            if(candidate!=it && (oldest==cache.end() || candidate->second.used<oldest->second.used))oldest=candidate;
        if(oldest==cache.end())break;
        count-=oldest->second.trees.size();cache.erase(oldest);
    }
    return it->second.trees;
}
} // namespace

void BiomePrepare(CEditGround *ground) {
    if (textures.empty()) {
        regionalTrees[0] = ReadTree("e02", "e02t01");
        regionalTrees[1] = ReadTree("e03", "e03t02");
        regionalTrees[2] = ReadTree("e04", "e04t01");
        cacti = ReadTree("e01", "e01t02", "e01t02_0");
        undergrowth[0] = ReadTree("e02", "e02t01", "ha__a40t");
        undergrowth[1] = ReadTree("e02", "e02g05", "kusa");
        undergrowth[2] = ReadTree("e03", "e03t01", "__a40by");
        // Scene foliage may cover an entire original map. Give extracted clumps
        // a predictable physical footprint before scattering them independently.
        const float widths[] = {55, 30, 22};
        for (size_t i = 0; i < undergrowth.size(); i++) {
            float radius = 0;
            for (const auto &src : undergrowth[i]) {
                for (const auto &v : src.vertices) {
                    radius = std::max(radius, std::max(std::abs(v.position[0]), std::abs(v.position[2])));
                }
            }
            float scale = radius > 0 ? std::min(1.f, widths[i] / (2 * radius)) : 1.f;
            for (auto &src : undergrowth[i]) {
                for (auto &v : src.vertices) {
                    for (float &axis : v.position) {
                        axis *= scale;
                    }
                }
            }
        }
        const char *towns[] = {"e01", "e02", "e03", "e04", "e01"};
        const char *parts[] = {"e01t01", "e02t01", "e03t02", "e04t01", "e01t02"};
        for (int lod = 0; lod < 3; lod++) {
            for (int species = 0; species < 5; species++) {
                treeLods[lod][species] = species>=3?DesertModel(species,lod):ReadTree(towns[species], parts[species], nullptr, lod);
                if (treeLods[lod][species].empty() && lod > 0) {
                    treeLods[lod][species] = treeLods[lod - 1][species];
                }
                IndexTreeModel(treeLods[lod][species]);
                size_t triangles = 0;
                for (const auto &src : treeLods[lod][species]) {
                    triangles += src.indices.size() / 3;
                }
                std::fprintf(stderr, "forest model: species %d lod %d triangles %zu\n", species, lod, triangles);
            }
        }
        requiredTextures["e01b06"]=true;
        LoadTextures("e01");
        LoadTextures("e02");
        LoadTextures("e03");
        LoadTextures("e04");
        for (int species = 0; species < 5; species++) {
            float low=1e20f,high=-1e20f;
            for(const auto &src:treeLods[0][species])for(const auto &v:src.vertices){low=std::min(low,v.position[1]);high=std::max(high,v.position[1]);}
            treeHeights[species]=high-low;
            std::array<double, 3> color{};
            double                n = 0;
            for (auto &src : treeLods[0][species]) {
                if (auto it = canopyColors.find(src.texture); it != canopyColors.end()) {
                    for (int k = 0; k < 3; k++) {
                        color[k] += it->second[k];
                    }
                    n++;
                }
            }
            if(species==3){color=speciesCanopyColors[1];n=1;}
            for (int k = 0; k < 3; k++) {
                speciesCanopyColors[species][k] = color[k] / std::max(1., n);
            }
            gfx::tim2::Image image;
            auto            &pixel = image.levels.emplace_back();
            pixel.width = pixel.height = 1;
            pixel.pixels = {uint8_t(color[0] / std::max(1., n)), uint8_t(color[1] / std::max(1., n)), uint8_t(color[2] / std::max(1., n)), 255};
            textures["canopy" + std::to_string(species)] = gfx::tim2::Upload(image);

        }
    }
    auto *root = ground->plot_parts[13].frame[0];
    if (!root) {
        return;
    }
    auto identify = [](auto &&self, CFrame *frame) -> gfx::MeshHandle {
        for (; frame; frame = frame->brother) {
            auto *vu = static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(frame)->visual);
            if (auto *visual = vu ? Draw3DFindVisual(vu->vu_data) : nullptr) {
                return visual->mesh;
            }
            if (auto child = self(self, frame->child)) {
                return child;
            }
        }
        return 0;
    };
    gfx::MeshHandle identity = identify(identify, root);
    if (identity && identity == treeIdentity) {
        return;
    }
    trees.clear();
    treeIdentity = identity;
    auto visit = [&](auto &&self, CFrame *frame) -> void {
        for (; frame; frame = frame->brother) {
            auto *vis = static_cast<CVisualPolyVu1 *>(static_cast<CFrameVu1 *>(frame)->visual);
            auto *src = vis ? Draw3DFindVisual(vis->vu_data) : nullptr;
            if (src && !src->indices.empty() && !std::strstr(frame->name, "jimen") && !std::strstr(frame->name, "kage")) {
                sceVu0FMATRIX matrix;
                frame->GetLWMatrix(matrix);
                for (auto &strip : src->strips) {
                    Source dest;
                    dest.texture = strip.texture_name;
                    for (unsigned i = strip.first_index; i < strip.first_index + strip.index_count; i++) {
                        auto v = src->vertices[src->indices[i]], out = v;
                        for (int a = 0; a < 3; a++) {
                            out.position[a] = matrix[3][a];
                            out.normal[a] = 0;
                            for (int b = 0; b < 3; b++) {
                                out.position[a] += matrix[b][a] * v.position[b];
                                out.normal[a] += matrix[b][a] * v.normal[b];
                            }
                        }
                        dest.indices.push_back(unsigned(dest.vertices.size()));
                        dest.vertices.push_back(out);
                    }
                    trees.push_back(std::move(dest));
                }
            }
            self(self, frame->child);
        }
    };
    visit(visit, root);
    if (std::getenv("DC_TERRAIN_TRACE")) {
        std::fprintf(stderr, "biomes: tree prototype %zu materials, %zu textures\n", trees.size(), textures.size());
    }
}

BiomeChunk BiomeBuild(const terrain::Tile &t, const std::vector<gfx::Vertex3D> &vertices, const std::vector<uint32_t> &indices) {
    BiomeChunk result;
    result.tileSize=float(t.size());
    result.lowY=1e20f;result.highY=-1e20f;
    for (const auto &v : vertices) {
        result.lowY=std::min(result.lowY,v.position[1]-1000);
        result.highY=std::max(result.highY,v.position[1]+1000);
    }
    result.waterU = float(std::fmod(t.left(), 350) / 350);
    result.waterV = float(std::fmod(t.top(), 350) / 350);
    const char                               *names[] = {"e02b02", "e04b01", "e03b03"};
    std::array<std::vector<gfx::Vertex3D>, 3> layers;
    bool                                      used[3]{};

    struct WaterVertex {
        gfx::Vertex3D v;
        double        mask;
    };

    for (auto v : vertices) {
        double x = t.left() + v.position[0], z = t.top() + v.position[2], h = v.position[1];
        auto   climate = terrain::ClimateAt(x, z, h, t.size()/(std::sqrt(double(vertices.size()))-1));
        auto   river = t.level <= 3 ? terrain::RiverAt(x, z) : terrain::RiverSample{};
        double fade = terrain::Smooth(1800, 3500, std::hypot(x, z));
        double bank = (1 - terrain::Smooth(river.width + 35, river.width + 150, river.distance)) * fade;
        double beach=terrain::Smooth(terrain::SeaLevel-10,terrain::SeaLevel+10,h)*(1-terrain::Smooth(terrain::SeaLevel+50,terrain::SeaLevel+160,h));
        bank=std::max(bank,beach);
        double soil=1-terrain::RockExposure(x,z,h,std::clamp(double(v.normal[1]),0.,1.));
        double weights[] = {climate.forest * fade * soil, climate.sand * fade * soil, bank * soil};
        for (int i = 0; i < 3; i++) {
            auto p = v;
            p.color[3] = uint8_t(128 * weights[i]);
            layers[i].push_back(p);
            used[i] |= weights[i] > .005;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (used[i]) {
            result.ground.push_back(PrepareMesh(std::move(layers[i]), indices, names[i]));
        }
    }
    // Coarse canopy arrives with terrain, independently of the object streamer.
    // Texture filtering preserves coverage without large triangle-shaped patches.
    {
        // Continental tiles represent subpixel canopy coverage. Match their
        // texture sampling to their coarse terrain instead of evaluating 1024
        // detailed climate samples for every horizon tile.
        const int size = t.level < 2 ? 8 : t.level >= 12 ? 8 : t.level >= 8 ? 16 : 32;
        gfx::tim2::Image image;
        auto            &pixels = image.levels.emplace_back();
        pixels.width = pixels.height = size;
        pixels.pixels.resize(size * size * 4);
        const int terrainGrid = int(std::sqrt(double(vertices.size()))) - 1;
        for (int iz = 0; iz < size; iz++) {
            for (int ix = 0; ix < size; ix++) {
                double x = t.left() + (ix + .5) * t.size() / size, z = t.top() + (iz + .5) * t.size() / size;
                // The aggregate follows the displayed terrain. Reuse its height
                // grid instead of evaluating mountain generation 1024 more times.
                double gx = (ix + .5) * terrainGrid / size, gz = (iz + .5) * terrainGrid / size;
                int vx = int(gx), vz = int(gz);
                auto height = [&](int dx, int dz) { return double(vertices[(vz + dz) * (terrainGrid + 1) + vx + dx].position[1]); };
                double h = std::lerp(std::lerp(height(0, 0), height(1, 0), gx - vx),
                                     std::lerp(height(0, 1), height(1, 1), gx - vx), gz - vz);
                double footprint=t.size()/size;
                auto c=terrain::ClimateAt(x,z,h,footprint);
                double density=terrain::VegetationDensity(x,z,c,footprint);
                auto normal=[&](int dx,int dz){return double(vertices[(vz+dz)*(terrainGrid+1)+vx+dx].normal[1]);};
                double normalY=std::lerp(std::lerp(normal(0,0),normal(1,0),gx-vx),std::lerp(normal(0,1),normal(1,1),gx-vx),gz-vz);
                density*=terrain::SlopeVegetation(normalY);
                auto speciesWeights=terrain::SpeciesWeights(c);
                std::array<double,3> color{};
                double count=1;
                for(int k=0;k<3;++k) for(int species=0;species<4;++species)
                    color[k]+=speciesWeights[species]*speciesCanopyColors[species][k];
                double coverage = (1 - std::exp(-density * std::lerp(3.5,.22,c.sand))) * terrain::Smooth(2200, 3500, std::hypot(x, z));
                size_t pixel = (iz * size + ix) * 4;
                for (int k = 0; k < 3; k++) {
                    pixels.pixels[pixel + k] = uint8_t(color[k] / std::max(1., count));
                }
                pixels.pixels[pixel + 3] = uint8_t(coverage * 255);
            }
        }
        MakeCanopyMips(image);
        result.canopyImage = std::move(image);
        auto canopy = vertices;
        for (auto &v : canopy) {
            // A canopy surface above the land keeps the forest silhouette; at
            // continental scale its area-filtered coverage becomes subpixel.
            v.position[1] += 150;
            v.color[0] = 200;
            v.color[1] = 0;
            v.color[3] = 128;
            v.uv[0] = v.position[0] / t.size();
            v.uv[1] = v.position[2] / t.size();
        }
        result.canopy.push_back(PrepareMesh(std::move(canopy), indices, {}));
    }
    // Clip each surface triangle at the shore instead of drawing a rectangular water sheet.
    {
        // Water has its own sampling rate. A distant terrain vertex must never
        // drag a river's surface towards the elevation of an unrelated catchment.
        const int n=t.level<=3?int(t.size()/64):int(std::sqrt(double(vertices.size())))-1;
        const double waterStep=t.size()/n;
        std::vector<WaterVertex> water;
        for (int z = 0; z <= n; z++) {
            for (int x = 0; x <= n; x++) {
                double        wx = t.left() + x * waterStep, wz = t.top() + z * waterStep;
                auto river=t.level<=3?terrain::RiverAt(wx,wz):terrain::RiverSample{};
                gfx::Vertex3D w{};
                w.position[0] = float(x * waterStep);
                w.position[2] = float(z * waterStep);
                w.position[1] = float(river.water + .2);
                w.normal[1] = 1;
                w.color[0] = 90;
                w.color[1] = 119;
                w.color[2] = 128;
                w.color[3] = 100;
                double ground=terrain::Height(wx,wz,waterStep);
                double ocean=terrain::SeaLevel-ground;
                double channel=std::min({river.width-river.distance,river.water-ground,std::hypot(wx,wz)-3300});
                if(ocean>channel){w.position[1]=float(terrain::SeaLevel);w.color[0]=64;w.color[1]=106;w.color[2]=128;w.color[3]=120;}
                water.push_back({w,std::max(ocean,channel)});
            }
        }
        std::vector<uint32_t> waterIndices;
        for (int z = 0; z < n; z++) {
            for (int x = 0; x < n; x++) {
                unsigned a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
                waterIndices.insert(waterIndices.end(), {a, c, b, b, c, d});
            }
        }
        std::vector<gfx::Vertex3D> verts;
        std::vector<uint32_t>      inds;
        for (size_t i = 0; i < waterIndices.size(); i += 3) {
            std::vector<WaterVertex> polygon;
            for (int k = 0; k < 3; k++) {
                auto a = water[waterIndices[i + k]], b = water[waterIndices[i + (k + 1) % 3]];
                if (a.mask >= 0) {
                    polygon.push_back(a);
                }
                if ((a.mask >= 0) != (b.mask >= 0)) {
                    double f = a.mask / (a.mask - b.mask);
                    auto   p = a;
                    for (int axis = 0; axis < 3; axis++) {
                        p.v.position[axis] = float(std::lerp(double(a.v.position[axis]), double(b.v.position[axis]), f));
                    }
                    polygon.push_back(p);
                }
            }
            for (size_t k = 1; k + 1 < polygon.size(); k++) {
                for (auto p : {polygon[0], polygon[k], polygon[k + 1]}) {
                    inds.push_back(unsigned(verts.size()));
                    verts.push_back(p.v);
                }
            }
        }
        if (!inds.empty()) {
            result.water = PrepareMesh(std::move(verts), inds, "e03b07");
        }
    }
    if (t.level <= 1 && !undergrowth[0].empty()) {
        std::map<std::string, Source> batches;
        // Independent world grids keep placements stable across chunks and LOD changes.
        for (int layer = 1; layer < 2; layer++) {
            const double spacing = layer == 0 ? 100 : layer == 1 ? 44
                                                                 : 72;
            auto         x0 = int64_t(std::floor(t.left() / spacing)), z0 = int64_t(std::floor(t.top() / spacing));
            for (auto iz = z0; iz * spacing < t.top() + t.size(); iz++) {
                for (auto ix = x0; ix * spacing < t.left() + t.size(); ix++) {
                    uint64_t seed = terrain::Mix(uint64_t(ix) ^ terrain::Mix(uint64_t(iz) + 9181 + layer * 7193));
                    double   x = (ix + .12 + .76 * double(seed & 65535) / 65535) * spacing, z = (iz + .12 + .76 * double((seed >> 16) & 65535) / 65535) * spacing;
                    // Own by jittered position, including candidates just outside the tile's nominal grid.
                    if (x < t.left() || x >= t.left() + t.size() || z < t.top() || z >= t.top() + t.size()) {
                        continue;
                    }
                    if (std::hypot(x, z) < 2200 || Occupied(x, z)) {
                        continue;
                    }
                    double h = terrain::SurfaceHeight(x, z);
                    auto   c = terrain::ClimateAt(x, z, h);
                    auto   river = terrain::RiverAt(x, z);
                    double density=terrain::VegetationDensity(x,z,c);
                    if(layer==1) density=terrain::UnderstoryDensity(x,z,c);
                    if (layer == 2) {
                        density *= .42;
                    }
                    if (double((seed >> 32) & 65535) / 65535 > density || river.distance < river.width + (layer == 1 ? 8 : 30)) {
                        continue;
                    }
                    if (std::abs(terrain::Height(x + 32, z) - h) + std::abs(terrain::Height(x, z + 32) - h) > 30) {
                        continue;
                    }
                    double angle = double(seed >> 48) / 65535 * 6.28318530718, scale = .9 + double((seed >> 24) & 255) / 255 * 1.3;
                    if (layer == 1) {
                        scale = .65 + double((seed >> 24) & 255) / 255 * .9;
                    }
                    if (layer == 2) {
                        scale = (c.sand > .4 && (seed & 1)) ? .7 + double((seed >> 24) & 255) / 255 : .25 + double((seed >> 24) & 255) / 255 * .4;
                    }
                    double      cs = std::cos(angle) * scale, sn = std::sin(angle) * scale;
                    int         species = c.sand > .4 ? 2 : c.temperature > .64 ? 1
                                                                                : (c.forest > .65 && ((seed >> 20) & 3) != 0 ? 0 : -1);
                    int         groundSpecies = int((seed >> 21) % 3);
                    const auto &model = layer == 1 ? undergrowth[groundSpecies] : layer == 2 && c.sand > .4 && (seed & 1)       ? cacti
                                                                              : species >= 0 && !regionalTrees[species].empty() ? regionalTrees[species]
                                                                                                                                : trees;
                    for (const auto &source : model) {
                        auto    &dest = batches[source.texture];
                        unsigned first = unsigned(dest.vertices.size());
                        for (auto v : source.vertices) {
                            auto p = v;
                            p.position[0] = float(x - t.left() + cs * v.position[0] + sn * v.position[2]);
                            p.position[1] = float(h + scale * v.position[1] - .5);
                            p.position[2] = float(z - t.top() - sn * v.position[0] + cs * v.position[2]);
                            p.normal[0] = float((cs * v.normal[0] + sn * v.normal[2]) / scale);
                            p.normal[2] = float((-sn * v.normal[0] + cs * v.normal[2]) / scale);
                            dest.vertices.push_back(p);
                        }
                        for (auto index : source.indices) {
                            dest.indices.push_back(first + index);
                        }
                    }
                }
            }
        }
        for (auto &[name, batch] : batches) {
            if (!batch.indices.empty()) {
                result.trees.push_back(PrepareMesh(std::move(batch.vertices), batch.indices, name));
            }
        }
    }
    // Tree geometry is part of the terrain's atomic readiness, not a second
    // streaming queue. The same world population owns every tile partition.
    if (t.level <= 5 && !treeLods[0][0].empty()) {
        const auto &population = TreePopulation(t);
        uint64_t ids = 0;
        for (const auto &tree : population) ids ^= tree.id;
        for (int lod = (t.level <= 2 ? 0 : 2); lod < 3; ++lod) {
            std::map<std::string, Source> batches;
            for (const auto &tree : population) {
                double cs = std::cos(tree.yaw)*tree.scale, sn = std::sin(tree.yaw)*tree.scale;
                float height=treeHeights[tree.species];
                for (const auto &src : treeLods[lod][tree.species]) {
                    auto &dest=batches[src.texture];
                    unsigned first=unsigned(dest.vertices.size());
                    for (auto v : src.vertices) {
                        auto p=v;
                        p.position[0]=float(tree.x-t.left()+cs*v.position[0]+sn*v.position[2]);
                        p.position[1]=float(tree.y+tree.scale*v.position[1]-.5);
                        p.position[2]=float(tree.z-t.top()-sn*v.position[0]+cs*v.position[2]);
                        p.normal[0]=float((cs*v.normal[0]+sn*v.normal[2])/tree.scale);
                        p.normal[2]=float((-sn*v.normal[0]+cs*v.normal[2])/tree.scale);
                        unsigned physicalHeight=unsigned(std::clamp(std::ceil(height*tree.scale),1.,65535.));
                        p.color[0]=uint8_t(physicalHeight&255);p.color[1]=uint8_t(physicalHeight>>8);p.color[3]=128;
                        dest.vertices.push_back(p);
                    }
                    for (auto index : src.indices) dest.indices.push_back(first+index);
                }
            }
            for (auto &[texture,batch] : batches) if (!batch.indices.empty()) {
                result.forest[lod].push_back(PrepareMesh(std::move(batch.vertices), batch.indices, texture));
            }
            result.forestMask |= 1u<<lod;
        }
        if (std::getenv("DC_FOREST_TRACE")) std::fprintf(stderr,"terrain forest ready: %lld %lld level %d trees %zu ids %llu mask %u\n",
            (long long)t.x,(long long)t.z,t.level,population.size(),(unsigned long long)ids,result.forestMask);
    }
    if (std::getenv("DC_FOREST_TRACE")) std::fprintf(stderr,"terrain vegetation ready: level %d canopy %d forestMask %u\n",t.level,int(!result.canopyImage.levels.empty()),result.forestMask);
    return result;
}

bool BiomeUpload(BiomeChunk &chunk, std::chrono::steady_clock::time_point deadline) {
    auto upload=[&](BiomeMesh &m) {
        if(m.indices.empty())return true;
        if(std::chrono::steady_clock::now()>=deadline)return false;
        m.mesh=gfx::CreateMesh(m.vertices,m.indices);
        std::vector<gfx::Vertex3D>().swap(m.vertices);
        std::vector<uint32_t>().swap(m.indices);
        return true;
    };
    for(auto &m:chunk.ground)if(!upload(m))return false;
    for(auto &m:chunk.trees)if(!upload(m))return false;
    for(auto &m:chunk.canopy)if(!upload(m))return false;
    for(auto &lod:chunk.forest)for(auto &m:lod)if(!upload(m))return false;
    if(!upload(chunk.water))return false;
    if(!chunk.canopyImage.levels.empty()) {
        if(std::chrono::steady_clock::now()>=deadline)return false;
        chunk.canopyTexture=UploadCanopy(chunk.canopyImage);chunk.canopyImage={};
    }
    return true;
}

void BiomeDestroy(BiomeChunk &chunk) {
    for (auto &lod : chunk.forest) for (auto &m : lod) gfx::DestroyMesh(m.mesh);
    for (auto &m : chunk.canopy) {
        gfx::DestroyMesh(m.mesh);
    }
    for (auto &m : chunk.ground) {
        gfx::DestroyMesh(m.mesh);
    }
    for (auto &m : chunk.trees) {
        gfx::DestroyMesh(m.mesh);
    }
    gfx::DestroyMesh(chunk.water.mesh);
    if (chunk.canopyTexture) {
        gfx::DestroyTexture(chunk.canopyTexture);
    }
    chunk = {};
}

void BiomeDraw(const BiomeChunk &chunk, gfx::MeshConstants constants, gfx::DrawState state, const gfx::MeshTransform &transform, double time, int pass) {
    auto overlay = state;
    overlay.blend = true;
    overlay.alpha = {};
    overlay.depth_write = false;
    overlay.depth_test = gfx::DepthTest::GEqual;
    constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere | gfx::kMeshVertexColor;
    if (pass == 0) {
        for (auto &m : chunk.ground) {
            gfx::DrawMesh(m.mesh, 0, m.count, constants, Binding(m.texture), overlay, &transform);
        }
        return;
    }
    auto foliage = state;
    foliage.alpha_test = true;
    foliage.alpha_func = gfx::AlphaFunc::Greater;
    foliage.alpha_ref = 32;
    constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere;
    if (pass == 1) {
        auto treeConstants=constants;
        treeConstants.flags=gfx::kMeshLit|gfx::kMeshAtmosphere|gfx::kMeshTreeLod;
        treeConstants.world_uv[0][1]=float(chunk.forestMask);
        treeConstants.world_uv[0][2]=std::abs(transform.projection[5])*224.f;
        treeConstants.world_uv[0][3]=1;
        treeConstants.world_uv[1][0]=0;
        treeConstants.world_uv[1][1]=0;
        const auto &eye=treeConstants.clip_plane[0];
        double dx=std::max({-eye[0],eye[0]-chunk.tileSize,0.f});
        double dz=std::max({-eye[2],eye[2]-chunk.tileSize,0.f});
        double dy=std::max({chunk.lowY-eye[1],eye[1]-chunk.highY,0.f});
        double nearDistance=std::sqrt(dx*dx+dy*dy+dz*dz);
        double farDistance=std::hypot(std::hypot(std::max(std::abs(eye[0]),std::abs(eye[0]-chunk.tileSize)),
                                               std::max(std::abs(eye[2]),std::abs(eye[2]-chunk.tileSize))),
                                     std::max(std::abs(eye[1]-chunk.lowY),std::abs(eye[1]-chunk.highY)));
        for (int lod=0;lod<3;++lod) {
            if(std::getenv("DC_DISABLE_FOREST_MESHES"))break;
            if(chunk.forestMask==7 && ((lod==0 && nearDistance>2600) ||
                (lod==1 && (nearDistance>7000 || farDistance<1800)) || (lod==2 && farDistance<5000)))continue;
            treeConstants.world_uv[0][0]=float(lod);
            for (const auto &m : chunk.forest[lod]) gfx::DrawMesh(m.mesh,0,m.count,treeConstants,Binding(m.texture),foliage,&transform);
        }
        for (auto &m : chunk.trees) {
            gfx::DrawMesh(m.mesh, 0, m.count, constants, Binding(m.texture), foliage, &transform);
        }
        constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere | gfx::kMeshCanopy | gfx::kMeshRegionalCanopy;
        constants.world_uv[0][2] = std::abs(transform.projection[5]) * 224.f;
        constants.world_uv[0][3] = 1;
        for (auto &m : chunk.canopy) {
            if (std::getenv("DC_DISABLE_CANOPY")) {
                continue;
            }
            gfx::TextureBinding binding{};
            binding.texture = chunk.canopyTexture;
            gfx::DrawMesh(m.mesh, 0, m.count, constants, binding, overlay, &transform);
        }
        return;
    }
    if (chunk.water.mesh) {
        constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere | gfx::kMeshVertexColor | gfx::kMeshWorldUv;
        constants.world_uv[0][0] = 1.f / 350;
        constants.world_uv[0][3] = chunk.waterU + float(std::fmod(time * .006, 1.0));
        constants.world_uv[1][0] = 0;
        constants.world_uv[1][2] = 1.f / 350;
        constants.world_uv[1][3] = chunk.waterV + float(std::fmod(time * .003, 1.0));
        gfx::DrawMesh(chunk.water.mesh, 0, chunk.water.count, constants, Binding(chunk.water.texture), overlay, &transform);
    }
}

