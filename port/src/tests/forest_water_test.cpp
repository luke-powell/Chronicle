#include <gtest/gtest.h>

#include <cmath>

// SDL must be parsed before the retail GS headers define single-letter macros.
#include "gfx_fixture.hpp"

#include "../town_biomes.hpp"

TEST(ChronicleWater, CameraAndAnimationDoNotMoveTheWaterFootprint) {
    dc::test::GfxFixture fixture(320, 240, 1, dc::test::GfxOptions{.offscreen = true});
    BiomeChunk           chunk;
    chunk.water.vertices.resize(4);
    const float points[4][3]{
        {-.7f, 0, -.2f},
        {.6f,  0, -.2f},
        {-.7f, 0, .3f },
        {.6f,  0, .3f }
    };
    for (int i = 0; i < 4; ++i) {
        auto &v = chunk.water.vertices[i];
        std::copy_n(points[i], 3, v.position);
        v.normal[1] = 1;
        std::fill_n(v.color, 4, 128);
        chunk.waterFlow.push_back({3, 7});
    }
    std::vector<uint32_t> indices{0, 2, 1, 1, 2, 3};
    chunk.water.mesh = gfx::CreateMesh(chunk.water.vertices, indices);
    chunk.water.count = 6;
    auto           reference = gfx::CreateMesh(chunk.water.vertices, indices);
    auto           transform = gfx::IdentityMeshTransform();
    gfx::DrawState state{};
    state.depth_test = gfx::DepthTest::GEqual;
    state.depth_write = true;
    for (float angle : {0.f, .7f, 1.6f, 3.1f}) {
        gfx::MeshConstants c{};
        c.mvp[0] = std::cos(angle);
        c.mvp[8] = std::sin(angle);
        c.mvp[1] = -std::sin(angle);
        c.mvp[9] = std::cos(angle);
        c.mvp[14] = .5f;
        c.mvp[15] = 1;
        for (int i = 0; i < 4; ++i) {
            c.diffuse[i] = c.ambient[i] = c.ambient_material[i] = 1;
        }
        c.clip_plane[0][0] = std::sin(angle) * 2;
        c.clip_plane[0][1] = 3;
        c.clip_plane[0][2] = std::cos(angle) * 2;
        c.clip_plane[1][1] = 1;
        fixture.Frame({0, 0, 0, 128}, [&] { gfx::DrawMesh(reference, 0, 6, c, {}, state); });
        auto expected = fixture.pixels;
        for (double tick : {0., 300., 30000.}) {
            fixture.Frame({0, 0, 0, 128}, [&] { BiomeDraw(chunk, c, state, transform, tick, 2); });
            ASSERT_EQ(expected.size(), fixture.pixels.size());
            unsigned visible = 0;
            for (size_t p = 0; p < expected.size(); p += 4) {
                bool a = expected[p] || expected[p + 1] || expected[p + 2];
                bool b = fixture.pixels[p] || fixture.pixels[p + 1] || fixture.pixels[p + 2];
                ASSERT_EQ(a, b) << "angle=" << angle << " tick=" << tick << " pixel=" << p / 4;
                visible += b;
            }
            EXPECT_GT(visible, 1000u);
        }
    }
    gfx::DestroyMesh(reference);
    gfx::DestroyMesh(chunk.water.mesh);
}
