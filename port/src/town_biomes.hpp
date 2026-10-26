#pragma once
#include "draw3d.hpp"
#include "gfx/tim2.hpp"
#include <chrono>
#include "terrain_kernel.hpp"
#include "foliage_bounds.hpp"
class CEditGround;

struct BiomeMesh {
    struct Page { unsigned first,count; foliage::Bounds bounds; };
    gfx::MeshHandle mesh = 0;
    unsigned        count = 0;
    std::string     texture;
    std::string     sceneObjects;
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t> indices;
    float           radius = 0;
    std::vector<Page> pages;
};

struct BiomeChunk {
    std::vector<BiomeMesh> ground, trees;
    std::vector<BiomeMesh> canopy;
    std::array<std::vector<BiomeMesh>, 3> forest;
    std::array<std::vector<BiomeMesh>, 7> instances;
    unsigned forestMask = 0;
    int treeLevel = 0;
    float tileSize = 0, lowY = 0, highY = 0;
    gfx::TextureHandle     canopyTexture = 0;
    gfx::tim2::Image canopyImage;
    BiomeMesh              water;
    std::vector<std::array<float,2>> waterFlow;
    float                  waterU = 0, waterV = 0, waterPhase = 0;
};

void       BiomePrepare(CEditGround *ground);
gfx::TextureBinding BiomeSurfaceBinding(const char *name);
BiomeChunk BiomeBuild(const terrain::Tile &, const std::vector<gfx::Vertex3D> &, const std::vector<uint32_t> &);
bool       BiomeUpload(BiomeChunk &, std::chrono::steady_clock::time_point deadline);
void       BiomeDestroy(BiomeChunk &);
void       BiomeDraw(const BiomeChunk &, gfx::MeshConstants, gfx::DrawState, const gfx::MeshTransform &, double time, int pass, int targetLevel = -1);
struct RenderInfo;

std::vector<BiomeMesh> BiomeReadScene(const char *town,const char *part);
void BiomeLoadSceneTextures(const char *town);
