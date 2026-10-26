#include "forest_generator_version.hpp"
#include "town_biomes.hpp"
#include "gfx/capture.hpp"

#include <chrono>
#include <cstring>
#include <fstream>

#include "editground.hpp"
#include "forest_kernel.hpp"
#include "forest_disk_cache.hpp"
#include "grass_geometry.hpp"
#include "tree_impostor.hpp"
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
    return {0, unsigned(indices.size()), std::move(texture), {}, std::move(vertices), std::move(indices)};
}

std::map<std::string, gfx::tim2::Uploaded>   textures;
std::map<std::string, std::array<double, 3>> canopyColors;
std::array<std::array<double, 3>, 7>         speciesCanopyColors{};

struct Source {
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    std::string                texture;
    std::string                object;
    std::string                path;
};

std::vector<Source>                               trees;
std::array<std::vector<Source>, 3>                regionalTrees;
std::array<std::vector<Source>, 4>                undergrowth;
std::vector<Source>                               cacti;
std::map<std::string, bool>                       requiredTextures;
std::array<std::array<std::vector<Source>, 7>, 3> treeLods;
std::array<std::array<std::vector<BiomeMesh>,7>,3> treePrototypes;
std::filesystem::path populationCacheRoot;
std::array<float,7> treeHeights{};
std::array<float,7> treeRadii{};
std::map<std::string,gfx::tim2::Image> treeImages;
std::array<gfx::TextureHandle,7> treeAtlases{};
std::array<std::pair<float,float>,7> treeBillboardBounds{};
gfx::MeshHandle treeBillboard=0;
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
    while (image.levels.back().width > 1 || image.levels.back().height > 1) {
        const auto      &previous = image.levels.back();
        gfx::tim2::Level mip;
        mip.width = std::max(1u, previous.width / 2);
        mip.height = std::max(1u, previous.height / 2);
        mip.pixels.resize(mip.width * mip.height * 4);
        for (unsigned z = 0; z < mip.height; z++) {
            for (unsigned x = 0; x < mip.width; x++) {
                double                a = 0;
                std::array<double, 3> rgb{};
                for (int dz = 0; dz < 2; dz++) {
                    for (int dx = 0; dx < 2; dx++) {
                        size_t i = (std::min(z * 2 + dz,previous.height-1) * previous.width + std::min(x * 2 + dx,previous.width-1)) * 4;
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

void LoadTextures(const char *town, bool sceneModel=false) {
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
                // Retail scene archives reuse names for different pixel data.
                // Keep provenance in the key (s04/e02b02 is not e02/e02b02).
                const std::string key = (sceneModel?"scene/":"") + std::string(town) + "/" + name;
                if(textures.contains(key))continue;
                if (name != "e02b02" && name != "e04b01" && name != "e03b03" && name != "e03b07" &&
                    !requiredTextures.contains(key) && name != "e02b18" && name != "s02b23" && name != "e03b04" && name != "e03b06" && name != "e04b10" && name != "e04b04") {
                    continue;
                }
                size_t offset = begin + U32(data, entry + 32);
                if (offset + 32 > begin + size) {
                    continue;
                }
                std::vector<uint8_t> image(data.begin() + offset, data.begin() + begin + size);
                // Some IM2 entries store the VRAM reservation (RGBA size),
                // not the serialized indexed-image size, in total_size.
                if(image.size()>=64 && image[5]==0 && image[6]==1 && image[7]==0){
                    uint16_t header=0;std::memcpy(&header,image.data()+28,2);
                    uint32_t serialized=header+U32(image,20)+U32(image,24);
                    if(serialized<=image.size()-16)std::memcpy(image.data()+16,&serialized,4);
                }
                auto pictures = gfx::tim2::Parse(image);
                if (!pictures || pictures->empty()) {
                    std::fprintf(stderr,"biome texture parse failed: %s offset=%zu bytes=%zu\n",key.c_str(),offset,image.size());
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
                    treeImages[key]=*decoded;
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
                        canopyColors[key] = sum;
                    }
                    // Queens' water tile contains a foam/beach strip in its lower half.
                    // Keep the original open-water texels as a repeatable surface.
                    if (name == "e03b07") {
                        for (auto &level : decoded->levels) {
                            level.height = std::max(1u, level.height / 2);
                            level.pixels.resize(size_t(level.width) * level.height * (decoded->format == gfx::TextureFormat::Index8 ? 1 : 4));
                        }
                    }
                    if (key=="e01/e01b01" || key=="e01/e01b05" || key=="e01/e01b06" || key=="e02/e02b21" || key=="e02/e02b02" || key=="e04/e04b01" || key=="e03/e03b03" || name=="e01b20" || name=="e02b02") {
                        gfx::tim2::Image surface;
                        gfx::tim2::Level base;
                        base.width=level.width;base.height=level.height;
                        base.pixels.resize(size_t(base.width)*base.height*4);
                        for(size_t i=0;i<size_t(base.width)*base.height;++i) {
                            uint32_t rgba;
                            if(decoded->format==gfx::TextureFormat::Index8)rgba=decoded->palette[level.pixels[i]];
                            else std::memcpy(&rgba,level.pixels.data()+i*4,4);
                            std::memcpy(base.pixels.data()+i*4,&rgba,4);
                        }
                        // The Matataki cliff includes a grassy lip intended for the top
                        // of a finite wall. Use its rock interior on unbounded faces.
                        if(key=="e02/e02b21") {
                            unsigned top=base.height/3;
                            base.pixels.erase(base.pixels.begin(),base.pixels.begin()+size_t(top)*base.width*4);
                            base.height-=top;
                        }
                        surface.levels.push_back(std::move(base));MakeCanopyMips(surface);
                        textures[key]={UploadCanopy(surface),0};
                    } else textures[key] = gfx::tim2::Upload(*decoded);
                }
            }
        }
        cursor += next;
    }
}

std::vector<Source> ReadTree(const char *town, const char *part, const char *filter = nullptr, int lod = 0, bool sceneModel = false) {
    // Read the original static scene resource directly into immutable render geometry.
    // No Georama slots, texture-manager slots, or retail model arenas are consumed.
    std::ifstream        file(PathsDataRoot() / "gedit" / town / "scene.scn", std::ios::binary);
    std::vector<uint8_t> scene((std::istreambuf_iterator<char>(file)), {}), data;
    for (unsigned i = 0; i < U32(scene, 4); i++) {
        size_t entry = 16 + i * 48;
        if (entry + 48 > scene.size()) {
            break;
        }
        size_t begin = U32(scene, entry + 16), size = U32(scene, entry + 20);
        bool extra=sceneModel && begin+224<=scene.size() && std::strncmp(reinterpret_cast<const char*>(scene.data()+begin+200),part,16)==0;
        if(std::strncmp(reinterpret_cast<const char*>(scene.data()+entry),part,16) && !extra)continue;
        if(extra){size_t offset=U32(scene,begin+216);size=U32(scene,begin+220);begin+=offset;}
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
    if(data.empty() && sceneModel) {
        std::ifstream pack(PathsDataRoot()/"gedit"/town/"mds.pak",std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(pack)),{});
        for(size_t at=0;at+80<=bytes.size() && bytes[at];){
            size_t start=at+U32(bytes,at+64),size=U32(bytes,at+68),next=U32(bytes,at+72);
            if(start+size>bytes.size() || next<80)break;
            if(std::strncmp(reinterpret_cast<const char*>(bytes.data()+at),part,64)==0){data.assign(bytes.begin()+start,bytes.begin()+start+size);break;}
            at+=next;
        }
    }
    if (data.size() < 16) { return {}; }
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
        if (filter && (filter[0]=='=' ? std::strcmp(object.name,filter+1)!=0 : !std::strstr(object.name, filter))) {
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
        if (!sceneModel && ((std::strcmp(part, "e01t02") == 0 && std::strstr(object.name, "nuki")) || std::strstr(object.name, "kage") || std::strstr(object.name, "jimen") || std::strstr(object.name, "cube37"))) {
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
            source.object=object.name;
            source.path="/"+std::string(object.name)+"/";
            for(int parent=object.parent;parent>=0&&unsigned(parent)<i;){MDS_OBJECT ancestor;std::memcpy(&ancestor,data.data()+16+parent*sizeof(ancestor),sizeof(ancestor));source.path+=std::string(ancestor.name)+"/";parent=ancestor.parent;}
            source.texture = std::string(town) + "/" + texture;
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
                if(h.colour_ofs>0){float color[4];if(vec(h.colour_ofs,U32(data,cursor+12),color))for(int a=0;a<4;++a)vertex.color[a]=uint8_t(std::clamp(color[a]*128.f+.5f,0.f,255.f));}

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
    if(foresttown::Occupied(x,z))return true;
    if(foresttown::Forest(x,z)>.1 && authoredwater::Carve(x,z,foresttown::Elevation)<foresttown::Elevation-5)return true;
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
    Source out;out.texture=species==3?"canopy1":"e01/e01b06";
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

std::shared_ptr<const std::vector<forest::Tree>> TreePopulation(const terrain::Tile &tile) {
    using Population=std::shared_ptr<const std::vector<forest::Tree>>;
    using Key=std::pair<terrain::Tile,std::vector<double>>;
    static SharedMemo<Key,Population,1024> cache;
    std::vector<double> edits;
    double x=tile.left(),z=tile.top(),size=tile.size();
    for(auto &p:terrain::Foundations())if(p.x+p.halfX+464>=x&&p.x-p.halfX-464<=x+size&&p.z+p.halfZ+464>=z&&p.z-p.halfZ-464<=z+size)
        edits.insert(edits.end(),{0,p.x,p.z,p.halfX,p.halfZ,p.height,p.blendRadius,double(p.water)});
    for(auto &r:terrain::Roads())if(std::max(r.x,r.endX)+264>=x&&std::min(r.x,r.endX)-264<=x+size&&std::max(r.z,r.endZ)+264>=z&&std::min(r.z,r.endZ)-264<=z+size)
        edits.insert(edits.end(),{1,r.x,r.z,r.endX,r.endZ,r.height,r.endHeight});
    gfx::capture::Scope populationProfile("world.population.total");
    return cache.Get(Key{tile,edits},[&]{
        // Generator content + exact edits identify the population. Bump the
        // CMake schema version if this key layout or edit filtering changes.
        std::string diskKey=DC_FOREST_GENERATOR_VERSION;
        auto append=[&](const auto &value){diskKey.append(reinterpret_cast<const char *>(&value),sizeof(value));};
        append(tile.x);append(tile.z);append(tile.level);
        for(double value:edits)append(value);
        auto path=populationCacheRoot/(std::to_string(forest::CacheHash(diskKey.data(),diskKey.size()))+".forest");
        // Small populations cost less to regenerate than opening/writing tiny
        // files. Keep them in the shared memory memo; persist expensive rings.
        if(tile.level<3){
            gfx::capture::Scope generationProfile("world.population.generate");
            return std::make_shared<const std::vector<forest::Tree>>(forest::GenerateCached(x,z,size));
        }
        gfx::capture::Scope diskProfile("world.population.disk_read");
        auto saved=forest::LoadPopulation(path,diskKey);diskProfile.End();
        gfx::capture::Counter("world.population.disk_hit",bool(saved));
        if(saved)return std::make_shared<const std::vector<forest::Tree>>(std::move(*saved));
        gfx::capture::Scope generationProfile("world.population.generate");
        auto population=forest::GenerateCached(x,z,size);generationProfile.End();
        gfx::capture::Scope saveProfile("world.population.disk_write");
        forest::SavePopulation(path,diskKey,population);
        return std::make_shared<const std::vector<forest::Tree>>(std::move(population));
    });
}

} // namespace

gfx::TextureBinding BiomeSurfaceBinding(const char *name) { return Binding(name); }

void BiomePrepare(CEditGround *ground) {
    static bool initialized=false;
    if (!initialized) {
        initialized=true;
        populationCacheRoot=PathsSaveRoot()/"terrain-cache";
        if(const char *root=std::getenv("DC_TERRAIN_CACHE_ROOT"))populationCacheRoot=root;
        forest::TrimPopulationCache(populationCacheRoot);
        regionalTrees[0] = ReadTree("e02", "e02t01");
        regionalTrees[1] = ReadTree("e03", "e03t02");
        regionalTrees[2] = ReadTree("e04", "e04t01");
        cacti = ReadTree("e01", "e01t02", "e01t02_0");
        undergrowth[0] = ReadTree("e02", "e02t01", "ha__a40t");
        undergrowth[1] = ReadTree("e01", "e01h01", "=kusa__A40N");
        undergrowth[2] = ReadTree("e03", "e03t01", "__a40by");
        undergrowth[3] = ReadTree("e04", "e04m01", "=kusa__a40t");
        for(size_t i=0;i<undergrowth.size();++i){
            const float heights[]{9,5,5,8};
            undergrowth[i]=grass::Split(undergrowth[i],heights[i]);
            std::fprintf(stderr,"biome understory: palette %zu independent tufts %zu\n",i,undergrowth[i].size());
        }
        const char *towns[] = {"e01", "e02", "e03", "e04", "e01", "e04", "e01"};
        const char *parts[] = {"e01t01", "e02t01", "e03t02", "e04t01", "e01t02", "e04t01", "e01t02"};
        for (int lod = 0; lod < 3; lod++) {
            for (int species = 0; species < 7; species++) {
                treeLods[lod][species] = (species==3 || species==4)?DesertModel(species,lod):ReadTree(towns[species], parts[species], species==6?"e01t02_":nullptr, lod);
                if (species == 4) {
                    auto nativeRock = ReadTree("s04", "s04g02", "iwa03__s", lod);
                    if (!nativeRock.empty()) {
                        for (auto &source : nativeRock) for (auto &v : source.vertices)
                            for (float &axis : v.position) axis *= .65f;
                        treeLods[lod][species] = std::move(nativeRock);
                    }
                }
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
        requiredTextures["e02/e01b20"]=true;
        requiredTextures["e01/e01b06"]=true;
        requiredTextures["e01/e01b05"]=true;
        for(int lod=0;lod<3;++lod)for(int species=0;species<7;++species)for(const auto &src:treeLods[lod][species]){
            auto mesh=PrepareMesh({},src.indices,src.texture);
            mesh.mesh=gfx::CreateMesh(src.vertices,src.indices);mesh.indices.clear();
            for(const auto &v:src.vertices)mesh.radius=std::max(mesh.radius,std::hypot(std::hypot(v.position[0],v.position[1]),v.position[2]));
            treeRadii[species]=std::max(treeRadii[species],mesh.radius);
            treePrototypes[lod][species].push_back(std::move(mesh));
        }
        requiredTextures["e01/e01b01"]=true;
        requiredTextures["e02/e02b21"]=true;
        LoadTextures("e01");
        LoadTextures("e02");
        LoadTextures("e03");
        LoadTextures("e04");
        LoadTextures("s04");
        for (int species = 0; species < 7; species++) {
            float low=1e20f,high=-1e20f;
            for(const auto &src:treeLods[0][species])for(const auto &v:src.vertices){low=std::min(low,v.position[1]);high=std::max(high,v.position[1]);}
            treeHeights[species]=high-low;
            auto atlas=impostor::Bake(treeLods[2][species],[](const std::string &name,float u,float v){
                std::array<uint8_t,4> result{};auto it=treeImages.find(name);if(it==treeImages.end())return result;
                const auto &image=it->second;const auto &level=image.levels.front();
                unsigned x=std::min(level.width-1,unsigned((u-std::floor(u))*level.width)),y=std::min(level.height-1,unsigned((v-std::floor(v))*level.height));
                size_t pixel=y*level.width+x;
                if(image.format==gfx::TextureFormat::Index8){auto rgba=image.palette[level.pixels[pixel]];std::memcpy(result.data(),&rgba,4);}
                else std::copy_n(level.pixels.data()+pixel*4,4,result.begin());return result;
            });
            treeBillboardBounds[species]={atlas.centre,atlas.radius};
            std::fprintf(stderr,"tree atlas: species %d centre %.2f radius %.2f height %.2f\n",species,atlas.centre,atlas.radius,treeHeights[species]);
            gfx::tim2::Image billboardImage;billboardImage.levels.push_back({impostor::Width,impostor::Height,std::move(atlas.pixels)});
            MakeCanopyMips(billboardImage);treeAtlases[species]=UploadCanopy(billboardImage);
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
            treeImages["canopy"+std::to_string(species)]=image;
            textures["canopy" + std::to_string(species)] = gfx::tim2::Upload(image);

        }
        treeImages.clear();
        std::vector<gfx::Vertex3D> quad(4);
        for(int i=0;i<4;++i){quad[i].position[0]=(i&1)?1:-1;quad[i].position[1]=(i&2)?1:-1;quad[i].uv[0]=(i&1)?1:0;quad[i].uv[1]=(i&2)?0:1;quad[i].normal[1]=1;}
        treeBillboard=gfx::CreateMesh(quad,std::vector<uint32_t>{0,1,2,2,1,3});
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
    gfx::capture::Scope biomeProfile("world.build.biome");
    gfx::capture::Scope materialProfile("world.build.materials");
    BiomeChunk result;
    result.treeLevel=t.level;
    const int surfaceGrid=int(std::sqrt(double(vertices.size())))-1;
    auto displayedGround=[&](double x,double z){
        double gx=(x-t.left())*surfaceGrid/t.size(),gz=(z-t.top())*surfaceGrid/t.size();
        int ix=std::clamp(int(gx),0,surfaceGrid-1),iz=std::clamp(int(gz),0,surfaceGrid-1);
        auto h=[&](int dx,int dz){return vertices[(iz+dz)*(surfaceGrid+1)+ix+dx].position[1];};
        double y=grass::TriangleHeight(h(0,0),h(1,0),h(0,1),h(1,1),gx-ix,gz-iz);
        return authoredwater::Current()?authoredwater::Current()->Support(x,z,y):y;
    };
    result.tileSize=float(t.size());
    result.lowY=1e20f;result.highY=-1e20f;
    for (const auto &v : vertices) {
        result.lowY=std::min(result.lowY,v.position[1]-1000);
        result.highY=std::max(result.highY,v.position[1]+1000);
    }
    result.waterPhase=float(std::fmod(t.left()*.062831853+t.top()*.041,6.283185307179586));
    result.waterU = float(std::fmod(t.left(), 100) / 100);
    result.waterV = float(std::fmod(t.top(), 100) / 100);
    const char                               *names[] = {"e02/e02b02", "e04/e04b01", "e03/e03b03"};
    std::array<std::vector<gfx::Vertex3D>, 3> layers;
    bool                                      used[3]{};
    std::vector<terrain::Climate> grassClimate;
    std::vector<double> grassDensity;

    struct WaterVertex {
        gfx::Vertex3D v;
        double        mask, ground, proximity;
    };

    for (auto v : vertices) {
        double x = t.left() + v.position[0], z = t.top() + v.position[2], h = v.position[1];
        auto   climate = terrain::ClimateAt(x, z, h, t.size()/(std::sqrt(double(vertices.size()))-1));
        if(t.level==0){
            grassClimate.push_back(climate);
            grassDensity.push_back(terrain::UnderstoryDensity(x,z,climate));
        }
        auto   river = t.level <= 3 ? terrain::RiverAt(x, z) : terrain::RiverSample{};
        double fade = terrain::Smooth(1800, 3500, std::hypot(x, z));
        double bank = (1 - terrain::Smooth(river.width + 35, river.width + 150, river.distance)) * fade;
        double beach=terrain::Smooth(terrain::SeaLevel-10,terrain::SeaLevel+10,h)*(1-terrain::Smooth(terrain::SeaLevel+50,terrain::SeaLevel+160,h));
        bank=std::max(bank,beach);
        auto pan=terrain::DryBasinAt(x,z);
        if(pan.radius)bank=std::max(bank,1-terrain::Smooth(pan.radius*.45,pan.radius*.8,std::hypot(x-pan.x,z-pan.z)));
        double soil=1-terrain::RockExposure(x,z,h,std::clamp(double(v.normal[1]),0.,1.));
        double grassCover=(1-terrain::Smooth(.12,.55,climate.sand))*(1-climate.snow)*climate.land;
        grassCover*=std::clamp(.65+climate.forest*.7,0.,1.);
        double weights[] = {grassCover * fade * soil, climate.sand * fade * soil, bank * soil};
        // Inside the authored clearing the original grass is the base surface.
        // Natural drainage must not paint pale procedural beaches into empty Georama cells.
        double clearing=foresttown::Clearing(x,z);
        if(clearing>0){auto stream=foresttown::Stream(x,z);double bankMask=(1-terrain::Smooth(stream.width+12,stream.width+65,stream.distance));
            weights[0]=std::lerp(weights[0],1.,clearing);weights[1]*=1-clearing;weights[2]=std::lerp(weights[2],bankMask*.35,clearing);}
        double nativeGround=terrain::CanyonLandform(x,z).weight;
        for(auto &weight:weights)weight*=1-nativeGround;
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
    materialProfile.End();
    // Coarse canopy arrives with terrain, independently of the object streamer.
    // Texture filtering preserves coverage without large triangle-shaped patches.
    {
        gfx::capture::Scope canopyProfile("world.build.canopy");
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
                    color[k]+=speciesWeights[species]*(species==2?.5*(speciesCanopyColors[2][k]+speciesCanopyColors[5][k]):speciesCanopyColors[species][k]);
                if(foresttown::Occupied(x,z))density=0;
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
            // Aggregate coverage belongs to the ground, never a floating sheet.
            v.position[1] += .02f;
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
        gfx::capture::Scope waterProfile("world.build.water");
        // Water has its own sampling rate. A distant terrain vertex must never
        // drag a river's surface towards the elevation of an unrelated catchment.
        const int n=int(std::sqrt(double(vertices.size())))-1;
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
                // Clip against the actual supporting triangles, including LOD edges.
                double ground=vertices[z*(n+1)+x].position[1];
                double ocean=terrain::SeaLevel-ground;
                double channel=std::min({river.width-river.distance,river.water-ground,std::hypot(wx,wz)-3300});
                if(ocean>channel){w.position[1]=float(terrain::SeaLevel);w.color[0]=64;w.color[1]=106;w.color[2]=128;w.color[3]=120;}
                water.push_back({w,std::max(ocean,channel),ground,std::max(ocean,river.width-river.distance)});
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
        auto midpoint=[&](const WaterVertex &a,const WaterVertex &b) {
            WaterVertex p=a;
            for(int axis=0;axis<3;++axis)p.v.position[axis]=(a.v.position[axis]+b.v.position[axis])*.5f;
            p.ground=(a.ground+b.ground)*.5;
            double wx=t.left()+p.v.position[0],wz=t.top()+p.v.position[2];
            auto r=terrain::RiverAt(wx,wz);
            double ocean=terrain::SeaLevel-p.ground;
            double channel=std::min({r.width-r.distance,r.water-p.ground,std::hypot(wx,wz)-3300});
            // A refined edge must stay on its parent surface plane. Resampling
            // the height here creates T-junction cracks against an unrefined neighbour.

            p.mask=std::max(ocean,channel);p.proximity=std::max(ocean,r.width-r.distance);
            return p;
        };
        auto emitWater=[&](auto &&self,const WaterVertex &a,const WaterVertex &b,const WaterVertex &c,int depth)->void {
            double edge=std::max({std::hypot(a.v.position[0]-b.v.position[0],a.v.position[2]-b.v.position[2]),
                                 std::hypot(a.v.position[0]-c.v.position[0],a.v.position[2]-c.v.position[2]),
                                 std::hypot(b.v.position[0]-c.v.position[0],b.v.position[2]-c.v.position[2])});
            // Refine only shore/channel triangles, not every dry cell or ocean
            // tile. Water is clipped against the same supporting terrain plane.
            if(t.level<=3 && depth<2 && edge>16 && std::min({a.mask,b.mask,c.mask})<0 &&
               std::max({a.proximity,b.proximity,c.proximity})>-edge) {
                auto ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
                self(self,a,ab,ca,depth+1);self(self,ab,b,bc,depth+1);
                self(self,ca,bc,c,depth+1);self(self,ab,bc,ca,depth+1);return;
            }
            std::array<WaterVertex,3> triangle{a,b,c};
            std::vector<WaterVertex> polygon;
            for (int k = 0; k < 3; k++) {
                auto p = triangle[k], q = triangle[(k+1)%3];
                if (p.mask >= 0)polygon.push_back(p);
                if ((p.mask >= 0) != (q.mask >= 0)) {
                    double f = p.mask / (p.mask - q.mask);
                    auto r=p;
                    for(int axis=0;axis<3;++axis)r.v.position[axis]=float(std::lerp(double(p.v.position[axis]),double(q.v.position[axis]),f));
                    polygon.push_back(r);
                }
            }
            for(size_t k=1;k+1<polygon.size();++k)for(auto p:{polygon[0],polygon[k],polygon[k+1]}) {
                double wx=t.left()+p.v.position[0],wz=t.top()+p.v.position[2];
                auto flow=t.level<=3?terrain::RiverAt(wx,wz):terrain::RiverSample{};
                p.v.uv[0]=float((std::fmod(t.left(),100)+p.v.position[0])/100);
                p.v.uv[1]=float((std::fmod(t.top(),100)+p.v.position[2])/100);
                if(t.level<=3)result.waterFlow.push_back({float(flow.flowX),float(flow.flowZ)});
                inds.push_back(unsigned(verts.size()));verts.push_back(p.v);
            }
        };
        for(size_t i=0;i<waterIndices.size();i+=3)
            emitWater(emitWater,water[waterIndices[i]],water[waterIndices[i+1]],water[waterIndices[i+2]],0);
        if (!inds.empty()) {
            result.water = PrepareMesh(std::move(verts), inds, "e02/e01b20");
        }
    }
    if (t.level == 0 && !undergrowth[0].empty()) {
        gfx::capture::Scope grassProfile("world.build.grass");
        std::map<std::string, Source> batches;
        // Scatter individual retail tufts, not an entire scene-sized clump.
        // Sample the actual rendered triangles at every blade vertex so grass
        // follows slopes, stitched edges and flattened Georama foundations.
        const int n=int(std::sqrt(double(vertices.size())))-1;
        const double step=t.size()/n;
        auto ground=[&](double x,double z){
            double gx=(x-t.left())/step,gz=(z-t.top())/step;
            if(gx<0||gz<0||gx>n||gz>n)return terrain::SurfaceHeight(x,z)-.04;
            int ix=std::min(n-1,int(gx)),iz=std::min(n-1,int(gz));
            auto y=[&](int dx,int dz){return vertices[(iz+dz)*(n+1)+ix+dx].position[1];};
            return grass::TriangleHeight(y(0,0),y(1,0),y(0,1),y(1,1),gx-ix,gz-iz);
        };
        constexpr double spacing=10;
        auto x0=int64_t(std::floor(t.left()/spacing)),z0=int64_t(std::floor(t.top()/spacing));
        for(auto iz=z0;iz*spacing<t.top()+t.size();++iz)for(auto ix=x0;ix*spacing<t.left()+t.size();++ix){
            uint64_t seed=terrain::Mix(uint64_t(ix)^terrain::Mix(uint64_t(iz)+16374));
            double x=(ix+.12+.76*double(seed&65535)/65535)*spacing;
            double z=(iz+.12+.76*double((seed>>16)&65535)/65535)*spacing;
            if(x<t.left()||x>=t.left()+t.size()||z<t.top()||z>=t.top()+t.size()||std::hypot(x,z)<2200||Occupied(x,z))continue;
            if(canyon::FloraMask(x,z)==0 && foresttown::Forest(x,z)==0)continue;
            // Ecology shares the ring's spatial sample grid, just like the
            // supporting surface. Do not regenerate drainage and weathering
            // for thousands of individual ten-unit grass candidates.
            double gx=(x-t.left())/step,gz=(z-t.top())/step;
            int cx=std::clamp(int(gx),0,n-1),cz=std::clamp(int(gz),0,n-1);
            auto interpolate=[&](auto value){return std::lerp(
                std::lerp(value(cz*(n+1)+cx),value(cz*(n+1)+cx+1),gx-cx),
                std::lerp(value((cz+1)*(n+1)+cx),value((cz+1)*(n+1)+cx+1),gx-cx),gz-cz);};
            terrain::Climate c{};
            c.sand=interpolate([&](int i){return grassClimate[i].sand;});
            c.forest=interpolate([&](int i){return grassClimate[i].forest;});
            double density=interpolate([&](int i){return grassDensity[i];});
            if(double((seed>>32)&65535)/65535>density)continue;
            auto river=terrain::RiverAt(x,z);
            if(river.distance<river.width+8)continue;
            double slope=std::hypot(ground(x+2,z)-ground(x-2,z),ground(x,z+2)-ground(x,z-2))/4;
            if(slope>.85)continue;
            // The tree canopy palette produces disconnected floating leaf discs at ground level.
            // Use the retail rooted grass tufts for dense forest understory.
            int species=c.sand>.55?3:c.forest>.65?1:1+int((seed>>21)%2);
            const auto &palette=undergrowth[species];if(palette.empty())continue;
            const auto &source=palette[terrain::Mix(seed)%palette.size()];
            double angle=double(seed>>48)/65535*6.28318530718,scale=.8+double((seed>>24)&255)/255*.6;
            double cs=std::cos(angle)*scale,sn=std::sin(angle)*scale;
            auto &dest=batches[source.texture];
            if(dest.vertices.size()+source.vertices.size()>16384 && !dest.indices.empty()){
                result.trees.push_back(PrepareMesh(std::move(dest.vertices),dest.indices,source.texture));dest={};
            }
            unsigned first=unsigned(dest.vertices.size());
            for(auto v:source.vertices){
                auto p=v;
                double wx=x+cs*v.position[0]+sn*v.position[2],wz=z-sn*v.position[0]+cs*v.position[2];
                p.position[0]=float(wx-t.left());p.position[2]=float(wz-t.top());
                p.position[1]=float(ground(wx,wz)+scale*v.position[1]-.08);
                p.normal[0]=float((cs*v.normal[0]+sn*v.normal[2])/scale);
                p.normal[2]=float((-sn*v.normal[0]+cs*v.normal[2])/scale);
                dest.vertices.push_back(p);
            }
            for(auto index:source.indices)dest.indices.push_back(first+index);
        }
        for (auto &[name, batch] : batches) {
            if (!batch.indices.empty()) {
                result.trees.push_back(PrepareMesh(std::move(batch.vertices), batch.indices, name));
            }
        }
    }
    // Tree geometry is part of the terrain's atomic readiness, not a second
    // streaming queue. The same world population owns every tile partition.
    if (t.level <= 7 && !treeLods[0][0].empty()) {
        gfx::capture::Scope treesProfile("world.build.forest");
        auto ownedPopulation = TreePopulation(t);
        const auto &population=*ownedPopulation;
        uint64_t ids = 0;
        for (const auto &tree : population) ids ^= tree.id;
        {
            // Keep every tree, but give coarse tiles spatially bounded batches.
            // A continental tile must not submit its entire forest merely
            // because one corner intersects the camera.
            using Page=std::pair<int,int>;
            std::array<std::map<Page,std::vector<gfx::Vertex3D>>,7> batches;
            std::array<BiomeMesh,7> buffers;
            auto finish=[&](int species){auto &mesh=buffers[species];if(mesh.vertices.empty())return;
                mesh.count=unsigned(mesh.vertices.size());mesh.indices={0,0,0};
                result.instances[species].push_back(std::move(mesh));mesh={};
            };
            auto flush=[&](int species,std::vector<gfx::Vertex3D> &vertices){if(vertices.empty())return;
                auto &mesh=buffers[species];if(mesh.vertices.size()+vertices.size()>16384)finish(species);
                BiomeMesh::Page page{unsigned(mesh.vertices.size()),unsigned(vertices.size()),{}};
                for(const auto &v:vertices)page.bounds.Add(v.position,treeRadii[species]*v.normal[2],float(unsigned(v.color[0])+256*unsigned(v.color[1])));
                mesh.pages.push_back(page);mesh.vertices.insert(mesh.vertices.end(),vertices.begin(),vertices.end());vertices={};
            };
            for(const auto &tree:population){
                gfx::Vertex3D instance{};
                instance.position[0]=float(tree.x-t.left());instance.position[1]=float(displayedGround(tree.x,tree.z)-.5);instance.position[2]=float(tree.z-t.top());
                instance.normal[0]=float(std::cos(tree.yaw)*tree.scale);instance.normal[1]=float(std::sin(tree.yaw)*tree.scale);instance.normal[2]=float(tree.scale);
                unsigned height=unsigned(std::clamp(std::ceil(treeHeights[tree.species]*tree.scale),1.,65535.));
                instance.color[0]=uint8_t(height&255);instance.color[1]=uint8_t(height>>8);instance.color[3]=128;
                auto &page=batches[tree.species][{int(instance.position[0]/4096),int(instance.position[2]/4096)}];
                page.push_back(instance);if(page.size()>=16384)flush(tree.species,page);
            }
            for(int species=0;species<7;++species){for(auto &[key,page]:batches[species])flush(species,page);finish(species);}
            result.forestMask=1u<<std::min(t.level,2);
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
        gfx::capture::Scope meshUpload("world.upload.mesh");
        gfx::capture::Counter("world.upload.mesh_bytes",double(m.vertices.size()*sizeof(gfx::Vertex3D)+m.indices.size()*sizeof(uint32_t)));
        m.mesh=gfx::CreateMesh(m.vertices,m.indices);
        std::vector<gfx::Vertex3D>().swap(m.vertices);
        std::vector<uint32_t>().swap(m.indices);
        return true;
    };
    for(auto &m:chunk.ground)if(!upload(m))return false;
    for(auto &m:chunk.trees)if(!upload(m))return false;
    for(auto &m:chunk.canopy)if(!upload(m))return false;
    for(auto &lod:chunk.forest)for(auto &m:lod)if(!upload(m))return false;
    for(auto &species:chunk.instances)for(auto &m:species)if(!upload(m))return false;
    if(chunk.water.count && !chunk.water.mesh){
        if(std::chrono::steady_clock::now()>=deadline)return false;
        chunk.water.mesh=gfx::CreateMesh(chunk.water.vertices,chunk.water.indices);
        std::vector<uint32_t>().swap(chunk.water.indices);
        if(chunk.waterFlow.empty())std::vector<gfx::Vertex3D>().swap(chunk.water.vertices);
    }
    if(!chunk.canopyImage.levels.empty()) {
        if(std::chrono::steady_clock::now()>=deadline)return false;
        gfx::capture::Scope textureUpload("world.upload.texture");
        chunk.canopyTexture=UploadCanopy(chunk.canopyImage);chunk.canopyImage={};
    }
    return true;
}

void BiomeDestroy(BiomeChunk &chunk) {
    for(auto &species:chunk.instances)for(auto &m:species)gfx::DestroyMesh(m.mesh);
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

void BiomeDraw(const BiomeChunk &chunk, gfx::MeshConstants constants, gfx::DrawState state, const gfx::MeshTransform &transform, double time, int pass, int targetLevel) {
    const int treeLevel=std::max(chunk.treeLevel,targetLevel);
    const unsigned forestMask=treeLevel<=7?(1u<<std::min(treeLevel,2)):0;
    auto overlay = state;
    overlay.blend = true;
    overlay.alpha = {};
    overlay.depth_write = false;
    overlay.depth_test = gfx::DepthTest::GEqual;
    constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere | gfx::kMeshVertexColor;
    if (pass == 0) {
        constants.flags |= gfx::kMeshWorldUv | gfx::kMeshTerrainTriplanar;
        for (auto &m : chunk.ground) {
            // Keep the retail forest grass sharp and phase-aligned with native ground.
            if(m.texture=="e02/e02b02")constants.flags &= ~gfx::kMeshTerrainTriplanar;
            else constants.flags |= gfx::kMeshTerrainTriplanar;
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
        treeConstants.flags=gfx::kMeshLit|gfx::kMeshAtmosphere|gfx::kMeshTreeLod|gfx::kMeshRingFoliage;
        treeConstants.world_uv[0][1]=float(forestMask);
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
        for (int lod=0;lod<3 && treeLevel<=7;++lod) {
            if(std::getenv("DC_DISABLE_FOREST_MESHES"))break;
            if(forestMask==7 && ((lod==0 && nearDistance>2600) ||
                (lod==1 && (nearDistance>7000 || farDistance<1800)) || (lod==2 && farDistance<5000)))continue;
            treeConstants.world_uv[0][0]=float(lod);
            for (const auto &m : chunk.forest[lod]) gfx::DrawMesh(m.mesh,0,m.count,treeConstants,Binding(m.texture),foliage,&transform);
            if(lod==std::min(treeLevel,2))for(int species=0;species<7;++species)for(const auto &instances:chunk.instances[species]){
             for(int representation=treeLevel<3?1:0;representation==(treeLevel<3?1:0);++representation){
              unsigned first=0,count=0;
              auto draw=[&]{if(!count)return;
               if(representation==0){
                auto bill=treeConstants;bill.flags|=gfx::kMeshTreeBillboard|gfx::kMeshRingFoliage;
                bill.world_uv[1][0]=treeBillboardBounds[species].first;
                bill.world_uv[1][1]=treeBillboardBounds[species].second;
                bill.world_uv[1][2]=treeRadii[species];
                gfx::TextureBinding atlasBinding{};atlasBinding.texture=treeAtlases[species];atlasBinding.filter=gfx::Filter::Linear;
                gfx::DrawMeshInstanced(treeBillboard,0,6,instances.mesh,count,bill,atlasBinding,foliage,&transform,first);
               }else{
                auto detailConstants=treeConstants;
                for(const auto &prototype:treePrototypes[lod][species]){
                    detailConstants.world_uv[1][2]=prototype.radius;
                    gfx::DrawMeshInstanced(prototype.mesh,0,prototype.count,instances.mesh,count,detailConstants,Binding(prototype.texture),foliage,&transform,first);
                }
               }
                count=0;
              };
              for(const auto &page:instances.pages){
                if(!page.bounds.Visible(treeConstants.mvp,treeConstants.world_uv[0][2],0.f)){draw();continue;}
                if(!count)first=page.first;
                count+=page.count;
              }
              draw();
             }
            }
        }
        auto grassConstants=constants;
        grassConstants.flags|=gfx::kMeshGrass;
        for (auto &m : chunk.trees) {
            if(nearDistance>2400)break;
            gfx::DrawMesh(m.mesh, 0, m.count, grassConstants, Binding(m.texture), foliage, &transform);
        }
        constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere | gfx::kMeshCanopy | gfx::kMeshRegionalCanopy;
        constants.world_uv[0][2] = std::abs(transform.projection[5]) * 224.f;
        constants.world_uv[0][3] = forestMask ? 1.f : 0.f;
        constants.flags |= gfx::kMeshRingFoliage;
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
        Draw3DIdentityScope identity(&chunk.water,1000);
        // Positions never depend on the eye, frame-copy UVs, or animation time.
        // Advect only texture coordinates using the drainage tangent at each vertex.
        auto vertices=chunk.water.vertices;
        double seconds=time/30.;
        for(size_t i=0;i<vertices.size();++i){
            auto &v=vertices[i];const auto &flow=chunk.waterFlow[i];
            v.uv[0]-=float(seconds*flow[0]/100);
            v.uv[1]-=float(seconds*flow[1]/100);
            double phase=chunk.waterPhase+v.position[0]*.062831853+v.position[2]*.041;
            v.uv[0]+=float(.006*std::sin(seconds*.7+phase));
            v.uv[1]+=float(.006*std::cos(seconds*.53-phase));
            double dx=constants.clip_plane[0][0]-v.position[0],dy=constants.clip_plane[0][1]-v.position[1],dz=constants.clip_plane[0][2]-v.position[2];
            double facing=std::abs(dy)/std::max(1.,std::sqrt(dx*dx+dy*dy+dz*dz));
            double fresnel=.04+.35*std::pow(1-facing,5);
            v.color[0]=uint8_t(90+20*fresnel);v.color[1]=uint8_t(112+12*fresnel);v.color[2]=uint8_t(118+10*fresnel);
            v.color[3]=uint8_t(124+4*fresnel);
        }
        if(!vertices.empty())gfx::UpdateMeshVertices(chunk.water.mesh,0,vertices);
        constants.flags = gfx::kMeshLit | gfx::kMeshAtmosphere | gfx::kMeshVertexColor;
        std::memset(constants.world_uv,0,sizeof(constants.world_uv));
        if(vertices.empty()){
            constants.flags|=gfx::kMeshWorldUv;
            constants.world_uv[0][0]=constants.world_uv[1][2]=1.f/100;
            constants.world_uv[0][3]=chunk.waterU+float(.006*std::sin(seconds*.7));
            constants.world_uv[1][3]=chunk.waterV+float(.006*std::cos(seconds*.53));
        }
        gfx::DrawMesh(chunk.water.mesh, 0, chunk.water.count, constants, Binding(chunk.water.texture), overlay, &transform);
    }
}

std::vector<BiomeMesh> BiomeReadScene(const char *town,const char *part) {
    auto sources=ReadTree(town,part,nullptr,0,true);
    std::ifstream file(PathsDataRoot()/"gedit"/town/"scene.scn",std::ios::binary);
    std::vector<uint8_t> scene((std::istreambuf_iterator<char>(file)),{});
    for(unsigned i=0;i<U32(scene,4);++i){size_t e=16+i*48;if(e+48>scene.size())break;
        if(std::strncmp(reinterpret_cast<const char*>(scene.data()+e),part,16))continue;
        size_t b=U32(scene,e+16);if(b+224>scene.size() || !U32(scene,b+220))break;
        std::string extra(reinterpret_cast<const char*>(scene.data()+b+200),strnlen(reinterpret_cast<const char*>(scene.data()+b+200),16));
        auto ripples=ReadTree(town,extra.c_str(),nullptr,0,true);
        for(auto &s:ripples)sources.push_back(std::move(s));break;
    }
    std::vector<BiomeMesh> result;
    for(auto &s:sources){
        if(std::getenv("DC_FOREST_MATERIAL_AUDIT")){float low=1e9f,high=-1e9f;for(auto &v:s.vertices){low=std::min(low,v.position[1]);high=std::max(high,v.position[1]);}std::fprintf(stderr,"forest material %s %s %s y=%g..%g\n",part,s.object.c_str(),s.texture.c_str(),low,high);}

        // Strip the retail arena walls and distant billboard trees; the world
        // supplies continuous terrain and an actual forest beyond the village.
        if(s.object=="cube4__f" || s.object=="extru2__a40ns" ||
           s.object.find("by")!=std::string::npos || s.object=="effect__cfzappn" || s.object=="kiri__a01zt")continue;
        if(std::getenv("DC_FOREST_MATERIAL_AUDIT") && s.texture=="e02/e02b02")for(size_t i=0;i+2<s.indices.size();i+=3){auto &a=s.vertices[s.indices[i]],&b=s.vertices[s.indices[i+1]],&c=s.vertices[s.indices[i+2]];
            double area=std::abs((b.position[0]-a.position[0])*(c.position[2]-a.position[2])-(c.position[0]-a.position[0])*(b.position[2]-a.position[2]));
            double uv=std::abs((b.uv[0]-a.uv[0])*(c.uv[1]-a.uv[1])-(c.uv[0]-a.uv[0])*(b.uv[1]-a.uv[1]));if(uv>.0001)std::fprintf(stderr,"forest grass period %g\n",std::sqrt(area/uv));}
        if(s.texture=="e02/e02b12")s.texture="e02/e02b14";
        if(s.texture=="e02/e02b13")s.texture="e02/e02b15";
        if(s.object.find("kage")!=std::string::npos || s.object.find("__a01")!=std::string::npos || s.object.find("__A01")!=std::string::npos)for(auto &v:s.vertices)v.color[3]=uint8_t(v.color[3]*.28f);
        s.texture="scene/"+s.texture;requiredTextures[s.texture]=true;
        auto mesh=PrepareMesh(std::move(s.vertices),std::move(s.indices),s.texture);mesh.sceneObjects=s.path;result.push_back(std::move(mesh));
    }
    return result;
}
void BiomeLoadSceneTextures(const char *town) { LoadTextures(town,true); }
