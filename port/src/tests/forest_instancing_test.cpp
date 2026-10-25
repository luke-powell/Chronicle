#include <gtest/gtest.h>
#include <algorithm>
#include "gfx_fixture.hpp"
#include "gfx/displaylist.hpp"

TEST(ChronicleForest, BillboardKeepsInstanceScaleAndPosition) {
    dc::test::GfxFixture fixture(320,240,1,dc::test::GfxOptions{.offscreen=true});
    std::vector<gfx::Vertex3D> quad(4),expectedVertices(4),instance(1);
    for(int i=0;i<4;++i){quad[i].position[0]=(i&1)?1:-1;quad[i].position[1]=(i&2)?1:-1;
        expectedVertices[i]=quad[i];expectedVertices[i].position[0]*=.2f;expectedVertices[i].position[1]*=.2f;expectedVertices[i].position[2]=.5f;}
    instance[0].position[2]=.5f;instance[0].normal[0]=instance[0].normal[2]=.2f;instance[0].color[0]=75;
    const std::vector<uint32_t> indices{0,1,2,2,1,3};
    auto model=gfx::CreateMesh(quad,indices),reference=gfx::CreateMesh(expectedVertices,indices),placements=gfx::CreateMesh(instance,indices);
    gfx::MeshConstants c{};for(int i=0;i<4;++i){c.mvp[i*5]=1;c.diffuse[i]=1;}
    fixture.Frame({0,0,0,128},[&]{gfx::DrawMesh(reference,0,6,c,{},{});});auto expected=fixture.pixels;
    c.flags=gfx::kMeshTreeLod|gfx::kMeshTreeBillboard;c.world_uv[0][0]=2;c.world_uv[0][1]=4;c.world_uv[0][2]=.01f;c.world_uv[0][3]=1;
    c.world_uv[1][1]=c.world_uv[1][2]=1;c.clip_plane[0][2]=2;
    fixture.Frame({0,0,0,128},[&]{gfx::DrawMeshInstanced(model,0,6,placements,1,c,{},{});});
    EXPECT_EQ(expected,fixture.pixels);
    for(float pixels:{1.5f,2.f,2.5f,3.f,3.5f,4.f,5.f}){
        c.world_uv[0][2]=pixels/75.f;
        auto detail=c;detail.flags=gfx::kMeshTreeLod|gfx::kMeshTreeDetail;
        fixture.Frame({0,0,0,128},[&]{
            gfx::DrawMeshInstanced(model,0,6,placements,1,c,{},{});
            gfx::DrawMeshInstanced(model,0,6,placements,1,detail,{},{});
        });
        EXPECT_EQ(expected,fixture.pixels)<<"Complementary transition at "<<pixels<<" pixels";
    }
    // Ring-owned trees have exactly one resident representation. Changing
    // projection/altitude must not fade it out or enlarge its physical quad.
    c.flags|=gfx::kMeshRingFoliage;
    for(float pixels:{.01f,.15f,1.f,4.f,1000.f}) {
        c.world_uv[0][2]=pixels/75.f;
        fixture.Frame({0,0,0,128},[&]{gfx::DrawMeshInstanced(model,0,6,placements,1,c,{},{});});
        EXPECT_EQ(expected,fixture.pixels)<<"Ring-owned billboard at "<<pixels<<" pixels";
    }
    gfx::DestroyMesh(model);gfx::DestroyMesh(reference);gfx::DestroyMesh(placements);
}

TEST(ChronicleForest, ConservativeEarlyCullingPreservesVisiblePixels) {
    dc::test::GfxFixture fixture(320,240,1,dc::test::GfxOptions{.offscreen=true});
    std::vector<gfx::Vertex3D> source(3),instances(8);
    source[0].position[0]=-.25f;source[0].position[1]=-.25f;
    source[1].position[0]= .25f;source[1].position[1]=-.25f;
    source[2].position[1]=.25f;
    const float roots[8][3]={{0,0,1},{1.1f,0,1},{-1.1f,0,1},{0,1.1f,1},
                            {100,0,1},{0,0,-1},{0,0,10000},{0,-1.1f,1}};
    for(int i=0;i<8;++i){auto &v=instances[i];std::copy(roots[i],roots[i]+3,v.position);
        v.normal[0]=v.normal[2]=1;v.color[0]=2;v.color[3]=128;}
    const std::vector<uint32_t> indices{0,1,2};
    auto model=gfx::CreateMesh(source,indices),placements=gfx::CreateMesh(instances,indices);
    gfx::MeshConstants c{};c.mvp[0]=c.mvp[5]=c.mvp[11]=1;c.mvp[14]=.1f;
    for(auto &v:c.diffuse)v=1;
    c.flags=gfx::kMeshTreeLod;c.world_uv[0][0]=2;c.world_uv[0][1]=4;
    c.world_uv[0][2]=224;c.world_uv[0][3]=1;
    fixture.Frame({0,0,0,128},[&]{gfx::DrawMeshInstanced(model,0,3,placements,8,c,{},{});});
    auto expected=fixture.pixels;
    c.world_uv[1][2]=.36f;
    fixture.Frame({0,0,0,128},[&]{gfx::DrawMeshInstanced(model,0,3,placements,8,c,{},{});});
    EXPECT_EQ(expected,fixture.pixels);
    gfx::DestroyMesh(model);gfx::DestroyMesh(placements);
}

TEST(ChronicleForest, InstancesMatchExpandedModelsAndRecordedReplay) {
    dc::test::GfxFixture fixture(320,240,1,dc::test::GfxOptions{.offscreen=true});
    std::vector<gfx::Vertex3D> source(3),instances(2),expanded;
    source[0].position[0]=-.25f;source[0].position[1]=-.25f;
    source[1].position[0]= .25f;source[1].position[1]=-.25f;
    source[2].position[1]=.25f;
    for(auto &v:source)v.normal[2]=1;
    instances[0].position[0]=-.5f;instances[1].position[0]=.5f;
    for(int i=0;i<2;++i){auto &v=instances[i];v.position[2]=.5f;v.normal[0]=i?-1:1;v.normal[2]=1;v.color[i]=64+32*i;v.color[3]=128;}
    std::vector<uint32_t> indices{0,1,2},expandedIndices;
    for(auto &instance:instances)for(auto v:source){
        float x=v.position[0],z=v.position[2],cs=instance.normal[0],sn=instance.normal[1];
        v.position[0]=instance.position[0]+cs*x+sn*z;v.position[1]+=instance.position[1];v.position[2]=instance.position[2]-sn*x+cs*z;
        std::copy(std::begin(instance.color),std::end(instance.color),v.color);
        expandedIndices.push_back(unsigned(expanded.size()));expanded.push_back(v);
    }
    auto model=gfx::CreateMesh(source,indices),placements=gfx::CreateMesh(instances,indices),reference=gfx::CreateMesh(expanded,expandedIndices);
    gfx::MeshConstants c{};for(int i=0;i<4;++i){c.mvp[i*5]=1;c.diffuse[i]=1;}
    c.flags=gfx::kMeshVertexColor;
    fixture.Frame({0,0,0,128},[&]{gfx::DrawMesh(reference,0,unsigned(expandedIndices.size()),c,{},{});});
    auto expected=fixture.pixels;
    ASSERT_GT(std::count_if(expected.begin(),expected.end(),[](auto p){return p!=0&&p!=255;}),10);
    fixture.Frame({0,0,0,128},[&]{gfx::DrawMeshInstanced(model,0,3,placements,2,c,{},{});});
    EXPECT_EQ(expected,fixture.pixels);
    gfx::BeginRecording();const uint8_t black[]{0,0,0,128};gfx::Clear(true,black,true,0);
    // Visible pages share one buffer and may start at a nonzero instance.
    gfx::DrawMeshInstanced(model,0,3,placements,1,c,{}, {},nullptr,0);
    gfx::DrawMeshInstanced(model,0,3,placements,1,c,{}, {},nullptr,1);
    auto list=gfx::EndRecording();
    ASSERT_TRUE(gfx::RenderList(*list,1,{.canonical=true}));
    ASSERT_TRUE(gfx::RenderList(*list,1,{}));ASSERT_TRUE(gfx::ReadbackFrame(fixture.pixels,fixture.width,fixture.height));
    EXPECT_EQ(expected,fixture.pixels);
    list.reset();gfx::DestroyMesh(model);gfx::DestroyMesh(placements);gfx::DestroyMesh(reference);
}
