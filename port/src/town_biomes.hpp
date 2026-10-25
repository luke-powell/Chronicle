#pragma once
#include "draw3d.hpp"
#include "gfx/tim2.hpp"
#include <chrono>
#include "terrain_kernel.hpp"
class CEditGround;

struct BiomeMesh {
    gfx::MeshHandle mesh = 0;
    unsigned        count = 0;
    std::string     texture;
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t> indices;
};

struct BiomeChunk {
    std::vector<BiomeMesh> ground, trees;
    std::vector<BiomeMesh> canopy;
    std::array<std::vector<BiomeMesh>, 3> forest;
    unsigned forestMask = 0;
    float tileSize = 0, lowY = 0, highY = 0;
    gfx::TextureHandle     canopyTexture = 0;
    gfx::tim2::Image canopyImage;
    BiomeMesh              water;
    float                  waterU = 0, waterV = 0;
};

void       BiomePrepare(CEditGround *ground);
gfx::TextureBinding BiomeSurfaceBinding(const char *name);
BiomeChunk BiomeBuild(const terrain::Tile &, const std::vector<gfx::Vertex3D> &, const std::vector<uint32_t> &);
bool       BiomeUpload(BiomeChunk &, std::chrono::steady_clock::time_point deadline);
void       BiomeDestroy(BiomeChunk &);
void       BiomeDraw(const BiomeChunk &, gfx::MeshConstants, gfx::DrawState, const gfx::MeshTransform &, double time, int pass);
struct RenderInfo;
